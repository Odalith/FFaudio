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

#ifndef FFAUDIO_FILTERGRAPH_H
#define FFAUDIO_FILTERGRAPH_H

#include "globals.h"

// (Re)builds the track's filter graph for the current output format.
// Returns AVERROR(EAGAIN) when no output device has been configured yet.
int configure_audio_filters(AudioPlayer *ap, TrackState *is);

// Frees the track's filter graph and clears the in/out filter pointers
void free_audio_filters(TrackState *is);

// Whether aresample can use the higher quality soxr resampler
bool filtergraph_soxr_available(void);

// Checks that a user filter string parses and links (>= 0 on success)
int filtergraph_validate(const char *filtergraph);

#endif //FFAUDIO_FILTERGRAPH_H
