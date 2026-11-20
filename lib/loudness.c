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

/*
 * ITU-R BS.1770-4 loudness measurement (EBU R128, EBU Tech 3341/3342), independent of playback.
 *
 * Each file is decoded with FFmpeg, converted to planar float at its own rate and layout, K-weighted, and
 * split into 100 ms sub-blocks. From those come the 400 ms gating blocks (integrated loudness, every 100 ms)
 * and the 3 s short-term windows (loudness range, every 100 ms). The scanner keeps the block energies of
 * every file so album loudness can be gated over all tracks together, as the standard requires.
 */

#include <math.h>
#include <stdlib.h>
#include "libavutil/avstring.h"
#include "libavutil/channel_layout.h"
#include "libavutil/mem.h"
#include "libavutil/samplefmt.h"
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libswresample/swresample.h"

#include "ffaudio_loudness.h"
#include "probe.h"
#include "network.h"

#define ABSOLUTE_GATE_LUFS (-70.0)
#define RELATIVE_GATE_LU (-10.0)
#define LRA_RELATIVE_GATE_LU (-20.0)
#define SUB_BLOCKS_PER_BLOCK 4      // 400 ms
#define SUB_BLOCKS_PER_SHORT_TERM 30 // 3 s
#define REPLAYGAIN_REFERENCE_LUFS (-18.0)
#define R128_REFERENCE_LUFS (-23.0)

typedef struct EnergyList {
    double *v;
    size_t n, cap;
} EnergyList;

struct AuLoudnessScanner {
    AuLoudnessConfig config;
    EnergyList blocks;          // gating block energies of every scanned track
    EnergyList short_term;      // short-term energies of every scanned track
    double sample_peak, true_peak, duration;
    int tracks;
};

typedef struct Meter {
    int rate, channels;
    double b[5], a[5];          // K-weighting: pre-filter (high shelf) and RLB high pass combined
    double (*z)[4];             // per channel filter state
    double *weight;             // per channel weight (0 = not measured, e.g. LFE)
    int sub_len;                // samples per 100 ms
    int sub_pos;
    double sub_energy;          // weighted sum of squares in the current sub-block
    double ring[SUB_BLOCKS_PER_SHORT_TERM];
    int ring_pos;
    int64_t sub_count;
    EnergyList blocks, short_term;
    double sample_peak, true_peak;
    int64_t samples;

    // True peak: upsampled copy of the signal
    SwrContext *tp;
    int tp_factor;
    uint8_t **tp_buf;
    int tp_buf_samples;
} Meter;

//+-----------------------------------------------------+
//| Energy lists and gating                             |
//+-----------------------------------------------------+

static int list_push(EnergyList *l, const double v) {
    if (l->n == l->cap) {
        const size_t cap = l->cap ? l->cap * 2 : 1024;
        double *nv = av_realloc_array(l->v, cap, sizeof(double));
        if (!nv) return AVERROR(ENOMEM);
        l->v = nv;
        l->cap = cap;
    }
    l->v[l->n++] = v;
    return 0;
}

static int list_append(EnergyList *dst, const EnergyList *src) {
    for (size_t i = 0; i < src->n; i++)
        if (list_push(dst, src->v[i]) < 0) return AVERROR(ENOMEM);
    return 0;
}

static void list_free(EnergyList *l) {
    av_freep(&l->v);
    l->n = l->cap = 0;
}

static double energy_to_lufs(const double e) { return -0.691 + 10.0 * log10(e); }
static double lufs_to_energy(const double l) { return pow(10.0, (l + 0.691) / 10.0); }

// Gated integrated loudness of a set of block energies; false when nothing passes the absolute gate
static bool integrated_loudness(const EnergyList *blocks, double *lufs) {
    const double abs_gate = lufs_to_energy(ABSOLUTE_GATE_LUFS);
    double sum = 0;
    size_t count = 0;
    for (size_t i = 0; i < blocks->n; i++)
        if (blocks->v[i] >= abs_gate) { sum += blocks->v[i]; count++; }
    if (!count) return false;

    const double rel_gate = (sum / (double)count) * pow(10.0, RELATIVE_GATE_LU / 10.0);
    const double gate = rel_gate > abs_gate ? rel_gate : abs_gate;
    sum = 0;
    count = 0;
    for (size_t i = 0; i < blocks->n; i++)
        if (blocks->v[i] >= gate) { sum += blocks->v[i]; count++; }
    if (!count) return false;
    *lufs = energy_to_lufs(sum / (double)count);
    return true;
}

