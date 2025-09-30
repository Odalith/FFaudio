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

void add_to_filter_chain(AudioPlayer *ap, const char *filter_name)
{
    if (ap->audio_filters && strlen(ap->audio_filters) > 0) {
        // Combine user filters with loudnorm
        ap->audio_filters = av_asprintf("%s,%s", ap->audio_filters, filter_name);
    } else {
        ap->audio_filters = av_strdup(filter_name);
    }
}

void clear_filter_chain(AudioPlayer *ap) {
    av_freep(&ap->audio_filters);
}


int configure_filtergraph(AVFilterGraph *graph, const char *filtergraph,
                                 AVFilterContext *source_ctx, AVFilterContext *sink_ctx)
{
    int ret;
    const int nb_filters = graph->nb_filters;


    if (filtergraph) {
        AVFilterInOut *outputs = avfilter_inout_alloc();
        AVFilterInOut *inputs  = avfilter_inout_alloc();
        if (!outputs || !inputs) {
            ret = AVERROR(ENOMEM);
            return ret;
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

        if (ret < 0) {
            return ret;
        }

    }
    else if ((ret = avfilter_link(source_ctx, 0, sink_ctx, 0)) < 0) {
        return ret;
    }

    /* Reorder the filters to ensure that inputs of the custom filters are merged first */
    for (int i = 0; i < graph->nb_filters - nb_filters; i++) {
        FFSWAP(AVFilterContext*, graph->filters[i], graph->filters[i + nb_filters]);
    }

    ret = avfilter_graph_config(graph, NULL);

    return ret;
}

int configure_audio_filters(AudioPlayer *ap, TrackState *is, const char *afilters, int force_output_format)
{
    static const enum AVSampleFormat sample_fmts[] = { AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_NONE };
    int sample_rates[2] = { 0, -1 };
    AVFilterContext *audio_source_filter = NULL, *audio_sink_filter = NULL;
    char aresample_swr_opts[512] = "";
    const AVDictionaryEntry *e = NULL;
    AVBPrint bp;
    char asrc_args[256];
    int ret;

    avfilter_graph_free(&is->agraph);
    if (!(is->agraph = avfilter_graph_alloc()))
        return AVERROR(ENOMEM);
    is->agraph->nb_threads = ap->filter_nbthreads;

    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_AUTOMATIC);

    //Todo swr_opts was empty so swr_opts_n is left null but this should probably be implemented at some point
    while ((e = av_dict_iterate(ap->swr_opts_n, e))) {
        av_strlcatf(aresample_swr_opts, sizeof(aresample_swr_opts), "%s=%s:", e->key, e->value);
    }

    if (strlen(aresample_swr_opts)) {
        aresample_swr_opts[strlen(aresample_swr_opts)-1] = '\0';
    }
    av_opt_set(is->agraph, "aresample_swr_opts", aresample_swr_opts, 0);

    av_channel_layout_describe_bprint(&is->audio_filter_src.ch_layout, &bp);

    ret = snprintf(asrc_args, sizeof(asrc_args),
                   "sample_rate=%d:sample_fmt=%s:time_base=%d/%d:channel_layout=%s",
                   is->audio_filter_src.freq, av_get_sample_fmt_name(is->audio_filter_src.fmt),
                   1, is->audio_filter_src.freq, bp.str);

    ret = avfilter_graph_create_filter(&audio_source_filter,
                                       avfilter_get_by_name("abuffer"), "ffaudio_abuffer",
                                       asrc_args, NULL, is->agraph);
    if (ret < 0) goto end;


    ret = avfilter_graph_create_filter(&audio_sink_filter,
                                       avfilter_get_by_name("abuffersink"), "ffaudio_abuffersink",
                                       NULL, NULL, is->agraph);
    if (ret < 0) goto end;

    if ((ret = av_opt_set_int_list(audio_sink_filter, "sample_fmts", sample_fmts,  AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0) goto end;
    if ((ret = av_opt_set_int(audio_sink_filter, "all_channel_counts", 1, AV_OPT_SEARCH_CHILDREN)) < 0) goto end;

    if (force_output_format) {
        av_bprint_clear(&bp);
        av_channel_layout_describe_bprint(&ap->audio_target->ch_layout, &bp);
        sample_rates[0] = ap->audio_target->freq;
        if ((ret = av_opt_set_int(audio_sink_filter, "all_channel_counts", 0, AV_OPT_SEARCH_CHILDREN)) < 0) goto end;
        if ((ret = av_opt_set(audio_sink_filter, "ch_layouts", bp.str, AV_OPT_SEARCH_CHILDREN)) < 0) goto end;
        if ((ret = av_opt_set_int_list(audio_sink_filter, "sample_rates", sample_rates, -1, AV_OPT_SEARCH_CHILDREN)) < 0) goto end;
    }

    if ((ret = configure_filtergraph(is->agraph, afilters, audio_source_filter, audio_sink_filter)) < 0) goto end;

    is->in_audio_filter  = audio_source_filter;
    is->out_audio_filter = audio_sink_filter;

end:
    if (ret < 0) {
        avfilter_graph_free(&is->agraph);
    }
    av_bprint_finalize(&bp, NULL);

    return ret;
}