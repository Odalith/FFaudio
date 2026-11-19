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

#endif //FFAUDIO_PROBE_H
