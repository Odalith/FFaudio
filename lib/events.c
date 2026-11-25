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
 * The event queue for au_poll_event() / au_wait_event(): a ring of copies, each owning its strings. The mutex and
 * condition outlive the player (created once), so polling right through au_shutdown() is safe.
 */

#include "events.h"

#include <math.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "libavutil/common.h"
#include "libavutil/mem.h"
#include "once.h"
#include "chaos.h"

typedef struct Entry {
    AuEvent ev;
    char *s1, *s2;              // the event's strings (which ones depends on the type)
} Entry;

static SDL_mutex *mutex;
static SDL_cond *cond;
static FfOnce once = FF_ONCE_INIT;

static Entry *ring;             // grows up to EVENT_QUEUE_CAPACITY
static int cap, head, count;
static int dropped;             // since the last AU_EVENT_OVERFLOW
static bool open_;
static Entry polled;            // the last event handed out: its strings live until the next poll

static void make_sync(void) {
    mutex = SDL_CreateMutex();
    cond = SDL_CreateCond();
}

static bool sync_ready(void) {
    ff_once(&once, make_sync);
    return mutex && cond;
}

static void entry_free(Entry *e) {
    av_freep(&e->s1);
    av_freep(&e->s2);
}

// The strings of an event, where they are
static void strings_of(AuEvent *e, const char ***a, const char ***b) {
    *a = *b = NULL;
    switch (e->type) {
        case AU_EVENT_LOG:             *a = &e->data.log.message; break;
        case AU_EVENT_DEVICE:          *a = &e->data.device.name; break;
        case AU_EVENT_STREAM_METADATA: *a = &e->data.metadata.title; *b = &e->data.metadata.artist; break;
        case AU_EVENT_TRACK_ENDED:     *a = &e->data.ended.message; break;
        default: break;
    }
}

// AuEvent's size is part of the ABI (au_poll_event() writes into the caller's): a new member must fit the padding
typedef char au_event_data_is_64_bytes[sizeof(((AuEvent *)0)->data) == 64 ? 1 : -1];

static void clear_locked(void) {
    for (int i = 0; i < count; i++) entry_free(&ring[(head + i) % cap]);
    head = count = dropped = 0;
    entry_free(&polled);
}

void events_open(void) {
    CHAOS("events_open");
    if (!sync_ready()) return;
    SDL_LockMutex(mutex);
    clear_locked();
    open_ = true;
    SDL_UnlockMutex(mutex);
}

void events_close(void) {
    CHAOS("events_close");
    if (!sync_ready()) return;
    SDL_LockMutex(mutex);
    clear_locked();
    av_freep(&ring);
    cap = 0;
    open_ = false;
    SDL_CondBroadcast(cond);
    SDL_UnlockMutex(mutex);
}

// Make room for one more: grow, or drop a log event, or the oldest one
static bool make_room_locked(const AuEvent *incoming) {
    if (count < cap) return true;
    if (cap < EVENT_QUEUE_CAPACITY) {
        const int new_cap = cap ? FFMIN(cap * 2, EVENT_QUEUE_CAPACITY) : 64;
        Entry *grown = av_malloc_array(new_cap, sizeof(Entry));
        if (grown) {
            for (int i = 0; i < count; i++) grown[i] = ring[(head + i) % cap];
            av_free(ring);
            ring = grown;
            cap = new_cap;
            head = 0;
            return true;
        }
    }
    if (cap == 0) return false;
    dropped++;
    if (incoming->type == AU_EVENT_LOG) return false;   // logs go first: drop this one
    // The oldest log event, or else the oldest event
    int victim = 0;
    for (int i = 0; i < count; i++)
        if (ring[(head + i) % cap].ev.type == AU_EVENT_LOG) { victim = i; break; }
    entry_free(&ring[(head + victim) % cap]);
    for (int i = victim; i > 0; i--) ring[(head + i) % cap] = ring[(head + i - 1) % cap];
    head = (head + 1) % cap;
    count--;
    return true;
}

void events_push(const AuEvent *e) {
    CHAOS("events_push");
    if (!sync_ready()) return;
    Entry n = { .ev = *e };
    const char **a, **b;
    strings_of(&n.ev, &a, &b);
    if (a && *a && !(n.s1 = av_strdup(*a))) return;
    if (b && *b && !(n.s2 = av_strdup(*b))) { entry_free(&n); return; }

    SDL_LockMutex(mutex);
    if (open_ && make_room_locked(e)) {
        ring[(head + count) % cap] = n;
        count++;
        n.s1 = n.s2 = NULL;
        SDL_CondSignal(cond);
    }
    SDL_UnlockMutex(mutex);
    entry_free(&n); // when it was not queued
}

// Next event; mutex held
static bool take_locked(AuEvent *out) {
    entry_free(&polled);
    if (dropped) {
        memset(out, 0, sizeof(*out));
        out->type = AU_EVENT_OVERFLOW;
        out->data.overflow.dropped = dropped;
        dropped = 0;
        return true;
    }
    if (!count) return false;
    polled = ring[head];
    head = (head + 1) % cap;
    count--;
    *out = polled.ev;
    const char **a, **b;
    strings_of(out, &a, &b);
    if (a) *a = polled.s1;
    if (b) *b = polled.s2;
    return true;
}

bool au_poll_event(AuEvent *out) {
    CHAOS("au_poll_event");
    if (!out || !sync_ready()) return false;
    SDL_LockMutex(mutex);
    const bool got = take_locked(out);
    SDL_UnlockMutex(mutex);
    return got;
}

bool au_wait_event(AuEvent *out, const double timeout_seconds) {
    CHAOS("au_wait_event");
    if (!out || !sync_ready()) return false;
    const bool forever = timeout_seconds < 0 || !isfinite(timeout_seconds);
    const Uint32 deadline = SDL_GetTicks() + (forever ? 0 : (Uint32)llround(timeout_seconds * 1000));
    SDL_LockMutex(mutex);
    bool got;
    while (!(got = take_locked(out)) && open_) {
        if (forever) {
            SDL_CondWait(cond, mutex);
            continue;
        }
        const Sint32 left = (Sint32)(deadline - SDL_GetTicks());
        if (left <= 0) break;
        SDL_CondWaitTimeout(cond, mutex, (Uint32)left);
    }
    SDL_UnlockMutex(mutex);
    return got;
}
