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
 * Output device and mixer.
 *
 * The SDL callback runs with the device lock held. It never blocks, allocates, frees or signals: it
 * takes float frames from the rings of the tracks in the mixer slots (lib/sample_ring.c), applies fades
 * and gain, and converts to the device format (with TPDF dither for integer formats). The status for the
 * getters goes out with a try-lock that skips a period rather than wait. Slot changes it makes itself (gapless
 * handover, crossfade at a natural end) are reported to the control thread through Mixer.events.
 */

#include "output.h"
#include "track.h"
#include <limits.h>
#include <math.h>

#define VOLUME_RAMP_SECONDS 0.010

//+-----------------------------------------------------+
//| Fade curves                                         |
//+-----------------------------------------------------+

static float curve_value(const AuFade *fade, float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    if (!fade) return t;
    switch (fade->curve) {
        case AU_FADE_EQUAL_POWER: return sinf(t * (float)M_PI_2);
        case AU_FADE_S_CURVE:     return t * t * (3.0f - 2.0f * t);
        case AU_FADE_LOGARITHMIC: return 1.0f - (1.0f - t) * (1.0f - t);
        case AU_FADE_EXPONENTIAL: return t * t;
        case AU_FADE_CUSTOM:
            if (fade->custom) {
                const float g = fade->custom(t, fade->custom_user);
                return g < 0.0f ? 0.0f : (g > 1.0f ? 1.0f : g);
            }
            return t;
        case AU_FADE_LINEAR:
        default:
            return t;
    }
}

void fade_curve_build(FadeCurve *out, const AuFade *fade) {
    for (int i = 0; i <= FADE_CURVE_STEPS; i++)
        out->g[i] = curve_value(fade, (float)i / FADE_CURVE_STEPS);
}

static inline float curve_at(const FadeCurve *c, float t) {
    if (t <= 0.0f) return c->g[0];
    if (t >= 1.0f) return c->g[FADE_CURVE_STEPS];
    const float x = t * FADE_CURVE_STEPS;
    const int i = (int)x;
    return c->g[i] + (c->g[i + 1] - c->g[i]) * (x - (float)i);
}

// Gain of a fade `offset` frames after its current position (an inactive fade-in is at 1, a fade-out at 0)
static inline float fade_gain_at(const FadeState *f, const int64_t offset, const bool fading_out) {
    if (!f->active || f->len <= 0) return fading_out ? 0.0f : 1.0f;
    const float t = (float)(f->pos + offset) / (float)f->len;
    return curve_at(&f->curve, fading_out ? 1.0f - t : t);
}

//+-----------------------------------------------------+
//| Events (callback -> control thread)                 |
//+-----------------------------------------------------+

static void push_event(Mixer *m, const MixerEventType type, TrackState *t, const double position) {
    if (m->n_events >= MIXER_EVENT_CAPACITY) {
        // The control thread also scans for tracks that left the mixer, so nothing leaks
        m->events_overflowed = true;
        return;
    }
    MixerEvent *e = &m->events[m->n_events++];
    e->type = type;
    e->track = t;
    e->position = position;
    m->wake_control = true;
}

//+-----------------------------------------------------+
//| Transitions                                         |
//+-----------------------------------------------------+

