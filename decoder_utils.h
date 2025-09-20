//
// Created by malkj on 2025-09-18.
//

#ifndef FFAUDIO_DECODER_UTILS_H
#define FFAUDIO_DECODER_UTILS_H

#include "ffaudio.h"
#include "frame_queue_utils.h"
#include "packet_queue_utils.h"

void decoder_destroy(Decoder *d);

int decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, SDL_cond *empty_queue_cond);

int decoder_decode_frame(Decoder *d, AVFrame *frame);

void decoder_abort(Decoder *d, FrameQueue *fq);

int decoder_start(Decoder *d, int (*fn)(void *), const char *thread_name, void* arg);

#endif //FFAUDIO_DECODER_UTILS_H