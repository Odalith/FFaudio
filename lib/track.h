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

#ifndef FFAUDIO_TRACK_H
#define FFAUDIO_TRACK_H

#include "globals.h"

// Control thread: allocate a track (its worker does not run yet). NULL on allocation failure.
TrackState *track_create(AudioPlayer *ap, int32_t handle, const char *filename, const AuPlayConfig *config);

// Control thread: start the track's worker, once the track is registered: the worker publishes its details for
// au_track_info() as soon as the file is open (docs/threading.md, "Creating a track"). < 0 when it could not start;
// the track can then be destroyed at once.
int track_start(TrackState *t);

// Control thread: stop the track's threads and free it. The track must not be in a mixer slot. Waits for the threads.
void track_destroy(TrackState *t);

// The same in two steps, so the control thread never waits for a worker (one stuck in a DNS lookup can take seconds):
// track_stop() asks the threads to end; once track_stopped() is true, track_destroy() frees the track at once.
// The worker posts MSG_TRACK_EXITED when it is done.
void track_stop(TrackState *t);
bool track_stopped(TrackState *t);

// Control thread: seek to this many seconds into the track (into its range); exact to the sample
void track_request_seek(TrackState *t, double seconds);

// Control thread: seek to a percentage of the duration (of the range), resolved once the duration is known
void track_request_seek_percent(TrackState *t, double percent);

// Any thread: recompute the ReplayGain gain from the current settings (no-op until the tags are known)
// late: the value arrived while the track may already be playing (a measurement): a change ramps over 2 s
void track_update_gain(TrackState *t, bool late);



// Any thread: replace the track's own filters (NULL = none); applied before the next decoded frame
int track_set_filters(TrackState *t, const char *filters);

// Any thread: run avfilter_graph_send_command() on the track's user graph before the next decoded frame
int track_queue_filter_command(TrackState *t, const char *target, const char *cmd, const char *arg);

// Any thread: duration in seconds of the part that will play (the range), -1 when unknown. Positions count from 0
// at the start of that part, so the track ends at this position.
double track_duration(TrackState *t);

// SDL callback: the track has started, or can start now (enough decoded, or all of it). track_pull() gives nothing
// before this is true.
bool track_ready(TrackState *t, int rate, int channels);

// SDL callback: copy up to `frames` interleaved float frames into dst without blocking.
// *eos is set when the track has delivered all of its audio. *looped is set when playback wrapped around.
int track_pull(TrackState *t, float *dst, int frames, int rate, int channels, bool *eos, bool *looped);

// SDL callback: whether decoded audio is waiting to be played
bool track_has_audio(TrackState *t, int rate, int channels);

// SDL callback: drop audio the mixer can not play (from before a seek, or for another output format)
void track_drop_stale(TrackState *t, int rate, int channels);

// Ring storage pool (AudioPlayer.ring_pool): give a buffer back, or free all at shutdown
void ring_storage_give(AudioPlayer *ap, float *buf, size_t floats);
void ring_storage_free_all(AudioPlayer *ap);

// Control thread: take the first queued "now playing" entry at or before `position` (NULL when none), and put its
// title/artist into the track's info. Free the result with track_free_stream_meta().
StreamMeta *track_take_stream_meta(TrackState *t, double position);
void track_free_stream_meta(StreamMeta *m);

#endif //FFAUDIO_TRACK_H
