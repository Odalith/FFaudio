/*
 * Background measurement queue tests: requests, de-duplication, priorities, albums, cancelling, errors,
 * worker configuration, and the player's measure_missing / measure_wait_ms.
 */
#include "test_common.h"
#include "../lib/ffaudio_loudness.h"
#include <math.h>

/* ---------- recording the callback ---------- */

#define MAX_DONE 256
static SDL_mutex *done_lock;
static char *done_path[MAX_DONE];
static int done_result[MAX_DONE];
static SDL_threadID done_thread[MAX_DONE];   // the worker that reported it
static int done_n = 0;

static void on_cached(const char *path, int result, const AuReplayGainInfo *rg, void *user) {
    (void)user;
    SDL_LockMutex(done_lock);
    if (done_n < MAX_DONE) {
        done_path[done_n] = strdup(path);
        done_result[done_n] = result;
        done_thread[done_n] = SDL_ThreadID();
        done_n++;
    }
    SDL_UnlockMutex(done_lock);
    if (result == AU_OK && !rg) tc_failures++;
}

static void reset_done(void) {
    SDL_LockMutex(done_lock);
    for (int i = 0; i < done_n; i++) free(done_path[i]);
    done_n = 0;
    SDL_UnlockMutex(done_lock);
}

static int done_count(void) {
    SDL_LockMutex(done_lock);
    const int n = done_n;
    SDL_UnlockMutex(done_lock);
    return n;
}

// Position of `file` among the callbacks, -1 if absent
static int done_index(const char *file) {
    int at = -1;
    SDL_LockMutex(done_lock);
    for (int i = 0; i < done_n && at < 0; i++)
        if (strstr(done_path[i], file)) at = i;
    SDL_UnlockMutex(done_lock);
    return at;
}

static bool wait_idle(uint32_t timeout_ms) {
    const uint32_t end = SDL_GetTicks() + timeout_ms;
    while (au_gain_cache_pending() > 0) {
        if ((int32_t)(end - SDL_GetTicks()) <= 0) return false;
        SDL_Delay(5);
    }
    return true;
}

static void configure(int threads) {
    const AuGainScanConfig cfg = { .threads = threads, .on_cached = on_cached };
    REQUIRE(au_gain_scan_configure(&cfg) == AU_OK);
}

static const char *scan_file(int i) {
    static char names[32][32];
    snprintf(names[i & 31], sizeof(names[0]), "scan_%02d.flac", i);
    return fx(names[i & 31]);
}

/* ---------- tests ---------- */

static void test_request(void) {
    reset_done();
    configure(1);
    const char *paths[3] = { scan_file(1), scan_file(2), scan_file(3) };
    CHECK(au_gain_cache_request(paths, 3, AU_SCAN_PREFETCH) == 3);
    CHECK(wait_idle(10000));
    CHECK(done_count() == 3);
    for (int i = 0; i < 3; i++) {
        AuReplayGainInfo rg;
        CHECK(au_gain_cache_get(paths[i], &rg) && fabs(rg.track_gain_db - 5.0) < 0.1);
    }
    CHECK(done_result[0] == AU_OK && done_result[1] == AU_OK && done_result[2] == AU_OK);
}

static void test_dedup(void) {
    reset_done();
    const char *again[3] = { scan_file(1), scan_file(2), scan_file(3) };
    CHECK(au_gain_cache_request(again, 3, AU_SCAN_PREFETCH) == 0);   // all cached
    const char *dup[4] = { scan_file(4), scan_file(4), scan_file(5), NULL };
    CHECK(au_gain_cache_request(dup, 4, AU_SCAN_PREFETCH) == 2);     // duplicate and NULL skipped
    CHECK(au_gain_cache_request(dup, 3, AU_SCAN_PREFETCH) <= 2);     // queued or being measured: skipped
    CHECK(wait_idle(10000));
    CHECK(done_count() == 2);
    CHECK(au_gain_cache_request(NULL, 3, AU_SCAN_PREFETCH) == 0);
}

static void test_urgent_goes_first(void) {
    reset_done();
    configure(1);
    for (int i = 6; i <= 12; i++) {
        const char *p = scan_file(i);
        au_gain_cache_request(&p, 1, AU_SCAN_PREFETCH);
    }
    const char *last = scan_file(12);
    au_gain_cache_request(&last, 1, AU_SCAN_URGENT);                 // already queued: moved to the front
    CHECK(wait_idle(10000));
    printf("   scan_12 finished at position %d of %d\n", done_index("scan_12"), done_count());
    CHECK(done_count() == 7);
    CHECK(done_index("scan_12") >= 0 && done_index("scan_12") <= 1); // at most the one already running is first
}

