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

enum AU_LOG_LEVEL {
 INFO = 0,
 WARNING = 1,
 ERROR = 2,
 FATAL = 3
};

typedef void (*NotifyOfLog)(const char* message, int64_t request, enum AU_LOG_LEVEL level);
typedef void (*NotifyOfEndOfFile)(bool is_eof_from_skip, bool is_from_error, int32_t handle);
typedef void (*NotifyOfRestart)(double position, bool is_from_looping, int32_t remaining_loop_count);
typedef void (*NotifyOfDurationUpdate)(double new_duration);
typedef char* (*NotifyOfPrepareNext)();

#endif //FFAUDIO_DELAGATES_H