static int compare_double(const void *a, const void *b) {
    const double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y);
}

// Loudness range (EBU Tech 3342): spread between the 10th and 95th percentile of the gated short-term loudness
static double loudness_range(const EnergyList *st) {
    const double abs_gate = lufs_to_energy(ABSOLUTE_GATE_LUFS);
    double sum = 0;
    size_t count = 0;
    for (size_t i = 0; i < st->n; i++)
        if (st->v[i] >= abs_gate) { sum += st->v[i]; count++; }
    if (count < 2) return 0;

    const double rel_gate = (sum / (double)count) * pow(10.0, LRA_RELATIVE_GATE_LU / 10.0);
    const double gate = rel_gate > abs_gate ? rel_gate : abs_gate;
    double *values = av_malloc_array(count, sizeof(double));
    if (!values) return 0;
    size_t n = 0;
    for (size_t i = 0; i < st->n; i++)
        if (st->v[i] >= gate) values[n++] = energy_to_lufs(st->v[i]);
    double lra = 0;
    if (n >= 2) {
        qsort(values, n, sizeof(double), compare_double);
        const size_t lo = (size_t)((double)(n - 1) * 0.10 + 0.5);
        const size_t hi = (size_t)((double)(n - 1) * 0.95 + 0.5);
        lra = values[hi] - values[lo];
    }
    av_free(values);
    return lra;
}

static void fill_result(AuLoudness *out, const EnergyList *blocks, const EnergyList *st,
                        const double sample_peak, const double true_peak, const double duration) {
    memset(out, 0, sizeof(*out));
    out->sample_peak = sample_peak;
    out->true_peak = true_peak;
    out->duration = duration;
    double lufs = ABSOLUTE_GATE_LUFS;
    out->silent = !integrated_loudness(blocks, &lufs);
    out->integrated_lufs = out->silent ? ABSOLUTE_GATE_LUFS : lufs;
    if (out->silent) return;

    out->loudness_range_lu = loudness_range(st);
    out->replaygain_gain_db = REPLAYGAIN_REFERENCE_LUFS - lufs;
    out->r128_gain_db = R128_REFERENCE_LUFS - lufs;
    const long q = lrint(out->r128_gain_db * 256.0);
    out->r128_gain_q78 = (int32_t)(q > 32767 ? 32767 : (q < -32768 ? -32768 : q));
}

//+-----------------------------------------------------+
//| Meter                                               |
//+-----------------------------------------------------+

