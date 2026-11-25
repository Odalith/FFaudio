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
#include "libavutil/mem.h"
#include "libavutil/tx.h"
#include "chaos.h"

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
    CHAOS("au_get_levels");
    if (!out) return AU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    AudioPlayer *ap = ff_player();
    if (!ap) return AU_ERR_NOT_INITIALIZED;
    ff_request_tap(ap);   // any call turns the meters on
    if (!(window_seconds > 0 && window_seconds <= 0.5)) return AU_ERR_INVALID_ARGUMENT;

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

//+-----------------------------------------------------+
//| Spectrum                                            |
//+-----------------------------------------------------+

struct AuSpectrum {
    int n;                       // FFT size
    AVTXContext *tx;
    av_tx_fn fn;
    float *window;               // Hann
    float scale;                 // 2 / sum of the window: a full-scale sine reads 1.0
    float *frames;               // the copied window, interleaved (up to 8 channels)
    float *in;                   // one channel, windowed
    AVComplexFloat *out;         // n / 2 + 1 bins
    double *power;               // n / 2 + 1, summed over channels
};

AuSpectrum *au_spectrum_create(const int fft_size) {
    if (fft_size < 256 || fft_size > 16384 || (fft_size & (fft_size - 1))) return NULL;
    AuSpectrum *s = av_mallocz(sizeof(*s));
    if (!s) return NULL;
    s->n = fft_size;
    const float one = 1.0f;
    s->window = av_malloc_array((size_t)fft_size, sizeof(float));
    s->frames = av_malloc_array((size_t)fft_size * 8, sizeof(float));
    s->in = av_malloc_array((size_t)fft_size + 2, sizeof(float));
    s->out = av_malloc_array((size_t)fft_size / 2 + 1, sizeof(AVComplexFloat));
    s->power = av_malloc_array((size_t)fft_size / 2 + 1, sizeof(double));
    if (!s->window || !s->frames || !s->in || !s->out || !s->power ||
        av_tx_init(&s->tx, &s->fn, AV_TX_FLOAT_RDFT, 0, fft_size, &one, 0) < 0) {
        au_spectrum_free(s);
        return NULL;
    }
    double sum = 0;
    for (int i = 0; i < fft_size; i++) {
        s->window[i] = (float)(0.5 - 0.5 * cos(2 * M_PI * i / fft_size));
        sum += s->window[i];
    }
    s->scale = (float)(2.0 / sum);
    return s;
}

void au_spectrum_free(AuSpectrum *s) {
    if (!s) return;
    av_tx_uninit(&s->tx);
    av_free(s->window);
    av_free(s->frames);
    av_free(s->in);
    av_free(s->out);
    av_free(s->power);
    av_free(s);
}

int au_spectrum_read(AuSpectrum *s, const int channel, float *magnitudes, const int capacity, int *sample_rate) {
    CHAOS("au_spectrum_read");
    if (!s || !magnitudes || capacity < s->n / 2 + 1) return AU_ERR_INVALID_ARGUMENT;
    const int bins = s->n / 2 + 1;
    memset(magnitudes, 0, (size_t)bins * sizeof(float));
    if (sample_rate) *sample_rate = 0;
    AudioPlayer *ap = ff_player();
    if (!ap) return AU_ERR_NOT_INITIALIZED;
    ff_request_tap(ap);   // any call turns the meters on
    if (channel < -1 || channel >= 8) return AU_ERR_INVALID_ARGUMENT;

    // Copy the window out, so the lock is only held for that; the FFT runs on the caller's thread
    StatusSnapshot st;
    take_status(ap, &st);
    SDL_LockMutex(ap->tap_mutex);
    const MeterTap *tp = &ap->tap;
    const int channels = tp->buf ? FFMIN(tp->channels, 8) : 0;
    const int rate = tp->buf ? tp->rate : 0;
    bool got = false;
    if (channel >= channels && channels > 0) {
        SDL_UnlockMutex(ap->tap_mutex);
        return AU_ERR_INVALID_ARGUMENT;
    }
    for (int attempt = 0; attempt < 3 && channels > 0; attempt++) {
        const TapView v = tap_view(ap, &st, (uint32_t)s->n);
        if (v.silent) break;
        const uint32_t pad = (uint32_t)s->n - v.frames;   // the meters just started: less than a window
        memset(s->frames, 0, (size_t)pad * channels * sizeof(float));
        for (uint32_t i = 0; i < v.frames; i++)
            for (int c = 0; c < channels; c++)
                s->frames[(size_t)(pad + i) * channels + c] = tap_sample(tp, v.start + i, c);
        if (tap_view_intact(&v)) { got = true; break; }
    }
    SDL_UnlockMutex(ap->tap_mutex);
    if (sample_rate) *sample_rate = rate;
    if (!got) return AU_OK;

    memset(s->power, 0, (size_t)bins * sizeof(double));
    const int first = channel < 0 ? 0 : channel, last = channel < 0 ? channels - 1 : channel;
    for (int c = first; c <= last; c++) {
        for (int i = 0; i < s->n; i++) s->in[i] = s->frames[(size_t)i * channels + c] * s->window[i];
        s->fn(s->tx, s->out, s->in, sizeof(float));
        for (int k = 0; k < bins; k++)
            s->power[k] += (double)s->out[k].re * s->out[k].re + (double)s->out[k].im * s->out[k].im;
    }
    const double count = last - first + 1;
    for (int k = 0; k < bins; k++) magnitudes[k] = (float)(sqrt(s->power[k] / count) * s->scale);
    return AU_OK;
}

int au_spectrum_bands(const float *magnitudes, const int fft_size, const int sample_rate, const double f_min,
                      double f_max, float *bands, const int n_bands) {
    if (!magnitudes || !bands || n_bands <= 0 || fft_size < 2 || sample_rate <= 0 || !(f_min > 0) || !(f_max > f_min))
        return AU_ERR_INVALID_ARGUMENT;
    f_max = FFMIN(f_max, sample_rate / 2.0);
    if (!(f_max > f_min)) return AU_ERR_INVALID_ARGUMENT;
    const int bins = fft_size / 2 + 1;
    const double hz_per_bin = (double)sample_rate / fft_size;
    const double ratio = f_max / f_min;
    for (int b = 0; b < n_bands; b++) {
        const double lo = f_min * pow(ratio, (double)b / n_bands), hi = f_min * pow(ratio, (double)(b + 1) / n_bands);
        int k0 = (int)ceil(lo / hz_per_bin), k1 = (int)ceil(hi / hz_per_bin);   // bins in [lo, hi)
        float v = 0;
        if (k1 > k0) {
            for (int k = FFMAX(k0, 0); k < k1 && k < bins; k++) v = fmaxf(v, magnitudes[k]);
        } else {
            const int k = (int)lrint(sqrt(lo * hi) / hz_per_bin);
            if (k >= 0 && k < bins) v = magnitudes[k];
        }
        bands[b] = v;
    }
    return AU_OK;
}