bool mixer_start_transition(AudioPlayer *ap, TrackState *outgoing, TrackState *incoming, const double seconds,
                            const FadeCurve *curve) {
    Mixer *m = &ap->mixer;
    const int64_t len = (curve && seconds > 0 && m->rate > 0) ? (int64_t)(seconds * m->rate) : 0;
    if (len <= 0) return false; // shorter than a frame, or no device yet: the caller cuts

    if (outgoing) {
        if (m->outgoing && m->outgoing != outgoing) {
            // Only one track can fade out at a time: the older one is cut
            push_event(m, MIX_EV_FADE_DONE, m->outgoing, m->outgoing->position);
        }
        // If it was still fading in, continue from its current loudness instead of jumping to full
        int64_t start = 0;
        if (outgoing == m->current && m->in_fade.active && m->in_fade.len > 0) {
            const float p = (float)m->in_fade.pos / (float)m->in_fade.len;
            start = (int64_t)((1.0f - p) * (float)len);
        }
        m->outgoing = outgoing;
        m->out_fade.active = true;
        m->out_fade.pos = start;
        m->out_fade.len = len;
        m->out_fade.curve = *curve;
    }

    if (outgoing == m->current) m->current = NULL;
    if (incoming == m->next) m->next = NULL;

    m->current = incoming;
    m->in_fade.active = false;
    if (incoming) {
        m->in_fade.active = true;
        m->in_fade.pos = 0;
        m->in_fade.len = len;
        m->in_fade.curve = *curve;
    }
    return true;
}

void mixer_set_pause(AudioPlayer *ap, const bool paused, const double seconds, const FadeCurve *curve) {
    Mixer *m = &ap->mixer;
    const int64_t len = (curve && seconds > 0 && m->rate > 0) ? (int64_t)(seconds * m->rate) : 0;
    m->pause_from = m->pause_gain;
    m->pause_to = paused ? 0.0f : 1.0f;
    if (!paused) m->frozen = false;
    if (len > 0) {
        m->pause_fade.active = true;
        m->pause_fade.pos = 0;
        m->pause_fade.len = len;
        m->pause_fade.curve = *curve;
    }
    else {
        // A very short ramp avoids a click
        m->pause_fade.active = true;
        m->pause_fade.pos = 0;
        m->pause_fade.len = FFMAX(1, m->vol_ramp_frames);
        m->pause_fade.curve = m->linear_curve;
    }
}

// Crossfade into the queued track when the current one is about to end
// Length (seconds) of the crossfade the current track should get at its end; 0 = play gapless.
// *remaining is set to the seconds left in the current track.
static double planned_end_fade(Mixer *m, const FadeCurve **curve_out, double *remaining) {
    TrackState *cur = m->current, *next = m->next;
    if (!cur || !next || m->outgoing) return 0;

    const bool own = next->has_fade_in_override;
    const AuFade *fade = own ? &next->fade_in_override : &m->end_fade;
    if (fade->seconds <= 0) return 0;

    // Everything below is in output (wall clock) seconds: file seconds / playback speed
    const double speed = cur->play_speed > 0 ? cur->play_speed : 1.0;
    const double end = track_end_position(cur);
    const double cur_len = track_duration(cur) / speed;
    if (end < 0 || cur_len < 0) return 0; // unknown length: play gapless instead

    const double min_len = m->min_track_seconds > 0 ? m->min_track_seconds : 2.0 * fade->seconds;
    if (cur_len < min_len) return 0;

    double fade_s = FFMIN(fade->seconds, cur_len / 2.0);
    const double next_len = track_duration(next) / speed;
    if (next_len >= 0) {
        if (next_len < min_len) return 0;
        fade_s = FFMIN(fade_s, next_len / 2.0);
    }

    *curve_out = own ? &next->fade_in_curve : &m->end_curve;
    *remaining = (end - cur->position) / speed;
    return fade_s;
}

// Frames that can be rendered before the end crossfade has to start (INT_MAX = no crossfade planned)
static int frames_until_end_crossfade(Mixer *m) {
    const FadeCurve *curve = NULL;
    double remaining = 0;
    const double fade_s = planned_end_fade(m, &curve, &remaining);
    if (fade_s <= 0 || remaining <= fade_s) return INT_MAX;
    const double frames = ceil((remaining - fade_s) * m->rate);
    return frames >= INT_MAX ? INT_MAX : FFMAX(1, (int)frames);
}

