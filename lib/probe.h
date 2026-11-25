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

#ifndef FFAUDIO_PROBE_H
#define FFAUDIO_PROBE_H

#include <stdbool.h>
#include <string.h>
#include "libavformat/avformat.h"

// Call between avformat_open_input() and avformat_find_stream_info(): true when the container already says that
// there is no audio. avformat_find_stream_info() would otherwise open decoders for the other streams, and data
// that is no media file at all (encrypted caches, random bytes) is often misdetected as a raw video stream (VVC,
// HEVC, MJPEG, ...), whose decoder then prints errors while it tries to make sense of it.
// Conservative: formats that add streams while reading (no header), and streams of unknown or data type, count
// as possibly audio. The exception are FFmpeg's image demuxers (image2, image2pipe, jpeg_pipe, png_pipe, ...):
// they are flagged as having no header but only ever have their one image stream.
static inline bool ff_is_image_demuxer(const char *name) {
    if (!name) return false;
    if (!strcmp(name, "image2") || !strcmp(name, "image2pipe")) return true;
    const size_t n = strlen(name);
    return n > 5 && !strcmp(name + n - 5, "_pipe");
}

static inline bool ff_input_lacks_audio(const AVFormatContext *ic) {
    if (!ic || ic->nb_streams == 0) return false;
    if ((ic->ctx_flags & AVFMTCTX_NOHEADER) && !ff_is_image_demuxer(ic->iformat->name)) return false;
    for (unsigned int i = 0; i < ic->nb_streams; i++) {
        const enum AVMediaType type = ic->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_AUDIO || type == AVMEDIA_TYPE_UNKNOWN || type == AVMEDIA_TYPE_DATA) return false;
    }
    return true;
}

typedef enum FfOpenStep { FF_OPEN_OK, FF_OPEN_INPUT, FF_OPEN_NO_AUDIO, FF_OPEN_STREAM_INFO } FfOpenStep;

// Open a file or URL and read only its header (lib/input.c): the protocol whitelist, the network settings and request
// headers (NULL for none), the interrupt callback (NULL for none), reconnects once open. No stream info is read. On
// failure *out_ic is NULL and the result an AVERROR.
int ff_open_input(const char *path, const char *http_headers, const AVIOInterruptCB *interrupt,
                  AVFormatContext **out_ic);

// Open a file or URL for its audio, the same way everywhere (lib/input.c): the protocol whitelist, the network
// settings and request headers (NULL for none), the interrupt callback (NULL for none), every MPEG-TS program,
// the stream info, and the best audio stream, with every other stream discarded. On success *out_ic is open and
// *out_stream is that stream's index; on failure *out_ic is NULL, the result is an AVERROR and *failed says which
// step failed. *out_chained (may be NULL): a chained Ogg file (ff_ogg_chain_info(); ic->duration is then the real
// one). Logs nothing: the callers word their own messages.
int ff_open_audio_input(const char *path, const char *http_headers, const AVIOInterruptCB *interrupt,
                        AVFormatContext **out_ic, int *out_stream, bool *out_chained, FfOpenStep *failed);

// Whether the input is a local Ogg file of parts one after another (chained: "cat a.ogg b.ogg", a recorded radio
// stream). FFmpeg can not seek past its first part (every part's timestamps start again), so ffaudio treats it as not
// seekable. *duration: its real duration (AV_TIME_BASE units; FFmpeg reports only the last part's), or AV_NOPTS_VALUE
// when it is not chained or its parts are not all Vorbis, Opus or FLAC (lib/ogg_chain.c). An Ogg file with several
// streams at once (multiplexed: Vorbis and Theora, Opus and a Skeleton) is not chained.
bool ff_ogg_chain_info(AVFormatContext *ic, const char *path, int64_t *duration);

struct AVCodecContext;
// An open decoder for the stream with `threads` threads. AVERROR_DECODER_NOT_FOUND when FFmpeg has none.
int ff_open_decoder(const AVStream *st, int threads, struct AVCodecContext **out);

#endif //FFAUDIO_PROBE_H
