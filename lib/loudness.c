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

#include "loudness.h"
#include "probe.h"
#include "network.h"

#define ABSOLUTE_GATE_LUFS (-70.0)
#define RELATIVE_GATE_LU (-10.0)
#define LRA_RELATIVE_GATE_LU (-20.0)
#define SUB_BLOCKS_PER_BLOCK 4      // 400 ms
#define SUB_BLOCKS_PER_SHORT_TERM 30 // 3 s
#define REPLAYGAIN_REFERENCE_LUFS (-18.0)
#define R128_REFERENCE_LUFS (-23.0)

// Gating block and short-term energies are counted in histograms of HIST_STEP LU from the absolute gate up, with the
// exact energy sum per bin: memory stays the same for any length, an album is the sum of its tracks, and only blocks in
// the bin of the relative gate (0.01 LU wide) are taken as all above or all below it
#define HIST_MIN_LUFS ABSOLUTE_GATE_LUFS
#define HIST_STEP 0.01
#define HIST_BINS 10000                  // -70 .. +30 LUFS (louder blocks count in the top bin)

typedef struct Histogram {
    uint32_t count[HIST_BINS];
    double energy[HIST_BINS];
} Histogram;

struct AuLoudnessMeter {
    Histogram *blocks, *short_term;     // every file measured into it
    double sample_peak, true_peak, duration;
    int files;
};

// True peak: interpolation to 4x (below 96 kHz) or 2x (below 192 kHz) with a windowed sinc in polyphase form (49
// taps, Hann window, as libebur128), run straight on the planar float input
#define TP_TAPS 49
#define TP_MAX_PHASE_TAPS 25

typedef struct TruePeak {
    int factor;
    int delay;                          // history kept per channel
    // Phases 1 .. factor-1 (phase 0 is the input sample itself, which the sample peak covers): ntaps coefficients
    // each, reversed, so an output is a forward dot product over the input window that ends at its sample
    int ntaps;
    float coef[4][TP_MAX_PHASE_TAPS];
    double gain;                        // largest sum of |coef| of a phase: no output exceeds gain x the input's peak
    float *hist;                        // channels x delay: the last input samples
    float *work;                        // history + one chunk of one channel
    size_t work_size;
    float *hist_peak;                   // per channel: largest |x| in the history
} TruePeak;

typedef struct Meter {
    int rate, channels;
    // K-weighting as two biquads (pre-filter high shelf, RLB high pass), transposed direct form II
    double pb[3], pa[3], rb[3], ra[3];
    double (*z)[4];             // per channel: two states per biquad
    double *weight;             // per channel weight (0 = not measured, e.g. LFE)
    int sub_len;                // samples per 100 ms
    int sub_pos;
    double sub_energy;          // weighted sum of squares in the current sub-block
    double ring[SUB_BLOCKS_PER_SHORT_TERM];
    int ring_pos;
    int64_t sub_count;
    Histogram *blocks, *short_term; // the scanner's, cleared for this file
    double sample_peak, true_peak;
    int64_t samples;
    TruePeak *tp;               // NULL: none (skipped, or 192 kHz and up)
} Meter;

//+-----------------------------------------------------+
//| Histograms and gating                               |
//+-----------------------------------------------------+

static double energy_to_lufs(const double e) { return -0.691 + 10.0 * log10(e); }
static double lufs_to_energy(const double l) { return pow(10.0, (l + 0.691) / 10.0); }

static int bin_of(const double lufs) {
    const int i = (int)floor((lufs - HIST_MIN_LUFS) / HIST_STEP);
    return i < 0 ? 0 : (i >= HIST_BINS ? HIST_BINS - 1 : i);
}

static double bin_center(const int i) { return HIST_MIN_LUFS + (i + 0.5) * HIST_STEP; }

// Blocks below the absolute gate are not counted at all
static void hist_add(Histogram *h, const double e) {
    if (!(e >= lufs_to_energy(ABSOLUTE_GATE_LUFS))) return;
    const int i = bin_of(energy_to_lufs(e));
    h->count[i]++;
    h->energy[i] += e;
}

static void hist_merge(Histogram *dst, const Histogram *src) {
    for (int i = 0; i < HIST_BINS; i++) {
        dst->count[i] += src->count[i];
        dst->energy[i] += src->energy[i];
    }
}

