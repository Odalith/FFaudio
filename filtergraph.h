//
// Created by malkj on 2025-09-18.
//

#ifndef FFAUDIO_FILTERGRAPH_H
#define FFAUDIO_FILTERGRAPH_H

#include "ffaudio.h"

void add_to_filter_chain(AudioPlayer *ap, const char *filter_name);

void clear_filter_chain(AudioPlayer *ap);

int configure_filtergraph(AVFilterGraph *graph, const char *filtergraph,
                                 AVFilterContext *source_ctx, AVFilterContext *sink_ctx);

int configure_audio_filters(AudioPlayer *ap, TrackState *is, const char *afilters, int force_output_format);

#endif //FFAUDIO_FILTERGRAPH_H