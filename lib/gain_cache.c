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
 * Persistent ReplayGain cache: path -> AuReplayGainInfo.
 *
 * In memory it is a hash table behind one mutex. On disk it is an append-only text file in the platform's
 * per-user data directory: every change appends one line, so a change is a single small write and a crash can
 * not lose earlier entries. The file is read on first use and rewritten (compacted) when superseded lines pile
 * up. Several processes can share the file: every write takes a lock (a separate ".lock" file, since compacting
 * replaces the cache file), a compaction reads the file again under it, and appends follow a replaced file.
 *
 *   ffaudio-replaygain-cache 1
 *   S <tab> track_gain <tab> track_peak <tab> album_gain <tab> album_peak <tab> flags <tab> path     (set)
 *   R <tab> path                                                                                     (remove)
 *   C                                                                                                (clear)
 *
 * Missing values are written as "-", numbers with '.' as decimal separator, paths with \\ \t \n \r escaped.
 *
 * The player only reads from the cache (gain_cache_lookup) and is told about changes so queued and playing
 * tracks pick them up. Media files are never touched.
 */

#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#endif

#include "gain_cache.h"
#include "once.h"
#include "numbers.h"
#include "ffaudio_loudness.h"
#include "SDL2/SDL_atomic.h"
#include "SDL2/SDL_filesystem.h"
#include "SDL2/SDL_mutex.h"
#include "SDL2/SDL_stdinc.h"

#define CACHE_HEADER "ffaudio-replaygain-cache 1"
#define CACHE_FILE_NAME "replaygain.cache"

typedef struct Entry {
    char *path;
    uint32_t hash;
    AuReplayGainInfo rg;
    struct Entry *next;
} Entry;

// Everything below is guarded by the cache mutex
static Entry **buckets = NULL;
static size_t n_buckets = 0;
static size_t n_entries = 0;
static bool loaded = false;
static bool custom_location = false;   // au_gain_cache_set_file() was called
static char *file_path = NULL;          // resolved location, "" = memory only
static FILE *append_file = NULL;
static size_t file_lines = 0;           // records in the file, to decide when to compact
static bool foreign = false;            // the file is not one this version reads: it is left alone

static SDL_mutex *cache_mutex_ptr = NULL;
static SDL_cond *cache_changed = NULL;   // broadcast when an entry is set or a measurement fails (gain_cache_wait)
static FfOnce cache_mutex_once = FF_ONCE_INIT;

// Paths whose measurement failed lately, for gain_cache_wait() (guarded by the cache mutex). A waiter that misses
// one because more than RECENT_FAILURES failed in the meantime only waits until its timeout, as before.
#define RECENT_FAILURES 8
static char *recent_failures[RECENT_FAILURES];
static unsigned failure_count = 0;      // failures so far; the newest is at (failure_count - 1) % RECENT_FAILURES

static void create_cache_mutex(void) {
    cache_mutex_ptr = SDL_CreateMutex(); // SDL mutexes work without SDL_Init
    cache_changed = SDL_CreateCond();
}

// Created on first use, from any thread
static SDL_mutex *cache_mutex(void) {
    ff_once(&cache_mutex_once, create_cache_mutex);
    return cache_mutex_ptr;
}

//+-----------------------------------------------------+
//| Platform file helpers (UTF-8 paths everywhere)      |
//+-----------------------------------------------------+

#ifdef _WIN32
static wchar_t *utf8_to_wide(const char *s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = av_malloc_array(n, sizeof(wchar_t));
    if (w && MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) av_freep(&w);
    return w;
}

static char *wide_to_utf8(const wchar_t *w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *s = av_malloc(n);
    if (s && WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) <= 0) av_freep(&s);
    return s;
}
#endif

static FILE *open_file(const char *path, const char *mode) {
#ifdef _WIN32
    wchar_t *wp = utf8_to_wide(path), *wm = utf8_to_wide(mode);
    FILE *f = (wp && wm) ? _wfopen(wp, wm) : NULL;
    av_free(wp);
    av_free(wm);
    return f;
#else
    return fopen(path, mode);
#endif
}

