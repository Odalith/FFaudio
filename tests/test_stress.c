/*
 * Stress tests: random API sequences, API calls from inside callbacks, and the
 * "bad file + queued next + play from on_eof" scenario recorded in the old `Segfault` notes.
 *
 * These are only meaningful under a sanitizer build (FFAUDIO_SANITIZER=address or thread),
 * but they also catch hangs and crashes in a plain build.
 *
 * Env: FFAUDIO_STRESS_ITERS (default 300), FFAUDIO_STRESS_SEED (default time based)
 */
#include "test_common.h"
#include <time.h>

static const char *good_files[] = {
    "s44_stereo.flac", "s48_stereo.mp3", "s96_stereo.flac", "s44_mono.wav",
    "s48_51.flac", "s44_flt.wav", "s48_stereo.ogg", "long_6s.flac",
};
static const char *bad_files[] = { "empty.flac", "text.mp3", "truncated.flac", "video_only.mkv" };
#define N_GOOD ((int)(sizeof(good_files) / sizeof(good_files[0])))
#define N_BAD ((int)(sizeof(bad_files) / sizeof(bad_files[0])))

static uint32_t rng_state = 1;
static uint32_t rnd(void) { /* xorshift32, deterministic per seed */
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static const char *any_file(void) {
    if (rnd() % 5 == 0) return fx(bad_files[rnd() % N_BAD]);
    return fx(good_files[rnd() % N_GOOD]);
}

/* ---------- 1. Random API sequences from the main thread ---------- */

static void test_random_ops(void) {
    const char *it_env = getenv("FFAUDIO_STRESS_ITERS");
    const int iters = it_env ? atoi(it_env) : 300;

    for (int i = 0; i < iters; i++) {
        switch (rnd() % 12) {
            case 0: case 1: case 2: au_play_audio(any_file(), NULL); break;
            case 3: case 4: au_set_next(any_file(), NULL); break;
            case 5: au_skip_to_next(); break;
            case 6: au_clear_next(); break;
            case 7: au_stop_audio(); break;
            case 8: au_pause_audio(rnd() % 2); break;
            case 9: au_seek_time(rnd() % 3000); break;
            case 10: au_seek_percent((double)(rnd() % 100)); break;
            case 11: au_set_audio_volume((int)(rnd() % 101)); (void)au_get_audio_play_time(); break;
        }
        tc_sleep_ms(rnd() % 40);
    }
    au_pause_audio(false);
    au_stop_audio();
}

/* ---------- 2. Playlist driven entirely from callbacks (the Segfault scenario) ---------- */

static SDL_atomic_t playlist_pos;
static int playlist_len = 0;

static const char *playlist_file(int i) {
    /* Alternate: every third entry is broken */
    if (i % 3 == 1) return fx(bad_files[i % N_BAD]);
    return fx(good_files[i % N_GOOD]);
}

static void playlist_on_eof(bool skip, bool error, int32_t handle) {
    (void)handle;
    if (skip) return;
    if (error) {
        /* What the old manual test did: start the next file right from the EOF callback */
        const int i = SDL_AtomicAdd(&playlist_pos, 1);
        if (i < playlist_len) au_play_audio(playlist_file(i), NULL);
    }
}

static void playlist_on_prepare_next(void) {
    const int i = SDL_AtomicAdd(&playlist_pos, 1);
    if (i < playlist_len) au_set_next(playlist_file(i), NULL);
}

static void test_callback_driven_playlist(void) {
    playlist_len = 24;
    SDL_AtomicSet(&playlist_pos, 1);
    SDL_LockMutex(tc_mutex);
    tc_on_eof_hook = playlist_on_eof;
    tc_on_prepare_next_hook = playlist_on_prepare_next;
    SDL_UnlockMutex(tc_mutex);

    au_play_audio(playlist_file(0), NULL);

    /* Each good track is ~1 s (one is 6 s); generous timeout, the point is no crash / no hang */
    const uint32_t deadline = SDL_GetTicks() + 60000;
    while (SDL_AtomicGet(&playlist_pos) < playlist_len && SDL_GetTicks() < deadline)
        tc_sleep_ms(50);
    CHECK(SDL_AtomicGet(&playlist_pos) >= playlist_len);

    SDL_LockMutex(tc_mutex);
    tc_on_eof_hook = NULL;
    tc_on_prepare_next_hook = NULL;
    SDL_UnlockMutex(tc_mutex);
    au_stop_audio();
    tc_sleep_ms(200);
}

/* ---------- 3. Rapid replacement of the current track ---------- */

static void test_rapid_replace(void) {
    for (int i = 0; i < 100; i++) {
        au_play_audio(any_file(), NULL);
        if (i % 4 == 0) au_set_next(any_file(), NULL);
    }
    tc_sleep_ms(300);
    au_stop_audio();
}

/* ---------- 4. A callback that makes many notifications ---------- */

// A device change from a callback runs inline and queues a device notification. 40 of them grow the list of
// notifications while it is being dispatched; that used to free through a pointer into the old list (ASan sees it).
static AudioDeviceConfig named_device;

static void change_device_many(bool skip, bool error, int32_t handle) {
    (void)skip; (void)error; (void)handle;
    for (int i = 0; i < 40; i++)
        au_configure_audio_device(i % 2 ? NULL : &named_device);
}

static void test_callback_adds_many_notes(void) {
    int n = 0;
    char **devs = NULL;
    if (au_get_audio_devices(&n, &devs) != AU_OK || n < 1) {
        printf("   no named output device: skipped\n");
        return;
    }
    named_device = (AudioDeviceConfig){ .audio_device = devs[0], .audio_device_index = 0 };
    SDL_LockMutex(tc_mutex);
    tc_on_eof_hook = change_device_many;
    SDL_UnlockMutex(tc_mutex);
    au_play_audio(fx("s44_stereo.flac"), NULL);
    CHECK(TC_WAIT_FIELD(eof, 1, 5000));
    CHECK(TC_WAIT_FIELD(device, 40, 10000));
    SDL_LockMutex(tc_mutex);
    tc_on_eof_hook = NULL;
    SDL_UnlockMutex(tc_mutex);
    au_configure_audio_device(NULL);
    au_free_audio_devices(devs, n);
}

int main(int argc, char **argv) {
    tc_setup(argc, argv);

    const char *seed_env = getenv("FFAUDIO_STRESS_SEED");
    rng_state = seed_env ? (uint32_t)strtoul(seed_env, NULL, 10) : (uint32_t)time(NULL);
    if (rng_state == 0) rng_state = 1;
    printf("seed=%u (set FFAUDIO_STRESS_SEED to reproduce)\n", rng_state);

    REQUIRE(tc_init_player(0) == 0);

    RUN_TEST(test_rapid_replace);
    RUN_TEST(test_callback_driven_playlist);
    RUN_TEST(test_random_ops);
    RUN_TEST(test_callback_adds_many_notes);

    au_shutdown();
    return tc_finish("test_stress");
}
