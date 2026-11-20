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

#ifndef FFAUDIO_OUTPUT_H
#define FFAUDIO_OUTPUT_H

#include "globals.h"

// Control thread: open the configured device (custom or default). Leaves it paused.
int output_open(AudioPlayer *ap, bool use_default, const char *device_name, int device_index);

// Control thread: close the device (waits for a running callback to return)
void output_close(AudioPlayer *ap);

// Control thread: start/stop the SDL callback
void output_set_running(AudioPlayer *ap, bool running);

// Gain (0..1) of a fade curve at t in [0, 1]
float fade_curve_gain(const AuFade *fade, float t);

// Control thread, device lock held: start a fade-out of `outgoing` and a fade-in of `incoming`.
// Either may be NULL. Returns false, changing nothing, when the fade is shorter than one frame (or no device is open
// yet); the caller then cuts.
bool mixer_start_transition(AudioPlayer *ap, TrackState *outgoing, TrackState *incoming, const AuFade *fade);

// Control thread, device lock held: fade the master gain for pause (to 0) or resume (to 1)
void mixer_set_pause(AudioPlayer *ap, bool paused, const AuFade *fade);

#endif //FFAUDIO_OUTPUT_H