static int make_dir(const char *path) {
#ifdef _WIN32
    wchar_t *wp = utf8_to_wide(path);
    const int ret = wp ? _wmkdir(wp) : -1;
    av_free(wp);
    return ret;
#else
    return mkdir(path, 0755);
#endif
}

static bool replace_file(const char *from, const char *to) {
#ifdef _WIN32
    wchar_t *wf = utf8_to_wide(from), *wt = utf8_to_wide(to);
    const bool ok = wf && wt && MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING);
    av_free(wf);
    av_free(wt);
    return ok;
#else
    return rename(from, to) == 0;
#endif
}

// Create the directories leading to `file` (only called before the first write)
static void make_parent_dirs(const char *file) {
    char *p = av_strdup(file);
    if (!p) return;
    for (char *c = p + 1; *c; c++) {
        if (*c == '/' || *c == '\\') {
            const char saved = *c;
            *c = '\0';
            if (make_dir(p) != 0 && errno != EEXIST) { /* the open below reports the failure */ }
            *c = saved;
        }
    }
    av_free(p);
}

static char *getenv_utf8(const char *name) {
#ifdef _WIN32
    wchar_t *wn = utf8_to_wide(name);
    const wchar_t *v = wn ? _wgetenv(wn) : NULL;
    av_free(wn);
    return (v && v[0]) ? wide_to_utf8(v) : NULL;
#else
    const char *v = getenv(name);
    return (v && v[0]) ? av_strdup(v) : NULL;
#endif
}

// The per-user data directory for ffaudio, ending with a separator
static char *default_dir(void) {
    char *dir = NULL;
#if defined(_WIN32)
    char *base = getenv_utf8("LOCALAPPDATA");   // machine-specific paths: local, not roaming
    if (base) dir = av_asprintf("%s\\ffaudio\\", base);
    av_free(base);
#elif defined(__APPLE__) && !defined(__IPHONEOS__) && !(defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
    char *home = getenv_utf8("HOME");
    if (home) dir = av_asprintf("%s/Library/Application Support/ffaudio/", home);
    av_free(home);
#elif defined(__unix__) && !defined(__ANDROID__)
    char *xdg = getenv_utf8("XDG_DATA_HOME");
    if (xdg && xdg[0] == '/') {
        dir = av_asprintf("%s/ffaudio/", xdg);
    }
    else {
        char *home = getenv_utf8("HOME");
        if (home) dir = av_asprintf("%s/.local/share/ffaudio/", home);
        av_free(home);
    }
    av_free(xdg);
#endif
    if (!dir) {
        // Android, iOS and anything else: SDL's per-application storage directory
        char *pref = SDL_GetPrefPath("ffaudio", "ffaudio");
        if (pref) dir = av_strdup(pref);
        SDL_free(pref);
    }
    return dir;
}

//+-----------------------------------------------------+
//| Record encoding                                     |
//+-----------------------------------------------------+

// "%.6f" in the C locale, "-" when missing
static void put_number(AVBPrint *bp, const bool has, const double v) {
    if (!has || !isfinite(v)) {
        av_bprint_chars(bp, '-', 1);
        return;
    }
    char tmp[64];
    av_bprintf(bp, "%s", ff_format_number(tmp, sizeof(tmp), v, 6, 'f'));
}

static bool parse_number(const char *s, bool *has, double *v) {
    *has = false;
    *v = 0;
    if (!strcmp(s, "-")) return true;
    const char *end;
    if (!ff_parse_number(s, &end, v, false) || *end) return false;
    *has = true;
    return true;
}

static void put_path(AVBPrint *bp, const char *p) {
    for (; *p; p++) {
        switch (*p) {
            case '\\': av_bprintf(bp, "\\\\"); break;
            case '\t': av_bprintf(bp, "\\t"); break;
            case '\n': av_bprintf(bp, "\\n"); break;
            case '\r': av_bprintf(bp, "\\r"); break;
            default:   av_bprint_chars(bp, *p, 1); break;
        }
    }
}

// Unescape in place
static void unescape_path(char *p) {
    char *w = p;
    for (; *p; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            *w++ = *p == 't' ? '\t' : *p == 'n' ? '\n' : *p == 'r' ? '\r' : *p;
        }
        else {
            *w++ = *p;
        }
    }
    *w = '\0';
}

