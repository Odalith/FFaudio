/*
 * Basic behavioural tests: playback to EOF, formats, bad inputs, queueing, pause, seek, stop, loop.
 */
#include "test_common.h"
#include "../lib/ffaudio_meters.h"
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
    // Each ends as an error that says why: a file that can not be read, or one with nothing playable in it
    const char *bad[] = { "empty.flac", "text.mp3", "video_only.mkv", "does_not_exist.flac" };
    const int why[] = { AU_ERR_UNSUPPORTED, AU_ERR_UNSUPPORTED, AU_ERR_UNSUPPORTED, AU_ERR_FILE };
    const int n = (int)(sizeof(bad) / sizeof(bad[0]));
    for (int i = 0; i < n; i++) {
        au_play(fx(bad[i]), NULL);
        if (!TC_WAIT_FIELD(eof, i + 1, 3000)) {
            fprintf(stderr, "no EOF for bad input %s\n", bad[i]);
            CHECK(false);
        }
        const TcEvents e = tc_snapshot();
        printf("   %s: %d \"%s\"\n", bad[i], e.last_end_error, e.last_end_message);
        CHECK(e.last_end_error == why[i] && strstr(e.last_end_message, bad[i]));
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

// au_set_prepare_next(): the event comes that long before the end
// The position heard when AU_EVENT_PREPARE_NEXT came, recorded by a hook on the control thread (guarded by tc_mutex).
// The event counter is bumped before the hook runs, so a test waits for the hook's own value (wait_prepare_position()),
// not just for the counter: under chaos delays it read the value before the hook wrote it.
static double prepare_position = -1;
static bool prepare_recorded = false;
static void record_prepare_position(void) {
    AuStatus s;
    au_get_status(&s);
    SDL_LockMutex(tc_mutex);
    if (!prepare_recorded) {
        prepare_position = s.position;
        prepare_recorded = true;
    }
    SDL_UnlockMutex(tc_mutex);
}

static void watch_prepare_position(void) {
    SDL_LockMutex(tc_mutex);
    prepare_position = -1;
    prepare_recorded = false;
    tc_on_prepare_next_hook = record_prepare_position;
    SDL_UnlockMutex(tc_mutex);
}

// The recorded position, -1 when no prepare-next came within ms
static double wait_prepare_position(const int ms) {
    for (int t = 0; t <= ms; t += 5) {
        SDL_LockMutex(tc_mutex);
        const bool got = prepare_recorded;
        const double at = prepare_position;
        SDL_UnlockMutex(tc_mutex);
        if (got) return at;
        tc_sleep_ms(5);
    }
    return -1;
}

static void test_prepare_next_setter(void) {
    CHECK(au_set_prepare_next(0) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_set_prepare_next(-1) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_set_prepare_next(NAN) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_set_prepare_next(INFINITY) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_set_prepare_next(1.5) == AU_OK);
    watch_prepare_position();

    au_play(fx("long_6s.flac"), NULL);           // the default 10 s would send it at the start
    tc_sleep_ms(2500);
    CHECK(tc_snapshot().prepare_next == 0);
    CHECK(TC_WAIT_FIELD(prepare_next, 1, 6000));
    const double at = wait_prepare_position(1000);
    printf("   prepare-next at %.2f s of 6\n", at);
    CHECK(at > 4.2 && at < 4.8);
    CHECK(TC_WAIT_FIELD(eof, 1, 4000));

    SDL_LockMutex(tc_mutex);
    tc_on_prepare_next_hook = NULL;
    SDL_UnlockMutex(tc_mutex);
    CHECK(au_set_prepare_next(10) == AU_OK);    // the other tests expect the default
}

// au_set_output_format(): the device is opened again in the new format, and playback continues
static void test_output_format_setter(void) {
    CHECK(au_set_output_format(9, 0) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_set_output_format(-1, 0) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_set_output_format(2, 7999) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_set_output_format(2, 400000) == AU_ERR_INVALID_ARGUMENT);

    au_play(fx("long_6s.flac"), NULL);
    tc_sleep_ms(500);
    CHECK(au_set_output_format(6, 44100) == AU_OK);
    CHECK(TC_WAIT_FIELD(device, 1, 3000));
    AuStatus s;
    au_get_status(&s);
    printf("   after (6, 44100): %d channels, %d Hz, at %.2f s\n", s.channels, s.sample_rate, s.position);
    CHECK(s.channels == 6 && s.sample_rate == 44100);
    CHECK(au_set_output_format(6, 44100) == AU_OK); // the same: nothing to do
    tc_sleep_ms(400);
    CHECK(tc_snapshot().device == 1);
    CHECK(au_set_output_format(0, 0) == AU_OK);     // back to what the device prefers
    CHECK(TC_WAIT_FIELD(device, 2, 3000));
    au_get_status(&s);
    printf("   after (0, 0): %d channels, %d Hz\n", s.channels, s.sample_rate);
    CHECK(TC_WAIT_FIELD(eof, 1, 9000));
    const TcEvents e = tc_snapshot();
    CHECK(e.eof_error == 0 && e.device_failed == 0);
}

// A range plays like its own track: positions, seeks, events and prepare-next count from its start
static void test_range_time(void) {
    CHECK(au_set_prepare_next(1.0) == AU_OK);
    watch_prepare_position();
    const int32_t h = tc_play(fx("long_6s.flac"), tc_options(2.0, 3.0, NULL));
    CHECK(TC_WAIT_FIELD(opened, 1, 3000));
    TcEvents e = tc_snapshot();
    printf("   opened: duration %.3f\n", e.opened_duration);
    CHECK(fabs(e.opened_duration - 3.0) < 1e-6);
    tc_sleep_ms(300);
    AuStatus s;
    au_get_status(&s);
    printf("   0.3 s in: position %.3f, duration %.3f\n", s.position, s.duration);
    CHECK(s.handle == h && s.position >= 0 && s.position < 1.0 && fabs(s.duration - 3.0) < 1e-6);

    au_seek(1.0);
    CHECK(TC_WAIT_FIELD(restart, 1, 2000));
    tc_sleep_ms(100);
    au_get_status(&s);
    printf("   after seek(1): position %.3f\n", s.position);
    CHECK(s.position >= 0.95 && s.position < 1.6);
    au_seek_fraction(0.5);
    CHECK(TC_WAIT_FIELD(restart, 2, 2000));
    tc_sleep_ms(100);
    au_get_status(&s);
    printf("   after fraction 0.5: position %.3f\n", s.position);
    CHECK(s.position >= 1.45 && s.position < 2.1);

    CHECK(TC_WAIT_FIELD(prepare_next, 1, 3000));
    const double at = wait_prepare_position(1000);
    printf("   prepare-next at %.2f (1 s before the end of 3)\n", at);
    CHECK(at > 1.8 && at < 2.2);
    au_seek(5.0);                               // past the end: clamped, the track ends
    CHECK(TC_WAIT_FIELD(eof, 1, 3000));
    e = tc_snapshot();
    CHECK(e.eof_error == 0);

    SDL_LockMutex(tc_mutex);
    tc_on_prepare_next_hook = NULL;
    SDL_UnlockMutex(tc_mutex);
    CHECK(au_set_prepare_next(10) == AU_OK);

    // A start past the end of the file plays nothing and ends cleanly
    tc_play(fx("long_6s.flac"), tc_options(60.0, 0, NULL));
    CHECK(TC_WAIT_FIELD(eof, 2, 3000));
    CHECK(tc_snapshot().eof_error == 0);
}

static float linear_curve(float t, void *user) { (void)user; return t; }

// Each play-options setter checks its value at the call and leaves the object as it was when it refuses one; the
// object is copied by au_play(), so it can be changed and freed right after
static void test_play_options(void) {
    const AuFade fade = { .seconds = 1, .curve = AU_FADE_LINEAR };
    const AuReplayGain rg = AU_REPLAYGAIN_NONE;
    CHECK(au_play_options_set_range(NULL, 0, 0) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_play_options_set_filters(NULL, NULL) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_play_options_set_fade_in(NULL, &fade) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_play_options_set_replaygain(NULL, &rg) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_play_options_set_http_headers(NULL, "X: 1") == AU_ERR_INVALID_ARGUMENT);
    au_play_options_free(NULL);

    AuPlayOptions *o = au_play_options_create();
    REQUIRE(o);
    CHECK(au_play_options_set_range(o, 2.0, 3.0) == AU_OK);
    CHECK(au_play_options_set_range(o, -1, 1) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_play_options_set_range(o, 1, -1) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_play_options_set_range(o, NAN, 1) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_play_options_set_range(o, 1, INFINITY) == AU_ERR_INVALID_ARGUMENT);
    AuFade bad = fade;
    bad.seconds = NAN;
    CHECK(au_play_options_set_fade_in(o, &bad) == AU_ERR_INVALID_ARGUMENT);
    bad = fade;
    bad.curve = (AU_FADE_CURVE)99;
    CHECK(au_play_options_set_fade_in(o, &bad) == AU_ERR_INVALID_ARGUMENT);
    bad.curve = AU_FADE_CUSTOM;                 // without a function
    CHECK(au_play_options_set_fade_in(o, &bad) == AU_ERR_INVALID_ARGUMENT);
    bad.custom = linear_curve;
    CHECK(au_play_options_set_fade_in(o, &bad) == AU_OK);
    CHECK(au_play_options_set_fade_in(o, NULL) == AU_OK);       // back to the AuCrossfade setting
    CHECK(au_play_options_set_filters(o, "no_such_filter") == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_play_options_set_filters(o, "") == AU_OK);
    CHECK(au_play_options_set_http_headers(o, "X-A: 1") == AU_OK);
    CHECK(au_play_options_set_http_headers(o, NULL) == AU_OK);

    // The refused values left the range 2 + 3: the track is 3 s long
    au_play(fx("long_6s.flac"), o);
    CHECK(au_play_options_set_range(o, 0, 1) == AU_OK);   // the queued track has its own copy
    au_play_options_free(o);
    CHECK(TC_WAIT_FIELD(opened, 1, 3000));
    CHECK(fabs(tc_snapshot().opened_duration - 3.0) < 1e-6);
    au_stop();
    CHECK(TC_WAIT_FIELD(eof, 1, 3000));
}

// Readers of the meters while the output format changes under them: the tap is remade for every format
static SDL_atomic_t meter_stop;
static int meter_reader(void *arg) {
    int *reads = arg;
    AuSpectrum *sp = au_spectrum_create(1024);
    static float mags[513];
    while (!SDL_AtomicGet(&meter_stop)) {
        AuLevels lv;
        if (au_get_levels(0.02, &lv) == AU_OK) (*reads)++;
        if (lv.channels > 8 || lv.peak[0] > 2.0f) (*reads) = -1000000;   // garbage
        int rate;
        if (sp && au_spectrum_read(sp, -1, mags, &rate) == AU_OK && mags[20] > 2.0f) (*reads) = -1000000;
        SDL_Delay(1);
    }
    au_spectrum_free(sp);
    return 0;
}

static void test_meters_during_format_changes(void) {
    int reads = 0;
    SDL_AtomicSet(&meter_stop, 0);
    SDL_Thread *th = SDL_CreateThread(meter_reader, "meter_reader", &reads);
    REQUIRE(th);
    au_play(fx("long_6s.flac"), NULL);
    CHECK(TC_WAIT_FIELD(started, 1, 3000));
    for (int i = 0; i < 20; i++) {
        CHECK(au_set_output_format(i % 2 ? 6 : 2, i % 4 < 2 ? 48000 : 44100) == AU_OK);
        CHECK(TC_WAIT_FIELD(device, i + 1, 3000));
        tc_sleep_ms(30);
    }
    SDL_AtomicSet(&meter_stop, 1);
    SDL_WaitThread(th, NULL);
    printf("   %d reads while the format changed 20 times\n", reads);
    CHECK(reads > 50);
    au_stop();
    CHECK(TC_WAIT_FIELD(eof, 1, 3000));
    CHECK(au_set_output_format(0, 0) == AU_OK);
    CHECK(TC_WAIT_FIELD(device, 21, 3000));
}

// A track that can not be played (cover art in a music folder) and was started with au_play(), nothing queued: the
// application must hear AU_EVENT_PREPARE_NEXT to start the next one. Found on Windows: the end of a failed track
// could reach the control thread before its failure, and then no prepare-next came and playback stopped.
static void test_failed_track_asks_for_next(void) {
    int missed = 0;
    for (int i = 0; i < 40; i++) {
        tc_reset_events();
        au_play(fx("not_audio.bin"), NULL);
        CHECK(TC_WAIT_FIELD(eof, 1, 3000));
        if (!TC_WAIT_FIELD(prepare_next, 1, 1000)) missed++;
    }
    printf("   prepare-next missing after %d of 40 failed tracks\n", missed);
    CHECK(missed == 0);
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
    RUN_TEST(test_failed_track_asks_for_next);
    RUN_TEST(test_prepare_next_setter);
    RUN_TEST(test_output_format_setter);
    RUN_TEST(test_range_time);
    RUN_TEST(test_play_options);
    RUN_TEST(test_meters_during_format_changes);
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
