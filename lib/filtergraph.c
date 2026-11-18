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

// Parse `filtergraph` and link it between source_ctx and sink_ctx (a plain link when empty)
static int link_filters(AVFilterGraph *graph, const char *filtergraph,
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
    return 0;
}

void free_filter_chain(FilterChain *c) {
    // Freeing the graph frees every filter in it, including the source and sink
    avfilter_graph_free(&c->graph);
    c->src = NULL;
    c->sink = NULL;
}

bool filtergraph_soxr_available(void) {
    // aresample only accepts resampler=soxr when libswresample was built with libsoxr
    SwrContext *swr = swr_alloc();
    if (!swr) return false;
    const bool ok = av_opt_set(swr, "resampler", "soxr", 0) >= 0;
    swr_free(&swr);
    return ok;
}

static int describe_layout(const AVChannelLayout *layout, char *buf, const size_t size) {
    const int ret = av_channel_layout_describe(layout, buf, size);
    return ret < 0 ? ret : (ret > (int)size ? AVERROR(EINVAL) : 0);
}

// abuffer for float frames at the output format
static int create_float_source(AVFilterGraph *graph, AVFilterContext **src, const int rate, const char *layout) {
    char args[256];
    const int ret = snprintf(args, sizeof(args), "sample_rate=%d:sample_fmt=flt:time_base=1/%d:channel_layout=%s",
                             rate, rate, layout);
    if (ret < 0 || ret >= (int)sizeof(args)) return AVERROR(EINVAL);
    return avfilter_graph_create_filter(src, avfilter_get_by_name("abuffer"), "ffaudio_src", args, NULL, graph);
}

// aformat (forces the output format) followed by abuffersink
static int create_output_end(AVFilterGraph *graph, AVFilterContext **format, AVFilterContext **sink,
                             const int rate, const char *layout) {
    char args[256];
    int ret = snprintf(args, sizeof(args), "sample_fmts=flt:sample_rates=%d:channel_layouts=%s", rate, layout);
    if (ret < 0 || ret >= (int)sizeof(args)) return AVERROR(EINVAL);
    if ((ret = avfilter_graph_create_filter(format, avfilter_get_by_name("aformat"), "ffaudio_output_format",
                                            args, NULL, graph)) < 0)
        return ret;
    if ((ret = avfilter_graph_create_filter(sink, avfilter_get_by_name("abuffersink"), "ffaudio_sink",
                                            NULL, NULL, graph)) < 0)
        return ret;
    return avfilter_link(*format, 0, *sink, 0);
}

int build_user_graph(FilterChain *out, const char *track_filters, const char *global_filters,
                     const int rate, const AVChannelLayout *layout, const int nb_threads) {
    memset(out, 0, sizeof(*out));
    const bool has_track = track_filters && track_filters[0];
    const bool has_global = global_filters && global_filters[0];
    if (!has_track && !has_global) return 0;

    char layout_str[128];
    int ret = describe_layout(layout, layout_str, sizeof(layout_str));
    if (ret < 0) return ret;

    AVFilterContext *src = NULL, *mid = NULL, *format = NULL, *sink = NULL;
    if (!(out->graph = avfilter_graph_alloc())) return AVERROR(ENOMEM);
    out->graph->nb_threads = nb_threads;

    if ((ret = create_float_source(out->graph, &src, rate, layout_str)) < 0) goto fail;
    if ((ret = create_output_end(out->graph, &format, &sink, rate, layout_str)) < 0) goto fail;

    if (has_track && has_global) {
        // Two independent strings: chain them through a pass-through filter
        if ((ret = avfilter_graph_create_filter(&mid, avfilter_get_by_name("anull"), "ffaudio_between",
                                                NULL, NULL, out->graph)) < 0) goto fail;
        if ((ret = link_filters(out->graph, track_filters, src, mid)) < 0) goto fail;
        if ((ret = link_filters(out->graph, global_filters, mid, format)) < 0) goto fail;
    }
    else {
        if ((ret = link_filters(out->graph, has_track ? track_filters : global_filters, src, format)) < 0) goto fail;
    }

    if ((ret = avfilter_graph_config(out->graph, NULL)) < 0) goto fail;
    out->src = src;
    out->sink = sink;
    return 0;

fail:
    free_filter_chain(out);
    return ret;
}

int configure_convert_graph(AudioPlayer *ap, TrackState *t) {
    if (!ap || !t) return AVERROR(EINVAL);

    char args[512], src_layout[128], out_layout[128];
    AVFilterContext *src = NULL, *convert = NULL, *format = NULL, *sink = NULL;
    int ret;

    // Snapshot the output format: the control thread may change it while the device is reconfigured
    SDL_LockMutex(ap->format_mutex);
    const int out_rate = ap->audio_target.freq;
    const bool ready = ap->output_ready;
    ret = ready ? describe_layout(&ap->audio_target.ch_layout, out_layout, sizeof(out_layout)) : 0;
    SDL_UnlockMutex(ap->format_mutex);
    if (ret < 0) return ret;
    if (!ready || out_rate <= 0) return AVERROR(EAGAIN);

    if ((ret = describe_layout(&t->audio_filter_src.ch_layout, src_layout, sizeof(src_layout))) < 0)
        return ret;

    // Never leave src/sink pointing into a graph that has been freed
    free_filter_chain(&t->convert);
    if (!(t->convert.graph = avfilter_graph_alloc())) return AVERROR(ENOMEM);
    t->convert.graph->nb_threads = ap->filter_nbthreads;
    t->convert_rate = out_rate;

    ret = snprintf(args, sizeof(args), "sample_rate=%d:sample_fmt=%s:time_base=1/%d:channel_layout=%s",
                   t->audio_filter_src.freq, av_get_sample_fmt_name(t->audio_filter_src.fmt),
                   t->audio_filter_src.freq, src_layout);
    if (ret < 0 || ret >= (int)sizeof(args)) { ret = AVERROR(EINVAL); goto end; }
    if ((ret = avfilter_graph_create_filter(&src, avfilter_get_by_name("abuffer"), "ffaudio_abuffer",
                                            args, NULL, t->convert.graph)) < 0) goto end;

    ret = snprintf(args, sizeof(args), "%sout_sample_rate=%d:out_chlayout=%s:out_sample_fmt=flt",
                   ap->use_soxr ? "resampler=soxr:precision=28:" : "", out_rate, out_layout);
    if (ret < 0 || ret >= (int)sizeof(args)) { ret = AVERROR(EINVAL); goto end; }
    if ((ret = avfilter_graph_create_filter(&convert, avfilter_get_by_name("aresample"), "ffaudio_convert",
                                            args, NULL, t->convert.graph)) < 0) goto end;

    if ((ret = create_output_end(t->convert.graph, &format, &sink, out_rate, out_layout)) < 0) goto end;
    if ((ret = avfilter_link(src, 0, convert, 0)) < 0) goto end;
    if ((ret = avfilter_link(convert, 0, format, 0)) < 0) goto end;
    if ((ret = avfilter_graph_config(t->convert.graph, NULL)) < 0) goto end;

    t->convert.src = src;
    t->convert.sink = sink;

end:
    if (ret < 0)
        free_filter_chain(&t->convert);
    return ret;
}
