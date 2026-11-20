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

#ifndef FFAUDIO_DELAGATES_H
#define FFAUDIO_DELAGATES_H

#include <stdbool.h>
#include <stdint.h>

enum AU_LOG_LEVEL {
    AU_LOG_INFO = 0,
    AU_LOG_WARNING = 1,
    AU_LOG_ERROR = 2,
    AU_LOG_FATAL = 3
};

/*
 * All callbacks run on ffaudio's control thread, one at a time, never on the audio thread.
 * Calling any au_* function from inside a callback is allowed (except au_shutdown); those calls are
 * queued and run right after the callback returns.
 * `user_data` is InitializeConfig.user_data.
 */

// A message from the library. `request` is the number of API requests made when it was logged.
typedef void (*NotifyOfLog)(const char *message, int64_t request, enum AU_LOG_LEVEL level, void *user_data);

// Called exactly once for every handle returned by au_play_audio() / au_set_next(), when that track
// leaves the player. is_eof_from_skip: it was replaced, skipped, cleared or stopped.
// is_from_error: it could not be opened or decoding failed. Both false: it played to the end.
typedef void (*NotifyOfEndOfFile)(bool is_eof_from_skip, bool is_from_error, int32_t handle, void *user_data);

// The current track restarted from `position` (seconds): after a seek, or after a loop when is_from_looping.
typedef void (*NotifyOfRestart)(int32_t handle, double position, bool is_from_looping, int32_t remaining_loop_count, void *user_data);

// The duration (seconds) of a track became known or changed.
typedef void (*NotifyOfDurationUpdate)(int32_t handle, double new_duration, void *user_data);

// The current track is close to its end (see InitializeConfig.prepare_next_seconds) and nothing is queued:
// a good moment to call au_set_next(). Also called when a queued track fails to open.
typedef void (*NotifyOfPrepareNext)(int32_t current_handle, void *user_data);

// A track became the current (audible) track: at the start of playback, after a gapless/crossfade
// transition, or after a skip. au_get_audio_play_time()/au_get_audio_duration() now refer to it.
typedef void (*NotifyOfTrackStarted)(int32_t handle, void *user_data);

// The output device was opened or changed. Useful to rebuild channel-dependent filter strings.
typedef void (*NotifyOfAudioDeviceUpdate)(int32_t channel_count, int32_t sample_rate, void *user_data);

// A network track waits for data (buffering = true: before it starts, or after the network fell behind) or plays
// again (false). While buffering it is called about every 500 ms with how much audio is queued; its position does
// not move meanwhile.
typedef void (*NotifyOfBuffering)(int32_t handle, bool buffering, double buffered_seconds, void *user_data);

// The "now playing" information of a stream changed: Icecast/SHOUTcast titles, or new tags inside an Ogg/Opus
// stream. Called when that part is heard (not when it is read, which can be up to max_buffer_seconds earlier).
// Icecast's "Artist - Title" is split at " - "; artist is NULL when there is no artist. au_get_track_info() shows
// the same values from then on.
typedef void (*NotifyOfStreamMetadata)(int32_t handle, const char *title, const char *artist, void *user_data);

// An output device was added or removed (headphones plugged in, a USB/Bluetooth device appeared, ...).
// Call au_get_audio_devices() for the new list.
typedef void (*NotifyOfAudioDevicesChanged)(void *user_data);

#endif //FFAUDIO_DELAGATES_H