static void encode_set(AVBPrint *bp, const char *path, const AuReplayGainInfo *rg) {
    av_bprintf(bp, "S\t");
    put_number(bp, rg->has_track_gain, rg->track_gain_db);
    av_bprint_chars(bp, '\t', 1);
    put_number(bp, rg->has_track_peak, rg->track_peak);
    av_bprint_chars(bp, '\t', 1);
    put_number(bp, rg->has_album_gain, rg->album_gain_db);
    av_bprint_chars(bp, '\t', 1);
    put_number(bp, rg->has_album_peak, rg->album_peak);
    av_bprintf(bp, "\t%d\t", rg->from_r128 ? 1 : 0);
    put_path(bp, path);
    av_bprint_chars(bp, '\n', 1);
}

//+-----------------------------------------------------+
//| Hash table                                          |
//+-----------------------------------------------------+

static uint32_t hash_path(const char *s) {
    uint32_t h = 2166136261u; // FNV-1a
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 16777619u;
    }
    return h;
}

static Entry **find_slot(const char *path, const uint32_t hash) {
    if (!n_buckets) return NULL;
    Entry **slot = &buckets[hash & (n_buckets - 1)];
    while (*slot && ((*slot)->hash != hash || strcmp((*slot)->path, path) != 0))
        slot = &(*slot)->next;
    return slot;
}

static bool grow(void) {
    const size_t size = n_buckets ? n_buckets * 2 : 64;
    Entry **nb = av_calloc(size, sizeof(Entry *));
    if (!nb) return false;
    for (size_t i = 0; i < n_buckets; i++) {
        Entry *e = buckets[i];
        while (e) {
            Entry *next = e->next;
            Entry **slot = &nb[e->hash & (size - 1)];
            e->next = *slot;
            *slot = e;
            e = next;
        }
    }
    av_free(buckets);
    buckets = nb;
    n_buckets = size;
    return true;
}

static int table_set(const char *path, const AuReplayGainInfo *rg) {
    const uint32_t hash = hash_path(path);
    Entry **slot = find_slot(path, hash);
    if (slot && *slot) {
        (*slot)->rg = *rg;
        return AU_OK;
    }
    if ((n_entries + 1) * 4 > n_buckets * 3 && !grow()) return AU_ERR_NO_MEMORY;
    Entry *e = av_mallocz(sizeof(Entry));
    if (!e || !(e->path = av_strdup(path))) {
        av_free(e);
        return AU_ERR_NO_MEMORY;
    }
    e->hash = hash;
    e->rg = *rg;
    slot = &buckets[hash & (n_buckets - 1)];
    e->next = *slot;
    *slot = e;
    n_entries++;
    return AU_OK;
}

static bool table_remove(const char *path) {
    Entry **slot = find_slot(path, hash_path(path));
    Entry *e = (slot && *slot) ? *slot : NULL;
    if (!e) return false;
    *slot = e->next;
    n_entries--;
    av_free(e->path);
    av_free(e);
    return true;
}

static void table_clear(void) {
    for (size_t i = 0; i < n_buckets; i++) {
        Entry *e = buckets[i];
        while (e) {
            Entry *next = e->next;
            av_free(e->path);
            av_free(e);
            e = next;
        }
    }
    av_freep(&buckets);
    n_buckets = 0;
    n_entries = 0;
}

//+-----------------------------------------------------+
//| File (cache mutex held)                             |
//+-----------------------------------------------------+

static bool persistent(void) { return file_path && file_path[0]; }

static void close_append(void) {
    if (append_file) fclose(append_file);
    append_file = NULL;
}

// Cross-process lock: a file next to the cache that is never replaced (a lock on the cache file itself would be lost
// when a compaction renames a new file over it). Every write takes it, so appends from several processes do not
// interleave and a compaction includes what the others appended. When it can not be taken (no permission, a file
// system without locks) writes go ahead unlocked, as before.
#ifdef _WIN32
static HANDLE lock_handle = INVALID_HANDLE_VALUE;
#else
static int lock_fd = -1;
#endif

static void close_file_lock(void) {
#ifdef _WIN32
    if (lock_handle != INVALID_HANDLE_VALUE) CloseHandle(lock_handle);
    lock_handle = INVALID_HANDLE_VALUE;
#else
    if (lock_fd >= 0) close(lock_fd);
    lock_fd = -1;
#endif
}

