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

int main(int argc, char **argv) {
    tc_setup(argc, argv);
    RUN_TEST(test_calls_before_init);
    RUN_TEST(test_init_null_config);
    RUN_TEST(test_shutdown_while_playing);
    RUN_TEST(test_reinit);
    return tc_finish("test_lifecycle");
}
