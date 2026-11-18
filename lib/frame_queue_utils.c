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

#include "frame_queue_utils.h"
#include "packet_queue_utils.h"

/*
 * Single producer (audio thread) / single consumer (SDL callback) queue of decoded frames.
 * windex is only touched by the producer, rindex only by the consumer, size under the mutex.
 */

void frame_queue_unref_item(Frame *vp)
{
    av_frame_unref(vp->frame);
}

int frame_queue_init(FrameQueue *f, PacketQueue *pktq, int max_size)
{
    memset(f, 0, sizeof(FrameQueue));
    if (!(f->mutex = SDL_CreateMutex()))
        return AVERROR(ENOMEM);
    if (!(f->cond = SDL_CreateCond()))
        return AVERROR(ENOMEM);
    f->pktq = pktq;
    f->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
    for (int i = 0; i < f->max_size; i++)
        if (!(f->queue[i].frame = av_frame_alloc()))
            return AVERROR(ENOMEM);
    return 0;
}

void frame_queue_destroy(FrameQueue *f)
{
    for (int i = 0; i < f->max_size; i++) {
        Frame *vp = &f->queue[i];
        if (vp->frame) {
            frame_queue_unref_item(vp);
            av_frame_free(&vp->frame);
        }
    }
    SDL_DestroyMutex(f->mutex);
    SDL_DestroyCond(f->cond);
    f->mutex = NULL;
    f->cond = NULL;
}

void frame_queue_signal(FrameQueue *f)
{
    SDL_LockMutex(f->mutex);
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

Frame *frame_queue_peek_writable(FrameQueue *f)
{
    /* wait until we have space to put a new frame */
    SDL_LockMutex(f->mutex);
    while ((f->size >= f->max_size || (f->max_samples > 0 && f->size > 0 && f->queued_samples >= f->max_samples))
           && !packet_queue_aborted(f->pktq)) {
        SDL_CondWaitTimeout(f->cond, f->mutex, 100);
    }
    SDL_UnlockMutex(f->mutex);

    if (packet_queue_aborted(f->pktq))
        return NULL;

    return &f->queue[f->windex];
}

Frame *frame_queue_peek_nb(FrameQueue *f)
{
    SDL_LockMutex(f->mutex);
    const int size = f->size;
    SDL_UnlockMutex(f->mutex);

    if (size <= 0)
        return NULL;
    return &f->queue[f->rindex];
}

void frame_queue_push(FrameQueue *f)
{
    const int samples = f->queue[f->windex].frame->nb_samples;
    if (++f->windex == f->max_size)
        f->windex = 0;
    SDL_LockMutex(f->mutex);
    f->size++;
    f->queued_samples += samples;
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

void frame_queue_next(FrameQueue *f)
{
    const int samples = f->queue[f->rindex].frame->nb_samples;
    frame_queue_unref_item(&f->queue[f->rindex]);
    if (++f->rindex == f->max_size)
        f->rindex = 0;
    SDL_LockMutex(f->mutex);
    f->size--;
    f->queued_samples -= samples;
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

void frame_queue_set_max_samples(FrameQueue *f, const int64_t max_samples)
{
    SDL_LockMutex(f->mutex);
    f->max_samples = max_samples;
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

int frame_queue_nb_remaining(FrameQueue *f)
{
    SDL_LockMutex(f->mutex);
    const int size = f->size;
    SDL_UnlockMutex(f->mutex);
    return size;
}
