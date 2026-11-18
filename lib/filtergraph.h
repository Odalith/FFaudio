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

/*
 * Each track runs two graphs:
 *
 *   convert: abuffer (decoder format) -> aresample (float, output rate + layout) -> aformat -> abuffersink
 *   user:    abuffer (output format) -> [track filters] -> [global filters] -> aformat -> abuffersink
 *
 * The convert graph is rebuilt only when the decoded format, the serial (seek) or the device changes.
 * The user graph exists only when filters are set and can be replaced while playing without
 * restarting the resampler.
 */

// (Re)builds the track's convert graph for the current output format.
// Returns AVERROR(EAGAIN) when no output device has been configured yet.
int configure_convert_graph(AudioPlayer *ap, TrackState *t);

// Builds a user graph for the given filter strings (either may be NULL) at the given output format.
// With no filters at all, *out is left empty and 0 is returned.
int build_user_graph(FilterChain *out, const char *track_filters, const char *global_filters,
                     int rate, const AVChannelLayout *layout, int nb_threads);

void free_filter_chain(FilterChain *c);

// Checks that custom downmix filters accept a 7.1 input and link to a stereo output (>= 0 on success)
int validate_downmix_filters(const char *filters);

// Whether aresample can use the higher quality soxr resampler
bool filtergraph_soxr_available(void);

#endif //FFAUDIO_FILTERGRAPH_H
