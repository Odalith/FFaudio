/*
 * Lifecycle tests: calls before init, shutdown while playing, init again after shutdown.
 */
#include "test_common.h"

static void test_calls_before_init(void) {
    /* None of these may crash when the player does not exist */
    au_play_audio(fx("s44_stereo.flac"), NULL);
    au_set_next(fx("s44_stereo.flac"), NULL);
    au_skip_to_next();
    au_clear_next();
    au_stop_audio();
    au_pause_audio(true);
    au_seek_time(10);
    au_seek_percent(10);
    au_set_audio_volume(10);
    au_mute_audio(true);
    CHECK(au_get_audio_play_time() < 0);
    CHECK(au_get_audio_duration() < 0);
    CHECK(au_configure_audio_device(NULL) != 0);
    au_shutdown();
}

static void test_init_null_config(void) {
    CHECK(au_initialize(NULL) != 0);
}

static void test_shutdown_while_playing(void) {
    REQUIRE(tc_init_player(0) == 0);
    au_play_audio(fx("long_6s.flac"), NULL);
    au_set_next(fx("s44_stereo.flac"), NULL);
    tc_sleep_ms(300);
    au_shutdown();
}

static void test_reinit(void) {
    for (int i = 0; i < 3; i++) {
        tc_reset_events();
        const int ret = tc_init_player(0);
        CHECK(ret == 0);
        if (ret != 0) return;
        au_play_audio(fx("s44_stereo.flac"), NULL);
        CHECK(TC_WAIT_FIELD(eof, 1, 5000));
        au_shutdown();
    }
}

static void test_device_name_and_no_spurious_changes(void) {
    char name[256];
    CHECK(au_get_audio_device_name(name, sizeof(name)) == AU_ERR_NOT_INITIALIZED);
    REQUIRE(tc_init_player(0) == 0);
    const int len = au_get_audio_device_name(name, sizeof(name));
    printf("   device in use: \"%s\"\n", name);
    CHECK(len > 0 && (int)strlen(name) == len);
    char tiny[2];
    CHECK(au_get_audio_device_name(tiny, sizeof(tiny)) == AU_ERR_INVALID_ARGUMENT || len < 2);
    CHECK(au_get_audio_device_name(NULL, 10) == AU_ERR_INVALID_ARGUMENT);

    // The device list is freed by ffaudio (the sanitizer builds check that nothing is left)
    int n = -1;
    char **devs = NULL;
    CHECK(au_get_audio_devices(&n, &devs) == AU_OK && n >= 0);
    for (int i = 0; i < n; i++) CHECK(devs[i] != NULL);
    au_free_audio_devices(devs, n);
    au_free_audio_devices(NULL, 0);
    au_free(NULL);

    // SDL announces every device at start-up and the list is polled every 2 s: neither is a change
    au_play_audio(fx("long_6s.flac"), NULL);
    tc_sleep_ms(4500);
    CHECK(tc_snapshot().devices_changed == 0);
    au_shutdown();
}

// A fade too short for a single frame is a cut. The replaced track must still end and be reported; it used to be left
// in no mixer slot with its threads running, and its on_eof never came.
static void test_fade_without_frames_cuts(void) {
    // No device yet: the mixer has no sample rate, so no fade can run
    REQUIRE(tc_initialize(0, 0) == 0);
    const CrossfadeConfig xf = { .on_play = { .seconds = 1.0, .curve = AU_FADE_LINEAR } };
    CHECK(au_set_crossfade(&xf) == AU_OK);
    const int32_t first = au_play_audio(fx("long_6s.flac"), NULL);
    au_play_audio(fx("s44_stereo.flac"), NULL);
    CHECK(TC_WAIT_FIELD(eof, 1, 2000));
    TcEvents e = tc_snapshot();
    CHECK(e.last_eof_handle == first && e.eof_skip == 1);
    REQUIRE(au_configure_audio_device(NULL) == AU_OK);
    CHECK(TC_WAIT_FIELD(eof, 2, 5000));   // the second track plays to its end
    au_shutdown();

    // Fades shorter than one sample, with a device
    tc_reset_events();
    REQUIRE(tc_init_player(0) == 0);
    const AuFade tiny = { .seconds = 1e-6, .curve = AU_FADE_LINEAR };
    const CrossfadeConfig xf2 = { .on_play = tiny, .on_skip = tiny, .on_stop = tiny };
    CHECK(au_set_crossfade(&xf2) == AU_OK);
    au_play_audio(fx("long_6s.flac"), NULL);
    tc_sleep_ms(200);
    au_play_audio(fx("long_6s.flac"), NULL);   // play over a playing track
    CHECK(TC_WAIT_FIELD(eof, 1, 2000));
    au_set_next(fx("long_6s.flac"), NULL);
    tc_sleep_ms(200);
    au_skip_to_next();
    CHECK(TC_WAIT_FIELD(eof, 2, 2000));
    tc_sleep_ms(200);
    au_stop_audio();
    CHECK(TC_WAIT_FIELD(eof, 3, 2000));
    e = tc_snapshot();
    CHECK(e.eof == 3 && e.eof_skip == 3 && e.eof_error == 0);
    au_shutdown();
}

int main(int argc, char **argv) {
    tc_setup(argc, argv);
    RUN_TEST(test_calls_before_init);
    RUN_TEST(test_init_null_config);
    RUN_TEST(test_shutdown_while_playing);
    RUN_TEST(test_reinit);
    RUN_TEST(test_device_name_and_no_spurious_changes);
    RUN_TEST(test_fade_without_frames_cuts);
    return tc_finish("test_lifecycle");
}
