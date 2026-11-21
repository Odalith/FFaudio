/*
* Copyright (c) 2003 Fabrice Bellard, 2025 Odalith
 *
 * This file was part of FFmpeg, particularly FFplay.
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

//https://ffmpeg.org/doxygen/trunk/ffplay_8c_source.html

/*
 * Public API and the control thread.
 *
 * Every public function that changes playback posts a message to the control thread and returns.
 * The control thread is the only place where tracks are created, moved between mixer slots
 * (together with the SDL callback, both under the device lock) and destroyed, and it is the only
 * thread that calls user callbacks. Callbacks are dispatched after a message has been fully
 * handled, so calling the API from a callback is safe: it just queues another message.
 */

#include <math.h>
#include "ffaudio.h"
#include "globals.h"
#include "filtergraph.h"
#include "output.h"
#include "track.h"
#include "metadata.h"
#include "gain_cache.h"
#include "once.h"
#include "numbers.h"


// The control thread sleeps until a message or a mixer event wakes it, or for this long: while tracks are loaded
// (prepare-next, "now playing", buffering progress), and when idle (device changes)
#define CONTROL_PLAYING_MS 50
#define CONTROL_IDLE_MS 500
#define DEFAULT_PREPARE_NEXT_SECONDS 10.0

static AudioPlayer *audio_player = NULL;

//+-----------------------------------------------------+
//| Message queue                                       |
//+-----------------------------------------------------+

static Msg *msg_new(const MsgType type) {
    Msg *m = av_mallocz(sizeof(Msg));
    if (m) m->type = type;
    return m;
}

static void msg_free(Msg *m) {
    if (!m) return;
    av_free(m->str);
    av_free(m->str2);
    av_free(m->str3);
    av_free(m);
}

static void msg_post(AudioPlayer *ap, Msg *m) {
    SDL_LockMutex(ap->msg_mutex);
    m->next = NULL;
    if (ap->msg_tail) ap->msg_tail->next = m;
    else ap->msg_head = m;
    ap->msg_tail = m;
    SDL_UnlockMutex(ap->msg_mutex);
    SDL_SemPost(ap->wake_sem);
}

static bool on_control_thread(const AudioPlayer *ap) {
    return ap->control_thread_id && SDL_ThreadID() == ap->control_thread_id;
}

// Held while audio_player is cleared in au_shutdown() and while the ReplayGain cache (usable from any thread,
// at any time) posts to the player, so a cache update racing with au_shutdown() never touches a freed player.
static SDL_mutex *lifecycle_mutex_ptr = NULL;
static FfOnce lifecycle_once = FF_ONCE_INIT;

static void create_lifecycle_mutex(void) {
    lifecycle_mutex_ptr = SDL_CreateMutex();
}

static SDL_mutex *lifecycle_mutex(void) {
    ff_once(&lifecycle_once, create_lifecycle_mutex);
    return lifecycle_mutex_ptr;
}

void ff_gain_cache_changed(const char *path) {
    SDL_mutex *lock = lifecycle_mutex();
    if (!lock) return;
    SDL_LockMutex(lock);
    AudioPlayer *ap = audio_player;
    if (ap && !SDL_AtomicGet(&ap->shutting_down)) {
        Msg *m = msg_new(CMD_GAIN_CACHE_CHANGED);
        if (m && path && !(m->str = av_strdup(path))) {
            msg_free(m);
            m = NULL;
        }
        if (m) msg_post(ap, m);
    }
    SDL_UnlockMutex(lock);
}

void ff_post_track_msg(const MsgType type, const int32_t handle, const double d) {
    AudioPlayer *ap = audio_player;
    if (!ap) return;
    Msg *m = msg_new(type);
    if (!m) return;
    m->handle = handle;
    m->d = d;
    msg_post(ap, m);
}

void ff_log(const enum AU_LOG_LEVEL level, const char *fmt, ...) {
    AudioPlayer *ap = audio_player;
    if (!fmt || !ap || !ap->notify_of_log_callback) return;

    va_list args;
    va_start(args, fmt);
    char *text = NULL;
    va_list copy;
    va_copy(copy, args);
    const int needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed >= 0 && (text = av_malloc((size_t)needed + 1)))
        vsnprintf(text, (size_t)needed + 1, fmt, args);
    va_end(args);
    if (!text) return;

    Msg *m = msg_new(MSG_LOG);
    if (!m) { av_free(text); return; }
    m->str = text;
    m->level = level;
    m->request = SDL_AtomicGet(&ap->request_count);
    msg_post(ap, m);
}

//+-----------------------------------------------------+
//| Notifications (user callbacks)                      |
//+-----------------------------------------------------+

typedef enum NoteType {
    NOTE_LOG, NOTE_EOF, NOTE_RESTART, NOTE_DURATION, NOTE_PREPARE_NEXT, NOTE_TRACK_STARTED, NOTE_DEVICE,
    NOTE_DEVICES_CHANGED, NOTE_BUFFERING, NOTE_STREAM_METADATA,
} NoteType;

typedef struct Note {
    NoteType type;
    int32_t handle;
    double d;
    bool b1, b2;
    int32_t i;
    int64_t request;
    char *text, *text2;
} Note;

typedef struct NoteList {
    Note *items;
    int n, cap;
} NoteList;

static NoteList notes;  // control thread only

static void note_add(const Note n) {
    if (notes.n == notes.cap) {
        const int cap = notes.cap ? notes.cap * 2 : 32;
        Note *items = av_realloc_array(notes.items, cap, sizeof(Note));
        if (!items) { av_free(n.text); av_free(n.text2); return; }
        notes.items = items;
        notes.cap = cap;
    }
    notes.items[notes.n++] = n;
}

static void dispatch_notes(AudioPlayer *ap) {
    void *ud = ap->user_data;
    // Each note is copied out before its callback: a callback can run API calls inline that add notes and move the
    // array. Notes added meanwhile are dispatched in this same pass.
    for (int i = 0; i < notes.n; i++) {
        const Note n = notes.items[i];
        if (!SDL_AtomicGet(&ap->shutting_down)) {
            switch (n.type) {
                case NOTE_LOG:
                    if (ap->notify_of_log_callback) ap->notify_of_log_callback(n.text, n.request, (enum AU_LOG_LEVEL)n.i, ud);
                    break;
                case NOTE_EOF:
                    if (ap->notify_of_eof_callback) ap->notify_of_eof_callback(n.b1, n.b2, n.handle, ud);
                    break;
                case NOTE_RESTART:
                    if (ap->notify_of_restart_callback) ap->notify_of_restart_callback(n.handle, n.d, n.b1, n.i, ud);
                    break;
                case NOTE_DURATION:
                    if (ap->notify_of_duration_update_callback) ap->notify_of_duration_update_callback(n.handle, n.d, ud);
                    break;
                case NOTE_PREPARE_NEXT:
                    if (ap->notify_of_prepare_next_callback) ap->notify_of_prepare_next_callback(n.handle, ud);
                    break;
                case NOTE_TRACK_STARTED:
                    if (ap->notify_of_track_started_callback) ap->notify_of_track_started_callback(n.handle, ud);
                    break;
                case NOTE_STREAM_METADATA:
                    if (ap->notify_of_stream_metadata_callback)
                        ap->notify_of_stream_metadata_callback(n.handle, n.text, n.text2, ud);
                    break;
                case NOTE_BUFFERING:
                    if (ap->notify_of_buffering_callback) ap->notify_of_buffering_callback(n.handle, n.b1, n.d, ud);
                    break;
                case NOTE_DEVICES_CHANGED:
                    if (ap->notify_of_audio_devices_changed_callback) ap->notify_of_audio_devices_changed_callback(ud);
                    break;
                case NOTE_DEVICE:
                    if (ap->notify_of_audio_device_update_callback) ap->notify_of_audio_device_update_callback(n.i, (int32_t)n.d, ud);
                    break;
            }
        }
        av_free(n.text);
        av_free(n.text2);
    }
    notes.n = 0;
}

//+-----------------------------------------------------+
//| Track registry (control thread)                     |
//+-----------------------------------------------------+

static TrackState *registry_find(AudioPlayer *ap, const int32_t handle) {
    for (int i = 0; i < ap->n_tracks; i++)
        if (ap->tracks[i]->handle == handle)
            return ap->tracks[i];
    return NULL;
}

static bool registry_add(AudioPlayer *ap, TrackState *t) {
    if (ap->n_tracks == ap->cap_tracks) {
        const int cap = ap->cap_tracks ? ap->cap_tracks * 2 : 8;
        TrackState **arr = av_realloc_array(ap->tracks, cap, sizeof(TrackState *));
        if (!arr) return false;
        ap->tracks = arr;
        ap->cap_tracks = cap;
    }
    ap->tracks[ap->n_tracks++] = t;
    return true;
}

