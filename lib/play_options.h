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

#ifndef FFAUDIO_PLAY_OPTIONS_H
#define FFAUDIO_PLAY_OPTIONS_H

#include "ffaudio.h"

// The options of one au_play()/au_queue() (ffaudio.h). An object is used by one thread at a time; au_play() and
// au_queue() take a copy on the calling thread, which travels in the message and is then the track's.
struct AuPlayOptions {
    double start_seconds;           // >= 0; 0 = from the start
    double length_seconds;          // >= 0; 0 = to the end
    char *filters;                  // owned; NULL = none
    char *http_headers;             // owned; NULL = none
    AuFade fade_in;
    bool has_fade_in;               // false = the AuCrossfade setting
    AuReplayGain replaygain;
    bool has_replaygain;            // false = the ReplayGain cache, then the file's tags
};

// A copy of o (NULL for NULL, or without memory: *failed then says which)
AuPlayOptions *play_options_clone(const AuPlayOptions *o, bool *failed);

// A fade that au_set_crossfade() and au_play_options_set_fade_in() take: finite seconds and a known curve, with a
// function for AU_FADE_CUSTOM
bool ff_fade_valid(const AuFade *fade);

#endif //FFAUDIO_PLAY_OPTIONS_H
