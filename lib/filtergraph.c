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

#include "filtergraph.h"

/*
 * Every track gets this graph:
 *
 *   abuffer (decoder format) -> aresample (to float, output rate + layout) -> [user filters] -> aformat -> abuffersink
 *
 * Converting first means user filters always see the output format, so channel-dependent filter strings
 * built for the device's channel count stay valid, and frames leaving the sink can be mixed directly.
 * The final aformat makes sure user filters that change the format are converted back.
 */

static int link_user_filters(AVFilterGraph *graph, const char *filtergraph,
                             AVFilterContext *source_ctx, AVFilterContext *sink_ctx) {
    int ret;
    const int nb_filters = graph->nb_filters;

    if (filtergraph && filtergraph[0]) {
        AVFilterInOut *outputs = avfilter_inout_alloc();
        AVFilterInOut *inputs  = avfilter_inout_alloc();
        if (!outputs || !inputs) {
            avfilter_inout_free(&outputs);
            avfilter_inout_free(&inputs);
            return AVERROR(ENOMEM);
        }

        outputs->name       = av_strdup("in");
        outputs->filter_ctx = source_ctx;
        outputs->pad_idx    = 0;
        outputs->next       = NULL;

        inputs->name        = av_strdup("out");
        inputs->filter_ctx  = sink_ctx;
        inputs->pad_idx     = 0;
        inputs->next        = NULL;

        ret = avfilter_graph_parse_ptr(graph, filtergraph, &inputs, &outputs, NULL);
        avfilter_inout_free(&outputs);
        avfilter_inout_free(&inputs);

        if (ret < 0)
            return ret;
    }
    else if ((ret = avfilter_link(source_ctx, 0, sink_ctx, 0)) < 0) {
        return ret;
    }

    /* Reorder the filters to ensure that inputs of the custom filters are merged first */
    for (int i = 0; i < (int)graph->nb_filters - nb_filters; i++) {
        FFSWAP(AVFilterContext*, graph->filters[i], graph->filters[i + nb_filters]);
    }

    return avfilter_graph_config(graph, NULL);
}

void free_audio_filters(TrackState *is) {
    // Freeing the graph frees every filter in it, including the source and sink
    avfilter_graph_free(&is->agraph);
    is->in_audio_filter = NULL;
    is->out_audio_filter = NULL;
}

bool filtergraph_soxr_available(void) {
    // aresample only accepts resampler=soxr when libswresample was built with libsoxr
    SwrContext *swr = swr_alloc();
    if (!swr) return false;
    const bool ok = av_opt_set(swr, "resampler", "soxr", 0) >= 0;
    swr_free(&swr);
    return ok;
}

int filtergraph_validate(const char *filtergraph) {
    // Build a throwaway graph with a fixed format to check that the string parses
    AVFilterGraph *graph = avfilter_graph_alloc();
    AVFilterContext *src = NULL, *sink = NULL;
    if (!graph) return AVERROR(ENOMEM);
    int ret = avfilter_graph_create_filter(&src, avfilter_get_by_name("abuffer"), "src",
                                           "sample_rate=48000:sample_fmt=flt:time_base=1/48000:channel_layout=stereo",
                                           NULL, graph);
    if (ret >= 0)
        ret = avfilter_graph_create_filter(&sink, avfilter_get_by_name("abuffersink"), "sink", NULL, NULL, graph);
    if (ret >= 0)
        ret = link_user_filters(graph, filtergraph, src, sink);
    avfilter_graph_free(&graph);
    return ret;
}

int configure_audio_filters(AudioPlayer *ap, TrackState *is) {
    if (!ap || !is) return AVERROR(EINVAL);

    AVFilterContext *src = NULL, *convert = NULL, *format = NULL, *sink = NULL;
    AVBPrint src_layout, out_layout;
    char args[512];
    int ret;

    // Snapshot the output format: the control thread may change it while the device is reconfigured
    SDL_LockMutex(ap->format_mutex);
    const int out_rate = ap->audio_target.freq;
    AVChannelLayout target_layout = {0};
    ret = av_channel_layout_copy(&target_layout, &ap->audio_target.ch_layout);
    const bool ready = ap->output_ready;
    SDL_UnlockMutex(ap->format_mutex);
    if (ret < 0) return ret;
    if (!ready || out_rate <= 0) {
        av_channel_layout_uninit(&target_layout);
        return AVERROR(EAGAIN);
    }

    av_bprint_init(&src_layout, 0, AV_BPRINT_SIZE_AUTOMATIC);
    av_bprint_init(&out_layout, 0, AV_BPRINT_SIZE_AUTOMATIC);
    av_channel_layout_describe_bprint(&is->audio_filter_src.ch_layout, &src_layout);
    av_channel_layout_describe_bprint(&target_layout, &out_layout);

    // Never leave in/out pointing into a graph that has been freed
    free_audio_filters(is);
    if (!(is->agraph = avfilter_graph_alloc())) { ret = AVERROR(ENOMEM); goto end; }
    is->agraph->nb_threads = ap->filter_nbthreads;

    ret = snprintf(args, sizeof(args), "sample_rate=%d:sample_fmt=%s:time_base=1/%d:channel_layout=%s",
                   is->audio_filter_src.freq, av_get_sample_fmt_name(is->audio_filter_src.fmt),
                   is->audio_filter_src.freq, src_layout.str);
    if (ret < 0 || ret >= (int)sizeof(args)) { ret = AVERROR(EINVAL); goto end; }
    if ((ret = avfilter_graph_create_filter(&src, avfilter_get_by_name("abuffer"), "ffaudio_abuffer",
                                            args, NULL, is->agraph)) < 0) goto end;

    ret = snprintf(args, sizeof(args), "%sout_sample_rate=%d:out_chlayout=%s:out_sample_fmt=flt",
                   ap->use_soxr ? "resampler=soxr:precision=28:" : "", out_rate, out_layout.str);
    if (ret < 0 || ret >= (int)sizeof(args)) { ret = AVERROR(EINVAL); goto end; }
    if ((ret = avfilter_graph_create_filter(&convert, avfilter_get_by_name("aresample"), "ffaudio_convert",
                                            args, NULL, is->agraph)) < 0) goto end;

    ret = snprintf(args, sizeof(args), "sample_fmts=flt:sample_rates=%d:channel_layouts=%s", out_rate, out_layout.str);
    if (ret < 0 || ret >= (int)sizeof(args)) { ret = AVERROR(EINVAL); goto end; }
    if ((ret = avfilter_graph_create_filter(&format, avfilter_get_by_name("aformat"), "ffaudio_output_format",
                                            args, NULL, is->agraph)) < 0) goto end;

    if ((ret = avfilter_graph_create_filter(&sink, avfilter_get_by_name("abuffersink"), "ffaudio_abuffersink",
                                            NULL, NULL, is->agraph)) < 0) goto end;

    if ((ret = avfilter_link(src, 0, convert, 0)) < 0) goto end;
    if ((ret = avfilter_link(format, 0, sink, 0)) < 0) goto end;
    if ((ret = link_user_filters(is->agraph, is->filtergraph_str, convert, format)) < 0) goto end;

    is->in_audio_filter  = src;
    is->out_audio_filter = sink;

end:
    if (ret < 0)
        free_audio_filters(is);
    av_bprint_finalize(&src_layout, NULL);
    av_bprint_finalize(&out_layout, NULL);
    av_channel_layout_uninit(&target_layout);
    return ret;
}