// One request's files are shared by the workers (a request used to be one job, measured by one worker)
static void test_request_uses_every_worker(void) {
    reset_done();
    configure(2);
    const char *paths[5] = { scan_file(20), scan_file(21), scan_file(22), scan_file(23), scan_file(24) };
    CHECK(au_gain_cache_request(paths, 5, AU_SCAN_PREFETCH) == 5);
    CHECK(wait_idle(10000));
    CHECK(done_count() == 5);
    int threads = 0;
    SDL_LockMutex(done_lock);
    for (int i = 0; i < done_n; i++) {
        bool seen = false;
        for (int k = 0; k < i && !seen; k++) seen = done_thread[k] == done_thread[i];
        threads += !seen;
    }
    SDL_UnlockMutex(done_lock);
    printf("   5 files of one request measured on %d threads\n", threads);
    CHECK(threads == 2);
    configure(1);
    for (int i = 0; i < 5; i++) au_gain_cache_remove(paths[i]); // the later tests use these files too
}

static void test_cancel(void) {
    reset_done();
    configure(1);
    for (int i = 13; i <= 19; i++) {
        const char *p = scan_file(i);
        au_gain_cache_request(&p, 1, AU_SCAN_PREFETCH);
    }
    au_gain_cache_cancel_requests();
    CHECK(wait_idle(3000));
    CHECK(au_gain_cache_pending() == 0);
    printf("   %d callbacks after cancelling 7 requests\n", done_count());
    CHECK(done_count() <= 1);                                        // only the one already running
    int cached = 0;
    for (int i = 13; i <= 19; i++) cached += au_gain_cache_get(scan_file(i), NULL);
    CHECK(cached <= 1);
}

static void test_album(void) {
    reset_done();
    const char *album[2] = { fx("ld_m23_48k.flac"), fx("ld_m33_48k.flac") };
    CHECK(au_gain_cache_request_album(album, 2, AU_SCAN_PREFETCH) == 2);
    CHECK(wait_idle(10000));
    AuReplayGainInfo a, b;
    CHECK(au_gain_cache_get(album[0], &a) && au_gain_cache_get(album[1], &b));
    CHECK(fabs(a.track_gain_db - 5.0) < 0.1 && fabs(b.track_gain_db - 15.0) < 0.1);
    // Album loudness over both tracks: -25.6 LUFS -> +7.6 dB
    CHECK(a.has_album_gain && fabs(a.album_gain_db - 7.6) < 0.1);
    CHECK(b.has_album_gain && a.album_gain_db == b.album_gain_db);
    CHECK(au_gain_cache_request_album(album, 2, AU_SCAN_PREFETCH) == 0); // album values present: skipped
}

static void test_errors(void) {
    reset_done();
    const char *bad[2] = { fx("does_not_exist.flac"), fx("text.mp3") };
    CHECK(au_gain_cache_request(bad, 2, AU_SCAN_URGENT) == 2);
    CHECK(wait_idle(5000));
    CHECK(done_count() == 2);
    CHECK(done_result[0] == AU_ERR_FILE && done_result[1] == AU_ERR_FILE);
    CHECK(!au_gain_cache_get(bad[0], NULL) && !au_gain_cache_get(bad[1], NULL));

    // They are remembered: asking again queues nothing (playlists used to re-read them on every track change)
    reset_done();
    CHECK(au_gain_cache_request(bad, 2, AU_SCAN_PREFETCH) == 0);
    CHECK(wait_idle(1000));
    CHECK(done_count() == 0);
}

static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    REQUIRE(f);
    fputs(text, f);
    fclose(f);
}

static void test_changed_failure_is_measured_again(void) {
    char path[1100];
    snprintf(path, sizeof(path), "%s/scan_changing.bin", tc_fixture_dir);
    write_text(path, "not audio yet");
    const char *p[1] = { path };

    reset_done();
    CHECK(au_gain_cache_request(p, 1, AU_SCAN_URGENT) == 1);
    CHECK(wait_idle(5000));
    CHECK(done_count() == 1 && done_result[0] == AU_ERR_FILE);
    CHECK(au_gain_cache_request(p, 1, AU_SCAN_URGENT) == 0); // unchanged: skipped

    // The file changes (here: its size), so it is tried again
    write_text(path, "still not audio, but longer now");
    reset_done();
    CHECK(au_gain_cache_request(p, 1, AU_SCAN_URGENT) == 1);
    CHECK(wait_idle(5000));
    CHECK(done_count() == 1 && done_result[0] == AU_ERR_FILE);

    // au_gain_scan_stop() forgets the failures
    au_gain_scan_stop();
    CHECK(au_gain_cache_request(p, 1, AU_SCAN_URGENT) == 1);
    CHECK(wait_idle(5000));
    remove(path);
}