static void registry_remove(AudioPlayer *ap, const TrackState *t) {
    for (int i = 0; i < ap->n_tracks; i++) {
        if (ap->tracks[i] == t) {
            ap->tracks[i] = ap->tracks[--ap->n_tracks];
            return;
        }
    }
}

// Mark a track finished and report it. It is destroyed at the end of the control loop iteration,
// after the mixer events that may still reference it have been drained.
static void end_track(AudioPlayer *ap, TrackState *t, TrackEndReason reason) {
    if (!t || t->ended) return;
    if (reason == END_NATURAL && SDL_AtomicGet(&t->had_error))
        reason = END_ERROR;
    t->ended = true;
    note_add((Note){ .type = NOTE_EOF, .handle = t->handle, .b1 = reason == END_SKIPPED, .b2 = reason == END_ERROR });
    (void)ap;
}

static void lock_mixer(AudioPlayer *ap);
static void unlock_mixer(AudioPlayer *ap);

// Ended tracks are stopped here and freed once their worker has returned (MSG_TRACK_EXITED wakes the loop): the control
// thread never waits for a worker
static void destroy_ended_tracks(AudioPlayer *ap) {
    for (int i = ap->n_tracks - 1; i >= 0; i--) {
        TrackState *t = ap->tracks[i];
        if (!t->ended) continue;
        lock_mixer(ap); // the SDL callback moves tracks out of its slots
        const bool in_mixer = mixer_has_track(&ap->mixer, t);
        unlock_mixer(ap);
        if (in_mixer) continue;
        if (ap->n_dying == ap->cap_dying) {
            const int cap = ap->cap_dying ? ap->cap_dying * 2 : 8;
            TrackState **arr = av_realloc_array(ap->dying, cap, sizeof(TrackState *));
            if (!arr) { // out of memory: wait for this one now
                registry_remove(ap, t);
                track_destroy(t);
                continue;
            }
            ap->dying = arr;
            ap->cap_dying = cap;
        }
        registry_remove(ap, t);
        track_stop(t);
        ap->dying[ap->n_dying++] = t;
    }
    for (int i = ap->n_dying - 1; i >= 0; i--) {
        TrackState *t = ap->dying[i];
        if (!track_stopped(t)) continue;
        track_destroy(t); // its worker has returned: no wait
        ap->dying[i] = ap->dying[--ap->n_dying];
    }
}

//+-----------------------------------------------------+
//| Mixer slot helpers (control thread)                 |
//+-----------------------------------------------------+

static void lock_mixer(AudioPlayer *ap) { if (ap->device_id) SDL_LockAudioDevice(ap->device_id); }
static void unlock_mixer(AudioPlayer *ap) { if (ap->device_id) SDL_UnlockAudioDevice(ap->device_id); }

static void remove_from_mixer(AudioPlayer *ap, TrackState *t) {
    Mixer *m = &ap->mixer;
    if (!t) return;
    if (m->current == t) { m->current = NULL; m->in_fade.active = false; }
    if (m->next == t) m->next = NULL;
    if (m->outgoing == t) { m->outgoing = NULL; m->out_fade.active = false; }
}

static void set_next_slot(AudioPlayer *ap, TrackState *t) {
    ap->mixer.next = t;
}

static void set_current_slot(AudioPlayer *ap, TrackState *t) {
    ap->mixer.current = t;
    ap->mixer.in_fade.active = false;
    if (t && ap->mixer.next == t) ap->mixer.next = NULL;
}

// Seconds and sampled curve of the fade into `incoming`: its own (PlayAudioConfig.fade_in) or the configured one
static double fade_for(const TrackState *incoming, const AuFade *fallback, const FadeCurve *fallback_curve,
                       const FadeCurve **curve) {
    if (incoming && incoming->has_fade_in_override) {
        *curve = &incoming->fade_in_curve;
        return incoming->fade_in_override.seconds;
    }
    *curve = fallback_curve;
    return fallback->seconds;
}

static void publish_idle_status(AudioPlayer *ap) {
    SDL_LockMutex(ap->status_mutex);
    ap->status = (StatusSnapshot){ .handle = 0, .position = -1, .duration = -1 };
    SDL_UnlockMutex(ap->status_mutex);
}

static void update_device_running(AudioPlayer *ap) {
    const Mixer *m = &ap->mixer;
    const bool want = m->current || m->next || m->outgoing;
    output_set_running(ap, want);
    if (!m->current && !m->outgoing)
        publish_idle_status(ap);
}

//+-----------------------------------------------------+
//| Commands                                            |
//+-----------------------------------------------------+

static TrackState *create_track(AudioPlayer *ap, const Msg *msg) {
    TrackState *t = track_create(ap, msg->handle, msg->str, msg->has_play_config ? &msg->play_config : NULL);
    if (!t || !registry_add(ap, t)) {
        if (t) track_destroy(t);
        ff_log(AU_LOG_FATAL, "Failed to create a track for '%s'", msg->str);
        note_add((Note){ .type = NOTE_EOF, .handle = msg->handle, .b1 = false, .b2 = true });
        return NULL;
    }
    return t;
}

static void cmd_pause(AudioPlayer *ap, bool value);

static void cmd_play(AudioPlayer *ap, const Msg *msg) {
    TrackState *t = create_track(ap, msg);
    if (!t) return;

    // Starting a track resumes a paused player
    if (SDL_AtomicGet(&ap->paused))
        cmd_pause(ap, false);

    lock_mixer(ap);
    Mixer *m = &ap->mixer;
    TrackState *old = m->current;
    const FadeCurve *curve;
    const double seconds = fade_for(t, &ap->crossfade.on_play, &ap->play_curve, &curve);
    const bool faded = old && !SDL_AtomicGet(&ap->paused) && mixer_start_transition(ap, old, t, seconds, curve);
    if (faded) old->fade_reason = END_SKIPPED;
    else {
        if (old) remove_from_mixer(ap, old);
        set_current_slot(ap, t);
    }
    unlock_mixer(ap);

    if (old && !faded) end_track(ap, old, END_SKIPPED);
    update_device_running(ap);
}

static void cmd_set_next(AudioPlayer *ap, const Msg *msg) {
    TrackState *t = create_track(ap, msg);
    if (!t) return;

    lock_mixer(ap);
    Mixer *m = &ap->mixer;
    TrackState *old_next = m->next;
    if (old_next) remove_from_mixer(ap, old_next);
    const bool start_now = !m->current;
    if (start_now) set_current_slot(ap, t); // nothing playing: start right away
    else set_next_slot(ap, t);
    unlock_mixer(ap);

    if (start_now && SDL_AtomicGet(&ap->paused))
        cmd_pause(ap, false);

    if (old_next) end_track(ap, old_next, END_SKIPPED);
    update_device_running(ap);
}

static void cmd_skip(AudioPlayer *ap) {
    lock_mixer(ap);
    Mixer *m = &ap->mixer;
    TrackState *cur = m->current, *next = m->next;
    const FadeCurve *curve;
    const double seconds = fade_for(next, &ap->crossfade.on_skip, &ap->skip_curve, &curve);
    const bool faded = cur && !SDL_AtomicGet(&ap->paused) && mixer_start_transition(ap, cur, next, seconds, curve);
    if (faded) cur->fade_reason = END_SKIPPED;
    else {
        if (cur) remove_from_mixer(ap, cur);
        if (next) set_current_slot(ap, next);
    }
    unlock_mixer(ap);

    if (cur && !faded) end_track(ap, cur, END_SKIPPED);
    if (!cur && !next) ff_log(AU_LOG_WARNING, "No track to skip");
    update_device_running(ap);
}

static void cmd_clear_next(AudioPlayer *ap) {
    lock_mixer(ap);
    TrackState *next = ap->mixer.next;
    if (next) {
        remove_from_mixer(ap, next);
        // Nothing is queued any more: the current track asks again near its end (only when something was removed,
        // so a callback that clears an empty queue does not get asked again and again)
        if (ap->mixer.current) ap->mixer.current->prepare_next_sent = false;
    }
    unlock_mixer(ap);
    if (next) end_track(ap, next, END_SKIPPED);
    update_device_running(ap);
}

