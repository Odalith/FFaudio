/*
 * Basic behavioural tests: playback to EOF, formats, bad inputs, queueing, pause, seek, stop, loop.
 */
#include "test_common.h"
#include <math.h>

static void test_play_to_eof(void) {
    au_play(fx("s44_stereo.flac"), NULL);
    CHECK(TC_WAIT_FIELD(eof, 1, 5000));
    const TcEvents e = tc_snapshot();
    CHECK(e.eof == 1);
    CHECK(e.eof_error == 0);
}

static void test_play_time_advances(void) {
    au_play(fx("long_6s.flac"), NULL);
    tc_sleep_ms(800);
    const double t = tc_status().position;
    CHECK(t > 0.2 && t < 2.0);
    const double d = tc_status().duration;
    CHECK(fabs(d - 6.0) < 0.2);
    au_stop();
    CHECK(TC_WAIT_FIELD(eof, 1, 3000));
}

static void test_all_formats(void) {
    const char *files[] = {
        "s44_stereo.flac", "s48_stereo.mp3", "s96_stereo.flac", "s44_mono.wav",
        "s48_51.flac", "s44_flt.wav", "s48_stereo.ogg", "s44_stereo.m4a",
    };
    const int n = (int)(sizeof(files) / sizeof(files[0]));
    for (int i = 0; i < n; i++) {
        au_play(fx(files[i]), NULL);
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
        au_play(fx(bad[i]), NULL);
        if (!TC_WAIT_FIELD(eof, i + 1, 3000)) {
            fprintf(stderr, "no EOF for bad input %s\n", bad[i]);
            CHECK(false);
        }
    }
    const TcEvents e = tc_snapshot();
    CHECK(e.eof_error == n);

    /* A truncated file must still end (error or not) */
    au_play(fx("truncated.flac"), NULL);
    CHECK(TC_WAIT_FIELD(eof, n + 1, 3000));

    /* And the player must still work afterwards */
    au_play(fx("s44_stereo.flac"), NULL);
    CHECK(TC_WAIT_FIELD(eof, n + 2, 5000));
}

static void test_queue_next(void) {
    au_play(fx("s44_stereo.flac"), NULL);
    tc_sleep_ms(200);
    au_queue(fx("s48_stereo.mp3"), NULL);
    CHECK(TC_WAIT_FIELD(eof, 2, 6000));
    const TcEvents e = tc_snapshot();
    CHECK(e.eof == 2);
    CHECK(e.eof_error == 0);
    CHECK(e.prepare_next >= 1);
}

static void test_skip_to_next(void) {
    au_play(fx("long_6s.flac"), NULL);
    tc_sleep_ms(300);
    au_queue(fx("s44_stereo.flac"), NULL);
    tc_sleep_ms(200);
    au_skip();
    /* long_6s ends by skip, then s44_stereo plays to its end */
    CHECK(TC_WAIT_FIELD(eof, 2, 4000));
    CHECK(tc_snapshot().eof_skip >= 1);
}

static void test_pause_resume(void) {
    au_play(fx("long_6s.flac"), NULL);
    tc_sleep_ms(600);
    au_pause(true);
    tc_sleep_ms(100);
    const double t1 = tc_status().position;
    tc_sleep_ms(500);
    const double t2 = tc_status().position;
    CHECK(fabs(t2 - t1) < 0.05);
    au_pause(false);
    tc_sleep_ms(500);
    const double t3 = tc_status().position;
    CHECK(t3 > t2 + 0.2);
    au_stop();
    CHECK(TC_WAIT_FIELD(eof, 1, 3000));
}

static void test_seek(void) {
    au_play(fx("long_6s.flac"), NULL);
    tc_sleep_ms(400);
    au_seek(4.0);
    tc_sleep_ms(400);
    const double t = tc_status().position;
    CHECK(t >= 3.9 && t < 5.5);
    CHECK(TC_WAIT_FIELD(restart, 1, 1000));
    /* Remaining ~1.6 s, then EOF */
    CHECK(TC_WAIT_FIELD(eof, 1, 4000));
}