static void maybe_start_end_crossfade(AudioPlayer *ap) {
    Mixer *m = &ap->mixer;
    const FadeCurve *curve = NULL;
    double remaining = 0;
    const double fade_s = planned_end_fade(m, &curve, &remaining);
    if (fade_s <= 0 || remaining > fade_s + 0.5 / m->rate || remaining <= 0.005) return;
    if (!track_has_audio(m->next, m->rate, m->channels)) return; // not decoded yet: next chunk (or end gapless)

    TrackState *cur = m->current;
    if (mixer_start_transition(ap, cur, m->next, remaining, curve)) // ends exactly when the current track does
        cur->fade_reason = END_NATURAL;
}

//+-----------------------------------------------------+
//| Mixing                                              |
//+-----------------------------------------------------+

#define LATE_GAIN_RAMP_SECONDS 2.0

// Frames per gain step: gains are exact at the edges of a block and linear in between (a 0.5 s equal-power fade is
// then within 3e-6 of its curve), so the inner loops are a multiply-add per sample
#define GAIN_BLOCK 64

// The track's ReplayGain gain: taken as it is with the first samples, then a linear ramp for every change (10 ms, or
// 2 s when the value arrived after the track started, e.g. a measurement that missed measure_wait_ms)
static float track_gain_step(Mixer *m, TrackState *t, float *target_out) {
    const int packed = SDL_AtomicGet(&t->gain_mb);
    if (!t->gain_started || packed != t->gain_packed) {
        t->gain_packed = packed;
        t->gain_target = powf(10.0f, (float)gain_centi_db(packed) / 2000.0f);
    }
    const float target = t->gain_target;
    if (!t->gain_started) {
        t->gain_cur = t->gain_seen = target;
        t->gain_step = 0;
        t->gain_started = true;
    }
    if (target != t->gain_seen) {
        const bool slow = gain_is_slow(packed);
        const float frames = slow ? (float)(LATE_GAIN_RAMP_SECONDS * m->rate) : (float)FFMAX(1, m->vol_ramp_frames);
        t->gain_seen = target;
        t->gain_step = (target - t->gain_cur) / frames;
    }
    *target_out = target;
    return t->gain_cur == target ? 0.0f : t->gain_step;
}

// A linear ramp `k` frames on, stopping at its target
static inline float ramp_at(const float from, const float step, const float target, const int64_t k) {
    if (step == 0.0f) return from;
    const float v = from + step * (float)k;
    return step > 0 ? (v > target ? target : v) : (v < target ? target : v);
}

// The gain of a track's sample `k` frames into the chunk: ReplayGain ramp x fade x fade-in after a network stall
static inline float voice_gain_at(const TrackState *t, const float rg_from, const float rg_step, const float rg_target,
                                  const FadeState *fade, const bool fading_out, const int64_t k) {
    float g = ramp_at(rg_from, rg_step, rg_target, k + 1) * fade_gain_at(fade, k, fading_out);
    if (t->resume_fade - k > 0) g *= 1.0f - (float)(t->resume_fade - k) / (float)t->resume_fade_len;
    return g;
}

static void mix_block(float *d, const float *s, const int frames, const int channels, float g, const float dg) {
    if (channels == 2) {
        for (int i = 0; i < frames; i++, s += 2, d += 2, g += dg) {
            d[0] += s[0] * g;
            d[1] += s[1] * g;
        }
    }
    else if (channels == 1) {
        for (int i = 0; i < frames; i++, g += dg) d[i] += s[i] * g;
    }
    else {
        for (int i = 0; i < frames; i++, s += channels, d += channels, g += dg)
            for (int c = 0; c < channels; c++) d[c] += s[c] * g;
    }
}