static bool take_file_lock(void) {
    if (!persistent()) return false;
#ifdef _WIN32
    if (lock_handle == INVALID_HANDLE_VALUE) {
        char *lp = av_asprintf("%s.lock", file_path);
        wchar_t *w = lp ? utf8_to_wide(lp) : NULL;
        if (w) {
            make_parent_dirs(file_path);
            lock_handle = CreateFileW(w, GENERIC_READ | GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, NULL);
        }
        av_free(w);
        av_free(lp);
        if (lock_handle == INVALID_HANDLE_VALUE) return false;
    }
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    return LockFileEx(lock_handle, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &ov) != 0;
#else
    if (lock_fd < 0) {
        char *lp = av_asprintf("%s.lock", file_path);
        if (!lp) return false;
        make_parent_dirs(file_path);
        lock_fd = open(lp, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        av_free(lp);
        if (lock_fd < 0) return false;
    }
    while (flock(lock_fd, LOCK_EX) != 0)
        if (errno != EINTR) return false;
    return true;
#endif
}

static void release_file_lock(const bool held) {
    if (!held) return;
#ifdef _WIN32
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    UnlockFileEx(lock_handle, 0, 1, 0, &ov);
#else
    flock(lock_fd, LOCK_UN);
#endif
}

static bool sync_file(FILE *f) {
    if (fflush(f) != 0) return false;
#ifdef _WIN32
    return _commit(_fileno(f)) == 0;
#else
    return fsync(fileno(f)) == 0;
#endif
}

static void apply_line(char *line) {
    const size_t len = strlen(line);
    if (len && line[len - 1] == '\n') line[len - 1] = '\0';
    if (line[0] && line[strlen(line) - 1] == '\r') line[strlen(line) - 1] = '\0';

    if (!strcmp(line, "C")) {
        table_clear();
        return;
    }
    if (line[0] == 'R' && line[1] == '\t') {
        unescape_path(line + 2);
        if (line[2]) table_remove(line + 2);
        return;
    }
    if (line[0] != 'S' || line[1] != '\t') return; // unknown or damaged line: skip

    // Five values, then the path (last, so it may contain anything; tabs in it are escaped)
    char *fields[6];
    char *p = line + 2;
    for (int i = 0; i < 5; i++) {
        fields[i] = p;
        char *tab = strchr(p, '\t');
        if (!tab) return;
        *tab = '\0';
        p = tab + 1;
    }
    fields[5] = p;

    AuReplayGainInfo rg;
    memset(&rg, 0, sizeof(rg));
    if (!parse_number(fields[0], &rg.has_track_gain, &rg.track_gain_db) ||
        !parse_number(fields[1], &rg.has_track_peak, &rg.track_peak) ||
        !parse_number(fields[2], &rg.has_album_gain, &rg.album_gain_db) ||
        !parse_number(fields[3], &rg.has_album_peak, &rg.album_peak))
        return;
    rg.from_r128 = fields[4][0] == '1';
    unescape_path(fields[5]);
    if (fields[5][0]) table_set(fields[5], &rg);
}

// Read the whole file into the (empty) table, in one read. A last line without its newline is another process's
// record still being written: not a record yet. false: not a file this version reads (another format), left alone.
static bool read_cache_file(void) {
    file_lines = 0;
    FILE *f = open_file(file_path, "rb");
    if (!f) return true; // nothing stored yet
    char *buf = NULL;
    size_t n = 0;
    if (fseek(f, 0, SEEK_END) == 0) {
        const long size = ftell(f);
        if (size > 0 && fseek(f, 0, SEEK_SET) == 0 && (buf = av_malloc((size_t)size + 1)))
            n = fread(buf, 1, (size_t)size, f);
    }
    fclose(f);
    if (!buf) return true;
    buf[n] = '\0';

    bool ours = true, header = true;
    for (char *line = buf, *end = buf + n; line < end;) {
        char *nl = memchr(line, '\n', (size_t)(end - line));
        if (!nl) break;
        *nl = '\0';
        if (header) {
            header = false;
            if (nl > line && nl[-1] == '\r') nl[-1] = '\0';
            if (strcmp(line, CACHE_HEADER) != 0) {
                ours = false;
                break;
            }
        }
        else {
            apply_line(line);
            file_lines++;
        }
        line = nl + 1;
    }
    av_free(buf);
    return ours;
}

// Rewrite the file with one line per entry: a temporary file, flushed to the disk, then renamed over the old one, so
// it is never half written. File lock held (when there is one).
static int write_file(void) {
    close_append();
    char *tmp = av_asprintf("%s.tmp", file_path);
    if (!tmp) return AU_ERR_NO_MEMORY;
    make_parent_dirs(file_path);
    FILE *f = open_file(tmp, "wb");
    if (!f) {
        av_free(tmp);
        return AU_ERR_FILE;
    }
    AVBPrint bp;
    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
    av_bprintf(&bp, "%s\n", CACHE_HEADER);
    bool ok = fwrite(bp.str, 1, bp.len, f) == bp.len;
    for (size_t i = 0; ok && i < n_buckets; i++) {
        for (Entry *e = buckets[i]; ok && e; e = e->next) {
            av_bprint_clear(&bp);
            encode_set(&bp, e->path, &e->rg);
            ok = av_bprint_is_complete(&bp) && fwrite(bp.str, 1, bp.len, f) == bp.len;
        }
    }
    av_bprint_finalize(&bp, NULL);
    ok = sync_file(f) && ok;
    ok = (fclose(f) == 0) && ok;
    if (ok) ok = replace_file(tmp, file_path);
    if (!ok) remove(tmp);
    av_free(tmp);
    if (ok) file_lines = n_entries;
    return ok ? AU_OK : AU_ERR_FILE;
}

// Compact the file: read it again under the lock, so what other processes appended since it was loaded stays, then
// rewrite it
static int compact(void) {
    if (!persistent() || foreign) return AU_OK;
    const bool locked = take_file_lock();
    table_clear();
    foreign = !read_cache_file();
    const int ret = foreign ? AU_OK : write_file();
    release_file_lock(locked);
    return ret;
}

static void ensure_loaded(void) {
    if (loaded) return;
    loaded = true;

    if (!custom_location && !file_path) {
        char *dir = default_dir();
        file_path = dir ? av_asprintf("%s%s", dir, CACHE_FILE_NAME) : av_strdup("");
        av_free(dir);
    }
    if (!persistent()) return;

    foreign = !read_cache_file();
    // Many superseded lines: rewrite the file compactly
    if (!foreign && file_lines > 2 * n_entries + 256) compact();
}

static int append_record(const char *record, const size_t len) {
    if (!persistent()) return AU_OK;
    if (foreign) return AU_ERR_FILE; // kept in memory only: the file is not ours to write
    const bool locked = take_file_lock();
#ifndef _WIN32
    // Another process may have compacted the cache since: append to the new file, not to the one it replaced
    if (append_file) {
        struct stat a, b;
        if (fstat(fileno(append_file), &a) != 0 || stat(file_path, &b) != 0 || a.st_ino != b.st_ino ||
            a.st_dev != b.st_dev)
            close_append();
    }
#endif
    if (!append_file) {
        make_parent_dirs(file_path);
        append_file = open_file(file_path, "ab");
        if (!append_file) {
            release_file_lock(locked);
            return AU_ERR_FILE;
        }
        fseek(append_file, 0, SEEK_END);
        if (ftell(append_file) == 0) {
            fputs(CACHE_HEADER "\n", append_file);
            file_lines = 0;
        }
    }
    // One write per record keeps lines intact
    const bool ok = fwrite(record, 1, len, append_file) == len && fflush(append_file) == 0;
    file_lines++;
#ifdef _WIN32
    close_append(); // Windows can only replace a file nobody has open: let other processes compact it
#endif
    release_file_lock(locked);
    return ok ? AU_OK : AU_ERR_FILE;
}

//+-----------------------------------------------------+
//| Player side                                         |
//+-----------------------------------------------------+

bool gain_cache_lookup(const char *path, AuReplayGainInfo *out) {
    if (!path || !path[0]) return false;
    SDL_mutex *m = cache_mutex();
    if (!m) return false;
    SDL_LockMutex(m);
    ensure_loaded();
    Entry **slot = find_slot(path, hash_path(path));
    const bool found = slot && *slot;
    if (found && out) *out = (*slot)->rg;
    SDL_UnlockMutex(m);
    return found;
}

//+-----------------------------------------------------+
//| Public API                                          |
//+-----------------------------------------------------+

int au_gain_cache_set_file(const char *path) {
    SDL_mutex *m = cache_mutex();
    if (!m) return AU_ERR_SDL;
    char *copy = path ? av_strdup(path) : NULL;
    if (path && !copy) return AU_ERR_NO_MEMORY;

    SDL_LockMutex(m);
    close_append();
    close_file_lock(); // it belongs to the old file
    table_clear();
    av_free(file_path);
    file_path = copy;
    custom_location = path != NULL;
    loaded = false;
    foreign = false;
    file_lines = 0;
    ensure_loaded();
    SDL_UnlockMutex(m);

    ff_gain_cache_changed(NULL);
    return AU_OK;
}

int au_gain_cache_get_file(char *buf, const size_t size) {
    if (!buf || !size) return AU_ERR_INVALID_ARGUMENT;
    SDL_mutex *m = cache_mutex();
    if (!m) return AU_ERR_SDL;
    SDL_LockMutex(m);
    ensure_loaded();
    const char *p = file_path ? file_path : "";
    const size_t len = strlen(p);
    const int ret = len < size ? (int)len : AU_ERR_INVALID_ARGUMENT;
    if (len < size) memcpy(buf, p, len + 1);
    SDL_UnlockMutex(m);
    return ret;
}

int au_gain_cache_reload(void) {
    SDL_mutex *m = cache_mutex();
    if (!m) return AU_ERR_SDL;
    SDL_LockMutex(m);
    close_append();
    table_clear();
    loaded = false;
    foreign = false;
    ensure_loaded();
    SDL_UnlockMutex(m);
    ff_gain_cache_changed(NULL);
    return AU_OK;
}

int au_gain_cache_set(const char *path, const AuReplayGainInfo *rg) {
    if (!path || !path[0] || !rg) return AU_ERR_INVALID_ARGUMENT;
    SDL_mutex *m = cache_mutex();
    if (!m) return AU_ERR_SDL;

    AVBPrint bp;
    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
    encode_set(&bp, path, rg);
    if (!av_bprint_is_complete(&bp)) {
        av_bprint_finalize(&bp, NULL);
        return AU_ERR_NO_MEMORY;
    }

    SDL_LockMutex(m);
    ensure_loaded();
    int ret = table_set(path, rg);
    if (ret == AU_OK) ret = append_record(bp.str, bp.len);
    if (cache_changed) SDL_CondBroadcast(cache_changed);
    SDL_UnlockMutex(m);
    av_bprint_finalize(&bp, NULL);

    ff_gain_cache_changed(path);
    return ret; // AU_ERR_FILE: kept in memory, but could not be written to the file
}

unsigned gain_cache_failure_mark(void) {
    SDL_mutex *m = cache_mutex();
    if (!m) return 0;
    SDL_LockMutex(m);
    const unsigned mark = failure_count;
    SDL_UnlockMutex(m);
    return mark;
}

void gain_cache_note_failure(const char *path) {
    SDL_mutex *m = cache_mutex();
    if (!m || !path) return;
    char *copy = av_strdup(path);
    if (!copy) return; // the waiter times out instead
    SDL_LockMutex(m);
    char **slot = &recent_failures[failure_count++ % RECENT_FAILURES];
    av_free(*slot);
    *slot = copy;
    if (cache_changed) SDL_CondBroadcast(cache_changed);
    SDL_UnlockMutex(m);
}

// cache mutex held
static bool failed_since(const char *path, const unsigned mark) {
    for (unsigned i = mark; i != failure_count; i++)
        if (failure_count - i <= RECENT_FAILURES && !strcmp(recent_failures[i % RECENT_FAILURES], path))
            return true;
    return false;
}

GainWait gain_cache_wait(const char *path, const int timeout_ms, SDL_atomic_t *abort, const unsigned mark) {
    if (!path || !path[0]) return GAIN_WAIT_FAILED;
    SDL_mutex *m = cache_mutex();
    if (!m) return GAIN_WAIT_FAILED;
    const Uint32 deadline = SDL_GetTicks() + (Uint32)(timeout_ms > 0 ? timeout_ms : 0);
    GainWait result;
    SDL_LockMutex(m);
    ensure_loaded();
    for (;;) {
        Entry **slot = find_slot(path, hash_path(path));
        if (slot && *slot) { result = GAIN_WAIT_CACHED; break; }
        if (failed_since(path, mark)) { result = GAIN_WAIT_FAILED; break; }
        if (abort && SDL_AtomicGet(abort)) { result = GAIN_WAIT_ABORTED; break; }
        const Sint32 left = (Sint32)(deadline - SDL_GetTicks());
        if (left <= 0) { result = GAIN_WAIT_TIMEOUT; break; }
        SDL_CondWaitTimeout(cache_changed, m, (Uint32)(left < 20 ? left : 20)); // re-check abort regularly
    }
    SDL_UnlockMutex(m);
    return result;
}

bool au_gain_cache_get(const char *path, AuReplayGainInfo *out) {
    return gain_cache_lookup(path, out);
}

bool au_gain_cache_remove(const char *path) {
    if (!path || !path[0]) return false;
    SDL_mutex *m = cache_mutex();
    if (!m) return false;

    AVBPrint bp;
    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
    av_bprintf(&bp, "R\t");
    put_path(&bp, path);
    av_bprint_chars(&bp, '\n', 1);

    SDL_LockMutex(m);
    ensure_loaded();
    const bool removed = table_remove(path);
    if (removed && av_bprint_is_complete(&bp)) append_record(bp.str, bp.len);
    SDL_UnlockMutex(m);
    av_bprint_finalize(&bp, NULL);

    if (removed) ff_gain_cache_changed(path);
    return removed;
}

void au_gain_cache_clear(void) {
    SDL_mutex *m = cache_mutex();
    if (!m) return;
    SDL_LockMutex(m);
    ensure_loaded();
    table_clear();
    if (persistent() && !foreign) { // an empty file
        const bool locked = take_file_lock();
        write_file();
        release_file_lock(locked);
    }
    SDL_UnlockMutex(m);
    ff_gain_cache_changed(NULL);
}

int au_gain_cache_count(void) {
    SDL_mutex *m = cache_mutex();
    if (!m) return 0;
    SDL_LockMutex(m);
    ensure_loaded();
    const int n = (int)n_entries;
    SDL_UnlockMutex(m);
    return n;
}

int au_gain_cache_foreach(const AuGainCacheVisitor visitor, void *user) {
    if (!visitor) return 0;
    SDL_mutex *m = cache_mutex();
    if (!m) return 0;

    // Copy first so the visitor can use the cache without deadlocking
    SDL_LockMutex(m);
    ensure_loaded();
    const size_t n = n_entries;
    Entry *copy = n ? av_malloc_array(n, sizeof(Entry)) : NULL;
    size_t k = 0;
    for (size_t i = 0; copy && i < n_buckets; i++) {
        for (Entry *e = buckets[i]; e; e = e->next) {
            copy[k] = *e;
            copy[k].path = av_strdup(e->path);
            if (!copy[k].path) break;
            k++;
        }
    }
    SDL_UnlockMutex(m);

    int visited = 0;
    bool go = true;
    for (size_t i = 0; i < k; i++) {
        if (go) {
            go = visitor(copy[i].path, &copy[i].rg, user);
            visited++;
        }
        av_free(copy[i].path);
    }
    av_free(copy);
    return visited;
}

int au_gain_cache_scan(const char *path, const AuLoudnessConfig *config, AuReplayGainInfo *out) {
    if (!path || !path[0]) return AU_ERR_INVALID_ARGUMENT;
    AuReplayGainInfo rg;
    if (gain_cache_lookup(path, &rg)) {
        if (out) *out = rg;
        return AU_OK;
    }

    AuLoudness l;
    const int ret = au_loudness_analyze(path, config, &l);
    if (ret != AU_OK) return ret;
    rg = au_loudness_to_replaygain(&l, NULL);
    const int set = au_gain_cache_set(path, &rg);
    if (out) *out = rg;
    return set; // AU_ERR_FILE: measured and cached in memory, but not written
}