static void cmd_stop(AudioPlayer *ap) {
    TrackState *removed[3];
    int n_removed = 0;

    lock_mixer(ap);
    Mixer *m = &ap->mixer;
    TrackState *cur = m->current, *next = m->next;
    if (next) {
        remove_from_mixer(ap, next);
        removed[n_removed++] = next;
    }
    if (cur && !SDL_AtomicGet(&ap->paused) &&
        mixer_start_transition(ap, cur, NULL, ap->crossfade.on_stop.seconds, &ap->stop_curve)) {
        // Fading out; a track that was already fading out is cut and reported through a mixer event
        cur->fade_reason = END_SKIPPED;
    }
    else {
        if (cur) {
            remove_from_mixer(ap, cur);
            removed[n_removed++] = cur;
        }
        if (m->outgoing) {
            removed[n_removed++] = m->outgoing;
            remove_from_mixer(ap, m->outgoing);
        }
    }
    unlock_mixer(ap);

    for (int i = 0; i < n_removed; i++)
        end_track(ap, removed[i], removed[i]->fade_reason ? removed[i]->fade_reason : END_SKIPPED);
    update_device_running(ap);
}

static void cmd_pause(AudioPlayer *ap, const bool value) {
    if ((SDL_AtomicGet(&ap->paused) != 0) == value) return;
    SDL_AtomicSet(&ap->paused, value);
    lock_mixer(ap);
    mixer_set_pause(ap, value, ap->crossfade.on_pause.seconds, &ap->pause_curve);
    unlock_mixer(ap);
    update_device_running(ap);
}

static void cancel_outgoing(AudioPlayer *ap) {
    lock_mixer(ap);
    TrackState *o = ap->mixer.outgoing;
    if (o) remove_from_mixer(ap, o);
    unlock_mixer(ap);
    if (o) end_track(ap, o, o->fade_reason ? o->fade_reason : END_SKIPPED);
}

static void cmd_seek(AudioPlayer *ap, const Msg *msg) {
    // A seek always cuts: a running crossfade is finished immediately
    cancel_outgoing(ap);
    lock_mixer(ap);
    TrackState *cur = ap->mixer.current;
    ap->mixer.in_fade.active = false;
    unlock_mixer(ap);
    if (!cur) return;
    if (msg->type == CMD_SEEK_US) track_request_seek(cur, msg->i64);
    else track_request_seek_percent(cur, msg->d);
}

static void cmd_set_loop(AudioPlayer *ap, const int loop_count) {
    SDL_AtomicSet(&ap->loop_count, loop_count);
    lock_mixer(ap);
    TrackState *cur = ap->mixer.current, *next = ap->mixer.next;
    if (cur) SDL_AtomicSet(&cur->loops_remaining, loop_count);
    if (next) SDL_AtomicSet(&next->loops_remaining, loop_count);
    unlock_mixer(ap);
}

// The curves are sampled here, on the control thread: a custom curve is called now and never by the mixer
static void apply_crossfade_config(AudioPlayer *ap, const CrossfadeConfig *cfg) {
    ap->crossfade = *cfg;
    fade_curve_build(&ap->play_curve, &cfg->on_play);
    fade_curve_build(&ap->skip_curve, &cfg->on_skip);
    fade_curve_build(&ap->stop_curve, &cfg->on_stop);
    fade_curve_build(&ap->pause_curve, &cfg->on_pause);
    FadeCurve end_curve;
    fade_curve_build(&end_curve, &cfg->on_track_end);
    lock_mixer(ap);
    ap->mixer.end_fade = cfg->on_track_end;
    ap->mixer.end_curve = end_curve;
    ap->mixer.min_track_seconds = cfg->min_track_seconds;
    unlock_mixer(ap);
}

static TrackState *track_for_handle(AudioPlayer *ap, const int32_t handle) {
    if (handle) return registry_find(ap, handle);
    lock_mixer(ap);
    TrackState *cur = ap->mixer.current;
    unlock_mixer(ap);
    return cur;
}

static void cmd_filters(AudioPlayer *ap, const Msg *msg) {
    switch (msg->type) {
        case CMD_SET_GLOBAL_FILTERS: {
            SDL_LockMutex(ap->format_mutex);
            av_free(ap->global_filters);
            ap->global_filters = msg->str && msg->str[0] ? av_strdup(msg->str) : NULL;
            SDL_UnlockMutex(ap->format_mutex);
            for (int i = 0; i < ap->n_tracks; i++)
                if (!ap->tracks[i]->ended)
                    SDL_AtomicSet(&ap->tracks[i]->user_filters_changed, 1);
            break;
        }
        case CMD_SET_DOWNMIX: {
            SDL_LockMutex(ap->format_mutex);
            ap->downmix = (AU_DOWNMIX)msg->i64;
            av_freep(&ap->downmix_filters);
            if (msg->str && msg->str[0]) ap->downmix_filters = av_strdup(msg->str);
            SDL_UnlockMutex(ap->format_mutex);
            // The downmix lives in the convert graph: rebuild it before the next decoded frame
            for (int i = 0; i < ap->n_tracks; i++)
                if (!ap->tracks[i]->ended)
                    SDL_AtomicSet(&ap->tracks[i]->filter_reconfigure, 1);
            break;
        }
        case CMD_UPDATE_TRACK_FILTERS: {
            TrackState *t = track_for_handle(ap, msg->handle);
            if (t && !t->ended) track_set_filters(t, msg->str);
            else ff_log(AU_LOG_WARNING, "au_update_filtergraph: no track with handle %d", msg->handle);
            break;
        }
        case CMD_FILTER_COMMAND:
            for (int i = 0; i < ap->n_tracks; i++) {
                TrackState *t = ap->tracks[i];
                if (!t->ended && (!msg->handle || t->handle == msg->handle))
                    track_queue_filter_command(t, msg->str, msg->str2, msg->str3);
            }
            break;
        default:
            break;
    }
}

// Re-open the output. When the format changes, the tracks are re-synchronised by seeking them to
// where they are, which flushes audio decoded for the old format.
static int reopen_output(AudioPlayer *ap, const bool use_default, const char *name, const int index) {
    Mixer *m = &ap->mixer;
    const int old_rate = m->rate, old_channels = m->channels;
    const bool had_device = ap->device_id != 0;

    lock_mixer(ap);
    TrackState *cur = m->current, *next = m->next;
    const double cur_pos = cur ? cur->position : 0;
    unlock_mixer(ap);

    output_close(ap);

    int ret = output_open(ap, use_default, name, index);
    if (ret < 0 && !use_default) {
        ff_log(AU_LOG_ERROR, "Failed to open audio device '%s', trying the default device", name ? name : "?");
        ret = output_open(ap, true, NULL, -1);
        if (ret >= 0) {
            ap->use_default_device = true;
            av_freep(&ap->audio_device_name);
            ap->audio_device_index = -1;
        }
    }
    if (ret < 0) {
        ff_log(AU_LOG_ERROR, "Failed to open the default audio device");
        return ret;
    }

    if (had_device && (old_rate != m->rate || old_channels != m->channels)) {
        // The callback is not running (device just opened, paused): slots can be changed freely
        cancel_outgoing(ap);
        if (old_rate > 0 && m->in_fade.active) {
            m->in_fade.pos = m->in_fade.pos * m->rate / old_rate;
            m->in_fade.len = FFMAX(1, m->in_fade.len * m->rate / old_rate);
        }
        if (cur) {
            SDL_AtomicSet(&cur->filter_reconfigure, 1);
            if (cur->started) track_request_seek(cur, SecondsToMicroseconds(cur_pos));
            else track_request_seek(cur, SecondsToMicroseconds(track_start_position(cur)));
        }
        if (next) {
            SDL_AtomicSet(&next->filter_reconfigure, 1);
            track_request_seek(next, SecondsToMicroseconds(track_start_position(next)));
        }
    }

    note_add((Note){ .type = NOTE_DEVICE, .i = m->channels, .d = m->rate });
    update_device_running(ap);
    return AU_OK;
}

static int cmd_configure_device(AudioPlayer *ap, const Msg *msg) {
    const bool use_default = !msg->has_device;
    if (!use_default && (!msg->device.audio_device || msg->device.audio_device_index < 0))
        return AU_ERR_INVALID_ARGUMENT;

    if (ap->device_id) {
        if (use_default && ap->use_default_device)
            return AU_ERR_NOTHING_TO_DO;
        if (!use_default && !ap->use_default_device && ap->audio_device_name &&
            strcmp(ap->audio_device_name, msg->device.audio_device) == 0)
            return AU_ERR_NOTHING_TO_DO;
    }

    av_freep(&ap->audio_device_name);
    ap->use_default_device = use_default;
    ap->audio_device_index = use_default ? -1 : msg->device.audio_device_index;
    if (!use_default && !(ap->audio_device_name = av_strdup(msg->device.audio_device)))
        return AU_ERR_NO_MEMORY;

    return reopen_output(ap, use_default, ap->audio_device_name, ap->audio_device_index);
}