static void add_voice(Mixer *m, TrackState *t, float *dst, const float *src, const int frames, const int channels,
                      const FadeState *fade, const bool fading_out) {
    // A track that has produced nothing yet (its file may not even be open) must not fix its starting gain:
    // that is read with its first samples, when the read thread has set it
    if (frames <= 0) return;
    float target;
    const float step = track_gain_step(m, t, &target);
    const float from = t->gain_cur;
    float g0 = voice_gain_at(t, from, step, target, fade, fading_out, 0);
    for (int b = 0; b < frames; b += GAIN_BLOCK) {
        const int n = FFMIN(GAIN_BLOCK, frames - b);
        const float g1 = voice_gain_at(t, from, step, target, fade, fading_out, b + n);
        mix_block(dst + (size_t)b * channels, src + (size_t)b * channels, n, channels, g0, (g1 - g0) / (float)n);
        g0 = g1;
    }
    t->gain_cur = ramp_at(from, step, target, frames);
    t->resume_fade = FFMAX(0, t->resume_fade - frames);
}

static void render_outgoing(AudioPlayer *ap, const int frames) {
    Mixer *m = &ap->mixer;
    TrackState *t = m->outgoing;
    if (!t) return;

    bool eos = false, looped = false;
    const int64_t left = m->out_fade.len - m->out_fade.pos;
    const int want = (int)FFMIN((int64_t)frames, FFMAX(0, left));
    const int got = want > 0 ? track_pull(t, m->voice_buf, want, m->rate, m->channels, &eos, &looped) : 0;
    add_voice(m, t, m->mix_buf, m->voice_buf, got, m->channels, &m->out_fade, true);
    m->out_fade.pos += got;

    if (eos || m->out_fade.pos >= m->out_fade.len) {
        m->out_fade.active = false;
        m->outgoing = NULL;
        push_event(m, MIX_EV_FADE_DONE, t, t->position);
    }
}

// Returns the number of frames the current track(s) produced
static int render_current(AudioPlayer *ap, const int frames) {
    Mixer *m = &ap->mixer;
    int pos = 0;

    while (pos < frames && m->current) {
        TrackState *t = m->current;

        bool eos = false, looped = false;
        const int want = frames - pos;
        const int got = track_pull(t, m->voice_buf, want, m->rate, m->channels, &eos, &looped);

        if (got > 0 && !t->announced) {
            t->announced = true;
            push_event(m, MIX_EV_TRACK_STARTED, t, t->position);
        }
        if (looped)
            push_event(m, MIX_EV_TRACK_LOOPED, t, t->position);

        add_voice(m, t, m->mix_buf + (size_t)pos * m->channels, m->voice_buf, got, m->channels, &m->in_fade, false);
        if (m->in_fade.active) {
            m->in_fade.pos += got;
            if (m->in_fade.pos >= m->in_fade.len)
                m->in_fade.active = false;
        }
        pos += got;

        if (eos) {
            // Gapless handover: the queued track continues in the same buffer
            m->current = NULL;
            m->in_fade.active = false;
            push_event(m, MIX_EV_TRACK_ENDED, t, t->position);
            if (m->next) {
                m->current = m->next;
                m->next = NULL;
            }
            continue;
        }
        if (got < want)
            break; // underrun: the decoder has not caught up, play silence for the rest
    }
    return pos;
}

static inline float dither_noise(uint32_t *state) {
    // Triangular (TPDF) noise in [-1, 1) LSB from two uniform values
    uint32_t x = *state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    const float a = (float)(x >> 8) * (1.0f / 16777216.0f);
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    const float b = (float)(x >> 8) * (1.0f / 16777216.0f);
    *state = x;
    return a - b;
}

static inline float clamp1(const float v) {
    return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
}

