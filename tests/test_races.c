/*
 * Regression tests for races between ffaudio's threads (docs/threading.md, "History of races found"). Built only with
 * -DFFAUDIO_CHAOS=ON: each case runs as its own CTest with FFAUDIO_CHAOS_AT forcing a delay at the chaos point that
 * opens the race's window, so the order that once went wrong happens every time.
 *
 *   test_races <fixture-dir> <case>
 */
#include "test_common.h"

// The worker must not publish a track's details before the track is registered for au_track_info(): a delay
// between starting the worker and registering the track made AU_EVENT_TRACK_OPENED come with nothing to read
// (FFAUDIO_CHAOS_AT=create_track: worker started=…)
static void race_registration(void) {
    for (int i = 0; i < 3; i++) {
        tc_reset_events();
        const int32_t h = au_play(fx("s44_stereo.flac"), NULL);
        REQUIRE(h > 0);
        CHECK(TC_WAIT_FIELD(opened, 1, 3000));
        const TcEvents e = tc_snapshot();
        CHECK(e.opened_with_info == e.opened);
        double db = 0;
        const int gain = au_track_gain_db(h, &db);
        CHECK(gain == AU_OK || tc_snapshot().eof > 0); // a short file may have ended meanwhile
        au_stop();
        CHECK(TC_WAIT_FIELD(eof, 1, 3000));
    }
}

// A broken file as the only track must give AU_EVENT_PREPARE_NEXT whichever of its two ends reaches the control
// thread first: the mixer's (FFAUDIO_CHAOS_AT=eos stored, failure not posted=…, the failure message is late) or the
// failure message (FFAUDIO_CHAOS_AT=sdl_audio_callback=…, the callback notices the end late)
static void race_failed_track(void) {
    for (int i = 0; i < 3; i++) {
        tc_reset_events();
        au_play(fx("not_audio.bin"), NULL);
        CHECK(TC_WAIT_FIELD(eof, 1, 5000));
        CHECK(TC_WAIT_FIELD(prepare_next, 1, 2000));
        CHECK(tc_snapshot().eof_error == 1);
    }
}

// au_pause(true) right after an au_queue() that starts a track (and unpauses) must stick, also when the control
// thread handles the queue late (FFAUDIO_CHAOS_AT=cmd_set_next=…): its own unpause must not undo the newer pause
static void race_pause_after_queue(void) {
    au_pause(true);
    tc_sleep_ms(100);
    const int32_t h = au_queue(fx("long_6s.flac"), NULL);
    REQUIRE(h > 0);
    au_pause(true);
    tc_sleep_ms(500);
    CHECK(tc_status().paused);
    const double p1 = tc_status().position;
    tc_sleep_ms(400);
    CHECK(tc_status().position == p1); // really paused, not only reported so
    au_pause(false);
    au_stop();
    CHECK(TC_WAIT_FIELD(eof, 1, 3000));
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <fixture-dir> registration|failed_track|pause_after_queue\n", argv[0]);
        return 2;
    }
    tc_setup(argc, argv);
    REQUIRE(tc_init_player(0) == 0);
    const char *which = argv[2];
    if (!strcmp(which, "registration")) RUN_TEST(race_registration);
    else if (!strcmp(which, "failed_track")) RUN_TEST(race_failed_track);
    else if (!strcmp(which, "pause_after_queue")) RUN_TEST(race_pause_after_queue);
    else { fprintf(stderr, "unknown case '%s'\n", which); return 2; }
    au_shutdown();
    return tc_finish(which);
}