//+-----------------------------------------------------+
//| Track messages and mixer events                     |
//+-----------------------------------------------------+

static void msg_track_failed(AudioPlayer *ap, TrackState *t) {
    if (!t || t->ended) return;
    lock_mixer(ap);
    Mixer *m = &ap->mixer;
    const bool was_current = m->current == t;
    const bool was_next = m->next == t;
    remove_from_mixer(ap, t);
    if (was_current && m->next) set_current_slot(ap, m->next);
    TrackState *cur = m->current;
    const bool have_next = m->next != NULL;
    unlock_mixer(ap);

    end_track(ap, t, END_ERROR);

    // Give the application a chance to queue something else
    if ((was_next || was_current) && !have_next)
        note_add((Note){ .type = NOTE_PREPARE_NEXT, .handle = cur ? cur->handle : 0 });
    update_device_running(ap);
}

static void report_duration(TrackState *t) {
    const double d = track_duration(t);
    if (d < 0) return;
    if (!t->duration_reported || fabs(d - t->reported_duration) > 0.01) {
        t->duration_reported = true;
        t->reported_duration = d;
        note_add((Note){ .type = NOTE_DURATION, .handle = t->handle, .d = d });
    }
}

static void handle_track_msg(AudioPlayer *ap, const Msg *msg) {
    TrackState *t = registry_find(ap, msg->handle);
    if (!t || t->ended) return;

    switch (msg->type) {
        case MSG_TRACK_OPENED:
            report_duration(t);
            break;
        case MSG_TRACK_FAILED:
            msg_track_failed(ap, t);
            break;
        case MSG_TRACK_DEMUX_EOF: {
            // Fallback for tracks whose duration is unknown
            lock_mixer(ap);
            const bool is_current_alone = ap->mixer.current == t && !ap->mixer.next;
            unlock_mixer(ap);
            if (is_current_alone && !t->prepare_next_sent && SDL_AtomicGet(&t->loops_remaining) == 0) {
                t->prepare_next_sent = true;
                note_add((Note){ .type = NOTE_PREPARE_NEXT, .handle = t->handle });
            }
            break;
        }
        case MSG_TRACK_SEEKED:
            t->prepare_next_sent = false; // after a seek back, the end comes again
            note_add((Note){ .type = NOTE_RESTART, .handle = t->handle, .d = msg->d, .b1 = false,
                             .i = SDL_AtomicGet(&t->loops_remaining) });
            break;
        default:
            break;
    }
}

static void process_mixer_events(AudioPlayer *ap) {
    MixerEvent events[MIXER_EVENT_CAPACITY];
    lock_mixer(ap);
    const int n = ap->mixer.n_events;
    memcpy(events, ap->mixer.events, (size_t)n * sizeof(MixerEvent));
    ap->mixer.n_events = 0;
    const bool overflowed = ap->mixer.events_overflowed;
    ap->mixer.events_overflowed = false;
    unlock_mixer(ap);

    for (int i = 0; i < n; i++) {
        MixerEvent *e = &events[i];
        TrackState *t = e->track;
        if (t->ended) continue;
        switch (e->type) {
            case MIX_EV_TRACK_STARTED:
                note_add((Note){ .type = NOTE_TRACK_STARTED, .handle = t->handle });
                break;
            case MIX_EV_TRACK_LOOPED:
                note_add((Note){ .type = NOTE_RESTART, .handle = t->handle, .d = e->position, .b1 = true,
                                 .i = SDL_AtomicGet(&t->loops_remaining) });
                break;
            case MIX_EV_TRACK_ENDED:
                end_track(ap, t, END_NATURAL);
                break;
            case MIX_EV_FADE_DONE:
                end_track(ap, t, t->fade_reason ? t->fade_reason : END_SKIPPED);
                break;
        }
    }

    // Safety net: any live track that is no longer in a slot has left the mixer
    if (overflowed || n > 0) {
        lock_mixer(ap);
        for (int i = 0; i < ap->n_tracks; i++) {
            TrackState *t = ap->tracks[i];
            if (!t->ended && !mixer_has_track(&ap->mixer, t)) {
                unlock_mixer(ap);
                end_track(ap, t, END_NATURAL);
                lock_mixer(ap);
            }
        }
        unlock_mixer(ap);
    }

    if (n > 0) update_device_running(ap);
}

#define DEVICE_POLL_MS 2000

static void free_device_list(char **list, const int n) {
    for (int i = 0; i < n; i++) av_free(list[i]);
    av_free(list);
}

// Compare the output devices with the last known list; notify the application when they differ.
// Comparing (instead of trusting the events) also ignores the "added" events SDL sends for every
// device at start-up.
static void check_device_list(AudioPlayer *ap) {
    ap->devices_checked_at = SDL_GetTicks();
    const int n = SDL_GetNumAudioDevices(0);
    if (n < 0) return;
    char **list = n ? av_calloc(n, sizeof(char *)) : NULL;
    if (n && !list) return;
    for (int i = 0; i < n; i++) {
        const char *name = SDL_GetAudioDeviceName(i, 0);
        list[i] = av_strdup(name ? name : "");
    }

    bool same = n == ap->n_known_devices;
    for (int i = 0; same && i < n; i++)
        same = list[i] && ap->known_devices[i] && !strcmp(list[i], ap->known_devices[i]);

    const bool had_list = ap->devices_checked_once;
    ap->devices_checked_once = true;
    if (same) {
        free_device_list(list, n);
        return;
    }
    free_device_list(ap->known_devices, ap->n_known_devices);
    ap->known_devices = list;
    ap->n_known_devices = n;
    if (had_list) {
        ff_log(AU_LOG_INFO, "Audio output devices changed (%d now)", n);
        note_add((Note){ .type = NOTE_DEVICES_CHANGED });
    }
}

// SDL calls this for every event added to its queue, on the thread that adds it. It only looks: the events stay in
// the queue for the application (taking them used to swallow the keyboard, mouse and window events of applications
// that also use SDL).
static int SDLCALL device_event_watch(void *userdata, SDL_Event *e) {
    AudioPlayer *ap = userdata;
    if ((e->type == SDL_AUDIODEVICEADDED || e->type == SDL_AUDIODEVICEREMOVED) && !e->adevice.iscapture) {
        SDL_AtomicSet(&ap->device_events, 1);
        const int open = SDL_AtomicGet(&ap->open_device);
        if (e->type == SDL_AUDIODEVICEREMOVED && open && e->adevice.which == (Uint32)open)
            SDL_AtomicSet(&ap->open_device_lost, 1);
    }
    return 1;
}

static void housekeeping(AudioPlayer *ap) {
    lock_mixer(ap);
    TrackState *cur = ap->mixer.current;
    const bool have_next = ap->mixer.next != NULL;
    const double pos = cur ? cur->position : 0;
    unlock_mixer(ap);

    if (cur && !have_next && !cur->prepare_next_sent && ffa_load_acquire(&cur->opened)
        && SDL_AtomicGet(&cur->loops_remaining) == 0) {
        const double end = track_end_position(cur);
        const double lead = ap->prepare_next_seconds + FFMAX(0.0, ap->crossfade.on_track_end.seconds);
        SDL_LockMutex(ap->format_mutex);
        const double speed = ap->speed > 0 ? ap->speed : 1.0;
        SDL_UnlockMutex(ap->format_mutex);
        if (end >= 0 && (end - pos) / speed <= lead) { // lead is wall clock time
            cur->prepare_next_sent = true;
            note_add((Note){ .type = NOTE_PREPARE_NEXT, .handle = cur->handle });
        }
    }

    // "Now playing" changes that playback has reached
    for (int i = 0; i < ap->n_tracks; i++) {
        TrackState *t = ap->tracks[i];
        if (t->ended) continue;
        lock_mixer(ap);
        const bool playing = t->started;
        const double at = t->play_clock;
        unlock_mixer(ap);
        if (!playing) continue;
        for (StreamMeta *m; (m = track_take_stream_meta(t, at));) {
            note_add((Note){ .type = NOTE_STREAM_METADATA, .handle = t->handle, .text = m->title, .text2 = m->artist });
            m->title = m->artist = NULL;   // owned by the note now
            track_free_stream_meta(m);
        }
    }

    // Network tracks waiting for data: every change, and progress every 500 ms while it lasts
    const uint32_t now = SDL_GetTicks();
    for (int i = 0; i < ap->n_tracks; i++) {
        TrackState *t = ap->tracks[i];
        if (t->ended || !t->network) continue;
        const int b = SDL_AtomicGet(&t->buffering);
        if (b != t->buffering_reported || (b && now - t->buffering_note_ticks >= 500)) {
            note_add((Note){ .type = NOTE_BUFFERING, .handle = t->handle, .b1 = b != 0,
                             .d = SDL_AtomicGet(&t->buffered_ms) / 1000.0 });
            t->buffering_reported = b;
            t->buffering_note_ticks = now;
        }
    }

    // Devices coming and going, as seen by device_event_watch. SDL3 (under sdl2-compat) only delivers device events
    // while the queue is pumped: an application with SDL video does that itself, on the thread SDL requires for
    // video, so it is only done here without video.
    if (!SDL_WasInit(SDL_INIT_VIDEO)) SDL_PumpEvents();
    const bool check_devices = SDL_AtomicSet(&ap->device_events, 0) != 0;
    if (SDL_AtomicSet(&ap->open_device_lost, 0) && ap->device_id) {
        ff_log(AU_LOG_WARNING, "Audio device removed, switching to the default device");
        ap->use_default_device = true;
        av_freep(&ap->audio_device_name);
        ap->audio_device_index = -1;
        reopen_output(ap, true, NULL, -1);
    }
    // SDL2 only reports the removal of the open device, so the list is also compared every few seconds
    if (check_devices || SDL_GetTicks() - ap->devices_checked_at >= DEVICE_POLL_MS)
        check_device_list(ap);
}