// Write `n` samples of the mix times a gain going from g by dg per frame, clamped, in the device format
static void write_block(Mixer *m, Uint8 *out, const float *src, const int frames, float g, const float dg,
                        uint32_t *dither) {
    const int ch = m->channels;
    switch (m->format) {
        case AUDIO_F32SYS: {
            float *d = (float *)out;
            for (int i = 0; i < frames; i++, g += dg)
                for (int c = 0; c < ch; c++, src++, d++) *d = clamp1(*src * g);
            break;
        }
        case AUDIO_S16SYS: {
            int16_t *d = (int16_t *)out;
            for (int i = 0; i < frames; i++, g += dg)
                for (int c = 0; c < ch; c++, src++, d++) {
                    const long r = lrintf(clamp1(*src * g) * 32767.0f + dither_noise(dither));
                    *d = (int16_t)(r > 32767 ? 32767 : (r < -32768 ? -32768 : r));
                }
            break;
        }
        case AUDIO_S32SYS: {
            int32_t *d = (int32_t *)out;
            for (int i = 0; i < frames; i++, g += dg)
                for (int c = 0; c < ch; c++, src++, d++) {
                    const double v = (double)clamp1(*src * g) * 2147483647.0;
                    *d = v >= 2147483647.0 ? INT32_MAX : (v <= -2147483648.0 ? INT32_MIN : (int32_t)llrint(v));
                }
            break;
        }
        case AUDIO_U8: {
            uint8_t *d = out;
            for (int i = 0; i < frames; i++, g += dg)
                for (int c = 0; c < ch; c++, src++, d++) {
                    const long r = lrintf(clamp1(*src * g) * 127.0f + dither_noise(dither)) + 128;
                    *d = (uint8_t)(r > 255 ? 255 : (r < 0 ? 0 : r));
                }
            break;
        }
        default:
            memset(out, 0, (size_t)frames * ch * m->bytes_per_sample);
            break;
    }
}

// Pause fade gain `k` frames after its current position
static inline float pause_gain_at(const Mixer *m, const int64_t k) {
    if (!m->pause_fade.active) return m->pause_gain;
    if (m->pause_fade.pos + k >= m->pause_fade.len) return m->pause_to;
    // Down (pausing), the curve runs backwards like a crossfade's fade-out: curve(1 - t), not 1 - curve(t)
    const float t = (float)(m->pause_fade.pos + k) / (float)m->pause_fade.len;
    const float from = m->pause_from, to = m->pause_to;
    return to < from ? to + (from - to) * curve_at(&m->pause_fade.curve, 1.0f - t)
                     : from + (to - from) * curve_at(&m->pause_fade.curve, t);
}

// Volume (with its 10 ms ramp), the pause fade, clamping and the device format, in one pass over the mix
static void output_pass(AudioPlayer *ap, Uint8 *out, const int frames) {
    Mixer *m = &ap->mixer;
    const float target = SDL_AtomicGet(&ap->muted) ? 0.0f : atomic_get_float(&ap->volume);
    const float step = 1.0f / (float)FFMAX(1, m->vol_ramp_frames);
    const int frame_bytes = m->channels * m->bytes_per_sample;
    uint32_t dither = m->dither_state;

    for (int b = 0; b < frames;) {
        int n = FFMIN(GAIN_BLOCK, frames - b);
        // Blocks end where a ramp or the pause fade does, so both are exact at every block edge
        const float v0 = m->vol_gain;
        if (v0 != target) n = FFMIN(n, FFMAX(1, (int)ceilf(fabsf(target - v0) / step)));
        if (m->pause_fade.active) n = (int)FFMIN((int64_t)n, FFMAX(1, m->pause_fade.len - m->pause_fade.pos));
        const float vstep = v0 < target ? step : -step;
        const float g0 = ramp_at(v0, v0 == target ? 0.0f : vstep, target, 1) * pause_gain_at(m, 0);
        const float g1 = ramp_at(v0, v0 == target ? 0.0f : vstep, target, n + 1) * pause_gain_at(m, n);
        write_block(m, out + (size_t)b * frame_bytes, m->mix_buf + (size_t)b * m->channels, n, g0,
                    (g1 - g0) / (float)n, &dither);

        m->vol_gain = ramp_at(v0, v0 == target ? 0.0f : vstep, target, n);
        if (m->pause_fade.active) {
            m->pause_fade.pos += n;
            if (m->pause_fade.pos >= m->pause_fade.len) {
                m->pause_fade.active = false;
                m->pause_gain = m->pause_to;
            }
            else {
                m->pause_gain = pause_gain_at(m, 0);
            }
        }
        b += n;
    }
    m->dither_state = dither;

    if (!m->pause_fade.active && m->pause_to == 0.0f && SDL_AtomicGet(&ap->paused))
        m->frozen = true;
}

