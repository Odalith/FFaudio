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
 * Opening an input and a decoder, the same way for the player, au_probe() and the loudness analysis.
 */

#include "probe.h"
#include "network.h"
#include "libavcodec/avcodec.h"

int ff_open_input(const char *path, const char *http_headers, const AVIOInterruptCB *interrupt,
                  AVFormatContext **out_ic) {
    *out_ic = NULL;
    AVFormatContext *ic = avformat_alloc_context();
    if (!ic) return AVERROR(ENOMEM);
    if (interrupt && interrupt->callback) ic->interrupt_callback = *interrupt;

    AVDictionary *opts = NULL;
    av_dict_set(&opts, "scan_all_pmts", "1", 0); // MPEG-TS: every program, also those announced late
    network_input_options(&opts, path, http_headers);
    const int ret = avformat_open_input(&ic, path, NULL, &opts); // probed, never chosen by the name; frees ic on failure
    av_dict_free(&opts);
    if (ret < 0) return ret;
    network_after_open(ic, path);
    *out_ic = ic;
    return 0;
}

int ff_open_audio_input(const char *path, const char *http_headers, const AVIOInterruptCB *interrupt,
                        AVFormatContext **out_ic, int *out_stream, bool *out_chained, FfOpenStep *failed) {
    *out_ic = NULL;
    *out_stream = -1;
    if (out_chained) *out_chained = false;
    *failed = FF_OPEN_INPUT;

    AVFormatContext *ic = NULL;
    int ret = ff_open_input(path, http_headers, interrupt, &ic);
    if (ret < 0) return ret;

    if (ff_input_lacks_audio(ic)) {
        *failed = FF_OPEN_NO_AUDIO;
        ret = AVERROR_STREAM_NOT_FOUND;
        goto fail;
    }
    if ((ret = avformat_find_stream_info(ic, NULL)) < 0) {
        *failed = FF_OPEN_STREAM_INFO;
        goto fail;
    }
    // A chained Ogg file (parts one after another): FFmpeg's duration is its last part's. Looked at once here (it
    // reads the file's last 64 KB), the callers get the answer
    int64_t chained_duration;
    const bool chained = ff_ogg_chain_info(ic, path, &chained_duration);
    if (chained_duration != AV_NOPTS_VALUE) ic->duration = chained_duration;

    const int st = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (st < 0) {
        *failed = FF_OPEN_NO_AUDIO;
        ret = st;
        goto fail;
    }
    for (unsigned int i = 0; i < ic->nb_streams; i++)
        ic->streams[i]->discard = (int)i == st ? AVDISCARD_DEFAULT : AVDISCARD_ALL;

    *failed = FF_OPEN_OK;
    *out_ic = ic;
    *out_stream = st;
    if (out_chained) *out_chained = chained;
    return 0;

fail:
    avformat_close_input(&ic);
    return ret;
}

int ff_open_decoder(const AVStream *st, const int threads, AVCodecContext **out) {
    *out = NULL;
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) return AVERROR_DECODER_NOT_FOUND;
    AVCodecContext *dec = avcodec_alloc_context3(codec);
    if (!dec) return AVERROR(ENOMEM);
    int ret = avcodec_parameters_to_context(dec, st->codecpar);
    if (ret >= 0) {
        dec->pkt_timebase = st->time_base;
        char n[16];
        snprintf(n, sizeof(n), "%d", threads > 0 ? threads : 1);
        AVDictionary *opts = NULL;
        av_dict_set(&opts, "threads", n, 0);
        ret = avcodec_open2(dec, codec, &opts);
        av_dict_free(&opts);
    }
    if (ret < 0) {
        avcodec_free_context(&dec);
        return ret;
    }
    *out = dec;
    return 0;
}