// K-weighting for any sample rate (the BS.1770 coefficients are given for 48 kHz; these are the
// analogue prototypes they come from, as in libebur128)
static void k_weighting(Meter *m) {
    const double rate = m->rate;

    double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
    double K = tan(M_PI * f0 / rate);
    const double Vh = pow(10.0, G / 20.0);
    const double Vb = pow(Vh, 0.4996667741545416);
    double a0 = 1.0 + K / Q + K * K;
    const double pb[3] = { (Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0 };
    const double pa[3] = { 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0 };

    f0 = 38.13547087602444;
    Q = 0.5003270373238773;
    K = tan(M_PI * f0 / rate);
    a0 = 1.0 + K / Q + K * K;
    const double rb[3] = { 1.0, -2.0, 1.0 };
    const double ra[3] = { 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0 };

    // Cascade the two biquads into one 4th order filter
    m->b[0] = pb[0] * rb[0];
    m->b[1] = pb[0] * rb[1] + pb[1] * rb[0];
    m->b[2] = pb[0] * rb[2] + pb[1] * rb[1] + pb[2] * rb[0];
    m->b[3] = pb[1] * rb[2] + pb[2] * rb[1];
    m->b[4] = pb[2] * rb[2];
    m->a[0] = pa[0] * ra[0];
    m->a[1] = pa[0] * ra[1] + pa[1] * ra[0];
    m->a[2] = pa[0] * ra[2] + pa[1] * ra[1] + pa[2] * ra[0];
    m->a[3] = pa[1] * ra[2] + pa[2] * ra[1];
    m->a[4] = pa[2] * ra[2];
}

// BS.1770-4 channel weights: surround channels 1.41 (+1.5 dB), LFE not measured, others 1.0
static double channel_weight(const enum AVChannel ch) {
    switch (ch) {
        case AV_CHAN_LOW_FREQUENCY:
        case AV_CHAN_LOW_FREQUENCY_2:
            return 0.0;
        case AV_CHAN_SIDE_LEFT:
        case AV_CHAN_SIDE_RIGHT:
        case AV_CHAN_BACK_LEFT:
        case AV_CHAN_BACK_RIGHT:
        case AV_CHAN_SURROUND_DIRECT_LEFT:
        case AV_CHAN_SURROUND_DIRECT_RIGHT:
            return 1.41;
        default:
            return 1.0;
    }
}

static void meter_free(Meter *m) {
    av_freep(&m->z);
    av_freep(&m->weight);
    list_free(&m->blocks);
    list_free(&m->short_term);
    swr_free(&m->tp);
    if (m->tp_buf) {
        av_freep(&m->tp_buf[0]);
        av_freep(&m->tp_buf);
    }
}

static int meter_init(Meter *m, const int rate, const AVChannelLayout *layout, const bool true_peak) {
    memset(m, 0, sizeof(*m));
    m->rate = rate;
    m->channels = layout->nb_channels;
    m->sub_len = (int)lrint(rate / 10.0);
    if (m->channels < 1 || m->sub_len < 1) return AVERROR(EINVAL);

    m->z = av_calloc(m->channels, sizeof(*m->z));
    m->weight = av_calloc(m->channels, sizeof(double));
    if (!m->z || !m->weight) return AVERROR(ENOMEM);
    for (int c = 0; c < m->channels; c++)
        m->weight[c] = layout->order == AV_CHANNEL_ORDER_UNSPEC
                           ? 1.0 : channel_weight(av_channel_layout_channel_from_index(layout, c));
    k_weighting(m);

    // True peak: oversample to at least 192 kHz
    m->tp_factor = rate < 96000 ? 4 : (rate < 192000 ? 2 : 1);
    if (true_peak && m->tp_factor > 1) {
        int ret = swr_alloc_set_opts2(&m->tp, layout, AV_SAMPLE_FMT_FLTP, rate * m->tp_factor,
                                      layout, AV_SAMPLE_FMT_FLTP, rate, 0, NULL);
        if (ret < 0 || (ret = swr_init(m->tp)) < 0) return ret;
    }
    return 0;
}

static int finish_sub_block(Meter *m) {
    m->ring[m->ring_pos] = m->sub_energy;
    m->ring_pos = (m->ring_pos + 1) % SUB_BLOCKS_PER_SHORT_TERM;
    m->sub_count++;
    m->sub_energy = 0;
    m->sub_pos = 0;

    if (m->sub_count >= SUB_BLOCKS_PER_BLOCK) {
        double e = 0;
        for (int i = 1; i <= SUB_BLOCKS_PER_BLOCK; i++)
            e += m->ring[(m->ring_pos - i + SUB_BLOCKS_PER_SHORT_TERM) % SUB_BLOCKS_PER_SHORT_TERM];
        if (list_push(&m->blocks, e / ((double)SUB_BLOCKS_PER_BLOCK * m->sub_len)) < 0) return AVERROR(ENOMEM);
    }
    if (m->sub_count >= SUB_BLOCKS_PER_SHORT_TERM) {
        double e = 0;
        for (int i = 0; i < SUB_BLOCKS_PER_SHORT_TERM; i++) e += m->ring[i];
        if (list_push(&m->short_term, e / ((double)SUB_BLOCKS_PER_SHORT_TERM * m->sub_len)) < 0) return AVERROR(ENOMEM);
    }
    return 0;
}

static int true_peak_feed(Meter *m, const float *const *data, const int samples) {
    if (!m->tp) return 0;
    const int out_max = swr_get_out_samples(m->tp, samples);
    if (out_max > m->tp_buf_samples) {
        if (m->tp_buf) {
            av_freep(&m->tp_buf[0]);
            av_freep(&m->tp_buf);
        }
        if (av_samples_alloc_array_and_samples(&m->tp_buf, NULL, m->channels, out_max, AV_SAMPLE_FMT_FLTP, 0) < 0)
            return AVERROR(ENOMEM);
        m->tp_buf_samples = out_max;
    }
    const int got = swr_convert(m->tp, m->tp_buf, m->tp_buf_samples, (const uint8_t **)data, samples);
    if (got < 0) return got;
    for (int c = 0; c < m->channels; c++) {
        if (m->weight[c] == 0.0) continue;
        const float *s = (const float *)m->tp_buf[c];
        for (int i = 0; i < got; i++) {
            const double v = fabs((double)s[i]);
            if (v > m->true_peak) m->true_peak = v;
        }
    }
    return 0;
}

static int meter_process(Meter *m, const float *const *data, const int samples) {
    const double b0 = m->b[0], b1 = m->b[1], b2 = m->b[2], b3 = m->b[3], b4 = m->b[4];
    const double a1 = m->a[1], a2 = m->a[2], a3 = m->a[3], a4 = m->a[4];
    int done = 0;

    while (done < samples) {
        const int n = FFMIN(samples - done, m->sub_len - m->sub_pos);
        double energy = 0;
        for (int c = 0; c < m->channels; c++) {
            const float *x = data[c] + done;
            const double w = m->weight[c];
            double peak = m->sample_peak;
            for (int i = 0; i < n; i++) {
                const double v = fabs((double)x[i]);
                if (v > peak && w != 0.0) peak = v;
            }
            m->sample_peak = peak;
            if (w == 0.0) continue;

            // Direct form II, 4th order
            double z1 = m->z[c][0], z2 = m->z[c][1], z3 = m->z[c][2], z4 = m->z[c][3];
            double sum = 0;
            for (int i = 0; i < n; i++) {
                const double v0 = (double)x[i] - a1 * z1 - a2 * z2 - a3 * z3 - a4 * z4;
                const double y = b0 * v0 + b1 * z1 + b2 * z2 + b3 * z3 + b4 * z4;
                z4 = z3; z3 = z2; z2 = z1; z1 = v0;
                sum += y * y;
            }
            // Flush denormals so silence does not slow the filter down
            if (fabs(z1) < 1e-30) z1 = 0;
            if (fabs(z2) < 1e-30) z2 = 0;
            if (fabs(z3) < 1e-30) z3 = 0;
            if (fabs(z4) < 1e-30) z4 = 0;
            m->z[c][0] = z1; m->z[c][1] = z2; m->z[c][2] = z3; m->z[c][3] = z4;
            energy += w * sum;
        }
        m->sub_energy += energy;
        m->sub_pos += n;
        done += n;
        if (m->sub_pos == m->sub_len) {
            const int ret = finish_sub_block(m);
            if (ret < 0) return ret;
        }
    }
    m->samples += samples;
    return true_peak_feed(m, data, samples);
}

//+-----------------------------------------------------+
//| Decoding                                            |
//+-----------------------------------------------------+

// Decode `filename` into `m`. Returns 0, AU_ERR_* on failure.
static int measure_file(const AuLoudnessConfig *cfg, const char *filename, Meter *m) {
    AVFormatContext *ic = NULL;
    AVCodecContext *dec = NULL;
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;
    SwrContext *conv = NULL;
    AVChannelLayout conv_in = {0}, layout = {0};
    int conv_in_rate = 0, conv_in_fmt = -1;
    uint8_t **buf = NULL;
    int buf_samples = 0;
    bool meter_ready = false;
    int result = AU_ERR_FILE;
    int ret;

    memset(m, 0, sizeof(*m));
    AVDictionary *in_opts = NULL;
    network_input_options(&in_opts, filename, NULL);
    ret = avformat_open_input(&ic, filename, NULL, &in_opts);
    av_dict_free(&in_opts);
    if (ret < 0) goto end;
    if (ff_input_lacks_audio(ic)) goto end;
    if (avformat_find_stream_info(ic, NULL) < 0) goto end;
    const int st_index = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (st_index < 0) goto end;
    AVStream *st = ic->streams[st_index];
    for (unsigned int i = 0; i < ic->nb_streams; i++)
        ic->streams[i]->discard = (int)i == st_index ? AVDISCARD_DEFAULT : AVDISCARD_ALL;

    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec || !(dec = avcodec_alloc_context3(codec))) goto end;
    if (avcodec_parameters_to_context(dec, st->codecpar) < 0) goto end;
    dec->pkt_timebase = st->time_base;
    AVDictionary *opts = NULL;
    // At most 4 threads: decoding is a large part of the analysis for hi-res files (a 24/192 FLAC took 1.28 s with
    // one thread, 0.65 s with four, 0.62 s with "auto" = one per core plus one), so this keeps the speed of "auto"
    // without a thread per core for every file being measured
    av_dict_set(&opts, "threads", "4", 0);
    ret = avcodec_open2(dec, codec, &opts);
    av_dict_free(&opts);
    if (ret < 0) goto end;

    if (!(pkt = av_packet_alloc()) || !(frame = av_frame_alloc())) { result = AU_ERR_NO_MEMORY; goto end; }

    const double duration = ic->duration != AV_NOPTS_VALUE ? ic->duration / (double)AV_TIME_BASE : -1;
    int64_t next_report = 0;
    bool draining = false;

    for (;;) {
        if (!draining) {
            ret = av_read_frame(ic, pkt);
            if (ret < 0) {
                draining = true;
                ret = avcodec_send_packet(dec, NULL);
            }
            else {
                if (pkt->stream_index == st_index) ret = avcodec_send_packet(dec, pkt);
                av_packet_unref(pkt);
                if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_INVALIDDATA) goto end;
            }
        }

        for (;;) {
            ret = avcodec_receive_frame(dec, frame);
            if (ret == AVERROR(EAGAIN)) break;
            if (ret == AVERROR_EOF) goto done;
            if (ret < 0) {
                if (ret == AVERROR_INVALIDDATA) continue; // skip a damaged frame
                goto end;
            }

            // Measure at the rate and layout of the first frame; later frames are converted to it
            if (!meter_ready) {
                if (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || !av_channel_layout_check(&frame->ch_layout))
                    av_channel_layout_default(&layout, frame->ch_layout.nb_channels);
                else if (av_channel_layout_copy(&layout, &frame->ch_layout) < 0) { result = AU_ERR_NO_MEMORY; goto end; }
                if (meter_init(m, frame->sample_rate, &layout, !cfg->skip_true_peak) < 0) goto end;
                meter_ready = true;
            }
            if (!conv || conv_in_rate != frame->sample_rate || conv_in_fmt != frame->format ||
                av_channel_layout_compare(&conv_in, &frame->ch_layout)) {
                swr_free(&conv);
                av_channel_layout_uninit(&conv_in);
                if (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || !av_channel_layout_check(&frame->ch_layout))
                    av_channel_layout_default(&conv_in, frame->ch_layout.nb_channels);
                else if (av_channel_layout_copy(&conv_in, &frame->ch_layout) < 0) { result = AU_ERR_NO_MEMORY; goto end; }
                if (swr_alloc_set_opts2(&conv, &layout, AV_SAMPLE_FMT_FLTP, m->rate,
                                        &conv_in, frame->format, frame->sample_rate, 0, NULL) < 0 ||
                    swr_init(conv) < 0)
                    goto end;
                conv_in_rate = frame->sample_rate;
                conv_in_fmt = frame->format;
            }

            const int out_max = swr_get_out_samples(conv, frame->nb_samples);
            if (out_max > buf_samples) {
                if (buf) { av_freep(&buf[0]); av_freep(&buf); }
                if (av_samples_alloc_array_and_samples(&buf, NULL, m->channels, out_max, AV_SAMPLE_FMT_FLTP, 0) < 0) {
                    result = AU_ERR_NO_MEMORY;
                    goto end;
                }
                buf_samples = out_max;
            }
            const int got = swr_convert(conv, buf, buf_samples, (const uint8_t **)frame->extended_data, frame->nb_samples);
            av_frame_unref(frame);
            if (got < 0) goto end;
            if (got > 0 && meter_process(m, (const float *const *)buf, got) < 0) { result = AU_ERR_NO_MEMORY; goto end; }

            if (cfg->progress && m->samples >= next_report) {
                next_report = m->samples + m->rate; // about once per second of audio
                const double fraction = duration > 0 ? FFMIN(1.0, m->samples / (m->rate * duration)) : -1;
                if (!cfg->progress(fraction, cfg->progress_user)) {
                    result = AU_ERR_CANCELLED;
                    goto end;
                }
            }
        }
    }

