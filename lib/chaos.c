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

// Chaos points (chaos.h): only built with FFAUDIO_CHAOS

#include "chaos.h"

#ifdef FFAUDIO_CHAOS
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "once.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif
#ifdef _MSC_VER
#define CHAOS_TLS __declspec(thread)
#else
#define CHAOS_TLS _Thread_local
#endif

static unsigned chaos_seed = 1, chaos_percent = 0, chaos_max_us = 2000;
static FfOnce chaos_once = FF_ONCE_INIT;
static CHAOS_TLS uint64_t rng_state;

// Points named in FFAUDIO_CHAOS_AT="name=us|name=us" (point names contain commas): they sleep exactly that long every
// time (tests/test_races.c)
#define MAX_FORCED 16
typedef struct Forced {
    char name[96];
    unsigned us;
} Forced;
static Forced forced[MAX_FORCED];
static int n_forced = 0;

static void parse_forced(const char *e) {
    while (e && *e && n_forced < MAX_FORCED) {
        const char *end = strchr(e, '|');
        const size_t len = end ? (size_t)(end - e) : strlen(e);
        const char *eq = memchr(e, '=', len);
        if (eq && eq > e && (size_t)(eq - e) < sizeof(forced[0].name)) {
            Forced *f = &forced[n_forced++];
            memcpy(f->name, e, (size_t)(eq - e));
            f->name[eq - e] = '\0';
            f->us = (unsigned)strtoul(eq + 1, NULL, 10);
            fprintf(stderr, "[ffaudio chaos] '%s' always sleeps %u us\n", f->name, f->us);
        }
        e = end ? end + 1 : NULL;
    }
}

static void chaos_init(void) {
    parse_forced(getenv("FFAUDIO_CHAOS_AT"));
    const char *e = getenv("FFAUDIO_CHAOS");
    if (!e || !e[0]) return;
    unsigned seed = 1, percent = 10, max_us = 2000;
    sscanf(e, "%u:%u:%u", &seed, &percent, &max_us);
    chaos_seed = seed;
    chaos_percent = percent > 100 ? 100 : percent;
    chaos_max_us = max_us;
    fprintf(stderr, "[ffaudio chaos] seed %u, %u%% of points sleep up to %u us\n", chaos_seed, chaos_percent,
            chaos_max_us);
}

static uint32_t next_random(const char *where) {
    if (!rng_state) {   // per thread, from the seed and the thread
        rng_state = ((uint64_t)chaos_seed << 32) ^ (uint64_t)SDL_ThreadID() ^ 0x9E3779B97F4A7C15ull;
        if (!rng_state) rng_state = 1;
    }
    // A little of the point's name, so different points of one thread get different sequences
    for (const char *p = where; *p; p++) rng_state ^= (uint64_t)(unsigned char)*p << ((p - where) % 56);
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 16);
}

static void sleep_us(const unsigned us) {
#ifdef _WIN32
    Sleep(us >= 1000 ? us / 1000 : 0);   // 0 still gives up the time slice
#else
    const struct timespec ts = { (time_t)(us / 1000000), (long)(us % 1000000) * 1000 };
    nanosleep(&ts, NULL);
#endif
}

void ff_chaos_point(const char *where, const int realtime) {
    ff_once(&chaos_once, chaos_init);
    for (int i = 0; i < n_forced; i++) {
        if (!strcmp(forced[i].name, where)) {
            sleep_us(forced[i].us); // also in the callback: underruns are expected then
            return;
        }
    }
    if (!chaos_percent) return;
    const uint32_t r = next_random(where);
    if (r % 100 >= chaos_percent) return;
    unsigned max = chaos_max_us;
    if (realtime) max = SDL_min(max / 20, 300u);
    if (max) sleep_us((r >> 8) % (max + 1));
}
#endif
