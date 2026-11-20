/*
* Copyright (c) 2025 Odalith
 *
 * ffaudio is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * ffaudio is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFaudio; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * Background measurement queue: jobs (one file, or one album) are measured by low-priority worker threads
 * and stored in the ReplayGain cache. Urgent jobs go to the front (newest first), prefetch jobs to the back.
 * Workers start on demand and keep running until au_gain_scan_stop().
 */

#include <sys/stat.h>

#include "gain_cache.h"
#include "once.h"
#include "ffaudio_loudness.h"
#include "network.h"

#define MAX_WORKERS 16
#define MAX_FAILED 1024          // files remembered as not measurable (the oldest are forgotten first)

typedef struct Job {
    char **paths;
    int n;
    bool album;
    bool from_player;           // not dropped by au_gain_cache_cancel_requests()
    struct Job *next;
} Job;

typedef struct Worker {
    SDL_Thread *thread;
    Job *job;                   // being measured
    bool cancel;                // stop the current job
} Worker;

// Guarded by lock
static SDL_mutex *lock = NULL;
static SDL_cond *wake = NULL;
static FfOnce init_once = FF_ONCE_INIT;
static Job *head = NULL, *tail = NULL;
static Worker workers[MAX_WORKERS];
static AuGainScanConfig config;
static int pending = 0;          // paths queued or being measured
static bool stopping = false;

// Files that could not be measured (not media, no audio stream, unreadable), with their size and modification
// time: requests skip them until the file changes, instead of reading and failing again on every request.
typedef struct Failed {
    char *path;
    int64_t size, mtime;         // -1 when stat() failed (e.g. a missing file)
} Failed;
static Failed failed[MAX_FAILED];
static int n_failed = 0, failed_oldest = 0;

static int default_threads(void) {
    return SDL_GetCPUCount() > 1 ? 2 : 1;
}

static void init(void) {
    lock = SDL_CreateMutex();
    wake = SDL_CreateCond();
    memset(&config, 0, sizeof(config));
    config.threads = default_threads();
}

static bool ready(void) {
    ff_once(&init_once, init);
    return lock && wake;
}

//+-----------------------------------------------------+
//| Files that could not be measured (lock held)        |
//+-----------------------------------------------------+

static void file_stamp(const char *path, int64_t *size, int64_t *mtime) {
    struct stat st;
    if (stat(path, &st) == 0) {
        *size = (int64_t)st.st_size;
        *mtime = (int64_t)st.st_mtime;
    }
    else {
        *size = *mtime = -1;
    }
}

static int find_failed(const char *path) {
    for (int i = 0; i < n_failed; i++)
        if (!strcmp(failed[i].path, path)) return i;
    return -1;
}

static void forget_failed_at(const int i) {
    av_free(failed[i].path);
    failed[i] = failed[--n_failed];
    if (failed_oldest >= n_failed) failed_oldest = 0;
}

// true when `path` failed before and has not changed since (a changed file is forgotten and measured again)
static bool known_failure(const char *path) {
    const int i = find_failed(path);
    if (i < 0) return false;
    int64_t size, mtime;
    file_stamp(path, &size, &mtime);
    if (size == failed[i].size && mtime == failed[i].mtime) return true;
    forget_failed_at(i);
    return false;
}

static void remember_failure(const char *path) {
    int i = find_failed(path);
    if (i < 0) {
        char *copy = av_strdup(path);
        if (!copy) return;
        if (n_failed < MAX_FAILED) {
            i = n_failed++;
        }
        else {
            i = failed_oldest;
            failed_oldest = (failed_oldest + 1) % MAX_FAILED;
            av_free(failed[i].path);
        }
        failed[i].path = copy;
    }
    file_stamp(path, &failed[i].size, &failed[i].mtime);
}

static void forget_all_failures(void) {
    for (int i = 0; i < n_failed; i++) av_free(failed[i].path);
    n_failed = failed_oldest = 0;
}