done:
    if (!meter_ready) goto end; // no audio at all
    if (m->tp) {
        // Push out what the upsampler still holds
        if (m->tp_buf) {
            const int got = swr_convert(m->tp, m->tp_buf, m->tp_buf_samples, NULL, 0);
            for (int c = 0; c < m->channels && got > 0; c++) {
                if (m->weight[c] == 0.0) continue;
                const float *s = (const float *)m->tp_buf[c];
                for (int i = 0; i < got; i++)
                    if (fabs((double)s[i]) > m->true_peak) m->true_peak = fabs((double)s[i]);
            }
        }
    }
    else {
        m->true_peak = m->sample_peak;
    }
    if (cfg->progress) cfg->progress(1.0, cfg->progress_user);
    result = AU_OK;

end:
    if (result != AU_OK && meter_ready) meter_free(m);
    if (buf) { av_freep(&buf[0]); av_freep(&buf); }
    swr_free(&conv);
    av_channel_layout_uninit(&conv_in);
    av_channel_layout_uninit(&layout);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    avformat_close_input(&ic);
    return result;
}

//+-----------------------------------------------------+
//| Public API                                          |
//+-----------------------------------------------------+

AuLoudnessScanner *au_loudness_scanner_create(const AuLoudnessConfig *config) {
    AuLoudnessScanner *s = av_mallocz(sizeof(AuLoudnessScanner));
    if (s && config) s->config = *config;
    return s;
}

