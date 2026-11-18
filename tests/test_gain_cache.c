/*
 * ReplayGain cache tests: the store itself, thread safety, get-or-scan, and how the player uses it.
 */
#include "test_common.h"
#include "../lib/ffaudio_loudness.h"
#include <math.h>

static AuReplayGainInfo track_gain(const double db) {
    AuReplayGainInfo rg;
    memset(&rg, 0, sizeof(rg));
    rg.has_track_gain = true;
    rg.track_gain_db = db;
    return rg;
}

/* ---------- the store ---------- */

static void test_basics(void) {
    au_gain_cache_clear();
    CHECK(au_gain_cache_count() == 0);
    CHECK(!au_gain_cache_get("/music/a.flac", NULL));

    AuReplayGainInfo rg = track_gain(-6.5), out;
    CHECK(au_gain_cache_set("/music/a.flac", &rg) == AU_OK);
    CHECK(au_gain_cache_count() == 1);
    CHECK(au_gain_cache_get("/music/a.flac", &out) && out.has_track_gain && out.track_gain_db == -6.5);
    CHECK(au_gain_cache_get("/music/a.flac", NULL));
    CHECK(!au_gain_cache_get("/music/A.flac", NULL));      // exact, case-sensitive match
    CHECK(!au_gain_cache_get("/music/a.flac ", NULL));

    rg = track_gain(-2);
    CHECK(au_gain_cache_set("/music/a.flac", &rg) == AU_OK); // replace
    CHECK(au_gain_cache_count() == 1);
    CHECK(au_gain_cache_get("/music/a.flac", &out) && out.track_gain_db == -2);

    CHECK(au_gain_cache_remove("/music/a.flac"));
    CHECK(!au_gain_cache_remove("/music/a.flac"));
    CHECK(au_gain_cache_count() == 0);

    CHECK(au_gain_cache_set(NULL, &rg) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_gain_cache_set("", &rg) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_gain_cache_set("/x", NULL) == AU_ERR_INVALID_ARGUMENT);
    CHECK(!au_gain_cache_get(NULL, &out));
    CHECK(!au_gain_cache_remove(NULL));
    CHECK(au_gain_cache_foreach(NULL, NULL) == 0);
}

static int visits = 0;
static bool visit_remove_all(const char *path, const AuReplayGainInfo *rg, void *user) {
    (void)rg; (void)user;
    visits++;
    au_gain_cache_remove(path); // using the cache from the visitor must not deadlock
    return true;
}
static bool visit_stop_after_3(const char *path, const AuReplayGainInfo *rg, void *user) {
    (void)path; (void)rg;
    return ++*(int *)user < 3;
}

static void test_foreach(void) {
    au_gain_cache_clear();
    char key[64];
    for (int i = 0; i < 10; i++) {
        snprintf(key, sizeof(key), "/music/%02d.flac", i);
        const AuReplayGainInfo rg = track_gain(-i);
        au_gain_cache_set(key, &rg);
    }
    int n = 0;
    CHECK(au_gain_cache_foreach(visit_stop_after_3, &n) == 3 && n == 3);
    visits = 0;
    CHECK(au_gain_cache_foreach(visit_remove_all, NULL) == 10);
    CHECK(visits == 10);
    CHECK(au_gain_cache_count() == 0);
}

static void test_many_entries(void) {
    au_gain_cache_clear();
    char key[64];
    const int n = 20000;
    for (int i = 0; i < n; i++) {
        snprintf(key, sizeof(key), "/library/artist %d/track %d.flac", i % 97, i);
        const AuReplayGainInfo rg = track_gain(i * 0.001);
        if (au_gain_cache_set(key, &rg) != AU_OK) { CHECK(false); break; }
    }
    CHECK(au_gain_cache_count() == n);
    int bad = 0;
    for (int i = 0; i < n; i += 7) {
        snprintf(key, sizeof(key), "/library/artist %d/track %d.flac", i % 97, i);
        AuReplayGainInfo out;
        if (!au_gain_cache_get(key, &out) || fabs(out.track_gain_db - i * 0.001) > 1e-12) bad++;
    }
    CHECK(bad == 0);
    au_gain_cache_clear();
    CHECK(au_gain_cache_count() == 0);
}

