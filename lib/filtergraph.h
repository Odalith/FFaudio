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
#include "ffaudio.h"

void add_to_filter_chain_end(AudioPlayer *ap, const char *filter_name);

void add_to_filter_chain_start(AudioPlayer *ap, const char *filter_name);

void clear_filter_chain(AudioPlayer *ap);

void update_anequalizer_array(AudioPlayer *ap, const EqualizerConfig* params);

void update_anequalizer_str(AudioPlayer *ap, const int16_t channels);

int configure_filtergraph(AVFilterGraph *graph, const char *filtergraph,
                                 AVFilterContext *source_ctx, AVFilterContext *sink_ctx);

int configure_audio_filters(const AudioPlayer *ap, TrackState *is, const char *track_filters, const bool force_output_format);

#endif //FFAUDIO_FILTERGRAPH_H