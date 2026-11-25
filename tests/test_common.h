/*
 * Shared helpers for the headless ffaudio test suite.
 * Tests run with SDL_AUDIODRIVER=dummy (set by CTest) so no sound hardware is needed.
 */
#ifndef FFAUDIO_TEST_COMMON_H
#define FFAUDIO_TEST_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>

#include "SDL2/SDL.h"
#include "../lib/ffaudio.h"

/* ---------- assertions ---------- */

static int tc_failures = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        tc_failures++; \
    } } while (0)

#define REQUIRE(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "REQUIRE FAILED %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        exit(1); \
    } } while (0)

/* ---------- fixtures ---------- */

static const char *tc_fixture_dir = NULL;
static SDL_mutex *tc_mutex = NULL;

static const char *fx(const char *name) {
    /* Ring of buffers so several fx() results can be used in one expression. Callbacks call this
     * from the control thread too, and the library copies the path right away, so a lock is enough. */
    static char bufs[16][1024];
    static int idx = 0;
    if (tc_mutex) SDL_LockMutex(tc_mutex);
    char *b = bufs[idx++ & 15];
    snprintf(b, 1024, "%s/%s", tc_fixture_dir, name);
    if (tc_mutex) SDL_UnlockMutex(tc_mutex);
    return b;
}

static AuStatus tc_status(void) {
    AuStatus s;
    au_get_status(&s);
    return s;
}

/* ---------- callback event recording ---------- */

typedef struct TcEvents {
    int eof;
    int eof_error;
    int eof_skip;
    int restart;
    int restart_loop;
    int prepare_next;
    int started;
    int duration;
    int device;
    int devices_changed;
    int device_failed;                 // AU_EVENT_DEVICE with ok = false
    int opened;                        // AU_EVENT_TRACK_OPENED
    int opened_with_info;              // ... where au_track_info() already had the track's details
    double opened_duration;            // data.opened.duration of the last one
    int buffering_on, buffering_off;   // on_buffering transitions (progress calls not counted)
    int meta_count;                    // on_stream_metadata calls; the first 8 are kept, with the play time then
    char meta_title[8][96], meta_artist[8][96];
    double meta_pos[8];
    int log_error;
    int32_t last_eof_handle;
    int32_t last_started_handle;
    int32_t last_prepare_handle;       // AU_EVENT_PREPARE_NEXT's handle (the track that nears its end)
    double last_duration;
} TcEvents;

static SDL_cond *tc_cond = NULL;
static TcEvents tc_ev;
static bool tc_last_buffering;   // last on_buffering state, to count transitions
static bool tc_verbose = false;

/* Optional hooks a test can install to act from inside callbacks */
static void (*tc_on_eof_hook)(bool skip, bool error, int32_t handle) = NULL;
static void (*tc_on_prepare_next_hook)(void) = NULL;

static void tc_bump(int *field);

static void tc_bump(int *field) {
    SDL_LockMutex(tc_mutex);
    (*field)++;
    SDL_CondBroadcast(tc_cond);
    SDL_UnlockMutex(tc_mutex);
}

// The last log line from FFmpeg (they start with "FFmpeg"), under tc_mutex
static char tc_ffmpeg_log[512];
static int tc_ffmpeg_log_level = -1;

static void tc_on_log(const AuEvent *e) {
    const char *message = e->data.log.message;
    if (message && !strncmp(message, "FFmpeg", 6)) {
        SDL_LockMutex(tc_mutex);
        snprintf(tc_ffmpeg_log, sizeof(tc_ffmpeg_log), "%s", message);
        tc_ffmpeg_log_level = (int)e->data.log.level;
        SDL_UnlockMutex(tc_mutex);
    }
    if (e->data.log.level >= AU_LOG_ERROR) tc_bump(&tc_ev.log_error);
    if (tc_verbose || e->data.log.level >= AU_LOG_ERROR)
        fprintf(stderr, "[ffaudio %d] %s\n", (int)e->data.log.level, message ? message : "(null)");
}

static void tc_on_ended(const AuEvent *e) {
    const bool skip = e->data.ended.reason == AU_END_SKIPPED, error = e->data.ended.reason == AU_END_ERROR;
    SDL_LockMutex(tc_mutex);
    tc_ev.eof++;
    if (error) tc_ev.eof_error++;
    if (skip) tc_ev.eof_skip++;
    tc_ev.last_eof_handle = e->handle;
    SDL_CondBroadcast(tc_cond);
    void (*eof_hook)(bool, bool, int32_t) = tc_on_eof_hook;
    SDL_UnlockMutex(tc_mutex);
    if (tc_verbose) fprintf(stderr, "ENDED handle=%d skip=%d error=%d\n", e->handle, skip, error);
    if (eof_hook) eof_hook(skip, error, e->handle);
}