static int hammer(void *arg) {
    const int id = (int)(intptr_t)arg;
    char key[64];
    for (int i = 0; i < 3000; i++) {
        snprintf(key, sizeof(key), "/shared/%d.flac", i % 50);
        const AuReplayGainInfo rg = track_gain(id);
        switch ((i + id) % 4) {
            case 0: case 1: au_gain_cache_set(key, &rg); break;
            case 2: au_gain_cache_get(key, NULL); break;
            case 3: au_gain_cache_remove(key); break;
        }
        if (i % 500 == 0) au_gain_cache_count();
    }
    return 0;
}

static bool count_visitor(const char *path, const AuReplayGainInfo *rg, void *user) {
    (void)path; (void)rg;
    (*(int *)user)++;
    return true;
}

static void test_threads(void) {
    au_gain_cache_clear();
    SDL_Thread *t[4];
    for (int i = 0; i < 4; i++) t[i] = SDL_CreateThread(hammer, "cache_hammer", (void *)(intptr_t)i);
    for (int i = 0; i < 20; i++) {
        int n = 0;
        au_gain_cache_foreach(count_visitor, &n);
        CHECK(n <= 50);
    }
    for (int i = 0; i < 4; i++) SDL_WaitThread(t[i], NULL);
    CHECK(au_gain_cache_count() <= 50);
    au_gain_cache_clear();
}

/* ---------- get-or-scan ---------- */

static void test_scan(void) {
    au_gain_cache_clear();
    AuReplayGainInfo rg;
    CHECK(au_gain_cache_scan(fx("ld_m23_48k.flac"), NULL, &rg) == AU_OK);
    CHECK(rg.has_track_gain && fabs(rg.track_gain_db - 5.0) < 0.1); // -23 LUFS -> +5 dB re -18 LUFS
    CHECK(rg.has_track_peak && fabs(rg.track_peak - 0.0708) < 0.002);
    CHECK(!rg.has_album_gain);
    CHECK(au_gain_cache_count() == 1);
    CHECK(au_gain_cache_get(fx("ld_m23_48k.flac"), NULL));

    // Cached: returned as stored, without measuring again
    const AuReplayGainInfo mine = track_gain(-1.25);
    au_gain_cache_set(fx("ld_m23_48k.flac"), &mine);
    CHECK(au_gain_cache_scan(fx("ld_m23_48k.flac"), NULL, &rg) == AU_OK && rg.track_gain_db == -1.25);

    CHECK(au_gain_cache_scan(fx("text.mp3"), NULL, &rg) == AU_ERR_FILE);
    CHECK(!au_gain_cache_get(fx("text.mp3"), NULL));
    CHECK(au_gain_cache_scan(NULL, NULL, NULL) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_gain_cache_count() == 1);
    au_gain_cache_clear();
}

/* ---------- the player ---------- */

static double applied_gain(const int32_t handle) {
    double db = -999;
    for (int i = 0; i < 100; i++) { // the file has to be open before the gain is known
        if (au_get_track_gain_db(handle, &db) == AU_OK) break;
        tc_sleep_ms(10);
    }
    return db;
}

static void start_player(void) {
    tc_reset_events();
    REQUIRE(tc_init_player(0) == 0);
    const AuReplayGainConfig cfg = { .mode = AU_REPLAYGAIN_TRACK };
    REQUIRE(au_set_replaygain(&cfg) == AU_OK);
}

static void test_player_priority(void) {
    au_gain_cache_clear();
    start_player();

    // Untagged file: the cache supplies the gain
    AuReplayGainInfo rg = track_gain(-10);
    au_gain_cache_set(fx("rg_untagged.flac"), &rg);
    int32_t h = au_play_audio(fx("rg_untagged.flac"), NULL);
    CHECK(fabs(applied_gain(h) + 10) < 0.01);

    // Tagged file (-6 dB): the cache wins over the tags
    rg = track_gain(-7);
    au_gain_cache_set(fx("rg_tagged.flac"), &rg);
    h = au_play_audio(fx("rg_tagged.flac"), NULL);
    CHECK(fabs(applied_gain(h) + 7) < 0.01);

    // PlayAudioConfig.replaygain wins over the cache
    const AuReplayGainInfo explicit_rg = track_gain(-3);
    const PlayAudioConfig cfg = { .replaygain = &explicit_rg };
    h = au_play_audio(fx("rg_tagged.flac"), &cfg);
    CHECK(fabs(applied_gain(h) + 3) < 0.01);

    au_stop_audio();
    tc_sleep_ms(200);
    au_shutdown();
}