//+-----------------------------------------------------+
//| Control thread                                      |
//+-----------------------------------------------------+

static void finish_sync(Msg *msg, const int result) {
    if (msg->result) *msg->result = result;
    if (msg->done) SDL_SemPost(msg->done);
}

static bool handle_msg(AudioPlayer *ap, Msg *msg) {
    switch (msg->type) {
        case CMD_PLAY:             cmd_play(ap, msg); break;
        case CMD_SET_NEXT:         cmd_set_next(ap, msg); break;
        case CMD_SKIP:             cmd_skip(ap); break;
        case CMD_CLEAR_NEXT:       cmd_clear_next(ap); break;
        case CMD_STOP:             cmd_stop(ap); break;
        case CMD_PAUSE:            cmd_pause(ap, msg->b); break;
        case CMD_SEEK_US:
        case CMD_SEEK_PERCENT:     cmd_seek(ap, msg); break;
        case CMD_SET_LOOP:         cmd_set_loop(ap, (int)msg->i64); break;
        case CMD_SET_CROSSFADE:    apply_crossfade_config(ap, &msg->crossfade); break;
        case CMD_SET_GLOBAL_FILTERS:
        case CMD_SET_DOWNMIX:
        case CMD_UPDATE_TRACK_FILTERS:
        case CMD_FILTER_COMMAND:   cmd_filters(ap, msg); break;
        case CMD_CONFIGURE_DEVICE: finish_sync(msg, cmd_configure_device(ap, msg)); msg->done = NULL; break;
        case CMD_GAIN_CACHE_CHANGED:
            for (int i = 0; i < ap->n_tracks; i++) {
                TrackState *t = ap->tracks[i];
                if (!t->ended && (!msg->str || strcmp(t->filename, msg->str) == 0)) {
                    track_update_gain(t, true); // a value that arrives while playing fades in slowly
                }
            }
            break;
        case CMD_SET_REPLAYGAIN:
            SDL_LockMutex(ap->format_mutex);
            ap->replaygain = msg->replaygain;
            SDL_UnlockMutex(ap->format_mutex);
            for (int i = 0; i < ap->n_tracks; i++)
                if (!ap->tracks[i]->ended)
                    track_update_gain(ap->tracks[i], false);
            break;
        case CMD_GET_TRACK_INFO: {
            TrackState *t = track_for_handle(ap, msg->handle);
            *msg->out_ptr = (t && !t->ended) ? track_copy_info(t) : NULL;
            break;
        }
        case CMD_GET_TRACK_GAIN: {
            TrackState *t = track_for_handle(ap, msg->handle);
            const bool known = t && !t->ended && ffa_load_acquire(&t->opened);
            if (known) *msg->out_double = gain_centi_db(SDL_AtomicGet(&t->gain_mb)) / 100.0;
            finish_sync(msg, known ? AU_OK : AU_ERR_NOTHING_TO_DO);
            msg->done = NULL;
            break;
        }
        case CMD_SHUTDOWN:         return false;
        case MSG_LOG:
            note_add((Note){ .type = NOTE_LOG, .text = msg->str, .i = msg->level, .request = msg->request });
            msg->str = NULL;
            break;
        case MSG_TRACK_OPENED:
        case MSG_TRACK_FAILED:
        case MSG_TRACK_DEMUX_EOF:
        case MSG_TRACK_SEEKED:     handle_track_msg(ap, msg); break;
        case MSG_TRACK_EXITED:     break; // reaped at the end of this loop iteration
    }
    return true;
}

static void shutdown_tracks(AudioPlayer *ap) {
    lock_mixer(ap);
    Mixer *m = &ap->mixer;
    for (int i = 0; i < ap->n_tracks; i++)
        remove_from_mixer(ap, ap->tracks[i]);
    m->current = m->next = m->outgoing = NULL;
    m->n_events = 0;
    unlock_mixer(ap);
    output_close(ap);
    for (int i = 0; i < ap->n_tracks; i++)
        track_stop(ap->tracks[i]);
    for (int i = 0; i < ap->n_tracks; i++)
        track_destroy(ap->tracks[i]);
    ap->n_tracks = 0;
    for (int i = 0; i < ap->n_dying; i++)
        track_destroy(ap->dying[i]);
    ap->n_dying = 0;
}

static int control_thread(void *arg) {
    ff_thread_use_c_locale();
    AudioPlayer *ap = arg;
    Msg *shutdown_msg = NULL;
    // Recognising itself is what lets callbacks call the API inline. SDL2 sets the id SDL_GetThreadID() returns only
    // when the thread runs, so au_initialize() usually read 0: every synchronous call from a callback then waited for
    // the thread making it.
    ap->control_thread_id = SDL_ThreadID();
    SDL_SemPost(ap->started_sem);

    while (!shutdown_msg) {
        SDL_SemWaitTimeout(ap->wake_sem, ap->n_tracks || ap->n_dying ? CONTROL_PLAYING_MS : CONTROL_IDLE_MS);
        SDL_LockMutex(ap->msg_mutex);
        Msg *list = ap->msg_head;
        ap->msg_head = ap->msg_tail = NULL;
        SDL_UnlockMutex(ap->msg_mutex);

        process_mixer_events(ap);

        while (list) {
            Msg *msg = list;
            list = list->next;
            if (shutdown_msg) {
                // Commands after shutdown are dropped; synchronous callers must not hang
                finish_sync(msg, AU_ERR_NOT_INITIALIZED);
                msg_free(msg);
                continue;
            }
            if (!handle_msg(ap, msg)) {
                shutdown_msg = msg;
                continue;
            }
            if (msg->done) finish_sync(msg, AU_OK);
            msg_free(msg);
            // Run callbacks between messages so they always see a consistent state
            process_mixer_events(ap);
            dispatch_notes(ap);
        }

        if (!shutdown_msg) {
            housekeeping(ap);
            process_mixer_events(ap);
            dispatch_notes(ap);
            destroy_ended_tracks(ap);
        }
    }

    SDL_AtomicSet(&ap->shutting_down, 1);
    shutdown_tracks(ap);
    dispatch_notes(ap); // frees pending notes without calling back
    av_freep(&notes.items);
    notes.n = notes.cap = 0;
    finish_sync(shutdown_msg, AU_OK);
    msg_free(shutdown_msg);
    return 0;
}

//+-----------------------------------------------------+
//| Public API: lifetime                                |
//+-----------------------------------------------------+

