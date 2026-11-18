/*
 * Basic behavioural tests: playback to EOF, formats, bad inputs, queueing, pause, seek, stop, loop.
 */
#include "test_common.h"
#include <math.h>

static void test_play_to_eof(void) {
    au_play_audio(fx("s44_stereo.flac"), NULL);
    CHECK(TC_WAIT_FIELD(eof, 1, 5000));
    const TcEvents e = tc_snapshot();
    CHECK(e.eof == 1);
    CHECK(e.eof_error == 0);
}

static void test_play_time_advances(void) {
    au_play_audio(fx("long_6s.flac"), NULL);
    tc_sleep_ms(800);
    const double t = au_get_audio_play_time();
    CHECK(t > 0.2 && t < 2.0);
    const double d = au_get_audio_duration();
    CHECK(fabs(d - 6.0) < 0.2);
    au_stop_audio();
    CHECK(TC_WAIT_FIELD(eof, 1, 3000));
}

static void test_all_formats(void) {
    const char *files[] = {
        "s44_stereo.flac", "s48_stereo.mp3", "s96_stereo.flac", "s44_mono.wav",
        "s48_51.flac", "s44_flt.wav", "s48_stereo.ogg", "s44_stereo.m4a",
    };
    const int n = (int)(sizeof(files) / sizeof(files[0]));
    for (int i = 0; i < n; i++) {
        au_play_audio(fx(files[i]), NULL);
        if (!TC_WAIT_FIELD(eof, i + 1, 5000)) {
            fprintf(stderr, "no EOF for %s\n", files[i]);
            CHECK(false);
        }
    }
    CHECK(tc_snapshot().eof_error == 0);
}

static void test_bad_inputs(void) {
    const char *bad[] = { "empty.flac", "text.mp3", "video_only.mkv", "does_not_exist.flac" };
    const int n = (int)(sizeof(bad) / sizeof(bad[0]));
    for (int i = 0; i < n; i++) {
        au_play_audio(fx(bad[i]), NULL);
        if (!TC_WAIT_FIELD(eof, i + 1, 3000)) {
            fprintf(stderr, "no EOF for bad input %s\n", bad[i]);
            CHECK(false);
        }
    }
    const TcEvents e = tc_snapshot();
    CHECK(e.eof_error == n);

    /* A truncated file must still end (error or not) */
    au_play_audio(fx("truncated.flac"), NULL);
    CHECK(TC_WAIT_FIELD(eof, n + 1, 3000));

    /* And the player must still work afterwards */
    au_play_audio(fx("s44_stereo.flac"), NULL);
    CHECK(TC_WAIT_FIELD(eof, n + 2, 5000));
}

static void test_queue_next(void) {
    au_play_audio(fx("s44_stereo.flac"), NULL);
    tc_sleep_ms(200);
    au_set_next(fx("s48_stereo.mp3"), NULL);
    CHECK(TC_WAIT_FIELD(eof, 2, 6000));
    const TcEvents e = tc_snapshot();
    CHECK(e.eof == 2);
    CHECK(e.eof_error == 0);
    CHECK(e.prepare_next >= 1);
}

static void test_skip_to_next(void) {
    au_play_audio(fx("long_6s.flac"), NULL);
    tc_sleep_ms(300);
    au_set_next(fx("s44_stereo.flac"), NULL);
    tc_sleep_ms(200);
    au_skip_to_next();
    /* long_6s ends by skip, then s44_stereo plays to its end */
    CHECK(TC_WAIT_FIELD(eof, 2, 4000));
    CHECK(tc_snapshot().eof_skip >= 1);
}

static void test_pause_resume(void) {
    au_play_audio(fx("long_6s.flac"), NULL);
    tc_sleep_ms(600);
    au_pause_audio(true);
    tc_sleep_ms(100);
    const double t1 = au_get_audio_play_time();
    tc_sleep_ms(500);
    const double t2 = au_get_audio_play_time();
    CHECK(fabs(t2 - t1) < 0.05);
    au_pause_audio(false);
    tc_sleep_ms(500);
    const double t3 = au_get_audio_play_time();
    CHECK(t3 > t2 + 0.2);
    au_stop_audio();
    CHECK(TC_WAIT_FIELD(eof, 1, 3000));
}

static void test_seek(void) {
    au_play_audio(fx("long_6s.flac"), NULL);
    tc_sleep_ms(400);
    au_seek_time(4000);
    tc_sleep_ms(400);
    const double t = au_get_audio_play_time();
    CHECK(t >= 3.9 && t < 5.5);
    CHECK(TC_WAIT_FIELD(restart, 1, 1000));
    /* Remaining ~1.6 s, then EOF */
    CHECK(TC_WAIT_FIELD(eof, 1, 4000));
}

static void test_stop(void) {
    au_play_audio(fx("long_6s.flac"), NULL);
    tc_sleep_ms(300);
    au_stop_audio();
    CHECK(TC_WAIT_FIELD(eof, 1, 2000));
    tc_sleep_ms(100);
    CHECK(au_get_audio_play_time() < 0);
    /* Stop when nothing is playing must be harmless */
    au_stop_audio();
}

static void test_loop(void) {
    au_set_loop_count(1);
    au_play_audio(fx("s44_stereo.flac"), NULL);
    CHECK(TC_WAIT_FIELD(restart, 1, 4000));
    CHECK(TC_WAIT_FIELD(eof, 1, 5000));
    au_set_loop_count(0);
}

int main(int argc, char **argv) {
    tc_setup(argc, argv);
    REQUIRE(tc_init_player(0) == 0);

    RUN_TEST(test_play_to_eof);
    RUN_TEST(test_play_time_advances);
    RUN_TEST(test_all_formats);
    RUN_TEST(test_bad_inputs);
    RUN_TEST(test_queue_next);
    RUN_TEST(test_skip_to_next);
    RUN_TEST(test_pause_resume);
    RUN_TEST(test_seek);
    RUN_TEST(test_stop);
    RUN_TEST(test_loop);

    au_shutdown();
    return tc_finish("test_basic");
}
