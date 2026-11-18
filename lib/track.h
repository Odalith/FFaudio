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

// Control thread: allocate a track and start its read thread. NULL on allocation failure.
TrackState *track_create(AudioPlayer *ap, int32_t handle, const char *filename, const PlayAudioConfig *config);

// Control thread: stop the track's threads and free it. The track must not be in a mixer slot.
void track_destroy(TrackState *t);

// Control thread: seek to an absolute file position (AV_TIME_BASE units)
void track_request_seek(TrackState *t, int64_t pos_us);

// Control thread: seek to a percentage of the duration, resolved once the duration is known
void track_request_seek_percent(TrackState *t, double percent);

// Any thread: recompute the ReplayGain gain from the current settings (no-op until the tags are known)
void track_update_gain(TrackState *t);

// Any thread: a copy of the track's media info, NULL before the file is open
AuMediaInfo *track_copy_info(TrackState *t);

// Any thread: replace the track's own filters (NULL = none); applied before the next decoded frame
int track_set_filters(TrackState *t, const char *filters);

// Any thread: run avfilter_graph_send_command() on the track's user graph before the next decoded frame
int track_queue_filter_command(TrackState *t, const char *target, const char *cmd, const char *arg);

// Any thread: duration in seconds of the part that will play, -1 when unknown
double track_duration(TrackState *t);

// Any thread: file position (seconds) at which playback of this track ends, -1 when unknown
double track_end_position(TrackState *t);

// Any thread: first file position (seconds) this track plays from
double track_start_position(const TrackState *t);

// SDL callback: copy up to `frames` interleaved float frames into dst without blocking.
// *eos is set when the track has delivered all of its audio. *looped is set when playback wrapped around.
int track_pull(TrackState *t, float *dst, int frames, int rate, int channels, bool *eos, bool *looped);

// SDL callback: whether decoded audio is waiting to be played
bool track_has_audio(TrackState *t);

#endif //FFAUDIO_TRACK_H