int au_initialize(const InitializeConfig* config) {
    if (!config) return AU_ERR_INVALID_ARGUMENT;
    if (audio_player) return AU_ERR_ALREADY_INITIALIZED;

    AudioPlayer *ap = av_mallocz(sizeof(AudioPlayer));
    if (!ap) return AU_ERR_NO_MEMORY;

    SDL_AtomicSet(&ap->volume, config->initial_volume >= 0 ? FFMIN(config->initial_volume, 100) : 100);
    SDL_AtomicSet(&ap->loop_count, config->initial_loop_count);
    ap->prepare_next_seconds = config->prepare_next_seconds > 0 ? config->prepare_next_seconds : DEFAULT_PREPARE_NEXT_SECONDS;
    if (config->output_channels < 0 || config->output_channels > 8 ||
        (config->output_sample_rate != 0 && (config->output_sample_rate < 8000 || config->output_sample_rate > 384000))) {
        av_free(ap);
        return AU_ERR_INVALID_ARGUMENT;
    }
    ap->output_channels = config->output_channels;
    ap->output_sample_rate = config->output_sample_rate;
    ap->downmix = AU_DOWNMIX_FULL_LEVEL;
    ap->speed = 1.0;
    ap->keep_pitch = true;
    ap->audio_device_index = -1;
    ap->use_default_device = true;
    ap->user_data = config->user_data;
    ap->notify_of_log_callback = config->on_log;
    ap->notify_of_eof_callback = config->on_eof;
    ap->notify_of_restart_callback = config->on_restart;
    ap->notify_of_duration_update_callback = config->on_duration_update;
    ap->notify_of_prepare_next_callback = config->on_prepare_next;
    ap->notify_of_track_started_callback = config->on_track_started;
    ap->notify_of_audio_device_update_callback = config->on_audio_device_update;
    ap->notify_of_audio_devices_changed_callback = config->on_audio_devices_changed;
    ap->notify_of_buffering_callback = config->on_buffering;
    ap->notify_of_stream_metadata_callback = config->on_stream_metadata;
    ap->audio_target.fmt = AV_SAMPLE_FMT_NONE;
    ap->mixer.pause_gain = 1.0f;
    ap->mixer.pause_to = 1.0f;
    ap->mixer.vol_gain = (float)SDL_AtomicGet(&ap->volume) / 100.0f;
    fade_curve_build(&ap->mixer.linear_curve, NULL);
    const CrossfadeConfig no_fades = {0};
    apply_crossfade_config(ap, &no_fades); // no device yet: lock_mixer() does nothing

    ap->msg_mutex = SDL_CreateMutex();
    ap->wake_sem = SDL_CreateSemaphore(0);
    ap->format_mutex = SDL_CreateMutex();
    ap->format_cond = SDL_CreateCond();
    ap->status_mutex = SDL_CreateMutex();
    ap->ring_pool_mutex = SDL_CreateMutex();
    ap->started_sem = SDL_CreateSemaphore(0);
    if (!ap->msg_mutex || !ap->wake_sem || !ap->format_mutex || !ap->format_cond || !ap->status_mutex ||
        !ap->ring_pool_mutex || !ap->started_sem)
        goto fail_sync;
    publish_idle_status(ap);

    av_log_set_level(AV_LOG_ERROR);

    // A library must not take over the host's Ctrl-C / SIGTERM handling
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    /* Try to work around an occasional ALSA buffer underflow issue when the
     * period size is NPOT due to ALSA resampling by forcing the buffer size. */
    if (!SDL_getenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE"))
        SDL_setenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE", "1", 1);
    if (config->app_name) {
        SDL_SetHint(SDL_HINT_APP_NAME, config->app_name);
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_NAME, config->app_name);
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_APP_NAME, config->app_name);
    }
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_ROLE, "music");
    SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");

    if (SDL_InitSubSystem(SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        if (config->on_log) config->on_log(SDL_GetError(), 0, AU_LOG_FATAL, config->user_data);
        goto fail_sync;
    }
    SDL_AddEventWatch(device_event_watch, ap);

    ap->use_soxr = filtergraph_soxr_available();
    // Filter graphs run audio in small chunks; FFmpeg's default (a thread pool per graph, one thread per core) gave
    // every track up to 3 x 12 idle threads
    ap->filter_nbthreads = 1;
    avformat_network_init();

    SDL_mutex *lock = lifecycle_mutex();
    SDL_LockMutex(lock);
    audio_player = ap;
    SDL_UnlockMutex(lock);
    ap->control_thread = SDL_CreateThread(control_thread, "ffa_control", ap);
    if (!ap->control_thread) {
        if (config->on_log) config->on_log(SDL_GetError(), 0, AU_LOG_FATAL, config->user_data);
        SDL_LockMutex(lock);
        audio_player = NULL;
        SDL_UnlockMutex(lock);
        // A cache update may have queued a message in the meantime
        while (ap->msg_head) {
            Msg *m = ap->msg_head;
            ap->msg_head = m->next;
            msg_free(m);
        }
        avformat_network_deinit();
        SDL_DelEventWatch(device_event_watch, ap);
        SDL_QuitSubSystem(SDL_INIT_AUDIO | SDL_INIT_EVENTS);
        goto fail_sync;
    }
    SDL_SemWait(ap->started_sem); // control_thread_id is set

    ff_log(AU_LOG_INFO, "ffaudio %s initialized (audio driver: %s, resampler: %s)", au_get_version(),
           SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "none", ap->use_soxr ? "soxr" : "swr");
    return AU_OK;

fail_sync:
    SDL_DestroyMutex(ap->msg_mutex);
    SDL_DestroySemaphore(ap->wake_sem);
    SDL_DestroyMutex(ap->format_mutex);
    SDL_DestroyCond(ap->format_cond);
    SDL_DestroyMutex(ap->status_mutex);
    SDL_DestroyMutex(ap->ring_pool_mutex);
    SDL_DestroySemaphore(ap->started_sem);
    av_free(ap);
    return AU_ERR_SDL;
}

void au_shutdown(void) {
    AudioPlayer *ap = audio_player;
    if (!ap) return;
    if (on_control_thread(ap)) {
        ff_log(AU_LOG_ERROR, "au_shutdown() must not be called from a callback");
        return;
    }

    Msg *m = msg_new(CMD_SHUTDOWN);
    SDL_sem *done = SDL_CreateSemaphore(0);
    if (m && done) {
        m->done = done;
        msg_post(ap, m);
        SDL_SemWait(done);
    }
    else {
        msg_free(m);
    }
    SDL_DestroySemaphore(done);
    SDL_WaitThread(ap->control_thread, NULL);
    SDL_DelEventWatch(device_event_watch, ap); // returns once a watch call in progress has finished

    SDL_mutex *lock = lifecycle_mutex();
    SDL_LockMutex(lock);
    audio_player = NULL;
    SDL_UnlockMutex(lock);

    Msg *rest = ap->msg_head;
    while (rest) {
        Msg *n = rest->next;
        finish_sync(rest, AU_ERR_NOT_INITIALIZED);
        msg_free(rest);
        rest = n;
    }
    av_free(ap->tracks);
    av_free(ap->dying);
    av_free(ap->audio_device_name);
    av_free(ap->device_in_use);
    free_device_list(ap->known_devices, ap->n_known_devices);
    av_free(ap->global_filters);
    av_free(ap->downmix_filters);
    av_channel_layout_uninit(&ap->audio_target.ch_layout);
    SDL_DestroyMutex(ap->msg_mutex);
    SDL_DestroySemaphore(ap->wake_sem);
    SDL_DestroyMutex(ap->format_mutex);
    SDL_DestroyCond(ap->format_cond);
    SDL_DestroyMutex(ap->status_mutex);
    ring_storage_free_all(ap); // the tracks gave their storage back when they were destroyed
    SDL_DestroyMutex(ap->ring_pool_mutex);
    SDL_DestroySemaphore(ap->started_sem);
    av_free(ap);

    avformat_network_deinit();
    SDL_QuitSubSystem(SDL_INIT_AUDIO | SDL_INIT_EVENTS);
}

// Posts a message and waits for its result, or runs it inline when called from a callback
static int run_sync(AudioPlayer *ap, Msg *m) {
    int32_t result = AU_OK;
    if (on_control_thread(ap)) {
        m->result = &result;
        handle_msg(ap, m);
        msg_free(m);
        return result;
    }
    SDL_sem *done = SDL_CreateSemaphore(0);
    if (!done) { msg_free(m); return AU_ERR_SDL; }
    m->done = done;
    m->result = &result;
    msg_post(ap, m);
    SDL_SemWait(done);
    SDL_DestroySemaphore(done);
    return result;
}

int au_configure_audio_device(const AudioDeviceConfig* custom_config) {
    AudioPlayer *ap = audio_player;
    if (!ap) return AU_ERR_NOT_INITIALIZED;
    SDL_AtomicIncRef(&ap->request_count);

    Msg *m = msg_new(CMD_CONFIGURE_DEVICE);
    if (!m) return AU_ERR_NO_MEMORY;
    if (custom_config) {
        if (!custom_config->audio_device || custom_config->audio_device_index < 0) {
            msg_free(m);
            return AU_ERR_INVALID_ARGUMENT;
        }
        m->has_device = true;
        if (!(m->str = av_strdup(custom_config->audio_device))) { msg_free(m); return AU_ERR_NO_MEMORY; }
        m->device.audio_device = m->str;
        m->device.audio_device_index = custom_config->audio_device_index;
    }
    return run_sync(ap, m);
}

