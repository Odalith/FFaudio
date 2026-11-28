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
#include "numbers.h"

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
    // aresample only accepts resampler=soxr when libswresample was built with libsoxr.
    // av_opt_set alone cannot prove that: the "soxr" constant is in the option table either
    // way, and the missing engine only surfaces when it is instantiated at swr_init().
    // Probe with real in/out parameters and require a successful init.
    AVChannelLayout in_layout, out_layout;
    av_channel_layout_default(&in_layout, 2);
    av_channel_layout_default(&out_layout, 2);
    SwrContext *swr = NULL;
    const int ret = swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_FLT, 48000,
                                        &in_layout, AV_SAMPLE_FMT_FLT, 48000, 0, NULL);
    bool ok = false;
    if (ret >= 0 && swr)
        ok = av_opt_set(swr, "resampler", "soxr", 0) >= 0 && swr_init(swr) >= 0;
    swr_free(&swr);
    av_channel_layout_uninit(&in_layout);
    av_channel_layout_uninit(&out_layout);
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

int build_speed_graph(FilterChain *out, const int rate, const AVChannelLayout *layout, const double speed,
                      const bool keep_pitch, const bool use_soxr, const int nb_threads) {
    memset(out, 0, sizeof(*out));
    char layout_str[128], args[256];
    int ret = describe_layout(layout, layout_str, sizeof(layout_str));
    if (ret < 0) return ret;

    AVFilterContext *src = NULL, *a = NULL, *b = NULL, *format = NULL, *sink = NULL;
    if (!(out->graph = avfilter_graph_alloc())) return AVERROR(ENOMEM);
    out->graph->nb_threads = nb_threads;
    if ((ret = create_float_source(out->graph, &src, rate, layout_str)) < 0) goto fail;
    if ((ret = create_output_end(out->graph, &format, &sink, rate, layout_str)) < 0) goto fail;

    if (keep_pitch) {
        // Two atempo stages at sqrt(speed) each keep both in their best range (0.5..2) for 0.25..4x;
        // their instance names let speed changes be sent as commands without rebuilding
        char tempo[32];
        snprintf(args, sizeof(args), "tempo=%s", ff_format_number(tempo, sizeof(tempo), sqrt(speed), 9, 'f'));
        if ((ret = avfilter_graph_create_filter(&a, avfilter_get_by_name("atempo"), SPEED_FILTER_1, args, NULL, out->graph)) < 0 ||
            (ret = avfilter_graph_create_filter(&b, avfilter_get_by_name("atempo"), SPEED_FILTER_2, args, NULL, out->graph)) < 0)
            goto fail;
    }
    else {
        // Tape-style: relabel the sample rate (faster and higher), then resample back to the output rate
        snprintf(args, sizeof(args), "sample_rate=%d", (int)lrint(rate * speed));
        if ((ret = avfilter_graph_create_filter(&a, avfilter_get_by_name("asetrate"), "ffaudio_setrate", args, NULL, out->graph)) < 0)
            goto fail;
        snprintf(args, sizeof(args), "%sout_sample_rate=%d", use_soxr ? "resampler=soxr:precision=28:" : "", rate);
        if ((ret = avfilter_graph_create_filter(&b, avfilter_get_by_name("aresample"), "ffaudio_speed_resample", args, NULL, out->graph)) < 0)
            goto fail;
    }
    if ((ret = avfilter_link(src, 0, a, 0)) < 0 || (ret = avfilter_link(a, 0, b, 0)) < 0 ||
        (ret = avfilter_link(b, 0, format, 0)) < 0)
        goto fail;
    if ((ret = avfilter_graph_config(out->graph, NULL)) < 0) goto fail;
    out->src = src;
    out->sink = sink;
    return 0;

fail:
    free_filter_chain(out);
    return ret;
}