static void tc_on_stream_metadata(const AuEvent *e) {
    const double pos = tc_status().position;
    SDL_LockMutex(tc_mutex);
    const int i = tc_ev.meta_count++;
    if (i < 8) {
        snprintf(tc_ev.meta_title[i], sizeof(tc_ev.meta_title[i]), "%s", e->data.metadata.title ? e->data.metadata.title : "");
        snprintf(tc_ev.meta_artist[i], sizeof(tc_ev.meta_artist[i]), "%s", e->data.metadata.artist ? e->data.metadata.artist : "");
        tc_ev.meta_pos[i] = pos;
    }
    SDL_CondBroadcast(tc_cond);
    SDL_UnlockMutex(tc_mutex);
}

// Every event of the players the tests start (on ffaudio's control thread)
static void tc_event_cb(const AuEvent *e, void *ud) {
    (void)ud;
    switch (e->type) {
        case AU_EVENT_LOG:
            tc_on_log(e);
            break;
        case AU_EVENT_TRACK_ENDED:
            tc_on_ended(e);
            break;
        case AU_EVENT_RESTARTED:
            if (tc_verbose) fprintf(stderr, "RESTART handle=%d pos=%.3f loop=%d\n", e->handle,
                                    e->data.restarted.position, e->data.restarted.from_loop);
            if (e->data.restarted.from_loop) tc_bump(&tc_ev.restart_loop);
            tc_bump(&tc_ev.restart);
            break;
        case AU_EVENT_PREPARE_NEXT: {
            if (tc_verbose) fprintf(stderr, "PREPARE_NEXT current=%d\n", e->handle);
            SDL_LockMutex(tc_mutex);
            tc_ev.last_prepare_handle = e->handle;
            SDL_UnlockMutex(tc_mutex);
            tc_bump(&tc_ev.prepare_next);
            SDL_LockMutex(tc_mutex);
            void (*prep_hook)(void) = tc_on_prepare_next_hook;
            SDL_UnlockMutex(tc_mutex);
            if (prep_hook) prep_hook();
            break;
        }
        case AU_EVENT_TRACK_STARTED:
            if (tc_verbose) fprintf(stderr, "STARTED handle=%d\n", e->handle);
            SDL_LockMutex(tc_mutex);
            tc_ev.started++;
            tc_ev.last_started_handle = e->handle;
            SDL_CondBroadcast(tc_cond);
            SDL_UnlockMutex(tc_mutex);
            break;
        case AU_EVENT_DURATION:
            SDL_LockMutex(tc_mutex);
            tc_ev.duration++;
            tc_ev.last_duration = e->data.duration.seconds;
            SDL_CondBroadcast(tc_cond);
            SDL_UnlockMutex(tc_mutex);
            break;
        case AU_EVENT_BUFFERING:
            SDL_LockMutex(tc_mutex);
            if (e->data.buffering.active != tc_last_buffering) {
                if (e->data.buffering.active) tc_ev.buffering_on++;
                else tc_ev.buffering_off++;
                tc_last_buffering = e->data.buffering.active;
            }
            SDL_UnlockMutex(tc_mutex);
            break;
        case AU_EVENT_STREAM_METADATA:
            tc_on_stream_metadata(e);
            break;
        case AU_EVENT_DEVICES_CHANGED:
            tc_bump(&tc_ev.devices_changed);
            break;
        case AU_EVENT_TRACK_OPENED: {
            const AuMediaInfo *info = au_track_info(e->handle);   // the event promises it is there now
            SDL_LockMutex(tc_mutex);
            tc_ev.opened++;
            if (info) tc_ev.opened_with_info++;
            tc_ev.opened_duration = e->data.opened.duration;
            SDL_CondBroadcast(tc_cond);
            SDL_UnlockMutex(tc_mutex);
            au_media_info_release(info);
            break;
        }
        case AU_EVENT_DEVICE:
            if (!e->data.device.ok) tc_bump(&tc_ev.device_failed);
            else tc_bump(&tc_ev.device);
            break;
        default:
            break;
    }
}

static TcEvents tc_snapshot(void) {
    SDL_LockMutex(tc_mutex);
    const TcEvents e = tc_ev;
    SDL_UnlockMutex(tc_mutex);
    return e;
}

