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

#include <string.h>
#include "sample_ring.h"

#define MARKER_MASK (RING_MARKERS - 1)

//+-----------------------------------------------------+
//| Producer                                            |
//+-----------------------------------------------------+

uint32_t ring_pending(const SampleRing *r) {
    return (uint32_t)ffa_load_relaxed(&r->write_marker) - (uint32_t)ffa_load_acquire(&r->read_marker);
}

uint32_t ring_used_frames(const SampleRing *r, const int32_t serial, bool *stale) {
    const uint32_t w = (uint32_t)ffa_load_relaxed(&r->write_marker);
    const uint32_t rm = (uint32_t)ffa_load_acquire(&r->read_marker);
    if (stale) *stale = false;
    if (w == rm) return 0;
    const RingMarker *oldest = &r->markers[rm & MARKER_MASK];
    if (stale) *stale = oldest->serial != serial;
    return r->wpos - oldest->start;
}

uint32_t ring_capacity(const SampleRing *r) {
    return r->base ? r->frame_mask + 1 : 0;
}

bool ring_has_room(const SampleRing *r, const uint32_t frames) {
    if (!r->base || ring_pending(r) >= RING_MARKERS) return false;
    return ring_used_frames(r, 0, NULL) + frames <= ring_capacity(r);
}

void ring_set_storage(SampleRing *r, float *base, const uint32_t capacity_frames, const int32_t stride,
                      const size_t floats, float **old, size_t *old_floats) {
    *old = r->base;
    *old_floats = r->floats;
    r->base = base;
    r->frame_mask = capacity_frames - 1;
    r->stride = stride;
    r->floats = floats;
}

void ring_write(SampleRing *r, const float *src, const int32_t frames, const RingMarker *info) {
    const uint32_t cap = r->frame_mask + 1;
    const uint32_t pos = r->wpos & r->frame_mask;
    const uint32_t first = (uint32_t)frames < cap - pos ? (uint32_t)frames : cap - pos;
    const size_t stride = (size_t)r->stride;
    memcpy(r->base + pos * stride, src, first * stride * sizeof(float));
    if ((uint32_t)frames > first)
        memcpy(r->base, src + first * stride, ((uint32_t)frames - first) * stride * sizeof(float));

    const uint32_t w = (uint32_t)ffa_load_relaxed(&r->write_marker);
    RingMarker *m = &r->markers[w & MARKER_MASK];
    *m = *info;
    m->start = r->wpos;
    m->frames = frames;
    r->wpos += (uint32_t)frames;
    ffa_store_release(&r->write_marker, (int32_t)(w + 1)); // publishes the frames and the marker
}

//+-----------------------------------------------------+
//| Consumer                                            |
//+-----------------------------------------------------+

const RingMarker *ring_peek(SampleRing *r) {
    if ((uint32_t)ffa_load_acquire(&r->write_marker) == r->rd) return NULL;
    return &r->markers[r->rd & MARKER_MASK];
}

int32_t ring_offset(const SampleRing *r) {
    return r->rd_offset;
}

static void advance(SampleRing *r) {
    r->rd++;
    r->rd_offset = 0;
    ffa_store_release(&r->read_marker, (int32_t)r->rd); // hands the marker and its frames back to the producer
}

int32_t ring_read(SampleRing *r, float *dst, const int32_t max) {
    const RingMarker *m = &r->markers[r->rd & MARKER_MASK];
    const int32_t left = m->frames - r->rd_offset;
    if (left <= 0) { // nothing (left) in it: never stop on it
        advance(r);
        return 0;
    }
    const int32_t n = max < left ? max : left;
    if (n <= 0) return 0;
    const uint32_t cap = r->frame_mask + 1;
    const uint32_t pos = (m->start + (uint32_t)r->rd_offset) & r->frame_mask;
    const uint32_t first = (uint32_t)n < cap - pos ? (uint32_t)n : cap - pos;
    const size_t stride = (size_t)r->stride;
    memcpy(dst, r->base + pos * stride, first * stride * sizeof(float));
    if ((uint32_t)n > first)
        memcpy(dst + first * stride, r->base, ((uint32_t)n - first) * stride * sizeof(float));
    r->rd_offset += n;
    if (r->rd_offset >= m->frames) advance(r);
    return n;
}

void ring_skip(SampleRing *r) {
    advance(r);
}
