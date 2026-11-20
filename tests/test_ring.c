/*
 * The lock-free sample ring (lib/sample_ring.c, built into this test): wrap-around, partial reads, skips and room
 * accounting on one thread, then a producer and a consumer thread checking every sample and marker (run it under
 * ThreadSanitizer to check the ordering of the indices).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "SDL2/SDL.h"
#include "../lib/sample_ring.c"

static int failures = 0;
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #expr); failures++; } } while (0)

#define STRIDE 2

static void fill(float *buf, const uint32_t first_frame, const int frames) {
    for (int i = 0; i < frames; i++) {
        buf[i * STRIDE] = (float)(first_frame + (uint32_t)i);
        buf[i * STRIDE + 1] = -(float)(first_frame + (uint32_t)i);
    }
}

static void test_single_thread(void) {
    SampleRing r;
    memset(&r, 0, sizeof(r));
    CHECK(!ring_has_room(&r, 1));   // no storage yet
    CHECK(ring_peek(&r) == NULL);

    const uint32_t cap = 1024;
    float *base = calloc(cap * STRIDE, sizeof(float));
    float *old;
    size_t old_floats;
    ring_set_storage(&r, base, cap, STRIDE, cap * STRIDE, &old, &old_floats);
    CHECK(old == NULL && ring_capacity(&r) == cap);

    // Chunks of 300 frames: the fourth wraps around the end of the storage
    float src[600 * STRIDE], dst[600 * STRIDE];
    uint32_t written = 0, readpos = 0;
    for (int round = 0; round < 20; round++) {
        while (ring_has_room(&r, 300)) {
            fill(src, written, 300);
            const RingMarker info = { .serial = round, .rate = 48000, .channels = STRIDE, .pts = written, .speed = 1 };
            ring_write(&r, src, 300, &info);
            written += 300;
        }
        CHECK(ring_used_frames(&r, round, NULL) + 300 > cap);
        CHECK(ring_used_frames(&r, round, NULL) <= cap);
        // Take one marker in two reads of different sizes
        const RingMarker *m = ring_peek(&r);
        CHECK(m && m->frames == 300 && m->start == readpos && m->pts == (double)readpos);
        CHECK(ring_read(&r, dst, 100) == 100 && ring_offset(&r) == 100);
        CHECK(ring_read(&r, dst + 100 * STRIDE, 500) == 200 && ring_offset(&r) == 0);
        for (int i = 0; i < 300; i++)
            CHECK(dst[i * STRIDE] == (float)(readpos + (uint32_t)i) && dst[i * STRIDE + 1] == -(float)(readpos + (uint32_t)i));
        readpos += 300;
        // Drop the next one
        if (ring_peek(&r)) {
            ring_skip(&r);
            readpos += 300;
        }
    }
    // Empty it: then the whole storage is free again
    while (ring_peek(&r)) ring_skip(&r);
    CHECK(ring_pending(&r) == 0 && ring_used_frames(&r, 0, NULL) == 0 && ring_has_room(&r, cap));

    // Storage can change while nothing is pending
    float *bigger = calloc(4096 * STRIDE, sizeof(float));
    ring_set_storage(&r, bigger, 4096, STRIDE, 4096 * STRIDE, &old, &old_floats);
    CHECK(old == base && old_floats == cap * STRIDE && ring_capacity(&r) == 4096);
    fill(src, 7, 10);
    const RingMarker info = { .serial = 1, .rate = 44100, .channels = STRIDE, .pts = NAN, .speed = 2 };
    ring_write(&r, src, 10, &info);
    const RingMarker *m = ring_peek(&r);
    CHECK(m && m->frames == 10 && m->rate == 44100 && isnan(m->pts) && m->speed == 2);
    CHECK(ring_read(&r, dst, 10) == 10 && dst[0] == 7.0f);
    free(base);
    free(bigger);
}

// Two threads: random chunk sizes in, random read sizes out
#define CHUNKS 4000
static SampleRing ring;
static uint32_t rng = 12345;
static uint32_t next_rand(uint32_t *s) { *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return *s; }

static int producer(void *arg) {
    (void)arg;
    float *src = malloc(3000 * STRIDE * sizeof(float));
    uint32_t frame = 0, seed = 777;
    for (int c = 0; c < CHUNKS; c++) {
        const int n = 1 + (int)(next_rand(&seed) % 3000);
        while (!ring_has_room(&ring, (uint32_t)n)) SDL_Delay(0);
        fill(src, frame, n);
        const RingMarker info = { .serial = c, .rate = 48000, .channels = STRIDE, .pts = frame, .speed = 1 };
        ring_write(&ring, src, n, &info);
        frame += (uint32_t)n;
    }
    free(src);
    return (int)frame;
}

static void test_two_threads(void) {
    memset(&ring, 0, sizeof(ring));
    const uint32_t cap = 8192;
    float *base = malloc(cap * STRIDE * sizeof(float));
    float *old;
    size_t old_floats;
    ring_set_storage(&ring, base, cap, STRIDE, cap * STRIDE, &old, &old_floats);

    SDL_Thread *th = SDL_CreateThread(producer, "producer", NULL);
    CHECK(th != NULL);
    float *dst = malloc(2048 * STRIDE * sizeof(float));
    uint32_t expect = 0;
    int markers = 0, bad = 0;
    while (markers < CHUNKS) {
        const RingMarker *m = ring_peek(&ring);
        if (!m) { SDL_Delay(0); continue; }
        // A marker is checked when reading of it starts; `markers` is the index of the current one
        if (ring_offset(&ring) == 0 && (m->serial != markers || m->start != expect || m->pts != (double)expect))
            bad++;
        const int got = ring_read(&ring, dst, 1 + (int)(next_rand(&rng) % 2048));
        for (int i = 0; i < got; i++)
            if (dst[i * STRIDE] != (float)(expect + (uint32_t)i) || dst[i * STRIDE + 1] != -(float)(expect + (uint32_t)i))
                bad++;
        expect += (uint32_t)got;
        markers = (int)ring.rd;
    }
    int produced = 0;
    SDL_WaitThread(th, &produced);
    printf("   %d chunks, %u frames through a %u-frame ring\n", CHUNKS, expect, cap);
    CHECK(bad == 0);
    CHECK((uint32_t)produced == expect);
    CHECK(ring_peek(&ring) == NULL && ring_pending(&ring) == 0);
    free(dst);
    free(base);
}

int main(void) {
    if (SDL_Init(0) != 0) return 2;
    printf("-- test_single_thread\n");
    test_single_thread();
    printf("-- test_two_threads\n");
    test_two_threads();
    SDL_Quit();
    if (failures) {
        fprintf(stderr, "test_ring: %d check(s) failed\n", failures);
        return 1;
    }
    printf("test_ring: OK\n");
    return 0;
}
