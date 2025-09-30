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

#ifndef FFAUDIO_DECODER_UTILS_H
#define FFAUDIO_DECODER_UTILS_H

#include "globals.h"
#include "frame_queue_utils.h"
#include "packet_queue_utils.h"

void decoder_destroy(Decoder *d);

int decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, SDL_cond *empty_queue_cond);

int decoder_decode_frame(Decoder *d, AVFrame *frame);

void decoder_abort(Decoder *d, FrameQueue *fq);

int decoder_start(Decoder *d, int (*fn)(void *), const char *thread_name, void* arg);

#endif //FFAUDIO_DECODER_UTILS_H