// For workers (lock not held)
static void note_result(const char *path, const int result) {
    if (result != AU_ERR_FILE) return; // not cancellations or memory errors: those say nothing about the file
    SDL_LockMutex(lock);
    remember_failure(path);
    SDL_UnlockMutex(lock);
}

//+-----------------------------------------------------+
//| Jobs (lock held)                                    |
//+-----------------------------------------------------+

static void job_free(Job *j) {
    if (!j) return;
    for (int i = 0; i < j->n; i++) av_free(j->paths[i]);
    av_free(j->paths);
    av_free(j);
}

static bool job_has(const Job *j, const char *path) {
    for (int i = 0; i < j->n; i++)
        if (!strcmp(j->paths[i], path)) return true;
    return false;
}

// The queued job containing `path` (and the one before it, for unlinking)
static Job *find_queued(const char *path, Job **prev_out) {
    Job *prev = NULL;
    for (Job *j = head; j; prev = j, j = j->next) {
        if (job_has(j, path)) {
            if (prev_out) *prev_out = prev;
            return j;
        }
    }
    return NULL;
}

static bool being_measured(const char *path) {
    for (int i = 0; i < MAX_WORKERS; i++)
        if (workers[i].job && job_has(workers[i].job, path)) return true;
    return false;
}

static void unlink_job(Job *j, Job *prev) {
    if (prev) prev->next = j->next;
    else head = j->next;
    if (tail == j) tail = prev;
    j->next = NULL;
}

static void push(Job *j, const bool urgent) {
    if (urgent) {
        j->next = head;
        head = j;
        if (!tail) tail = j;
    }
    else {
        j->next = NULL;
        if (tail) tail->next = j;
        else head = j;
        tail = j;
    }
    pending += j->n;
}

static int worker_main(void *arg);

// lock held
static int wanted_workers(void) {
    return config.threads > 0 ? FFMIN(config.threads, MAX_WORKERS) : default_threads();
}

static void start_workers(void) {
    const int n = wanted_workers();
    for (int i = 0; i < n; i++) {
        if (workers[i].thread) continue;
        workers[i].thread = SDL_CreateThread(worker_main, "ffa_gain_scan", (void *)(intptr_t)i);
    }
    SDL_CondBroadcast(wake);
}

//+-----------------------------------------------------+
//| Measuring                                           |
//+-----------------------------------------------------+

typedef struct ProgressCtx {
    int index;
} ProgressCtx;

static bool keep_going(double fraction, void *user) {
    (void)fraction;
    const ProgressCtx *ctx = user;
    SDL_LockMutex(lock);
    const bool go = !workers[ctx->index].cancel && !stopping;
    SDL_UnlockMutex(lock);
    return go;
}

static void notify(const AuGainScanConfig *cfg, const char *path, const int result, const AuReplayGainInfo *rg) {
    if (cfg->on_cached) cfg->on_cached(path, result, rg, cfg->user);
}