// First bin at or above a gate: its own bin counts when the gate is in its lower half
static int gate_bin(const double gate_lufs) {
    const int i = bin_of(gate_lufs);
    return gate_lufs > bin_center(i) ? i + 1 : i;
}

// Mean energy of the counted blocks, and how many there are
static double hist_mean(const Histogram *h, const int from, uint64_t *count) {
    double sum = 0;
    uint64_t n = 0;
    for (int i = from; i < HIST_BINS; i++) {
        sum += h->energy[i];
        n += h->count[i];
    }
    *count = n;
    return n ? sum / (double)n : 0;
}

// Gated integrated loudness; false when nothing passes the absolute gate
static bool integrated_loudness(const Histogram *h, double *lufs) {
    uint64_t n;
    const double mean = hist_mean(h, 0, &n);
    if (!n) return false;
    const double gate = FFMAX(energy_to_lufs(mean) + RELATIVE_GATE_LU, ABSOLUTE_GATE_LUFS);
    const double gated = hist_mean(h, gate_bin(gate), &n);
    if (!n) return false;
    *lufs = energy_to_lufs(gated);
    return true;
}

// Loudness range (EBU Tech 3342): spread between the 10th and 95th percentile of the gated short-term loudness
static double loudness_range(const Histogram *h) {
    uint64_t n;
    const double mean = hist_mean(h, 0, &n);
    if (n < 2) return 0;
    const int from = gate_bin(FFMAX(energy_to_lufs(mean) + LRA_RELATIVE_GATE_LU, ABSOLUTE_GATE_LUFS));
    uint64_t total = 0;
    for (int i = from; i < HIST_BINS; i++) total += h->count[i];
    if (total < 2) return 0;
    const uint64_t lo_rank = (uint64_t)((double)(total - 1) * 0.10 + 0.5);
    const uint64_t hi_rank = (uint64_t)((double)(total - 1) * 0.95 + 0.5);
    double lo = 0, hi = 0;
    uint64_t seen = 0;
    bool have_lo = false;
    for (int i = from; i < HIST_BINS; i++) {
        seen += h->count[i];
        if (!have_lo && seen > lo_rank) { lo = bin_center(i); have_lo = true; }
        if (seen > hi_rank) { hi = bin_center(i); break; }
    }
    return hi - lo;
}