int au_get_audio_devices(int *out_total, char ***out_devices) {
    if (!out_total || !out_devices) return AU_ERR_INVALID_ARGUMENT;
    *out_total = 0;
    *out_devices = NULL;
    if (!audio_player) return AU_ERR_NOT_INITIALIZED;

    const int count = SDL_GetNumAudioDevices(0);
    if (count <= 0) return count == 0 ? AU_OK : AU_ERR_SDL;

    char **devices = calloc((size_t)count, sizeof(char *));
    if (!devices) return AU_ERR_NO_MEMORY;

    for (int i = 0; i < count; i++) {
        const char *name = SDL_GetAudioDeviceName(i, 0);
        if (!name || !(devices[i] = strdup(name))) {
            for (int j = 0; j < i; j++) free(devices[j]);
            free(devices);
            return AU_ERR_SDL;
        }
    }
    *out_total = count;
    *out_devices = devices;
    return AU_OK;
}

void au_free_audio_devices(char **devices, const int total) {
    if (!devices) return;
    for (int i = 0; i < total; i++) free(devices[i]);
    free(devices);
}

int au_get_audio_device_name(char *buf, const size_t size) {
    AudioPlayer *ap = audio_player;
    if (!buf || !size) return AU_ERR_INVALID_ARGUMENT;
    if (!ap) return AU_ERR_NOT_INITIALIZED;
    SDL_LockMutex(ap->format_mutex);
    const char *name = ap->device_in_use ? ap->device_in_use : "";
    const size_t len = strlen(name);
    const int ret = len < size ? (int)len : AU_ERR_INVALID_ARGUMENT;
    if (len < size) memcpy(buf, name, len + 1);
    SDL_UnlockMutex(ap->format_mutex);
    return ret;
}

int au_get_output_spec(int *out_sample_rate, int *out_channels) {
    AudioPlayer *ap = audio_player;
    if (!ap) return AU_ERR_NOT_INITIALIZED;
    SDL_LockMutex(ap->format_mutex);
    const bool ready = ap->output_ready;
    const int rate = ap->audio_target.freq, channels = ap->audio_target.ch_layout.nb_channels;
    SDL_UnlockMutex(ap->format_mutex);
    if (!ready) return AU_ERR_DEVICE;
    if (out_sample_rate) *out_sample_rate = rate;
    if (out_channels) *out_channels = channels;
    return AU_OK;
}

//+-----------------------------------------------------+
//| Public API: playback                                |
//+-----------------------------------------------------+

static int32_t post_track_command(const MsgType type, const char *filename, const PlayAudioConfig *config) {
    AudioPlayer *ap = audio_player;
    if (!ap) return AU_ERR_NOT_INITIALIZED;
    if (!filename || !filename[0]) return AU_ERR_INVALID_ARGUMENT;
    SDL_AtomicIncRef(&ap->request_count);

    Msg *m = msg_new(type);
    if (!m || !(m->str = av_strdup(filename))) { msg_free(m); return AU_ERR_NO_MEMORY; }
    if (config) {
        m->has_play_config = true;
        m->play_config = *config;
        if (config->av_filtergraph) {
            if (!(m->str2 = av_strdup(config->av_filtergraph))) { msg_free(m); return AU_ERR_NO_MEMORY; }
            m->play_config.av_filtergraph = m->str2;
        }
        if (config->http_headers) {
            if (!(m->str3 = av_strdup(config->http_headers))) { msg_free(m); return AU_ERR_NO_MEMORY; }
            m->play_config.http_headers = m->str3;
        }
        if (config->fade_in) {
            m->fade_in_override = *config->fade_in;
            m->has_fade_in_override = true;
            m->play_config.fade_in = &m->fade_in_override;
        }
        if (config->replaygain) {
            m->replaygain_override = *config->replaygain;
            m->has_replaygain_override = true;
            m->play_config.replaygain = &m->replaygain_override;
        }
    }
    m->handle = SDL_AtomicAdd(&ap->handle_count, 1) + 1;
    const int32_t handle = m->handle;
    msg_post(ap, m);
    return handle;
}

int32_t au_play_audio(const char *filename, const PlayAudioConfig* config) {
    return post_track_command(CMD_PLAY, filename, config);
}

int32_t au_set_next(const char *filename, const PlayAudioConfig *config) {
    return post_track_command(CMD_SET_NEXT, filename, config);
}

static void post_simple(const MsgType type, const int64_t i64, const double d, const bool b) {
    AudioPlayer *ap = audio_player;
    if (!ap) return;
    SDL_AtomicIncRef(&ap->request_count);
    Msg *m = msg_new(type);
    if (!m) return;
    m->i64 = i64;
    m->d = d;
    m->b = b;
    msg_post(ap, m);
}

void au_skip_to_next(void) { post_simple(CMD_SKIP, 0, 0, false); }

void au_clear_next(void) { post_simple(CMD_CLEAR_NEXT, 0, 0, false); }

void au_stop_audio(void) { post_simple(CMD_STOP, 0, 0, false); }

void au_pause_audio(const bool value) { post_simple(CMD_PAUSE, 0, 0, value); }

bool au_is_paused(void) {
    AudioPlayer *ap = audio_player;
    return ap && SDL_AtomicGet(&ap->paused);
}

void au_seek_percent(const double percentPos) {
    if (percentPos < 0.0 || percentPos > 100.0 || isnan(percentPos)) return;
    post_simple(CMD_SEEK_PERCENT, 0, percentPos, false);
}

void au_seek_time(const int64_t milliseconds) {
    if (milliseconds < 0) return;
    post_simple(CMD_SEEK_US, milliseconds * 1000, 0, false);
}

int au_set_playback_speed(const double speed, const bool keep_pitch) {
    AudioPlayer *ap = audio_player;
    if (!ap) return AU_ERR_NOT_INITIALIZED;
    if (!isfinite(speed) || speed < 0.25 || speed > 4.0) return AU_ERR_INVALID_ARGUMENT;
    SDL_AtomicIncRef(&ap->request_count);
    // The audio threads pick the change up before their next decoded frame
    SDL_LockMutex(ap->format_mutex);
    if (ap->speed != speed || ap->keep_pitch != keep_pitch) {
        ap->speed = speed;
        ap->keep_pitch = keep_pitch;
        ap->speed_gen++;
    }
    SDL_UnlockMutex(ap->format_mutex);
    return AU_OK;
}

double au_get_playback_speed(void) {
    AudioPlayer *ap = audio_player;
    if (!ap) return 1.0;
    SDL_LockMutex(ap->format_mutex);
    const double speed = ap->speed;
    SDL_UnlockMutex(ap->format_mutex);
    return speed;
}

int au_set_crossfade(const CrossfadeConfig *config) {
    AudioPlayer *ap = audio_player;
    if (!ap) return AU_ERR_NOT_INITIALIZED;
    SDL_AtomicIncRef(&ap->request_count);
    Msg *m = msg_new(CMD_SET_CROSSFADE);
    if (!m) return AU_ERR_NO_MEMORY;
    if (config) m->crossfade = *config;
    msg_post(ap, m);
    return AU_OK;
}

//+-----------------------------------------------------+
//| Public API: filters                                 |
//+-----------------------------------------------------+

int au_validate_filtergraph(const char *filters) {
    AudioPlayer *ap = audio_player;
    if (!filters || !filters[0]) return AU_OK;

    int rate = 48000;
    AVChannelLayout layout = AV_CHANNEL_LAYOUT_STEREO;
    if (ap) {
        SDL_LockMutex(ap->format_mutex);
        if (ap->output_ready) {
            rate = ap->audio_target.freq;
            av_channel_layout_copy(&layout, &ap->audio_target.ch_layout);
        }
        SDL_UnlockMutex(ap->format_mutex);
    }
    FilterChain c = {0};
    const FfLocale saved = ff_c_locale_begin(); // parsed on the caller's thread
    const int ret = build_user_graph(&c, filters, NULL, rate, &layout, 0);
    ff_c_locale_end(saved);
    free_filter_chain(&c);
    av_channel_layout_uninit(&layout);
    return ret < 0 ? AU_ERR_INVALID_ARGUMENT : AU_OK;
}