static void measure_job(const int index, const Job *job, const AuGainScanConfig *cfg) {
    ProgressCtx ctx = { .index = index };
    const AuLoudnessConfig lcfg = { .skip_true_peak = cfg->skip_true_peak, .progress = keep_going,
                                    .progress_user = &ctx };

    if (!job->album) {
        for (int i = 0; i < job->n; i++) {
            const char *path = job->paths[i];
            if (au_gain_cache_get(path, NULL)) continue; // measured by someone else meanwhile
            AuLoudness l;
            const int ret = au_loudness_analyze(path, &lcfg, &l);
            if (ret == AU_OK) {
                const AuReplayGainInfo rg = au_loudness_to_replaygain(&l, NULL);
                au_gain_cache_set(path, &rg);
                notify(cfg, path, AU_OK, &rg);
            }
            else {
                note_result(path, ret);
                notify(cfg, path, ret, NULL);
                if (ret == AU_ERR_CANCELLED) return;
            }
        }
        return;
    }

    // Album: every track through one scanner, then store track + album values together
    AuLoudnessScanner *scanner = au_loudness_scanner_create(&lcfg);
    AuLoudness *tracks = av_calloc(job->n, sizeof(AuLoudness));
    int *results = av_calloc(job->n, sizeof(int));
    if (!scanner || !tracks || !results) {
        for (int i = 0; i < job->n; i++) notify(cfg, job->paths[i], AU_ERR_NO_MEMORY, NULL);
        goto end;
    }
    for (int i = 0; i < job->n; i++) {
        results[i] = au_loudness_scan_file(scanner, job->paths[i], &tracks[i]);
        if (results[i] == AU_ERR_CANCELLED) {
            for (int k = i; k < job->n; k++) notify(cfg, job->paths[k], AU_ERR_CANCELLED, NULL);
            goto end;
        }
    }
    AuLoudness album;
    const bool have_album = au_loudness_album(scanner, &album) == AU_OK;
    for (int i = 0; i < job->n; i++) {
        if (results[i] != AU_OK) {
            note_result(job->paths[i], results[i]);
            notify(cfg, job->paths[i], results[i], NULL);
            continue;
        }
        const AuReplayGainInfo rg = au_loudness_to_replaygain(&tracks[i], have_album ? &album : NULL);
        au_gain_cache_set(job->paths[i], &rg);
        notify(cfg, job->paths[i], AU_OK, &rg);
    }
end:
    au_loudness_scanner_free(scanner);
    av_free(tracks);
    av_free(results);
}

static int worker_main(void *arg) {
    const int index = (int)(intptr_t)arg;
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);

    SDL_LockMutex(lock);
    for (;;) {
        // The wanted count is read again after every wake-up: au_gain_scan_configure() may have lowered it
        // while this worker was idle, and it then waits for this thread to leave
        while (!head && !stopping && index < wanted_workers())
            SDL_CondWait(wake, lock);
        if (stopping || index >= wanted_workers()) break;

        Job *job = head;
        unlink_job(job, NULL);
        workers[index].job = job;
        workers[index].cancel = false;
        const AuGainScanConfig cfg = config;
        SDL_UnlockMutex(lock);

        measure_job(index, job, &cfg);

        SDL_LockMutex(lock);
        workers[index].job = NULL;
        pending -= job->n;
        job_free(job);
    }
    SDL_UnlockMutex(lock);
    return 0;
}

//+-----------------------------------------------------+
//| Requests                                            |
//+-----------------------------------------------------+

static Job *job_new(const bool album, const bool from_player, const int capacity) {
    Job *j = av_mallocz(sizeof(Job));
    if (!j) return NULL;
    j->paths = av_calloc(capacity > 0 ? capacity : 1, sizeof(char *));
    if (!j->paths) { av_free(j); return NULL; }
    j->album = album;
    j->from_player = from_player;
    return j;
}

static int request(const char *const *paths, const int count, const bool urgent, const bool album,
                   const bool from_player) {
    if (!paths || count <= 0) return 0;
    if (!ready()) return AU_ERR_SDL;

    // Albums are only skipped when every track already has album values
    if (album) {
        bool all_done = true;
        for (int i = 0; i < count && all_done; i++) {
            AuReplayGainInfo rg;
            all_done = paths[i] && au_gain_cache_get(paths[i], &rg) && rg.has_album_gain;
        }
        if (all_done) return 0;
    }

    char ua[8];
    const bool measure_streams = network_config(ua, sizeof(ua)).measure_streams;
    Job *job = job_new(album, from_player, count);
    if (!job) return AU_ERR_NO_MEMORY;

    SDL_LockMutex(lock);
    for (int i = 0; i < count; i++) {
        const char *path = paths[i];
        if (!path || !path[0] || job_has(job, path)) continue;
        if (!album) {
            if (au_gain_cache_get(path, NULL) || being_measured(path) || known_failure(path)) continue;
            if (au_is_network_url(path) && !measure_streams) continue; // would download the whole file
            Job *prev = NULL;
            Job *queued = find_queued(path, &prev);
            if (queued) {
                // Already queued: an urgent request moves that job to the front
                if (urgent && queued != head) {
                    unlink_job(queued, prev);
                    pending -= queued->n;
                    push(queued, true);
                }
                if (from_player) queued->from_player = true;
                continue;
            }
        }
        if (!(job->paths[job->n] = av_strdup(path))) break;
        job->n++;
    }

    const int queued_paths = job->n;
    if (job->n) {
        push(job, urgent);
        start_workers();
    }
    else {
        job_free(job);
    }
    SDL_UnlockMutex(lock);
    return queued_paths;
}