static void test_player_live_updates(void) {
    au_gain_cache_clear();
    start_player();

    const int32_t h = au_play_audio(fx("rg_long.flac"), NULL);
    CHECK(fabs(applied_gain(h) + 6) < 0.01);       // from the tags

    AuReplayGainInfo rg = track_gain(-9);
    au_gain_cache_set(fx("rg_long.flac"), &rg);      // arrives while the track plays
    tc_sleep_ms(100);
    CHECK(fabs(applied_gain(h) + 9) < 0.01);

    // A queued track picks up a value that arrives after au_set_next()
    const int32_t next = au_set_next(fx("rg_untagged.flac"), NULL);
    tc_sleep_ms(100);
    CHECK(fabs(applied_gain(next)) < 0.01);         // no tags, no cache yet
    rg = track_gain(-4.5);
    au_gain_cache_set(fx("rg_untagged.flac"), &rg);
    tc_sleep_ms(100);
    CHECK(fabs(applied_gain(next) + 4.5) < 0.01);

    CHECK(au_gain_cache_remove(fx("rg_long.flac")));
    tc_sleep_ms(100);
    CHECK(fabs(applied_gain(h) + 6) < 0.01);       // back to the tags
    au_gain_cache_clear();
    tc_sleep_ms(100);
    CHECK(fabs(applied_gain(next)) < 0.01);

    au_stop_audio();
    tc_sleep_ms(200);
    au_shutdown();
}

static void test_survives_shutdown(void) {
    au_gain_cache_clear();
    const AuReplayGainInfo rg = track_gain(-8);
    au_gain_cache_set(fx("rg_untagged.flac"), &rg);  // before any player exists
    for (int i = 0; i < 2; i++) {
        start_player();
        const int32_t h = au_play_audio(fx("rg_untagged.flac"), NULL);
        CHECK(fabs(applied_gain(h) + 8) < 0.01);
        au_shutdown();
    }
    CHECK(au_gain_cache_count() == 1);
    au_gain_cache_clear();
}

static SDL_atomic_t keep_setting;
static int set_loop(void *arg) {
    (void)arg;
    int i = 0;
    while (SDL_AtomicGet(&keep_setting)) {
        const AuReplayGainInfo rg = track_gain(-(i++ % 10));
        au_gain_cache_set(fx("rg_untagged.flac"), &rg);
        if (i % 3 == 0) au_gain_cache_remove(fx("rg_untagged.flac"));
    }
    return 0;
}

static void test_updates_during_shutdown(void) {
    // Cache updates from another thread while the player starts and stops must be safe
    au_gain_cache_clear();
    SDL_AtomicSet(&keep_setting, 1);
    SDL_Thread *t = SDL_CreateThread(set_loop, "cache_setter", NULL);
    for (int i = 0; i < 5; i++) {
        start_player();
        au_play_audio(fx("rg_untagged.flac"), NULL);
        tc_sleep_ms(50);
        au_shutdown();
    }
    SDL_AtomicSet(&keep_setting, 0);
    SDL_WaitThread(t, NULL);
    au_gain_cache_clear();
}

int main(int argc, char **argv) {
    tc_setup(argc, argv);
    RUN_TEST(test_basics);
    RUN_TEST(test_foreach);
    RUN_TEST(test_many_entries);
    RUN_TEST(test_threads);
    RUN_TEST(test_scan);
    RUN_TEST(test_player_priority);
    RUN_TEST(test_player_live_updates);
    RUN_TEST(test_survives_shutdown);
    RUN_TEST(test_updates_during_shutdown);
    return tc_finish("test_gain_cache");
}