static int post_filter_msg(const MsgType type, const int32_t handle, const char *s1, const char *s2, const char *s3) {
    AudioPlayer *ap = audio_player;
    if (!ap) return AU_ERR_NOT_INITIALIZED;
    SDL_AtomicIncRef(&ap->request_count);
    Msg *m = msg_new(type);
    if (!m) return AU_ERR_NO_MEMORY;
    m->handle = handle;
    if ((s1 && !(m->str = av_strdup(s1))) || (s2 && !(m->str2 = av_strdup(s2))) || (s3 && !(m->str3 = av_strdup(s3)))) {
        msg_free(m);
        return AU_ERR_NO_MEMORY;
    }
    msg_post(ap, m);
    return AU_OK;
}

int au_set_global_filtergraph(const char *filters) {
    if (!audio_player) return AU_ERR_NOT_INITIALIZED;
    if (au_validate_filtergraph(filters) != AU_OK) {
        ff_log(AU_LOG_ERROR, "au_set_global_filtergraph: invalid filters \"%s\"", filters);
        return AU_ERR_INVALID_ARGUMENT;
    }
    return post_filter_msg(CMD_SET_GLOBAL_FILTERS, 0, filters, NULL, NULL);
}

int au_update_filtergraph(const int32_t handle, const char *filters) {
    if (!audio_player) return AU_ERR_NOT_INITIALIZED;
    if (handle < 0) return AU_ERR_INVALID_ARGUMENT;
    if (au_validate_filtergraph(filters) != AU_OK) {
        ff_log(AU_LOG_ERROR, "au_update_filtergraph: invalid filters \"%s\"", filters);
        return AU_ERR_INVALID_ARGUMENT;
    }
    return post_filter_msg(CMD_UPDATE_TRACK_FILTERS, handle, filters, NULL, NULL);
}

//+-----------------------------------------------------+
//| Public API: ReplayGain, track info                  |
//+-----------------------------------------------------+

int au_set_replaygain(const AuReplayGainConfig *config) {
    AudioPlayer *ap = audio_player;
    if (!ap) return AU_ERR_NOT_INITIALIZED;
    AuReplayGainConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (config) {
        cfg = *config;
        if (cfg.mode != AU_REPLAYGAIN_OFF && cfg.mode != AU_REPLAYGAIN_TRACK && cfg.mode != AU_REPLAYGAIN_ALBUM)
            return AU_ERR_INVALID_ARGUMENT;
        if (!isfinite(cfg.preamp_db) || cfg.preamp_db < -15 || cfg.preamp_db > 15) return AU_ERR_INVALID_ARGUMENT;
        if (!isfinite(cfg.fallback_gain_db) || cfg.fallback_gain_db < -30 || cfg.fallback_gain_db > 15)
            return AU_ERR_INVALID_ARGUMENT;
        if (cfg.measure_wait_ms > 10000) return AU_ERR_INVALID_ARGUMENT;
    }
    SDL_AtomicIncRef(&ap->request_count);
    Msg *m = msg_new(CMD_SET_REPLAYGAIN);
    if (!m) return AU_ERR_NO_MEMORY;
    m->replaygain = cfg;
    msg_post(ap, m);
    return AU_OK;
}

AuMediaInfo *au_get_track_info(const int32_t handle) {
    AudioPlayer *ap = audio_player;
    if (!ap || handle < 0) return NULL;
    void *info = NULL;
    Msg *m = msg_new(CMD_GET_TRACK_INFO);
    if (!m) return NULL;
    m->handle = handle;
    m->out_ptr = &info;
    run_sync(ap, m);
    return info;
}

int au_get_track_gain_db(const int32_t handle, double *out_gain_db) {
    AudioPlayer *ap = audio_player;
    if (!ap) return AU_ERR_NOT_INITIALIZED;
    if (handle < 0 || !out_gain_db) return AU_ERR_INVALID_ARGUMENT;
    Msg *m = msg_new(CMD_GET_TRACK_GAIN);
    if (!m) return AU_ERR_NO_MEMORY;
    m->handle = handle;
    m->out_double = out_gain_db;
    return run_sync(ap, m);
}

int au_set_downmix(const AU_DOWNMIX mode, const char *custom_filters) {
    if (!audio_player) return AU_ERR_NOT_INITIALIZED;
    if (mode != AU_DOWNMIX_FULL_LEVEL && mode != AU_DOWNMIX_NORMALIZED && mode != AU_DOWNMIX_CUSTOM)
        return AU_ERR_INVALID_ARGUMENT;
    if (mode == AU_DOWNMIX_CUSTOM) {
        const int ret = validate_downmix_filters(custom_filters);
        if (ret < 0) {
            ff_log(AU_LOG_ERROR, "au_set_downmix: invalid custom downmix \"%s\": %s",
                   custom_filters ? custom_filters : "", av_err2str(ret));
            return AU_ERR_INVALID_ARGUMENT;
        }
    }
    AudioPlayer *ap = audio_player;
    SDL_AtomicIncRef(&ap->request_count);
    Msg *m = msg_new(CMD_SET_DOWNMIX);
    if (!m) return AU_ERR_NO_MEMORY;
    m->i64 = mode;
    if (mode == AU_DOWNMIX_CUSTOM && !(m->str = av_strdup(custom_filters))) {
        msg_free(m);
        return AU_ERR_NO_MEMORY;
    }
    msg_post(ap, m);
    return AU_OK;
}

int au_send_filter_command(const int32_t handle, const char *target, const char *command, const char *arg) {
    if (!target || !command || handle < 0) return AU_ERR_INVALID_ARGUMENT;
    return post_filter_msg(CMD_FILTER_COMMAND, handle, target, command, arg);
}

//+-----------------------------------------------------+
//| Public API: volume, looping, status                 |
//+-----------------------------------------------------+

void au_set_audio_volume(const int volume) {
    AudioPlayer *ap = audio_player;
    if (!ap || volume < 0 || volume > 100) return;
    SDL_AtomicIncRef(&ap->request_count);
    SDL_AtomicSet(&ap->volume, volume);
}

int au_get_audio_volume(void) {
    AudioPlayer *ap = audio_player;
    return ap ? SDL_AtomicGet(&ap->volume) : -1;
}

void au_mute_audio(const bool value) {
    AudioPlayer *ap = audio_player;
    if (!ap) return;
    SDL_AtomicIncRef(&ap->request_count);
    SDL_AtomicSet(&ap->muted, value);
}

bool au_is_muted(void) {
    AudioPlayer *ap = audio_player;
    return ap && SDL_AtomicGet(&ap->muted);
}

void au_set_loop_count(const int loop_count) {
    post_simple(CMD_SET_LOOP, loop_count < -1 ? -1 : loop_count, 0, false);
}

int au_get_loop_count(void) {
    AudioPlayer *ap = audio_player;
    return ap ? SDL_AtomicGet(&ap->loop_count) : 0;
}

static StatusSnapshot read_status(AudioPlayer *ap) {
    SDL_LockMutex(ap->status_mutex);
    const StatusSnapshot s = ap->status;
    SDL_UnlockMutex(ap->status_mutex);
    return s;
}

int32_t au_get_current_handle(void) {
    AudioPlayer *ap = audio_player;
    return ap ? read_status(ap).handle : 0;
}

double au_get_audio_play_time(void) {
    AudioPlayer *ap = audio_player;
    if (!ap) return -1;
    const StatusSnapshot s = read_status(ap);
    if (!s.handle || s.position < 0) return -1;
    if (!s.advancing) return s.position;

    // The position is that of the last sample handed to SDL; what is audible lags by the buffered audio
    const double period = s.period;
    const double now = (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency();
    const double elapsed = FFMIN(FFMAX(now - s.callback_time, 0.0), period);
    const double speed = s.speed > 0 ? s.speed : 1.0;
    return FFMAX(0.0, s.position + (elapsed - 2 * period) * speed);
}

double au_get_audio_duration(void) {
    AudioPlayer *ap = audio_player;
    if (!ap) return -1;
    const StatusSnapshot s = read_status(ap);
    return s.handle ? s.duration : -1;
}

void au_wait_loop(void) {
    // au_shutdown() on another thread clears audio_player under the lifecycle lock before it frees the player, so a
    // player read under that lock is still alive
    SDL_mutex *lock = lifecycle_mutex();
    for (;;) {
        SDL_LockMutex(lock);
        const AudioPlayer *ap = audio_player;
        const bool done = !ap || SDL_AtomicGet((SDL_atomic_t *)&ap->shutting_down);
        SDL_UnlockMutex(lock);
        if (done) return;
        SDL_Delay(10);
    }
}

const char *au_get_version(void) {
#define FFA_STR2(x) #x
#define FFA_STR(x) FFA_STR2(x)
    return FFA_STR(FFAUDIO_VERSION_MAJOR) "." FFA_STR(FFAUDIO_VERSION_MINOR) "." FFA_STR(FFAUDIO_VERSION_PATCH);
}