static void tc_reset_events(void) {
    SDL_LockMutex(tc_mutex);
    memset(&tc_ev, 0, sizeof(tc_ev));
    tc_last_buffering = false;
    SDL_UnlockMutex(tc_mutex);
}

/* Wait until `field` of the event counters reaches `target` or the timeout elapses. */
#define TC_WAIT_FIELD(field, target, timeout_ms) tc_wait_field(offsetof(TcEvents, field), (target), (timeout_ms))

#include <stddef.h>
static bool tc_wait_field(size_t offset, int target, uint32_t timeout_ms) {
    const uint32_t deadline = SDL_GetTicks() + timeout_ms;
    bool ok = false;
    SDL_LockMutex(tc_mutex);
    for (;;) {
        const int value = *(int *)((char *)&tc_ev + offset);
        if (value >= target) { ok = true; break; }
        const uint32_t now = SDL_GetTicks();
        if ((int32_t)(deadline - now) <= 0) break;
        SDL_CondWaitTimeout(tc_cond, tc_mutex, deadline - now);
    }
    SDL_UnlockMutex(tc_mutex);
    return ok;
}

static void tc_sleep_ms(uint32_t ms) { SDL_Delay(ms); }

/* ---------- player setup ---------- */

// Output channel count and sample rate for the next tc_init_player*() call (0 = device default)
static int tc_output_channels = 0;
static int tc_output_rate = 0;

// Start a player with the tests' settings, and open the system default device in the format asked for (one
// AU_EVENT_DEVICE)
static int tc_initialize(int loop_count, double prepare_next_seconds) {
    const int ret = au_init("ffaudio-tests", tc_event_cb, NULL);
    if (ret != AU_OK) return ret;
    au_set_volume(0.5f);
    au_set_loop_count(loop_count);
    if (prepare_next_seconds > 0) au_set_prepare_next(prepare_next_seconds);
    if (au_set_output_format(tc_output_channels, tc_output_rate) != AU_OK) return AU_ERR_INVALID_ARGUMENT;
    return au_open_device();
}

// A player with its device open: waits for the first AU_EVENT_DEVICE (the tests count from a known state)
static int tc_init_player_cfg(int loop_count, double prepare_next_seconds) {
    const int ret = tc_initialize(loop_count, prepare_next_seconds);
    if (ret != 0) return ret;
    if (!TC_WAIT_FIELD(device, 1, 5000)) return AU_ERR_DEVICE;
    return AU_OK;
}

static int tc_init_player(int loop_count) {
    return tc_init_player_cfg(loop_count, 0);
}

// A play-options object with a range (0, 0 = the whole file) and filters (NULL = none). au_play() and au_queue()
// copy it: tc_play()/tc_queue() free it right after the call.
static AuPlayOptions *tc_options(double start_seconds, double length_seconds, const char *filters) {
    AuPlayOptions *o = au_play_options_create();
    REQUIRE(o);
    REQUIRE(au_play_options_set_range(o, start_seconds, length_seconds) == AU_OK);
    REQUIRE(au_play_options_set_filters(o, filters) == AU_OK);
    return o;
}

static int32_t tc_play(const char *path, AuPlayOptions *options) {
    const int32_t h = au_play(path, options);
    au_play_options_free(options);
    return h;
}

static int32_t tc_queue(const char *path, AuPlayOptions *options) {
    const int32_t h = au_queue(path, options);
    au_play_options_free(options);
    return h;
}

static void tc_setup(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <fixture-dir>\n", argv[0]);
        exit(2);
    }
    tc_fixture_dir = argv[1];
    tc_verbose = getenv("FFAUDIO_TEST_VERBOSE") != NULL;
    if (!getenv("SDL_AUDIODRIVER")) {
#ifdef _WIN32
        _putenv("SDL_AUDIODRIVER=dummy");
#else
        setenv("SDL_AUDIODRIVER", "dummy", 0);
#endif
    }
    /* SDL_CreateMutex works before SDL_Init in SDL2 */
    tc_mutex = SDL_CreateMutex();
    tc_cond = SDL_CreateCond();
    REQUIRE(tc_mutex && tc_cond);
}

static int tc_finish(const char *name) {
    if (tc_failures) {
        fprintf(stderr, "%s: %d check(s) failed\n", name, tc_failures);
        return 1;
    }
    printf("%s: OK\n", name);
    return 0;
}

#define RUN_TEST(fn) do { \
    printf("-- %s\n", #fn); fflush(stdout); \
    tc_reset_events(); \
    fn(); \
} while (0)

#endif // FFAUDIO_TEST_COMMON_H