static void publish_status(AudioPlayer *ap, const bool advancing, const int frames) {
    Mixer *m = &ap->mixer;
    TrackState *t = m->current;
    StatusSnapshot s;
    s.handle = t ? t->handle : 0;
    s.position = t ? t->position : -1;
    s.duration = t ? track_duration(t) : -1;
    s.callback_time = (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency();
    s.advancing = advancing;
    s.speed = t ? t->play_speed : 1.0;
    s.period = m->rate > 0 ? (double)frames / m->rate : 0;
    s.buffering = t && SDL_AtomicGet(&t->buffering) != 0;
    // Never waits: while a getter holds the lock (a copy of a few bytes), this period's status is skipped and the
    // next one, a period later, is published instead
    if (SDL_TryLockMutex(ap->status_mutex) == 0) {
        ap->status = s;
        SDL_UnlockMutex(ap->status_mutex);
    }
}

static void sdl_audio_callback(void *opaque, Uint8 *stream, int len) {
    AudioPlayer *ap = opaque;
    Mixer *m = &ap->mixer;
    const int frame_bytes = m->channels * m->bytes_per_sample;
    const int total = frame_bytes > 0 ? len / frame_bytes : 0;
    bool advancing = false;

    // Audio from before a seek (or for another format) goes right away, also while paused, so no decoder waits
    if (m->current) track_drop_stale(m->current, m->rate, m->channels);
    if (m->next) track_drop_stale(m->next, m->rate, m->channels);
    if (m->outgoing) track_drop_stale(m->outgoing, m->rate, m->channels);

    int done = 0;
    while (done < total) {
        int n = FFMIN(total - done, m->buf_frames);
        // Split the chunk so a crossfade at a track end starts on the exact sample
        if (!m->frozen)
            n = FFMIN(n, frames_until_end_crossfade(m));
        memset(m->mix_buf, 0, (size_t)n * m->channels * sizeof(float));

        if (!m->frozen) {
            // Start an end-of-track crossfade before rendering, so the outgoing track plays this chunk too
            maybe_start_end_crossfade(ap);
            render_outgoing(ap, n);
            if (render_current(ap, n) > 0)
                advancing = true;
            output_pass(ap, stream + (size_t)done * frame_bytes, n);
        }
        else {
            // Paused and faded out: silence, without dither noise
            memset(stream + (size_t)done * frame_bytes, m->format == AUDIO_U8 ? 0x80 : 0, (size_t)n * frame_bytes);
        }
        done += n;
    }
    if (done * frame_bytes < len)
        memset(stream + (size_t)done * frame_bytes, m->format == AUDIO_U8 ? 0x80 : 0, (size_t)(len - done * frame_bytes));

    publish_status(ap, advancing && !m->frozen, total);
    // Events are rare (a few per track): only then is the control thread woken, with a semaphore post that never
    // blocks; otherwise it comes by itself every CONTROL_PLAYING_MS
    if (m->wake_control) {
        m->wake_control = false;
        SDL_SemPost(ap->wake_sem);
    }
}

//+-----------------------------------------------------+
//| Device                                              |
//+-----------------------------------------------------+

// SDL2's channel order for each channel count (see SDL_audio.h), as an FFmpeg layout.
// FFmpeg's native order sorts channels by their bit, which gives the same order in every case.
static AVChannelLayout sdl_channel_layout(const int channels) {
    switch (channels) {
        case 1: return (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;
        case 2: return (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
        case 3: return (AVChannelLayout)AV_CHANNEL_LAYOUT_2POINT1;                    // FL FR LFE
        case 4: return (AVChannelLayout)AV_CHANNEL_LAYOUT_QUAD;                       // FL FR BL BR
        case 5: return (AVChannelLayout)AV_CHANNEL_LAYOUT_MASK(5,                     // FL FR LFE BL BR (4.1)
                    AV_CH_FRONT_LEFT | AV_CH_FRONT_RIGHT | AV_CH_LOW_FREQUENCY | AV_CH_BACK_LEFT | AV_CH_BACK_RIGHT);
        case 6: return (AVChannelLayout)AV_CHANNEL_LAYOUT_5POINT1;                    // FL FR FC LFE SL SR
        case 7: return (AVChannelLayout)AV_CHANNEL_LAYOUT_6POINT1;
        case 8: return (AVChannelLayout)AV_CHANNEL_LAYOUT_7POINT1;
        default: return (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
    }
}

static bool format_supported(const SDL_AudioFormat f) {
    return f == AUDIO_F32SYS || f == AUDIO_S16SYS || f == AUDIO_S32SYS || f == AUDIO_U8;
}

void output_set_running(AudioPlayer *ap, const bool running) {
    if (!ap->device_id || ap->device_running == running) return;
    ap->device_running = running;
    SDL_PauseAudioDevice(ap->device_id, running ? 0 : 1);
}

void output_close(AudioPlayer *ap) {
    if (ap->device_id) {
        SDL_AtomicSet(&ap->open_device, 0);
        SDL_CloseAudioDevice(ap->device_id); // waits for a running callback
        ap->device_id = 0;
    }
    ap->device_running = false;

    SDL_LockMutex(ap->format_mutex);
    ap->output_ready = false;
    av_freep(&ap->device_in_use);
    SDL_UnlockMutex(ap->format_mutex);

    av_freep(&ap->mixer.mix_buf);
    av_freep(&ap->mixer.voice_buf);
    ap->mixer.buf_frames = 0;
}

int output_open(AudioPlayer *ap, const bool use_default, const char *device_name, const int device_index) {
    SDL_AudioSpec preferred = {0}, wanted = {0}, obtained = {0};
    char *in_use = NULL;

    if (use_default) {
        char *name = NULL;
        if (SDL_GetDefaultAudioInfo(&name, &preferred, 0) != 0) {
            preferred.freq = 48000;
            preferred.channels = 2;
        }
        in_use = av_strdup(name && name[0] ? name : "System default");
        SDL_free(name);
    }
    else {
        in_use = av_strdup(device_name ? device_name : "");
    }
    if (!in_use) return AU_ERR_NO_MEMORY;
    if (!use_default) {
        // Prefer the name: indexes change when devices come and go
        int index = -1;
        const int count = SDL_GetNumAudioDevices(0);
        for (int i = 0; i < count && device_name; i++) {
            const char *n = SDL_GetAudioDeviceName(i, 0);
            if (n && strcmp(n, device_name) == 0) { index = i; break; }
        }
        if (index < 0) index = device_index;
        if (index < 0 || SDL_GetAudioDeviceSpec(index, 0, &preferred) != 0) {
            ff_log(AU_LOG_ERROR, "Failed to get the preferred spec of audio device '%s'", device_name ? device_name : "?");
            av_free(in_use);
            return AU_ERR_DEVICE;
        }
    }

    // A forced channel count / rate (AuConfig) wins over the device's preference; SDL converts
    const bool force_channels = ap->output_channels > 0;
    const bool force_rate = ap->output_sample_rate > 0;
    wanted.freq = force_rate ? ap->output_sample_rate : (preferred.freq > 0 ? preferred.freq : 48000);
    wanted.channels = (Uint8)(force_channels ? ap->output_channels
                                             : ((preferred.channels > 0 && preferred.channels <= 8) ? preferred.channels : 2));
    wanted.format = AUDIO_F32SYS;
    wanted.samples = (Uint16)FFMAX(512, 2 << av_log2((unsigned)(wanted.freq / 40)));
    wanted.callback = sdl_audio_callback;
    wanted.userdata = ap;

    int allowed = SDL_AUDIO_ALLOW_FORMAT_CHANGE | SDL_AUDIO_ALLOW_SAMPLES_CHANGE;
    if (!force_rate) allowed |= SDL_AUDIO_ALLOW_FREQUENCY_CHANGE;
    if (!force_channels) allowed |= SDL_AUDIO_ALLOW_CHANNELS_CHANGE;
    const char *open_name = use_default ? NULL : device_name;

    SDL_AudioDeviceID id = SDL_OpenAudioDevice(open_name, 0, &wanted, &obtained, allowed);
    if (id && (!format_supported(obtained.format) || obtained.channels < 1 || obtained.channels > 8)) {
        // Let SDL convert from float instead
        SDL_CloseAudioDevice(id);
        allowed = SDL_AUDIO_ALLOW_SAMPLES_CHANGE;
        if (!force_rate) allowed |= SDL_AUDIO_ALLOW_FREQUENCY_CHANGE;
        if (!force_channels && obtained.channels >= 1 && obtained.channels <= 8)
            allowed |= SDL_AUDIO_ALLOW_CHANNELS_CHANGE;
        id = SDL_OpenAudioDevice(open_name, 0, &wanted, &obtained, allowed);
    }
    if (!id) {
        ff_log(AU_LOG_ERROR, "SDL_OpenAudioDevice (%d channels, %d Hz): %s", wanted.channels, wanted.freq, SDL_GetError());
        av_free(in_use);
        return AU_ERR_DEVICE;
    }

    Mixer *m = &ap->mixer;
    m->rate = obtained.freq;
    m->channels = obtained.channels;
    m->format = obtained.format;
    m->bytes_per_sample = SDL_AUDIO_BITSIZE(obtained.format) / 8;
    m->vol_ramp_frames = FFMAX(1, (int)(VOLUME_RAMP_SECONDS * obtained.freq));
    m->buf_frames = FFMAX(1024, obtained.samples);
    m->mix_buf = av_malloc_array((size_t)m->buf_frames * m->channels, sizeof(float));
    m->voice_buf = av_malloc_array((size_t)m->buf_frames * m->channels, sizeof(float));
    if (!m->dither_state) m->dither_state = 0x9E3779B9u;
    if (!m->mix_buf || !m->voice_buf) {
        SDL_CloseAudioDevice(id);
        av_freep(&m->mix_buf);
        av_freep(&m->voice_buf);
        av_free(in_use);
        return AU_ERR_NO_MEMORY;
    }

    ap->device_id = id;
    SDL_AtomicSet(&ap->open_device, (int)id);
    ap->device_running = false;

    SDL_LockMutex(ap->format_mutex);
    ap->audio_target.freq = obtained.freq;
    av_channel_layout_uninit(&ap->audio_target.ch_layout);
    ap->audio_target.ch_layout = sdl_channel_layout(obtained.channels);
    ap->audio_target.fmt = AV_SAMPLE_FMT_FLT;
    ap->output_ready = true;
    av_free(ap->device_in_use);
    ap->device_in_use = in_use;
    in_use = NULL;
    SDL_CondBroadcast(ap->format_cond);
    SDL_UnlockMutex(ap->format_mutex);

    ff_log(AU_LOG_INFO, "Opened audio device '%s': %d Hz, %d channels, format 0x%x, %d frames per buffer",
           use_default ? "system default" : device_name, obtained.freq, obtained.channels, obtained.format, obtained.samples);
    return AU_OK;
}
