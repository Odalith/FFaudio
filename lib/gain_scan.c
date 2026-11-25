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
 * Workers start on demand and keep running until au_gain_scan_stop(), or until fewer are wanted (then they leave
 * after their current job, and nobody waits for that).
 */

#include <sys/stat.h>

#include <math.h>
#include "gain_cache.h"
#include "once.h"
#include "numbers.h"
#include "loudness.h"
#include "network.h"
#include "chaos.h"

#define MAX_WORKERS 16
#define MAX_FAILED 1024          // files remembered as not measurable (the oldest are forgotten first)

typedef struct Job {
    char **paths;
    int n;
    bool album;
    bool from_player;           // not dropped by au_gain_scan_cancel()
    bool measuring;             // taken by a worker
    struct Job *next;
} Job;

typedef struct Worker {
    SDL_Thread *thread;
    Job *job;                   // being measured
    FfAtomic cancel;        // 1 = stop the current job (read without the lock while measuring)
    bool exited;                // the thread left (no longer wanted): joining it does not wait, then the slot is free
} Worker;

// Set on the scan workers' own threads: au_gain_scan_stop() from on_cached must not join its caller
static _Thread_local bool on_scan_worker = false;

// Guarded by lock
static SDL_mutex *lock = NULL;
static SDL_cond *wake = NULL;
static FfOnce init_once = FF_ONCE_INIT;
static Job *head = NULL, *tail = NULL;
static Worker workers[MAX_WORKERS];
static AuGainScanSettings config;
static int pending = 0;          // paths queued or being measured
static FfAtomic stopping;    // 1 while au_gain_scan_stop() runs (read without the lock while measuring)

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