static void test_configure_and_stop(void) {
    reset_done();
    configure(4);
    const char *paths[3] = { scan_file(20), scan_file(21), scan_file(22) };
    CHECK(au_gain_cache_request(paths, 3, AU_SCAN_PREFETCH) == 3);
    configure(1);                                                    // extra workers leave after their job
    CHECK(wait_idle(10000));
    CHECK(done_count() == 3);

    // Idle workers must also leave when the count is lowered (they used to keep waiting, and configure hung)
    configure(4);
    const char *more[4] = { scan_file(9), scan_file(10), scan_file(11), scan_file(12) };
    au_gain_cache_remove(more[0]); au_gain_cache_remove(more[1]);
    au_gain_cache_remove(more[2]); au_gain_cache_remove(more[3]);
    for (int i = 0; i < 4; i++) au_gain_cache_request(&more[i], 1, AU_SCAN_PREFETCH);
    CHECK(wait_idle(10000));
    tc_sleep_ms(50);                                                 // all four workers are idle now
    const uint32_t t0 = SDL_GetTicks();
    configure(1);
    printf("   lowering 4 idle workers to 1 took %u ms\n", SDL_GetTicks() - t0);
    CHECK(SDL_GetTicks() - t0 < 2000);
    au_gain_scan_stop();
    const AuGainScanConfig bad = { .threads = 99 };
    CHECK(au_gain_scan_configure(&bad) == AU_ERR_INVALID_ARGUMENT);
    // Workers start again on the next request
    const char *p = scan_file(23);
    CHECK(au_gain_cache_request(&p, 1, AU_SCAN_PREFETCH) == 1);
    CHECK(wait_idle(10000));
    CHECK(au_gain_cache_get(p, NULL));
}

/* ---------- the player ---------- */

static double gain_when_open(const int32_t handle) {
    double db = -999;
    for (int i = 0; i < 200; i++) {
        if (au_track_gain_db(handle, &db) == AU_OK) break;
        SDL_Delay(5);
    }
    return db;
}

static void test_player_measures_missing(void) {
    reset_done();
    configure(1);
    REQUIRE(tc_init_player(0) == 0);
    AuReplayGainConfig rg = { .mode = AU_REPLAYGAIN_TRACK, .fallback_gain_db = -6, .measure_missing = true };
    CHECK(au_set_replaygain(&rg) == AU_OK);

    // Untagged and not cached: measured before it starts (well within the default 500 ms)
    const uint32_t t0 = SDL_GetTicks();
    const char *fresh = scan_file(24);
    int32_t h = au_play(fresh, NULL);
    const double first = gain_when_open(h);
    printf("   measured before start: %+.2f dB after %u ms\n", first, SDL_GetTicks() - t0);
    CHECK(fabs(first - 5.0) < 0.1);
    CHECK(au_gain_cache_get(fresh, NULL));

    // No waiting: starts at the fallback gain, the measured value follows
    au_gain_cache_remove(fresh);
    rg.measure_wait_ms = -1;
    CHECK(au_set_replaygain(&rg) == AU_OK);
    tc_sleep_ms(50);
    h = au_play(fresh, NULL);
    const double start = gain_when_open(h);
    printf("   without waiting: starts at %+.2f dB\n", start);
    CHECK(fabs(start + 6.0) < 0.01 || fabs(start - 5.0) < 0.1); // the scan may still win the race
    double later = -999;
    for (int i = 0; i < 100 && fabs(later - 5.0) > 0.1; i++) {
        tc_sleep_ms(20);
        au_track_gain_db(h, &later);
    }
    CHECK(fabs(later - 5.0) < 0.1);

    // Tagged files are never measured
    reset_done();
    h = au_play(fx("rg_tagged.flac"), NULL);
    CHECK(fabs(gain_when_open(h) + 6.0) < 0.01);
    tc_sleep_ms(200);
    CHECK(done_count() == 0);
    CHECK(!au_gain_cache_get(fx("rg_tagged.flac"), NULL));

    // measure_missing off: nothing is requested
    rg.measure_missing = false;
    CHECK(au_set_replaygain(&rg) == AU_OK);
    tc_sleep_ms(50);
    h = au_play(fx("rg_untagged.flac"), NULL);
    CHECK(fabs(gain_when_open(h) + 6.0) < 0.01);
    tc_sleep_ms(200);
    CHECK(!au_gain_cache_get(fx("rg_untagged.flac"), NULL));

    const AuReplayGainConfig too_long = { .mode = AU_REPLAYGAIN_TRACK, .measure_wait_ms = 20000 };
    CHECK(au_set_replaygain(&too_long) == AU_ERR_INVALID_ARGUMENT);

    au_stop();
    tc_sleep_ms(200);
    au_shutdown();
}

int main(int argc, char **argv) {
    tc_setup(argc, argv);
    done_lock = SDL_CreateMutex();
    char cache[1100];
    snprintf(cache, sizeof(cache), "%s/test_gain_scan.cache", tc_fixture_dir);
    remove(cache);
    REQUIRE(au_gain_cache_set_file(cache) == AU_OK);

    RUN_TEST(test_request);
    RUN_TEST(test_dedup);
    RUN_TEST(test_urgent_goes_first);
    RUN_TEST(test_request_uses_every_worker);
    RUN_TEST(test_cancel);
    RUN_TEST(test_album);
    RUN_TEST(test_errors);
    RUN_TEST(test_changed_failure_is_measured_again);
    RUN_TEST(test_configure_and_stop);
    RUN_TEST(test_player_measures_missing);

    au_gain_scan_stop();
    reset_done();
    return tc_finish("test_gain_scan");
}