static void fill_result(AuLoudness *out, const Histogram *blocks, const Histogram *st,
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
    m->pb[0] = (Vh + Vb * K / Q + K * K) / a0;
    m->pb[1] = 2.0 * (K * K - Vh) / a0;
    m->pb[2] = (Vh - Vb * K / Q + K * K) / a0;
    m->pa[0] = 1.0;
    m->pa[1] = 2.0 * (K * K - 1.0) / a0;
    m->pa[2] = (1.0 - K / Q + K * K) / a0;

    f0 = 38.13547087602444;
    Q = 0.5003270373238773;
    K = tan(M_PI * f0 / rate);
    a0 = 1.0 + K / Q + K * K;
    m->rb[0] = 1.0;
    m->rb[1] = -2.0;
    m->rb[2] = 1.0;
    m->ra[0] = 1.0;
    m->ra[1] = 2.0 * (K * K - 1.0) / a0;
    m->ra[2] = (1.0 - K / Q + K * K) / a0;
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

static void true_peak_free(TruePeak **tp) {
    if (!*tp) return;
    av_free((*tp)->hist);
    av_free((*tp)->work);
    av_free((*tp)->hist_peak);
    av_freep(tp);
}

static TruePeak *true_peak_create(const int factor, const int channels) {
    TruePeak *tp = av_mallocz(sizeof(TruePeak));
    if (!tp) return NULL;
    tp->factor = factor;
    tp->delay = (TP_TAPS + factor - 1) / factor;
    tp->ntaps = TP_TAPS / factor; // taps j = f + k * factor of phase f >= 1 (12 at 4x, 24 at 2x)
    for (int f = 1; f < factor; f++) {
        double g = 0;
        for (int k = 0; k < tp->ntaps; k++) {
            const int j = f + k * factor;
            const double x = (double)j - (TP_TAPS - 1) / 2.0;
            double c = fabs(x) > 1e-12 ? sin(x * M_PI / factor) / (x * M_PI / factor) : 1.0;
            c *= 0.5 * (1.0 - cos(2.0 * M_PI * j / (TP_TAPS - 1)));
            tp->coef[f][tp->ntaps - 1 - k] = (float)c; // tap k weighs the input k samples back
            g += fabs(c);
        }
        tp->gain = FFMAX(tp->gain, g);
    }
    tp->hist = av_calloc((size_t)channels * tp->delay, sizeof(float));
    tp->hist_peak = av_calloc(channels, sizeof(float));
    if (!tp->hist || !tp->hist_peak) true_peak_free(&tp);
    return tp;
}

// One channel's chunk: the largest interpolated value goes into *peak. Skipped (only the history moves on) when no
// output can exceed *peak: after the loudest part of a file, nearly every chunk is.
static int true_peak_channel(TruePeak *tp, const int c, const float *x, const int n, const float chunk_peak,
                             double *peak) {
    const int d = tp->delay;
    float *hist = tp->hist + (size_t)c * d;
    const bool needed = (double)FFMAX(chunk_peak, tp->hist_peak[c]) * tp->gain > *peak;
    if (needed) {
        const size_t need = (size_t)(d + n) * sizeof(float);
        if (need > tp->work_size) {
            float *w = av_realloc(tp->work, need);
            if (!w) return AVERROR(ENOMEM);
            tp->work = w;
            tp->work_size = need;
        }
        float *w = tp->work;
        memcpy(w, hist, (size_t)d * sizeof(float));
        memcpy(w + d, x, (size_t)n * sizeof(float));
        // Outputs in blocks, one tap at a time across the block: the inner loop is a vectorizable multiply-add
        float p = (float)*peak;
        const int nt = tp->ntaps;
        float acc[256];
        for (int b = 0; b < n; b += 256) {
            const int len = FFMIN(256, n - b);
            const float *base = w + d + b - (nt - 1);
            for (int f = 1; f < tp->factor; f++) {
                for (int i = 0; i < len; i++) acc[i] = 0.0f;
                for (int k = 0; k < nt; k++) {
                    const float c = tp->coef[f][k];
                    const float *src = base + k;
                    for (int i = 0; i < len; i++) acc[i] += c * src[i];
                }
                for (int i = 0; i < len; i++) p = FFMAX(p, fabsf(acc[i]));
            }
        }
        *peak = FFMAX(*peak, (double)p);
    }
    // Keep the last `delay` samples
    if (n >= d) {
        memcpy(hist, x + n - d, (size_t)d * sizeof(float));
    }
    else {
        memmove(hist, hist + n, (size_t)(d - n) * sizeof(float));
        memcpy(hist + d - n, x, (size_t)n * sizeof(float));
    }
    float hp = 0;
    for (int i = 0; i < d; i++) hp = FFMAX(hp, fabsf(hist[i]));
    tp->hist_peak[c] = hp;
    return 0;
}

static void meter_free(Meter *m) {
    av_freep(&m->z);
    av_freep(&m->weight);
    true_peak_free(&m->tp);
}

static int meter_init(Meter *m, const int rate, const AVChannelLayout *layout, const bool true_peak,
                      Histogram *blocks, Histogram *short_term) {
    memset(m, 0, sizeof(*m));
    m->rate = rate;
    m->channels = layout->nb_channels;
    m->sub_len = (int)lrint(rate / 10.0);
    if (m->channels < 1 || m->sub_len < 1) return AVERROR(EINVAL);
    m->blocks = blocks;
    m->short_term = short_term;
    memset(blocks, 0, sizeof(*blocks));
    memset(short_term, 0, sizeof(*short_term));

    m->z = av_calloc(m->channels, sizeof(*m->z));
    m->weight = av_calloc(m->channels, sizeof(double));
    if (!m->z || !m->weight) {
        meter_free(m);
        return AVERROR(ENOMEM);
    }
    for (int c = 0; c < m->channels; c++)
        m->weight[c] = layout->order == AV_CHANNEL_ORDER_UNSPEC
                           ? 1.0 : channel_weight(av_channel_layout_channel_from_index(layout, c));
    k_weighting(m);

    const int factor = rate < 96000 ? 4 : (rate < 192000 ? 2 : 1);
    if (true_peak && factor > 1 && !(m->tp = true_peak_create(factor, m->channels))) {
        meter_free(m);
        return AVERROR(ENOMEM);
    }
    return 0;
}

static void finish_sub_block(Meter *m) {
    m->ring[m->ring_pos] = m->sub_energy;
    m->ring_pos = (m->ring_pos + 1) % SUB_BLOCKS_PER_SHORT_TERM;
    m->sub_count++;
    m->sub_energy = 0;
    m->sub_pos = 0;

    if (m->sub_count >= SUB_BLOCKS_PER_BLOCK) {
        double e = 0;
        for (int i = 1; i <= SUB_BLOCKS_PER_BLOCK; i++)
            e += m->ring[(m->ring_pos - i + SUB_BLOCKS_PER_SHORT_TERM) % SUB_BLOCKS_PER_SHORT_TERM];
        hist_add(m->blocks, e / ((double)SUB_BLOCKS_PER_BLOCK * m->sub_len));
    }
    if (m->sub_count >= SUB_BLOCKS_PER_SHORT_TERM) {
        double e = 0;
        for (int i = 0; i < SUB_BLOCKS_PER_SHORT_TERM; i++) e += m->ring[i];
        hist_add(m->short_term, e / ((double)SUB_BLOCKS_PER_SHORT_TERM * m->sub_len));
    }
}

static int meter_process(Meter *m, const float *const *data, const int samples) {
    const double pb0 = m->pb[0], pb1 = m->pb[1], pb2 = m->pb[2], pa1 = m->pa[1], pa2 = m->pa[2];
    const double rb0 = m->rb[0], rb1 = m->rb[1], rb2 = m->rb[2], ra1 = m->ra[1], ra2 = m->ra[2];

    // Peaks of every channel, LFE included (it is played, and it can clip)
    for (int c = 0; c < m->channels; c++) {
        const float *x = data[c];
        float peak = 0;
        for (int i = 0; i < samples; i++) peak = FFMAX(peak, fabsf(x[i]));
        if (peak > m->sample_peak) m->sample_peak = peak;
        if (m->tp) {
            const int ret = true_peak_channel(m->tp, c, x, samples, peak, &m->true_peak);
            if (ret < 0) return ret;
        }
    }

    int done = 0;
    while (done < samples) {
        const int n = FFMIN(samples - done, m->sub_len - m->sub_pos);
        double energy = 0;
        for (int c = 0; c < m->channels; c++) {
            const double w = m->weight[c];
            if (w == 0.0) continue;
            const float *x = data[c] + done;
            double s1 = m->z[c][0], s2 = m->z[c][1], t1 = m->z[c][2], t2 = m->z[c][3];
            double sum = 0;
            for (int i = 0; i < n; i++) {
                const double in = (double)x[i];
                const double y1 = pb0 * in + s1;
                s1 = pb1 * in - pa1 * y1 + s2;
                s2 = pb2 * in - pa2 * y1;
                const double y = rb0 * y1 + t1;
                t1 = rb1 * y1 - ra1 * y + t2;
                t2 = rb2 * y1 - ra2 * y;
                sum += y * y;
            }
            // Flush denormals so silence does not slow the filter down
            if (fabs(s1) < 1e-30) s1 = 0;
            if (fabs(s2) < 1e-30) s2 = 0;
            if (fabs(t1) < 1e-30) t1 = 0;
            if (fabs(t2) < 1e-30) t2 = 0;
            m->z[c][0] = s1; m->z[c][1] = s2; m->z[c][2] = t1; m->z[c][3] = t2;
            energy += w * sum;
        }
        m->sub_energy += energy;
        m->sub_pos += n;
        done += n;
        if (m->sub_pos == m->sub_len) finish_sub_block(m);
    }
    m->samples += samples;
    return 0;
}

// The end: the interpolator's last outputs need input after the last sample
static int meter_finish(Meter *m) {
    if (!m->tp) {
        m->true_peak = m->sample_peak;
        return 0;
    }
    float zeros[TP_MAX_PHASE_TAPS + 1] = {0};
    for (int c = 0; c < m->channels; c++) {
        const int ret = true_peak_channel(m->tp, c, zeros, m->tp->delay, 0.0f, &m->true_peak);
        if (ret < 0) return ret;
    }
    m->true_peak = FFMAX(m->true_peak, m->sample_peak); // the interpolator's phase 0: the samples themselves
    return 0;
}

//+-----------------------------------------------------+
//| Decoding                                            |
//+-----------------------------------------------------+

// Decode `filename` into `m`. Returns 0, AU_ERR_* on failure.
static int measure_file(const AuLoudnessOptions *cfg, const AVIOInterruptCB *interrupt, const char *filename,
                        Meter *m, Histogram *blocks, Histogram *short_term) {
    AVFormatContext *ic = NULL;
    AVCodecContext *dec = NULL;
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;
    SwrContext *conv = NULL;
    AVChannelLayout conv_src = {0}, conv_in = {0}, layout = {0}; // conv_src: the frames' layout as it came
    int conv_in_rate = 0, conv_in_fmt = -1;
    uint8_t **buf = NULL;
    int buf_samples = 0;
    bool meter_ready = false;
    int result = AU_ERR_FILE;
    int ret;

    memset(m, 0, sizeof(*m));
    int st_index = -1;
    FfOpenStep failed;
    if ((ret = ff_open_audio_input(filename, NULL, interrupt, &ic, &st_index, NULL, &failed)) < 0) {
        if (ret == AVERROR(ENOMEM)) result = AU_ERR_NO_MEMORY;
        goto end;
    }
    if (au_is_network_url(filename) && (ic->duration == AV_NOPTS_VALUE || ic->duration <= 0)) {
        result = AU_ERR_UNSUPPORTED;   // a live stream: it never ends, so it can not be measured
        goto end;
    }
    // At most 4 threads: decoding is a large part of the analysis for hi-res files (a 24/192 FLAC took 1.28 s with
    // one thread, 0.65 s with four, 0.62 s with "auto" = one per core plus one), so this keeps the speed of "auto"
    // without a thread per core for every file being measured
    if (ff_open_decoder(ic->streams[st_index], 4, &dec) < 0) goto end;

    if (!(pkt = av_packet_alloc()) || !(frame = av_frame_alloc())) { result = AU_ERR_NO_MEMORY; goto end; }

    const double duration = ic->duration != AV_NOPTS_VALUE ? ic->duration / (double)AV_TIME_BASE : -1;
    int64_t next_report = 0;
    bool draining = false;

    for (;;) {
        if (!draining) {
            ret = av_read_frame(ic, pkt);
            if (ret < 0) {
                // The end, or as far as a damaged file goes: what was decoded is measured. A failed read (a disk
                // error, a transfer that broke off after its reconnects) is not the end of the file, and the result
                // would be stored as its loudness, so it fails the measurement instead.
                if (ic->pb && ic->pb->error < 0 && ic->pb->error != AVERROR_EOF) goto end;
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
                if (meter_init(m, frame->sample_rate, &layout, !cfg->skip_true_peak, blocks, short_term) < 0) goto end;
                meter_ready = true;
            }
            // Compared with the frames' own layout: an unspecified one is replaced for the converter, and
            // comparing with that would rebuild the converter for every frame
            if (!conv || conv_in_rate != frame->sample_rate || conv_in_fmt != frame->format ||
                av_channel_layout_compare(&conv_src, &frame->ch_layout)) {
                swr_free(&conv);
                av_channel_layout_uninit(&conv_in);
                av_channel_layout_uninit(&conv_src);
                if (av_channel_layout_copy(&conv_src, &frame->ch_layout) < 0) { result = AU_ERR_NO_MEMORY; goto end; }
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
            if (got > 0 && (ret = meter_process(m, (const float *const *)buf, got)) < 0) {
                result = ret == AVERROR(ENOMEM) ? AU_ERR_NO_MEMORY : AU_ERR_FILE;
                goto end;
            }

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
    if ((ret = meter_finish(m)) < 0) {
        result = AU_ERR_NO_MEMORY;
        goto end;
    }
    if (cfg->progress) cfg->progress(1.0, cfg->progress_user);
    result = AU_OK;

end:
    // Whatever failed while the interrupt was set (connecting, a read, a reconnect wait), it was a cancellation
    if (result != AU_OK && result != AU_ERR_CANCELLED && interrupt && interrupt->callback &&
        interrupt->callback(interrupt->opaque))
        result = AU_ERR_CANCELLED;
    if (result != AU_OK && meter_ready) meter_free(m);
    if (buf) { av_freep(&buf[0]); av_freep(&buf); }
    swr_free(&conv);
    av_channel_layout_uninit(&conv_src);
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

void au_loudness_meter_free(AuLoudnessMeter *m) {
    if (!m) return;
    av_free(m->blocks);
    av_free(m->short_term);
    av_free(m);
}

AuLoudnessMeter *loudness_meter_create(void) {
    AuLoudnessMeter *m = av_mallocz(sizeof(AuLoudnessMeter));
    if (!m) return NULL;
    m->blocks = av_mallocz(sizeof(Histogram));
    m->short_term = av_mallocz(sizeof(Histogram));
    if (!m->blocks || !m->short_term) {
        au_loudness_meter_free(m);
        return NULL;
    }
    return m;
}

void loudness_meter_clear(AuLoudnessMeter *m) {
    if (!m) return;
    memset(m->blocks, 0, sizeof(Histogram));
    memset(m->short_term, 0, sizeof(Histogram));
    m->sample_peak = m->true_peak = m->duration = 0;
    m->files = 0;
}

int loudness_measure_into(AuLoudnessMeter *m, const char *path, const AuLoudnessOptions *config,
                          const AVIOInterruptCB *interrupt) {
    if (!m || !path || !path[0]) return AU_ERR_INVALID_ARGUMENT;
    const AuLoudnessOptions none = { 0 };
    const AVIOInterruptCB no_interrupt = { 0 };
    Meter meter;
    const int ret = measure_file(config ? config : &none, interrupt ? interrupt : &no_interrupt, path, &meter,
                                 m->blocks, m->short_term);
    if (ret != AU_OK) {
        loudness_meter_clear(m);
        return ret;
    }
    m->sample_peak = meter.sample_peak;
    m->true_peak = meter.true_peak;
    m->duration = (double)meter.samples / meter.rate;
    m->files = 1;
    meter_free(&meter);
    return AU_OK;
}

int au_loudness_measure(const char *path, const AuLoudnessOptions *config, AuLoudnessMeter **out) {
    if (!out) return AU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    if (!path || !path[0]) return AU_ERR_INVALID_ARGUMENT;
    AuLoudnessMeter *m = loudness_meter_create();
    if (!m) return AU_ERR_NO_MEMORY;
    const int ret = loudness_measure_into(m, path, config, NULL);
    if (ret != AU_OK) {
        au_loudness_meter_free(m);
        return ret;
    }
    *out = m;
    return AU_OK;
}

int au_loudness_merge(AuLoudnessMeter *into, const AuLoudnessMeter *other) {
    if (!into || !other) return AU_ERR_INVALID_ARGUMENT;
    if (into == other) return AU_ERR_INVALID_ARGUMENT;
    hist_merge(into->blocks, other->blocks);
    hist_merge(into->short_term, other->short_term);
    into->sample_peak = FFMAX(into->sample_peak, other->sample_peak);
    into->true_peak = FFMAX(into->true_peak, other->true_peak);
    into->duration += other->duration;
    into->files += other->files;
    return AU_OK;
}

int au_loudness_result(const AuLoudnessMeter *m, AuLoudness *out) {
    if (!m || !out) return AU_ERR_INVALID_ARGUMENT;
    if (!m->files) return AU_ERR_NOTHING_TO_DO;
    fill_result(out, m->blocks, m->short_term, m->sample_peak, m->true_peak, m->duration);
    return AU_OK;
}

int au_loudness_file_count(const AuLoudnessMeter *m) {
    return m ? m->files : 0;
}

int au_loudness_analyze(const char *path, const AuLoudnessOptions *config, AuLoudness *out) {
    if (!out) return AU_ERR_INVALID_ARGUMENT;
    AuLoudnessMeter *m = NULL;
    const int ret = au_loudness_measure(path, config, &m);
    if (ret == AU_OK) au_loudness_result(m, out);
    au_loudness_meter_free(m);
    return ret;
}

AuReplayGain au_loudness_to_replaygain(const AuLoudness *track, const AuLoudness *album) {
    AuReplayGain rg = AU_REPLAYGAIN_NONE;
    if (track && !track->silent) {
        rg.track_gain_db = (float)track->replaygain_gain_db;
        if (track->true_peak > 0) rg.track_peak = (float)track->true_peak;
    }
    if (album && !album->silent) {
        rg.album_gain_db = (float)album->replaygain_gain_db;
        if (album->true_peak > 0) rg.album_peak = (float)album->true_peak;
    }
    return rg;
}