// A container whose timestamps start late (MPEG-TS: 1.4 s) reports positions from 0, like seeks and the duration;
// they used to include the start time, so end crossfades and on_prepare_next came that much early
static void test_position_of_late_start(void) {
    au_play(fx("start_offset.ts"), NULL);
    tc_sleep_ms(400);
    const double t = tc_status().position;
    printf("   0.4 s in: play time %.3f\n", t);
    CHECK(t >= 0 && t < 1.0);
    au_seek(4.0);
    tc_sleep_ms(400);
    const double t2 = tc_status().position;
    printf("   after a seek to 4 s: play time %.3f\n", t2);
    CHECK(t2 >= 3.8 && t2 < 4.9);
    CHECK(TC_WAIT_FIELD(eof, 1, 4000));
}

static void test_stop(void) {
    au_play(fx("long_6s.flac"), NULL);
    tc_sleep_ms(300);
    au_stop();
    CHECK(TC_WAIT_FIELD(eof, 1, 2000));
    tc_sleep_ms(100);
    CHECK(tc_status().position < 0);
    /* Stop when nothing is playing must be harmless */
    au_stop();
}

static void test_loop(void) {
    au_set_loop_count(1);
    au_play(fx("s44_stereo.flac"), NULL);
    CHECK(TC_WAIT_FIELD(restart, 1, 4000));
    CHECK(TC_WAIT_FIELD(eof, 1, 5000));
    CHECK(tc_snapshot().restart_loop == 1);
    au_set_loop_count(0);
}

static void test_speed_changes_are_not_loops(void) {
    // Ramping the speed up and down used to look like a loop (the position estimate drifted and jumped back
    // at 1x). Only real loops may be reported.
    au_play(fx("long_6s.flac"), NULL);
    tc_sleep_ms(300);
    double last = 0, max_back = 0;
    const double steps[] = { 1.25, 1.5, 1.75, 2, 2.25, 2.5, 2.75, 3, 2.75, 2.5, 2.25, 2, 1.75, 1.5, 1.25, 1,
                             0.75, 0.5, 0.25, 0.5, 0.25, 0.5, 0.75, 1, 1.25, 1.5, 1.25, 1 };
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        au_set_speed(steps[i], true);
        tc_sleep_ms(60);
        const double pos = tc_status().position;
        if (pos >= 0 && last - pos > max_back) max_back = last - pos;
        if (pos >= 0) last = pos;
    }
    printf("   largest backward step of the position: %.3f s\n", max_back);
    CHECK(tc_snapshot().restart_loop == 0);
    CHECK(tc_snapshot().restart == 0);
    CHECK(max_back < 0.25);
    au_set_speed(1.0, true);
    au_stop();
    CHECK(TC_WAIT_FIELD(eof, 1, 3000));

    // A real loop at 2x is still reported once; a backward seek is a restart, not a loop
    tc_reset_events();
    au_set_speed(2.0, true);
    au_set_loop_count(1);
    au_play(fx("s44_stereo.flac"), NULL);
    CHECK(TC_WAIT_FIELD(eof, 1, 5000));
    CHECK(tc_snapshot().restart_loop == 1);
    au_set_loop_count(0);
    au_set_speed(1.0, true);

    tc_reset_events();
    au_play(fx("long_6s.flac"), NULL);
    tc_sleep_ms(1500);
    au_seek(0.2);
    CHECK(TC_WAIT_FIELD(restart, 1, 2000));
    tc_sleep_ms(300);
    CHECK(tc_snapshot().restart_loop == 0);
    au_stop();
    CHECK(TC_WAIT_FIELD(eof, 1, 3000));
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
    RUN_TEST(test_position_of_late_start);
    RUN_TEST(test_stop);
    RUN_TEST(test_loop);
    RUN_TEST(test_speed_changes_are_not_loops);

    au_shutdown();
    return tc_finish("test_basic");
}
