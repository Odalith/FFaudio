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
 * Decoded audio between one producer (a track's decoder thread) and one consumer (the SDL audio callback), without
 * locks. The frames are interleaved floats in one buffer; every chunk the producer writes gets a marker that says where
 * its frames are and what they are (rate, channels, seek serial, file position, speed). The producer only moves
 * write_marker and the consumer only read_marker, both through atomics.h, so each side sees the other's work in order.
 * The consumer never allocates, frees, locks or signals; how the producer waits for room is up to its caller.
 *
 * No SDL or FFmpeg in here, so tests/test_ring.c can build it on its own.
 */

#ifndef FFAUDIO_SAMPLE_RING_H
#define FFAUDIO_SAMPLE_RING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "atomics.h"

#define RING_MARKERS 256              // chunks that can be in the ring at once (a power of two)
#define RING_CACHE_LINE 64

typedef struct RingMarker {
    uint32_t start;                   // first frame, counted since the ring was created (wraps)
    int32_t frames;
    int32_t serial;                   // seek generation it was decoded for
    int32_t rate, channels;
    double pts;                       // file position of the first frame in seconds, NAN = unknown
    double speed;                     // playback speed it was made with (file seconds per output second)
} RingMarker;

typedef struct SampleRing {
    // Storage. Changed by the producer only while no marker is pending, and read by the consumer only for pending
    // markers, so the marker that publishes data also publishes where it is.
    float *base;
    uint32_t frame_mask;              // capacity in frames - 1 (the capacity is a power of two)
    int32_t stride;                   // floats per frame
    size_t floats;                    // allocated size of base
    RingMarker markers[RING_MARKERS];

    char pad0[RING_CACHE_LINE];
    int32_t write_marker;             // markers published by the producer (atomics.h)
    uint32_t wpos;                    // producer only: position of the next frame

    char pad1[RING_CACHE_LINE];
    int32_t read_marker;              // markers taken or dropped by the consumer (atomics.h)
    uint32_t rd;                      // consumer only: the same, not atomic
    int32_t rd_offset;                // consumer only: frames of markers[rd] already taken
    char pad2[RING_CACHE_LINE];
} SampleRing;

//+-----------------------------------------------------+
//| Producer                                            |
//+-----------------------------------------------------+

// Markers the consumer has not taken or dropped yet
uint32_t ring_pending(const SampleRing *r);

// Frames in the ring (the oldest marker counts whole until it is done). *stale: the oldest marker is not of `serial`
// (the consumer drops it); may be NULL.
uint32_t ring_used_frames(const SampleRing *r, int32_t serial, bool *stale);

// A chunk of `frames` fits: a free marker and room in the storage
bool ring_has_room(const SampleRing *r, uint32_t frames);

// Frames the storage holds; 0 = none yet
uint32_t ring_capacity(const SampleRing *r);

// New storage of capacity_frames (a power of two) frames of `stride` floats. Only while ring_pending() == 0.
// *old / *old_floats receive the previous storage (NULL when there was none) for the caller to keep or free.
void ring_set_storage(SampleRing *r, float *base, uint32_t capacity_frames, int32_t stride, size_t floats,
                      float **old, size_t *old_floats);

// Copy `frames` interleaved frames in and publish them as one marker described by *info (its start and frames are
// filled in). ring_has_room(r, frames) must be true and info->channels must equal the storage stride.
void ring_write(SampleRing *r, const float *src, int32_t frames, const RingMarker *info);

//+-----------------------------------------------------+
//| Consumer                                            |
//+-----------------------------------------------------+

// The marker to take from, or NULL when the ring is empty. ring_offset() frames of it are taken already.
const RingMarker *ring_peek(SampleRing *r);
int32_t ring_offset(const SampleRing *r);

// Copy up to max frames of the current marker to dst; returns how many. Moves to the next marker when it is done.
int32_t ring_read(SampleRing *r, float *dst, int32_t max);

// Drop the rest of the current marker
void ring_skip(SampleRing *r);

// At least `frames` frames are there to take (counting from the current marker, all serials), or half of the markers
// are in use (in tiny chunks the ring may never hold that many frames: waiting for them would wait forever)
bool ring_has_frames(SampleRing *r, uint32_t frames);

#endif //FFAUDIO_SAMPLE_RING_H