// Build a convert graph:
// abuffer (src_args) -> [source filters] -> [downmix/upmix filters] -> aresample -> aformat -> abuffersink
static int build_convert_graph(FilterChain *out, const char *src_args, const char *source_filters,
                               const char *downmix_filters, const char *downmix_maxval, const bool use_soxr,
                               const int out_rate, const char *out_layout, const int nb_threads) {
    char args[512];
    AVFilterContext *src = NULL, *mid = NULL, *convert = NULL, *format = NULL, *sink = NULL;
    int ret;

    memset(out, 0, sizeof(*out));
    if (!(out->graph = avfilter_graph_alloc())) return AVERROR(ENOMEM);
    out->graph->nb_threads = nb_threads;

    if ((ret = avfilter_graph_create_filter(&src, avfilter_get_by_name("abuffer"), "ffaudio_abuffer",
                                            src_args, NULL, out->graph)) < 0) goto end;

    // rematrix_maxval decides whether swr scales the downmix matrix down to avoid clipping. Its default
    // depends on the internal sample format (it normalizes for integer input such as 16-bit FLAC), so
    // it is always set explicitly. This is what made 5.1 files ~7.7 dB quieter than in mpv (issue #19).
    ret = snprintf(args, sizeof(args), "%sout_sample_rate=%d:out_chlayout=%s:out_sample_fmt=flt:rematrix_maxval=%s",
                   use_soxr ? "resampler=soxr:precision=28:" : "", out_rate, out_layout, downmix_maxval);
    if (ret < 0 || ret >= (int)sizeof(args)) { ret = AVERROR(EINVAL); goto end; }
    if ((ret = avfilter_graph_create_filter(&convert, avfilter_get_by_name("aresample"), "ffaudio_convert",
                                            args, NULL, out->graph)) < 0) goto end;

    if ((ret = create_output_end(out->graph, &format, &sink, out_rate, out_layout)) < 0) goto end;
    mid = src;
    if (source_filters && source_filters[0]) {
        // Two independent strings: chain them through a pass-through filter
        if ((ret = avfilter_graph_create_filter(&mid, avfilter_get_by_name("anull"), "ffaudio_source_end",
                                                NULL, NULL, out->graph)) < 0) goto end;
        if ((ret = link_filters(out->graph, source_filters, src, mid)) < 0) goto end;
    }
    if ((ret = link_filters(out->graph, downmix_filters, mid, convert)) < 0) goto end;
    if ((ret = avfilter_link(convert, 0, format, 0)) < 0) goto end;
    if ((ret = avfilter_graph_config(out->graph, NULL)) < 0) goto end;

    out->src = src;
    out->sink = sink;

end:
    if (ret < 0)
        free_filter_chain(out);
    return ret;
}

int validate_upmix_filters(const char *filters) {
    if (!filters || !filters[0]) return AVERROR(EINVAL);
    FilterChain c = {0};
    const FfLocale saved = ff_c_locale_begin(); // parsed on the caller's thread
    const int ret = build_convert_graph(&c, "sample_rate=48000:sample_fmt=flt:time_base=1/48000:channel_layout=stereo",
                                        NULL, filters, "1000", false, 48000, "5.1(side)", 0);
    ff_c_locale_end(saved);
    free_filter_chain(&c);
    return ret;
}

int validate_source_filters(const char *filters) {
    if (!filters || !filters[0]) return 0;
    FilterChain c = {0};
    const FfLocale saved = ff_c_locale_begin(); // parsed on the caller's thread
    const int ret = build_convert_graph(&c, "sample_rate=44100:sample_fmt=s16:time_base=1/44100:channel_layout=stereo",
                                        filters, NULL, "1000", false, 48000, "stereo", 0);
    ff_c_locale_end(saved);
    free_filter_chain(&c);
    return ret;
}

int validate_downmix_filters(const char *filters) {
    if (!filters || !filters[0]) return AVERROR(EINVAL);
    FilterChain c = {0};
    const FfLocale saved = ff_c_locale_begin(); // parsed on the caller's thread
    const int ret = build_convert_graph(&c, "sample_rate=48000:sample_fmt=flt:time_base=1/48000:channel_layout=7.1",
                                        NULL, filters, "1000", false, 48000, "stereo", 0);
    ff_c_locale_end(saved);
    free_filter_chain(&c);
    return ret;
}

