/*
* Copyright (c) 2003 Fabrice Bellard, 2025 Odalith
 *
 * This file was part of FFmpeg, particularly cmdutils.
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
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef CMDUTILS_H
#define CMDUTILS_H

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avassert.h>
#include <libavutil/avstring.h>
#include <libavutil/dict.h>
#include <libavutil/opt.h>

void init_dynload(void);

int filter_codec_opts_n(const AVDictionary *opts, enum AVCodecID codec_id,
                      AVFormatContext *s, AVStream *st, const AVCodec *codec,
                      AVDictionary **dst, AVDictionary **opts_used);

int setup_find_stream_info_opts_n(AVFormatContext *s,
                                AVDictionary *local_codec_opts,
                                AVDictionary ***dst);

#endif //CMDUTILS_H