// For workers (lock not held). Cancellations and memory errors say nothing about the file. A URL has no size or time
// to see a change by, so an error opening it (a server that was down for a moment) is not remembered; a live stream
// (AU_ERR_UNSUPPORTED: it never ends) is, so it is not opened again on every request.
static void note_result(const char *path, const int result) {
    const bool url = au_is_network_url(path);
    if (result != AU_ERR_UNSUPPORTED && (result != AU_ERR_FILE || url)) return;
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

static void index_remove(const Job *j);

// A queued job that will not be measured: a player waiting for one of its paths stops waiting
static void job_drop(Job *j) {
    if (!j) return;
    index_remove(j);
    for (int i = 0; i < j->n; i++) gain_cache_note_failure(j->paths[i]);
    job_free(j);
}

// Every single-file job, queued or being measured, by path (open addressing, linear probing): a request checks a
// path in constant time instead of comparing it with everything queued
typedef struct IndexSlot {
    uint32_t hash;
    Job *job;                   // NULL = empty
} IndexSlot;
static IndexSlot *index_slots = NULL;
static uint32_t index_cap = 0, index_n = 0;

static uint32_t path_hash(const char *s) {
    uint32_t h = 2166136261u; // FNV-1a
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

static Job *index_find(const char *path) {
    if (!index_cap) return NULL;
    const uint32_t h = path_hash(path), mask = index_cap - 1;
    for (uint32_t i = h & mask;; i = (i + 1) & mask) {
        if (!index_slots[i].job) return NULL;
        if (index_slots[i].hash == h && !strcmp(index_slots[i].job->paths[0], path)) return index_slots[i].job;
    }
}

static void index_insert(IndexSlot *slots, const uint32_t cap, const uint32_t h, Job *j) {
    uint32_t i = h & (cap - 1);
    while (slots[i].job) i = (i + 1) & (cap - 1);
    slots[i].hash = h;
    slots[i].job = j;
}

static bool index_add(Job *j) {
    if ((index_n + 1) * 10 > index_cap * 7) { // keep it under 70 % full
        const uint32_t cap = index_cap ? index_cap * 2 : 64;
        IndexSlot *slots = av_calloc(cap, sizeof(IndexSlot));
        if (!slots) return false;
        for (uint32_t i = 0; i < index_cap; i++)
            if (index_slots[i].job) index_insert(slots, cap, index_slots[i].hash, index_slots[i].job);
        av_free(index_slots);
        index_slots = slots;
        index_cap = cap;
    }
    index_insert(index_slots, index_cap, path_hash(j->paths[0]), j);
    index_n++;
    return true;
}

// Remove a job, moving later entries of its run back so no probe sequence is broken
static void index_remove(const Job *j) {
    if (!index_cap || j->album || !j->n) return;
    const uint32_t mask = index_cap - 1;
    uint32_t i = path_hash(j->paths[0]) & mask;
    while (index_slots[i].job && index_slots[i].job != j) i = (i + 1) & mask;
    if (!index_slots[i].job) return;
    index_n--;
    for (uint32_t k = (i + 1) & mask; index_slots[k].job; k = (k + 1) & mask) {
        const uint32_t home = index_slots[k].hash & mask;
        // Move k into the hole at i when its home position is not between i (exclusive) and k (inclusive)
        const bool between = i <= k ? (home > i && home <= k) : (home > i || home <= k);
        if (!between) {
            index_slots[i] = index_slots[k];
            i = k;
        }
    }
    index_slots[i].job = NULL;
}

// The job before `j` in the queue (for unlinking), NULL when it is the first
static Job *queued_before(const Job *j) {
    Job *prev = NULL;
    for (Job *q = head; q && q != j; q = q->next) prev = q;
    return prev;
}

// An album job with the same first track, queued (*queued_out, *prev_out set) or being measured
static bool album_pending(const char *first, Job **queued_out, Job **prev_out) {
    *queued_out = NULL;
    for (int i = 0; i < MAX_WORKERS; i++) {
        const Job *j = workers[i].job;
        if (j && j->album && j->n && !strcmp(j->paths[0], first)) return true;
    }
    Job *prev = NULL;
    for (Job *j = head; j; prev = j, j = j->next) {
        if (j->album && j->n && !strcmp(j->paths[0], first)) {
            *queued_out = j;
            *prev_out = prev;
            return true;
        }
    }
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

// lock held. A slot is reused only once its thread has left: a worker that is no longer wanted finishes its job first,
// and two threads must never share a slot
static void start_workers(void) {
    CHAOS("start_workers");
    const int n = wanted_workers();
    for (int i = 0; i < n; i++) {
        if (workers[i].thread && workers[i].exited) {
            // It flagged its slot as its last step under the lock and needs the lock no more: this does not wait
            SDL_WaitThread(workers[i].thread, NULL);
            workers[i].thread = NULL;
            workers[i].exited = false;
        }
        if (workers[i].thread) continue;
        workers[i].exited = false;
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

static int interrupted(void *user) {
    const ProgressCtx *ctx = user;
    return ffa_load(&workers[ctx->index].cancel) || ffa_load(&stopping);
}

static bool keep_going(double fraction, void *user) {
    (void)fraction;
    return !interrupted(user);
}

static void notify(const AuGainScanSettings *cfg, const char *path, const int result, const AuReplayGain *rg) {
    CHAOS("notify");
    if (result != AU_OK) gain_cache_note_failure(path); // a player waiting for this value stops waiting
    if (cfg->on_cached) cfg->on_cached(path, result, rg, cfg->user);
}

// `file` and `album` are the worker's own meters, reused for every file
static void measure_job(const int index, const Job *job, const AuGainScanSettings *cfg, AuLoudnessMeter *file,
                        AuLoudnessMeter *album) {
    ProgressCtx ctx = { .index = index };
    const AuLoudnessOptions lcfg = { .skip_true_peak = cfg->skip_true_peak, .progress = keep_going,
                                    .progress_user = &ctx };
    const AVIOInterruptCB interrupt = { .callback = interrupted, .opaque = &ctx };
    if (!file || !album) {
        for (int i = 0; i < job->n; i++) notify(cfg, job->paths[i], AU_ERR_NO_MEMORY, NULL);
        return;
    }

    if (!job->album) {
        for (int i = 0; i < job->n; i++) {
            const char *path = job->paths[i];
            if (au_gain_cache_get(path, NULL)) continue; // measured by someone else meanwhile
            AuLoudness l;
            const int ret = loudness_measure_into(file, path, &lcfg, &interrupt);
            if (ret == AU_OK) {
                au_loudness_result(file, &l);
                const AuReplayGain rg = au_loudness_to_replaygain(&l, NULL);
                au_gain_cache_set(path, &rg);
                notify(cfg, path, AU_OK, &rg);
            }
            else {
                note_result(path, ret);
                notify(cfg, path, ret, NULL);
                if (ret == AU_ERR_CANCELLED) break;
            }
        }
        return;
    }

    // Album: every track measured and merged, then track + album values stored together
    AuLoudness *tracks = av_calloc(job->n, sizeof(AuLoudness));
    int *results = av_calloc(job->n, sizeof(int));
    if (!tracks || !results) {
        for (int i = 0; i < job->n; i++) notify(cfg, job->paths[i], AU_ERR_NO_MEMORY, NULL);
        goto end;
    }
    loudness_meter_clear(album);
    for (int i = 0; i < job->n; i++) {
        results[i] = loudness_measure_into(file, job->paths[i], &lcfg, &interrupt);
        if (results[i] == AU_ERR_CANCELLED) {
            for (int k = i; k < job->n; k++) notify(cfg, job->paths[k], AU_ERR_CANCELLED, NULL);
            goto end;
        }
        if (results[i] == AU_OK) {
            au_loudness_result(file, &tracks[i]);
            au_loudness_merge(album, file);
        }
    }
    AuLoudness album_values;
    const bool have_album = au_loudness_result(album, &album_values) == AU_OK;
    for (int i = 0; i < job->n; i++) {
        if (results[i] != AU_OK) {
            note_result(job->paths[i], results[i]);
            notify(cfg, job->paths[i], results[i], NULL);
            continue;
        }
        const AuReplayGain rg = au_loudness_to_replaygain(&tracks[i], have_album ? &album_values : NULL);
        au_gain_cache_set(job->paths[i], &rg);
        notify(cfg, job->paths[i], AU_OK, &rg);
    }
end:
    av_free(tracks);
    av_free(results);
}

static int worker_main(void *arg) {
    CHAOS("worker_main");
    ff_thread_use_c_locale();
    const int index = (int)(intptr_t)arg;
    on_scan_worker = true;
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);
    // About 320 KB each, kept for every file
    AuLoudnessMeter *file = loudness_meter_create(), *album = loudness_meter_create();

    SDL_LockMutex(lock);
    for (;;) {
        // The wanted count is read again after every wake-up and job: au_set_gain_scan() may have lowered it.
        // A worker no longer wanted leaves by itself; nobody waits for it (start_workers() or au_gain_scan_stop()
        // joins it later, at once)
        while (!head && !ffa_load(&stopping) && index < wanted_workers())
            SDL_CondWait(wake, lock);
        if (ffa_load(&stopping)) break;   // au_gain_scan_stop() has taken the slot and joins this thread
        if (index >= wanted_workers()) {
            workers[index].exited = true;  // the slot is still this thread's: start_workers() joins it
            break;
        }

        Job *job = head;
        unlink_job(job, NULL);
        job->measuring = true;
        workers[index].job = job;
        ffa_store(&workers[index].cancel, 0);
        const AuGainScanSettings cfg = config;
        SDL_UnlockMutex(lock);

        measure_job(index, job, &cfg, file, album);

        SDL_LockMutex(lock);
        workers[index].job = NULL;
        pending -= job->n;
        index_remove(job);
        job_free(job);
    }
    SDL_UnlockMutex(lock);
    au_loudness_meter_free(file);
    au_loudness_meter_free(album);
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

    SDL_LockMutex(lock);
    if (ffa_load(&stopping)) {
        // au_gain_scan_stop() is waiting for the workers; new ones started now would never be joined
        SDL_UnlockMutex(lock);
        return 0;
    }
    const bool measure_streams = config.measure_streams;
    if (album && !measure_streams) {
        // Measuring would download whole files, and an album measured without them would be wrong
        for (int i = 0; i < count; i++) {
            if (paths[i] && au_is_network_url(paths[i])) {
                SDL_UnlockMutex(lock);
                return 0;
            }
        }
    }

    if (album) {
        // Skipped when every track already has album values. A track that could not be measured before and has not
        // changed does not count, and is left out of the job: one cover image or broken file would otherwise make
        // every request measure the whole album again.
        bool all_done = true;
        for (int i = 0; i < count && all_done; i++) {
            AuReplayGain rg;
            all_done = !paths[i] || !paths[i][0] || known_failure(paths[i]) ||
                       (au_gain_cache_get(paths[i], &rg) && !isnan(rg.album_gain_db));
        }
        if (all_done) {
            SDL_UnlockMutex(lock);
            return 0;
        }
        if (paths[0] && paths[0][0]) {
            Job *queued = NULL, *prev = NULL;
            if (album_pending(paths[0], &queued, &prev)) {
                if (queued && urgent && queued != head) {
                    unlink_job(queued, prev);
                    pending -= queued->n;
                    push(queued, true);
                }
                SDL_UnlockMutex(lock);
                return 0;
            }
        }
        Job *job = job_new(true, from_player, count);
        if (!job) {
            SDL_UnlockMutex(lock);
            return AU_ERR_NO_MEMORY;
        }
        for (int i = 0; i < count; i++) {
            const char *path = paths[i];
            bool dup = false;
            for (int k = 0; k < job->n && !dup; k++) dup = path && !strcmp(job->paths[k], path);
            if (!path || !path[0] || dup || known_failure(path)) continue;
            if (!(job->paths[job->n] = av_strdup(path))) break;
            job->n++;
        }
        const int n = job->n;
        if (n) {
            push(job, urgent);
            start_workers();
        }
        else {
            job_free(job);
        }
        SDL_UnlockMutex(lock);
        return n;
    }

    // Single files: a job each, so the workers share a request and an urgent one moves exactly the files it names
    Job **made = av_malloc_array(count, sizeof(Job *));
    if (!made) {
        SDL_UnlockMutex(lock);
        return AU_ERR_NO_MEMORY;
    }
    int n = 0;
    for (int i = 0; i < count; i++) {
        const char *path = paths[i];
        if (!path || !path[0]) continue;
        Job *known = index_find(path);
        if (known) {
            // Already queued (also earlier in this request) or being measured
            if (!known->measuring && urgent && known != head) {
                bool mine = false;
                for (int k = 0; k < n && !mine; k++) mine = made[k] == known;
                if (!mine) {
                    unlink_job(known, queued_before(known));
                    pending -= known->n;
                    push(known, true);
                }
            }
            if (from_player) known->from_player = true;
            continue;
        }
        if (au_gain_cache_get(path, NULL) || known_failure(path)) continue;
        if (au_is_network_url(path) && !measure_streams) continue; // would download the whole file
        Job *job = job_new(false, from_player, 1);
        if (!job) break;
        if (!(job->paths[0] = av_strdup(path))) {
            job_free(job);
            break;
        }
        job->n = 1;
        if (!index_add(job)) {
            job_free(job);
            break;
        }
        made[n++] = job;
    }
    // Urgent ones go to the front in the order given; prefetches to the back
    if (urgent)
        for (int k = n - 1; k >= 0; k--) push(made[k], true);
    else
        for (int k = 0; k < n; k++) push(made[k], false);
    if (n) start_workers();
    av_free(made);
    SDL_UnlockMutex(lock);
    return n;
}

bool gain_scan_measures_streams(void) {
    if (!ready()) return false;
    SDL_LockMutex(lock);
    const bool yes = config.measure_streams;
    SDL_UnlockMutex(lock);
    return yes;
}

bool gain_scan_request_for_player(const char *path) {
    CHAOS("gain_scan_request_for_player");
    if (!path || !ready()) return false;
    SDL_LockMutex(lock);
    const bool bad = known_failure(path);
    SDL_UnlockMutex(lock);
    if (bad) return false;
    const char *paths[1] = { path };
    request(paths, 1, true, false, true);
    return true;
}

int au_gain_scan_request(const char *const *paths, const int count, const AU_SCAN_PRIORITY priority) {
    CHAOS("au_gain_scan_request");
    return request(paths, count, priority == AU_SCAN_URGENT, false, false);
}

int au_gain_scan_request_album(const char *const *paths, const int count, const AU_SCAN_PRIORITY priority) {
    CHAOS("au_gain_scan_request_album");
    return request(paths, count, priority == AU_SCAN_URGENT, true, false);
}

int au_set_gain_scan(const AuGainScanSettings *cfg) {
    if (cfg && (cfg->threads < 0 || cfg->threads > MAX_WORKERS)) return AU_ERR_INVALID_ARGUMENT;
    if (!ready()) return AU_ERR_SDL;

    SDL_LockMutex(lock);
    if (cfg) config = *cfg;
    else memset(&config, 0, sizeof(config));
    if (config.threads == 0) config.threads = default_threads();
    // Workers above the new count leave by themselves after their current job; this does not wait for them
    if (head) start_workers();
    SDL_CondBroadcast(wake);
    SDL_UnlockMutex(lock);
    return AU_OK;
}

void au_gain_scan_cancel(void) {
    CHAOS("au_gain_scan_cancel");
    if (!ready()) return;
    SDL_LockMutex(lock);
    Job *prev = NULL, *j = head;
    while (j) {
        Job *next = j->next;
        if (!j->from_player) {
            unlink_job(j, prev);
            pending -= j->n;
            job_drop(j);
        }
        else {
            prev = j;
        }
        j = next;
    }
    for (int i = 0; i < MAX_WORKERS; i++)
        if (workers[i].job && !workers[i].job->from_player) ffa_store(&workers[i].cancel, 1);
    SDL_UnlockMutex(lock);
}

int au_gain_scan_pending(void) {
    if (!ready()) return 0;
    SDL_LockMutex(lock);
    const int n = pending;
    SDL_UnlockMutex(lock);
    return n;
}

int au_gain_scan_stop(void) {
    CHAOS("au_gain_scan_stop");
    if (!ready()) return AU_ERR_SDL;
    if (on_scan_worker) return AU_ERR_WRONG_THREAD; // from on_cached: it would wait for the thread making the call
    SDL_Thread *threads[MAX_WORKERS];
    int n = 0;
    SDL_LockMutex(lock);
    ffa_store(&stopping, 1);
    while (head) {
        Job *j = head;
        unlink_job(j, NULL);
        pending -= j->n;
        job_drop(j);
    }
    for (int i = 0; i < MAX_WORKERS; i++) {
        if (workers[i].job) ffa_store(&workers[i].cancel, 1);
        if (workers[i].thread) threads[n++] = workers[i].thread; // also those that left already
        workers[i].thread = NULL;
        workers[i].exited = false;
    }
    SDL_CondBroadcast(wake);
    SDL_UnlockMutex(lock);

    for (int i = 0; i < n; i++) SDL_WaitThread(threads[i], NULL);

    SDL_LockMutex(lock);
    ffa_store(&stopping, 0);
    forget_all_failures();
    SDL_UnlockMutex(lock);
    return AU_OK;
}