void au_loudness_scanner_reset(AuLoudnessScanner *s) {
    if (!s) return;
    list_free(&s->blocks);
    list_free(&s->short_term);
    s->sample_peak = s->true_peak = s->duration = 0;
    s->tracks = 0;
}

void au_loudness_scanner_free(AuLoudnessScanner *s) {
    if (!s) return;
    au_loudness_scanner_reset(s);
    av_free(s);
}

int au_loudness_track_count(const AuLoudnessScanner *s) {
    return s ? s->tracks : 0;
}

int au_loudness_scan_file(AuLoudnessScanner *s, const char *filename, AuLoudness *out) {
    if (!s || !filename || !filename[0]) return AU_ERR_INVALID_ARGUMENT;

    Meter m;
    const int ret = measure_file(&s->config, filename, &m);
    if (ret != AU_OK) return ret;

    const double duration = (double)m.samples / m.rate;
    if (out) fill_result(out, &m.blocks, &m.short_term, m.sample_peak, m.true_peak, duration);

    // Add to the album; on failure leave the album as it was
    const size_t nb = s->blocks.n, ns = s->short_term.n;
    if (list_append(&s->blocks, &m.blocks) < 0 || list_append(&s->short_term, &m.short_term) < 0) {
        s->blocks.n = nb;
        s->short_term.n = ns;
        meter_free(&m);
        return AU_ERR_NO_MEMORY;
    }
    s->sample_peak = FFMAX(s->sample_peak, m.sample_peak);
    s->true_peak = FFMAX(s->true_peak, m.true_peak);
    s->duration += duration;
    s->tracks++;
    meter_free(&m);
    return AU_OK;
}

