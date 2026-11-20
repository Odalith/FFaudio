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
 * The SDL callback runs with the device lock held. It never blocks, allocates or frees: it pulls
 * float frames from the tracks in the mixer slots, applies fades and gain, and converts to the
 * device format (with TPDF dither for integer formats). Slot changes it makes itself (gapless
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

float fade_curve_gain(const AuFade *fade, float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
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

static float fade_in_gain(const FadeState *f, const int64_t offset) {
    if (!f->active || f->len <= 0) return 1.0f;
    return fade_curve_gain(&f->fade, (float)(f->pos + offset) / (float)f->len);
}

static float fade_out_gain(const FadeState *f, const int64_t offset) {
    if (!f->active || f->len <= 0) return 0.0f;
    return fade_curve_gain(&f->fade, 1.0f - (float)(f->pos + offset) / (float)f->len);
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
}

//+-----------------------------------------------------+
//| Transitions                                         |
//+-----------------------------------------------------+

bool mixer_start_transition(AudioPlayer *ap, TrackState *outgoing, TrackState *incoming, const AuFade *fade) {
    Mixer *m = &ap->mixer;
    const int64_t len = (fade && fade->seconds > 0 && m->rate > 0) ? (int64_t)(fade->seconds * m->rate) : 0;
    if (len <= 0) return false; // shorter than a frame, or no device yet: the caller cuts

    if (outgoing) {
        if (m->outgoing && m->outgoing != outgoing) {
            // Only one track can fade out at a time: the older one is cut
            m->outgoing->role = ROLE_NONE;
            push_event(m, MIX_EV_FADE_DONE, m->outgoing, m->outgoing->position);
        }
        // If it was still fading in, continue from its current loudness instead of jumping to full
        int64_t start = 0;
        if (outgoing == m->current && m->in_fade.active && m->in_fade.len > 0) {
            const float p = (float)m->in_fade.pos / (float)m->in_fade.len;
            start = (int64_t)((1.0f - p) * (float)len);
        }
        m->outgoing = outgoing;
        outgoing->role = ROLE_OUTGOING;
        m->out_fade.active = true;
        m->out_fade.pos = start;
        m->out_fade.len = len;
        m->out_fade.fade = *fade;
    }

    if (outgoing == m->current) m->current = NULL;
    if (incoming == m->next) m->next = NULL;

    m->current = incoming;
    m->in_fade.active = false;
    if (incoming) {
        incoming->role = ROLE_CURRENT;
        m->in_fade.active = true;
        m->in_fade.pos = 0;
        m->in_fade.len = len;
        m->in_fade.fade = *fade;
    }
    return true;
}

void mixer_set_pause(AudioPlayer *ap, const bool paused, const AuFade *fade) {
    Mixer *m = &ap->mixer;
    const int64_t len = (fade && fade->seconds > 0 && m->rate > 0) ? (int64_t)(fade->seconds * m->rate) : 0;
    m->pause_from = m->pause_gain;
    m->pause_to = paused ? 0.0f : 1.0f;
    if (!paused) m->frozen = false;
    if (len > 0) {
        m->pause_fade.active = true;
        m->pause_fade.pos = 0;
        m->pause_fade.len = len;
        m->pause_fade.fade = *fade;
    }
    else {
        // A very short ramp avoids a click
        m->pause_fade.active = true;
        m->pause_fade.pos = 0;
        m->pause_fade.len = FFMAX(1, m->vol_ramp_frames);
        m->pause_fade.fade = (AuFade){ .seconds = VOLUME_RAMP_SECONDS, .curve = AU_FADE_LINEAR };
    }
}

// Crossfade into the queued track when the current one is about to end
// Length (seconds) of the crossfade the current track should get at its end; 0 = play gapless.
// *remaining is set to the seconds left in the current track.
static double planned_end_fade(Mixer *m, const AuFade **fade_out, double *remaining) {
    TrackState *cur = m->current, *next = m->next;
    if (!cur || !next || m->outgoing) return 0;

    const AuFade *fade = next->has_fade_in_override ? &next->fade_in_override : &m->end_fade;
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

    *fade_out = fade;
    *remaining = (end - cur->position) / speed;
    return fade_s;
}

// Frames that can be rendered before the end crossfade has to start (INT_MAX = no crossfade planned)
static int frames_until_end_crossfade(Mixer *m) {
    const AuFade *fade = NULL;
    double remaining = 0;
    const double fade_s = planned_end_fade(m, &fade, &remaining);
    if (fade_s <= 0 || remaining <= fade_s) return INT_MAX;
    const double frames = ceil((remaining - fade_s) * m->rate);
    return frames >= INT_MAX ? INT_MAX : FFMAX(1, (int)frames);
}

static void maybe_start_end_crossfade(AudioPlayer *ap) {
    Mixer *m = &ap->mixer;
    const AuFade *fade = NULL;
    double remaining = 0;
    const double fade_s = planned_end_fade(m, &fade, &remaining);
    if (fade_s <= 0 || remaining > fade_s + 0.5 / m->rate || remaining <= 0.005) return;
    if (!track_has_audio(m->next)) return; // not decoded yet: try again next chunk (or end gapless)

    AuFade f = *fade;
    f.seconds = remaining; // end exactly when the current track does
    TrackState *cur = m->current;
    if (mixer_start_transition(ap, cur, m->next, &f))
        cur->fade_reason = END_NATURAL;
}

//+-----------------------------------------------------+
//| Mixing                                              |
//+-----------------------------------------------------+

#define LATE_GAIN_RAMP_SECONDS 2.0

// The track's ReplayGain gain: taken as it is with the first samples, then a linear ramp for every change (10 ms, or
// 2 s when the value arrived after the track started, e.g. a measurement that missed measure_wait_ms)
static float track_gain_step(Mixer *m, TrackState *t, float *target_out) {
    const int packed = SDL_AtomicGet(&t->gain_mb);
    const float target = powf(10.0f, (float)gain_centi_db(packed) / 2000.0f);
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

static void add_voice(Mixer *m, TrackState *t, float *dst, const float *src, const int frames, const int channels,
                      const FadeState *fade, const bool fading_out) {
    // A track that has produced nothing yet (its file may not even be open) must not fix its starting gain:
    // that is read with its first samples, when the read thread has set it
    if (frames <= 0) return;
    const bool constant = !fade->active;
    float target;
    const float step = track_gain_step(m, t, &target);
    float gain = t->gain_cur;
    for (int i = 0; i < frames; i++) {
        if (step != 0.0f) {
            gain += step;
            if ((step > 0 && gain > target) || (step < 0 && gain < target)) gain = target;
        }
        float g = gain * (constant ? (fading_out ? 0.0f : 1.0f)
                                   : (fading_out ? fade_out_gain(fade, i) : fade_in_gain(fade, i)));
        if (t->resume_fade > 0) { // back after a network stall
            g *= 1.0f - (float)t->resume_fade / (float)t->resume_fade_len;
            t->resume_fade--;
        }
        const float *s = src + (size_t)i * channels;
        float *d = dst + (size_t)i * channels;
        for (int c = 0; c < channels; c++)
            d[c] += s[c] * g;
    }
    t->gain_cur = gain;
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
        t->role = ROLE_NONE;
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
            t->role = ROLE_NONE;
            m->current = NULL;
            m->in_fade.active = false;
            push_event(m, MIX_EV_TRACK_ENDED, t, t->position);
            if (m->next) {
                m->current = m->next;
                m->current->role = ROLE_CURRENT;
                m->next = NULL;
            }
            continue;
        }
        if (got < want)
            break; // underrun: the decoder has not caught up, play silence for the rest
    }
    return pos;
}

static void apply_master_gain(AudioPlayer *ap, const int frames) {
    Mixer *m = &ap->mixer;
    const float target = SDL_AtomicGet(&ap->muted) ? 0.0f : (float)SDL_AtomicGet(&ap->volume) / 100.0f;
    const float step = 1.0f / (float)FFMAX(1, m->vol_ramp_frames);
    const int ch = m->channels;

    for (int i = 0; i < frames; i++) {
        if (m->vol_gain < target) m->vol_gain = FFMIN(target, m->vol_gain + step);
        else if (m->vol_gain > target) m->vol_gain = FFMAX(target, m->vol_gain - step);

        if (m->pause_fade.active) {
            // Down (pausing), the curve runs backwards like a crossfade's fade-out: curve(1 - t), not 1 - curve(t)
            const float t = (float)m->pause_fade.pos / (float)m->pause_fade.len;
            const float from = m->pause_from, to = m->pause_to;
            m->pause_gain = to < from ? to + (from - to) * fade_curve_gain(&m->pause_fade.fade, 1.0f - t)
                                      : from + (to - from) * fade_curve_gain(&m->pause_fade.fade, t);
            if (++m->pause_fade.pos >= m->pause_fade.len) {
                m->pause_fade.active = false;
                m->pause_gain = m->pause_to;
            }
        }

        const float g = m->vol_gain * m->pause_gain;
        float *s = m->mix_buf + (size_t)i * ch;
        for (int c = 0; c < ch; c++) {
            float v = s[c] * g;
            s[c] = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
        }
    }

    if (!m->pause_fade.active && m->pause_to == 0.0f && SDL_AtomicGet(&ap->paused))
        m->frozen = true;
}

static inline float dither_noise(Mixer *m) {
    // Triangular (TPDF) noise in [-1, 1) LSB from two uniform values
    uint32_t x = m->dither_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    const float a = (float)(x >> 8) * (1.0f / 16777216.0f);
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    const float b = (float)(x >> 8) * (1.0f / 16777216.0f);
    m->dither_state = x;
    return a - b;
}

static void convert_output(Mixer *m, Uint8 *out, const int frames) {
    const int n = frames * m->channels;
    const float *src = m->mix_buf;
    switch (m->format) {
        case AUDIO_F32SYS:
            memcpy(out, src, (size_t)n * sizeof(float));
            break;
        case AUDIO_S16SYS: {
            int16_t *d = (int16_t *)out;
            for (int i = 0; i < n; i++) {
                const float v = src[i] * 32767.0f + dither_noise(m);
                const long r = lrintf(v);
                d[i] = (int16_t)(r > 32767 ? 32767 : (r < -32768 ? -32768 : r));
            }
            break;
        }
        case AUDIO_S32SYS: {
            int32_t *d = (int32_t *)out;
            for (int i = 0; i < n; i++) {
                const double v = (double)src[i] * 2147483647.0;
                d[i] = (int32_t)(v >= 2147483647.0 ? 2147483647 : (v <= -2147483648.0 ? INT32_MIN : (int32_t)llrint(v)));
            }
            break;
        }
        case AUDIO_U8: {
            uint8_t *d = out;
            for (int i = 0; i < n; i++) {
                const long r = lrintf(src[i] * 127.0f + dither_noise(m)) + 128;
                d[i] = (uint8_t)(r > 255 ? 255 : (r < 0 ? 0 : r));
            }
            break;
        }
        default:
            memset(out, 0, (size_t)n * m->bytes_per_sample);
            break;
    }
}

static void publish_status(AudioPlayer *ap, const bool advancing) {
    Mixer *m = &ap->mixer;
    TrackState *t = m->current;
    StatusSnapshot s;
    s.handle = t ? t->handle : 0;
    s.position = t ? t->position : -1;
    s.duration = t ? track_duration(t) : -1;
    s.callback_time = (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency();
    s.advancing = advancing;
    s.speed = t ? t->play_speed : 1.0;
    SDL_LockMutex(ap->status_mutex);
    ap->status = s;
    SDL_UnlockMutex(ap->status_mutex);
}

static void sdl_audio_callback(void *opaque, Uint8 *stream, int len) {
    AudioPlayer *ap = opaque;
    Mixer *m = &ap->mixer;
    const int frame_bytes = m->channels * m->bytes_per_sample;
    const int total = frame_bytes > 0 ? len / frame_bytes : 0;
    bool advancing = false;

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
            apply_master_gain(ap, n);
        }

        convert_output(m, stream + (size_t)done * frame_bytes, n);
        done += n;
    }
    if (done * frame_bytes < len)
        memset(stream + (size_t)done * frame_bytes, m->format == AUDIO_U8 ? 0x80 : 0, (size_t)(len - done * frame_bytes));

    publish_status(ap, advancing && !m->frozen);
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
    ap->is_audio_device_initialized = false;

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

    // A forced channel count / rate (InitializeConfig) wins over the device's preference; SDL converts
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
    m->period_frames = obtained.samples;
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
    ap->obtained_spec = obtained;
    ap->device_running = false;
    ap->is_audio_device_initialized = true;

    SDL_LockMutex(ap->format_mutex);
    ap->audio_target.freq = obtained.freq;
    av_channel_layout_uninit(&ap->audio_target.ch_layout);
    ap->audio_target.ch_layout = sdl_channel_layout(obtained.channels);
    ap->audio_target.fmt = AV_SAMPLE_FMT_FLT;
    ap->audio_target.frame_size = (int32_t)(obtained.channels * sizeof(float));
    ap->audio_target.bytes_per_sec = ap->audio_target.frame_size * obtained.freq;
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
