//
// Created by malkj on 2025-09-18.
//

#ifndef FFAUDIO_FRAME_QUEUE_UTILS_H
#define FFAUDIO_FRAME_QUEUE_UTILS_H

#include "ffaudio.h"

void frame_queue_unref_item(Frame *vp);

int frame_queue_init(FrameQueue *f, PacketQueue *pktq, int max_size, int keep_last);

void frame_queue_destroy(FrameQueue *f);

void frame_queue_signal(FrameQueue *f);

Frame *frame_queue_peek_writable(FrameQueue *f);

Frame *frame_queue_peek_readable(FrameQueue *f);

void frame_queue_push(FrameQueue *f);

void frame_queue_next(FrameQueue *f);

/* return the number of undisplayed frames in the queue */
int frame_queue_nb_remaining(FrameQueue *f);

/* return last shown position */
int64_t frame_queue_last_pos(FrameQueue *f);

#endif //FFAUDIO_FRAME_QUEUE_UTILS_H