bool gain_scan_request_for_player(const char *path) {
    if (!path || !ready()) return false;
    SDL_LockMutex(lock);
    const bool bad = known_failure(path);
    SDL_UnlockMutex(lock);
    if (bad) return false;
    const char *paths[1] = { path };
    request(paths, 1, true, false, true);
    return true;
}

int au_gain_cache_request(const char *const *paths, const int count, const AU_SCAN_PRIORITY priority) {
    return request(paths, count, priority == AU_SCAN_URGENT, false, false);
}

int au_gain_cache_request_album(const char *const *paths, const int count, const AU_SCAN_PRIORITY priority) {
    return request(paths, count, priority == AU_SCAN_URGENT, true, false);
}

int au_gain_scan_configure(const AuGainScanConfig *cfg) {
    if (cfg && (cfg->threads < 0 || cfg->threads > MAX_WORKERS)) return AU_ERR_INVALID_ARGUMENT;
    if (!ready()) return AU_ERR_SDL;

    SDL_Thread *finished[MAX_WORKERS];
    int n_finished = 0;
    SDL_LockMutex(lock);
    if (cfg) config = *cfg;
    else memset(&config, 0, sizeof(config));
    if (config.threads == 0) config.threads = default_threads();
    // Workers above the new count leave after their current job
    for (int i = config.threads; i < MAX_WORKERS; i++) {
        if (workers[i].thread) {
            finished[n_finished++] = workers[i].thread;
            workers[i].thread = NULL;
        }
    }
    if (head) start_workers();
    SDL_CondBroadcast(wake);
    SDL_UnlockMutex(lock);
    for (int i = 0; i < n_finished; i++) SDL_WaitThread(finished[i], NULL);
    return AU_OK;
}

void au_gain_cache_cancel_requests(void) {
    if (!ready()) return;
    SDL_LockMutex(lock);
    Job *prev = NULL, *j = head;
    while (j) {
        Job *next = j->next;
        if (!j->from_player) {
            unlink_job(j, prev);
            pending -= j->n;
            job_free(j);
        }
        else {
            prev = j;
        }
        j = next;
    }
    for (int i = 0; i < MAX_WORKERS; i++)
        if (workers[i].job && !workers[i].job->from_player) workers[i].cancel = true;
    SDL_UnlockMutex(lock);
}

int au_gain_cache_pending(void) {
    if (!ready()) return 0;
    SDL_LockMutex(lock);
    const int n = pending;
    SDL_UnlockMutex(lock);
    return n;
}

void au_gain_scan_stop(void) {
    if (!ready()) return;
    SDL_Thread *threads[MAX_WORKERS];
    int n = 0;
    SDL_LockMutex(lock);
    stopping = true;
    while (head) {
        Job *j = head;
        unlink_job(j, NULL);
        pending -= j->n;
        job_free(j);
    }
    for (int i = 0; i < MAX_WORKERS; i++) {
        if (workers[i].job) workers[i].cancel = true;
        if (workers[i].thread) threads[n++] = workers[i].thread;
        workers[i].thread = NULL;
    }
    SDL_CondBroadcast(wake);
    SDL_UnlockMutex(lock);

    for (int i = 0; i < n; i++) SDL_WaitThread(threads[i], NULL);

    SDL_LockMutex(lock);
    stopping = false;
    forget_all_failures();
    SDL_UnlockMutex(lock);
}
