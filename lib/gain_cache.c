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
 * Process-wide ReplayGain cache: path -> AuReplayGainInfo, a hash table behind one mutex.
 * It is independent of the player; the player only reads from it (gain_cache_lookup) and is told
 * about changes so queued and playing tracks pick them up.
 */

#include "gain_cache.h"
#include "ffaudio_loudness.h"
#include "SDL2/SDL_atomic.h"
#include "SDL2/SDL_mutex.h"

typedef struct Entry {
    char *path;
    uint32_t hash;
    AuReplayGainInfo rg;
    struct Entry *next;
} Entry;

static Entry **buckets = NULL;  // guarded by the cache mutex
static size_t n_buckets = 0;
static size_t n_entries = 0;
static void *cache_mutex_ptr = NULL;

// Created on first use; SDL mutexes work without SDL_Init
static SDL_mutex *cache_mutex(void) {
    SDL_mutex *m = SDL_AtomicGetPtr(&cache_mutex_ptr);
    if (m) return m;
    SDL_mutex *created = SDL_CreateMutex();
    if (!created) return NULL;
    if (!SDL_AtomicCASPtr(&cache_mutex_ptr, NULL, created))
        SDL_DestroyMutex(created); // another thread was first
    return SDL_AtomicGetPtr(&cache_mutex_ptr);
}

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

bool gain_cache_lookup(const char *path, AuReplayGainInfo *out) {
    if (!path || !path[0] || !SDL_AtomicGetPtr(&cache_mutex_ptr)) return false;
    SDL_mutex *m = cache_mutex();
    SDL_LockMutex(m);
    Entry **slot = find_slot(path, hash_path(path));
    const bool found = slot && *slot;
    if (found && out) *out = (*slot)->rg;
    SDL_UnlockMutex(m);
    return found;
}

//+-----------------------------------------------------+
//| Public API                                          |
//+-----------------------------------------------------+

int au_gain_cache_set(const char *path, const AuReplayGainInfo *rg) {
    if (!path || !path[0] || !rg) return AU_ERR_INVALID_ARGUMENT;
    SDL_mutex *m = cache_mutex();
    if (!m) return AU_ERR_SDL;

    const uint32_t hash = hash_path(path);
    int ret = AU_OK;
    SDL_LockMutex(m);
    Entry **slot = find_slot(path, hash);
    if (slot && *slot) {
        (*slot)->rg = *rg;
    }
    else if ((n_entries + 1) * 4 > n_buckets * 3 && !grow()) {
        ret = AU_ERR_NO_MEMORY;
    }
    else {
        Entry *e = av_mallocz(sizeof(Entry));
        if (!e || !(e->path = av_strdup(path))) {
            av_free(e);
            ret = AU_ERR_NO_MEMORY;
        }
        else {
            e->hash = hash;
            e->rg = *rg;
            slot = &buckets[hash & (n_buckets - 1)];
            e->next = *slot;
            *slot = e;
            n_entries++;
        }
    }
    SDL_UnlockMutex(m);

    if (ret == AU_OK) ff_gain_cache_changed(path);
    return ret;
}

bool au_gain_cache_get(const char *path, AuReplayGainInfo *out) {
    return gain_cache_lookup(path, out);
}

bool au_gain_cache_remove(const char *path) {
    if (!path || !path[0] || !SDL_AtomicGetPtr(&cache_mutex_ptr)) return false;
    SDL_mutex *m = cache_mutex();
    SDL_LockMutex(m);
    Entry **slot = find_slot(path, hash_path(path));
    Entry *e = (slot && *slot) ? *slot : NULL;
    if (e) {
        *slot = e->next;
        n_entries--;
    }
    SDL_UnlockMutex(m);
    if (!e) return false;
    av_free(e->path);
    av_free(e);
    ff_gain_cache_changed(path);
    return true;
}

void au_gain_cache_clear(void) {
    if (!SDL_AtomicGetPtr(&cache_mutex_ptr)) return;
    SDL_mutex *m = cache_mutex();
    SDL_LockMutex(m);
    Entry **old = buckets;
    const size_t old_n = n_buckets;
    buckets = NULL;
    n_buckets = 0;
    n_entries = 0;
    SDL_UnlockMutex(m);

    for (size_t i = 0; i < old_n; i++) {
        Entry *e = old[i];
        while (e) {
            Entry *next = e->next;
            av_free(e->path);
            av_free(e);
            e = next;
        }
    }
    av_free(old);
    ff_gain_cache_changed(NULL);
}

int au_gain_cache_count(void) {
    if (!SDL_AtomicGetPtr(&cache_mutex_ptr)) return 0;
    SDL_mutex *m = cache_mutex();
    SDL_LockMutex(m);
    const int n = (int)n_entries;
    SDL_UnlockMutex(m);
    return n;
}

int au_gain_cache_foreach(const AuGainCacheVisitor visitor, void *user) {
    if (!visitor || !SDL_AtomicGetPtr(&cache_mutex_ptr)) return 0;

    // Copy first so the visitor can use the cache without deadlocking
    SDL_mutex *m = cache_mutex();
    SDL_LockMutex(m);
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
    if (set != AU_OK) return set;
    if (out) *out = rg;
    return AU_OK;
}