int configure_convert_graph(AudioPlayer *ap, TrackState *t) {
    if (!ap || !t) return AVERROR(EINVAL);

    char src_args[512], src_layout[128], out_layout[128];
    char *custom = NULL;
    int ret;

    // Snapshot the output format: the control thread may change it while the device is reconfigured
    SDL_LockMutex(ap->format_mutex);
    const int out_rate = ap->audio_target.freq;
    const int out_channels = ap->audio_target.ch_layout.nb_channels;
    const bool ready = ap->output_ready;
    const AU_DOWNMIX mode = ap->downmix;
    const AU_UPMIX upmode = ap->upmix;
    ret = ready ? describe_layout(&ap->audio_target.ch_layout, out_layout, sizeof(out_layout)) : 0;
    if (ret >= 0 && mode == AU_DOWNMIX_CUSTOM && ap->downmix_filters && !(custom = av_strdup(ap->downmix_filters)))
        ret = AVERROR(ENOMEM);
    char *up_custom = NULL, *source = NULL;
    if (ret >= 0 && upmode == AU_UPMIX_CUSTOM && ap->upmix_filters && !(up_custom = av_strdup(ap->upmix_filters)))
        ret = AVERROR(ENOMEM);
    if (ret >= 0 && ap->source_filters && !(source = av_strdup(ap->source_filters)))
        ret = AVERROR(ENOMEM);
    SDL_UnlockMutex(ap->format_mutex);
    if (ret < 0 || !ready || out_rate <= 0) {
        av_free(custom);
        av_free(up_custom);
        av_free(source);
        return ret < 0 ? ret : AVERROR(EAGAIN);
    }

    ret = describe_layout(&t->audio_filter_src.ch_layout, src_layout, sizeof(src_layout));
    if (ret >= 0) {
        ret = snprintf(src_args, sizeof(src_args), "sample_rate=%d:sample_fmt=%s:time_base=1/%d:channel_layout=%s",
                       t->audio_filter_src.freq, av_get_sample_fmt_name(t->audio_filter_src.fmt),
                       t->audio_filter_src.freq, src_layout);
        ret = (ret < 0 || ret >= (int)sizeof(src_args)) ? AVERROR(EINVAL) : 0;
    }
    if (ret < 0) {
        av_free(custom);
        av_free(up_custom);
        av_free(source);
        return ret;
    }

    // Downmix settings only matter when the file has more channels than the output, upmix settings when it has fewer
    // than a surround output
    const int src_channels = t->audio_filter_src.ch_layout.nb_channels;
    const bool downmixing = src_channels > out_channels;
    const bool upmixing = src_channels < out_channels && out_channels > 2;
    const char *maxval = (downmixing && mode == AU_DOWNMIX_NORMALIZED) ? "1" : "1000";
    char surround[192];
    const char *downmix_filters = NULL;   // the filters before the conversion, for either direction
    if (downmixing) downmix_filters = custom;
    else if (upmixing && upmode == AU_UPMIX_CUSTOM) downmix_filters = up_custom;
    else if (upmixing && upmode == AU_UPMIX_SURROUND && src_channels <= 2) {
        // surround takes stereo: a mono file becomes two identical channels, which it steers to the centre
        snprintf(surround, sizeof(surround), "aformat=channel_layouts=stereo,surround=chl_in=stereo:chl_out=%s",
                 out_layout);
        downmix_filters = surround;
    }

    // Never leave src/sink pointing into a graph that has been freed
    free_filter_chain(&t->convert);
    t->convert_rate = out_rate;
    t->convert_channels = out_channels;
    ret = build_convert_graph(&t->convert, src_args, source, downmix_filters, maxval, ap->use_soxr, out_rate,
                              out_layout, ap->filter_nbthreads);
    if (ret < 0 && downmix_filters) {
        ff_log(AU_LOG_WARNING, "%s \"%s\" failed for '%s' (%s): using the standard conversion",
               downmixing ? "Custom downmix" : "Upmix", downmix_filters, t->filename, av_err2str(ret));
        ret = build_convert_graph(&t->convert, src_args, source, NULL, "1000", ap->use_soxr, out_rate, out_layout,
                                  ap->filter_nbthreads);
    }
    if (ret < 0 && source) {
        // Validated against stereo: a layout or format they can not take plays without them
        ff_log(AU_LOG_WARNING, "Source filters \"%s\" failed for '%s' (%s): playing without them",
               source, t->filename, av_err2str(ret));
        ret = build_convert_graph(&t->convert, src_args, NULL, NULL, "1000", ap->use_soxr, out_rate, out_layout,
                                  ap->filter_nbthreads);
    }
    av_free(custom);
    av_free(up_custom);
    av_free(source);
    return ret;
}

