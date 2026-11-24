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
 * Level meters and the spectrum: readers of the meter tap (MeterTap in globals.h), which the audio callback fills.
 * A reader takes tap_mutex (never the device lock), works out which frames are being heard now from the last status
 * the callback published, reads them with atomic loads and then checks the callback has not overwritten them.
 */

#include "ffaudio_meters.h"

#include <math.h>
#include <string.h>
#include "globals.h"

// A window of the tap that is being heard now
typedef struct TapView {
    const MeterTap *tap;
    uint32_t start;              // first frame (tap counter)
    uint32_t frames;
    bool silent;                 // nothing plays (idle, paused, or the meters are starting)
} TapView;

static double now_seconds(void) {
    return (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency();
}

// tap_mutex held. The `want` frames heard last: the device plays what the callback wrote about two periods later
// (the model au_get_status() uses for positions).
static TapView tap_view(AudioPlayer *ap, const StatusSnapshot *st, const uint32_t want) {
    TapView v = { .tap = &ap->tap, .silent = true };
    const MeterTap *tp = &ap->tap;
    if (!tp->buf || st->period <= 0 || tp->rate <= 0) return v;
    const double elapsed = now_seconds() - st->callback_time;
    if (elapsed > 4 * st->period + 0.1) return v;   // the device stopped (nothing to play)
    const uint32_t published = (uint32_t)ffa_load_acquire(&tp->published);
    uint32_t end = published;
    if (st->tap_gen == tp->gen) {
        const double behind = (2 * st->period - FFMIN(elapsed, st->period)) * tp->rate;
        end = (uint32_t)st->tap_end - (uint32_t)llrint(FFMAX(0.0, behind));
        if ((int32_t)(end - published) > 0) end = published;
    }
    // Never more than what is there and safe from being overwritten while it is read (a few periods)
    const uint32_t room = tp->frames - FFMIN(tp->frames / 2, (uint32_t)llrint(4 * st->period * tp->rate));
    const uint32_t avail = FFMIN(published, room);
    const uint32_t n = FFMIN(want, avail);
    if (n == 0 || (uint32_t)(published - end) > room - n) return v;
    v.start = end - n;
    v.frames = n;
    v.silent = false;
    return v;
}

// Still there after reading it?
static bool tap_view_intact(const TapView *v) {
    const uint32_t written = (uint32_t)ffa_load_acquire(&v->tap->written);
    return (uint32_t)(written - v->start) <= v->tap->frames;
}

static float tap_sample(const MeterTap *tp, const uint32_t frame, const int ch) {
    const int32_t bits = ffa_load_acquire(&tp->buf[(size_t)(frame & (tp->frames - 1)) * tp->channels + ch]);
    float v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

static bool take_status(AudioPlayer *ap, StatusSnapshot *st) {
    SDL_LockMutex(ap->status_mutex);
    *st = ap->status;
    SDL_UnlockMutex(ap->status_mutex);
    return true;
}

int au_get_levels(const double window_seconds, AuLevels *out) {
    if (!out) return AU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    AudioPlayer *ap = ff_player();
    if (!ap) return AU_ERR_NOT_INITIALIZED;
    if (!(window_seconds > 0 && window_seconds <= 0.5)) return AU_ERR_INVALID_ARGUMENT;
    ff_request_tap(ap);

    StatusSnapshot st;
    take_status(ap, &st);   // released before tap_mutex: the locks are never nested
    SDL_LockMutex(ap->tap_mutex);
    const MeterTap *tp = &ap->tap;
    out->channels = tp->buf ? FFMIN(tp->channels, 8) : 0;
    out->sample_rate = tp->buf ? tp->rate : 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        const TapView v = tap_view(ap, &st, (uint32_t)llrint(window_seconds * FFMAX(tp->rate, 1)));
        if (v.silent) break;
        double sum[8] = { 0 };
        float peak[8] = { 0 };
        for (uint32_t i = 0; i < v.frames; i++)
            for (int c = 0; c < out->channels; c++) {
                const float x = tap_sample(tp, v.start + i, c);
                sum[c] += (double)x * x;
                peak[c] = fmaxf(peak[c], fabsf(x));
            }
        if (!tap_view_intact(&v)) continue;   // the callback got to it meanwhile: again
        out->window_seconds = (double)v.frames / tp->rate;
        for (int c = 0; c < out->channels; c++) {
            out->peak[c] = peak[c];
            out->rms[c] = (float)sqrt(sum[c] / v.frames);
        }
        break;
    }
    SDL_UnlockMutex(ap->tap_mutex);
    return AU_OK;
}