int au_loudness_album(const AuLoudnessScanner *s, AuLoudness *out) {
    if (!s || !out) return AU_ERR_INVALID_ARGUMENT;
    if (!s->tracks) return AU_ERR_NOTHING_TO_DO;
    fill_result(out, &s->blocks, &s->short_term, s->sample_peak, s->true_peak, s->duration);
    return AU_OK;
}

int au_loudness_analyze(const char *filename, const AuLoudnessConfig *config, AuLoudness *out) {
    if (!out) return AU_ERR_INVALID_ARGUMENT;
    AuLoudnessScanner *s = au_loudness_scanner_create(config);
    if (!s) return AU_ERR_NO_MEMORY;
    const int ret = au_loudness_scan_file(s, filename, out);
    au_loudness_scanner_free(s);
    return ret;
}

AuReplayGainInfo au_loudness_to_replaygain(const AuLoudness *track, const AuLoudness *album) {
    AuReplayGainInfo rg;
    memset(&rg, 0, sizeof(rg));
    if (track && !track->silent) {
        rg.has_track_gain = true;
        rg.track_gain_db = track->replaygain_gain_db;
        rg.has_track_peak = track->true_peak > 0;
        rg.track_peak = track->true_peak;
    }
    if (album && !album->silent) {
        rg.has_album_gain = true;
        rg.album_gain_db = album->replaygain_gain_db;
        rg.has_album_peak = album->true_peak > 0;
        rg.album_peak = album->true_peak;
    }
    return rg;
}
