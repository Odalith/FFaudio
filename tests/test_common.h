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

static const char *fx(const char *name) {
    /* Small ring of buffers so several fx() results can be used in one expression */
    static char bufs[8][1024];
    static int idx = 0;
    char *b = bufs[idx++ & 7];
    snprintf(b, 1024, "%s/%s", tc_fixture_dir, name);
    return b;
}

/* ---------- callback event recording ---------- */

typedef struct TcEvents {
    int eof;
    int eof_error;
    int eof_skip;
    int restart;
    int prepare_next;
    int log_error;
    int32_t last_eof_handle;
} TcEvents;

static SDL_mutex *tc_mutex = NULL;
static SDL_cond *tc_cond = NULL;
static TcEvents tc_ev;
static bool tc_verbose = false;

/* Optional hooks a test can install to act from inside callbacks */
static void (*tc_on_eof_hook)(bool skip, bool error, int32_t handle) = NULL;
static void (*tc_on_prepare_next_hook)(void) = NULL;

static void tc_log_cb(const char *message, int64_t request, enum AU_LOG_LEVEL level) {
    (void)request;
    if (level >= ERROR) {
        SDL_LockMutex(tc_mutex);
        tc_ev.log_error++;
        SDL_UnlockMutex(tc_mutex);
    }
    if (tc_verbose || level >= ERROR)
        fprintf(stderr, "[ffaudio %d] %s\n", (int)level, message ? message : "(null)");
}

static void tc_eof_cb(bool is_eof_from_skip, bool is_from_error, int32_t handle) {
    SDL_LockMutex(tc_mutex);
    tc_ev.eof++;
    if (is_from_error) tc_ev.eof_error++;
    if (is_eof_from_skip) tc_ev.eof_skip++;
    tc_ev.last_eof_handle = handle;
    SDL_CondBroadcast(tc_cond);
    SDL_UnlockMutex(tc_mutex);
    if (tc_verbose)
        fprintf(stderr, "EOF handle=%d skip=%d error=%d\n", handle, is_eof_from_skip, is_from_error);
    if (tc_on_eof_hook) tc_on_eof_hook(is_eof_from_skip, is_from_error, handle);
}

static void tc_restart_cb(double position, bool is_from_looping, int32_t remaining) {
    (void)position; (void)is_from_looping; (void)remaining;
    SDL_LockMutex(tc_mutex);
    tc_ev.restart++;
    SDL_CondBroadcast(tc_cond);
    SDL_UnlockMutex(tc_mutex);
}

static void tc_prepare_next_cb(void) {
    SDL_LockMutex(tc_mutex);
    tc_ev.prepare_next++;
    SDL_CondBroadcast(tc_cond);
    SDL_UnlockMutex(tc_mutex);
    if (tc_on_prepare_next_hook) tc_on_prepare_next_hook();
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

static int tc_init_player(int loop_count) {
    const InitializeConfig config = {
        .app_name = "ffaudio-tests",
        .initial_volume = 50,
        .initial_loop_count = loop_count,
        .on_log = tc_log_cb,
        .on_eof = tc_eof_cb,
        .on_restart = tc_restart_cb,
        .on_duration_update = NULL,
        .on_prepare_next = tc_prepare_next_cb,
    };
    const int ret = au_initialize(&config);
    if (ret != 0) return ret;
    return au_configure_audio_device(NULL);
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
