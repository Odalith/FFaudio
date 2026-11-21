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
    CHECK(au_set_device(NULL) == AU_ERR_NOT_INITIALIZED);
    int count = -1;
    CHECK(au_list_devices(&count) == NULL && count == 0);
    au_shutdown();
}

static void test_init_null_config(void) {
    CHECK(au_init(NULL) != 0);
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
    CHECK(au_get_device_name(name, sizeof(name)) == AU_ERR_NOT_INITIALIZED);
    REQUIRE(tc_init_player(0) == 0);
    const int len = au_get_device_name(name, sizeof(name));
    printf("   device in use: \"%s\"\n", name);
    CHECK(len > 0 && (int)strlen(name) == len);
    char tiny[2];
    CHECK(au_get_device_name(tiny, sizeof(tiny)) == AU_ERR_INVALID_ARGUMENT || len < 2);
    CHECK(au_get_device_name(NULL, 10) == AU_ERR_INVALID_ARGUMENT);

    // The device list is freed by ffaudio (the sanitizer builds check that nothing is left)
    int n = -1;
    char **devs = au_list_devices(&n);
    CHECK(n >= 0 && (n == 0) == (devs == NULL));
    for (int i = 0; i < n; i++) CHECK(devs[i] != NULL);
    au_free_devices(devs, n);
    au_free_devices(NULL, 0);
    au_free(NULL);

    // SDL announces every device at start-up and the list is polled every 2 s: neither is a change
    au_play_audio(fx("long_6s.flac"), NULL);
    tc_sleep_ms(4500);
    CHECK(tc_snapshot().devices_changed == 0);
    au_shutdown();
}

// A fade too short for a single frame is a cut. The replaced track must still end and be reported; it used to be left
// in no mixer slot with its threads running, and its end never came. (2.x could also get there by playing before the
// device was opened; au_init() opens it before any track, so only a device that can not be opened at all would.)
static void test_fade_without_frames_cuts(void) {
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
    const TcEvents e = tc_snapshot();
    CHECK(e.eof == 3 && e.eof_skip == 3 && e.eof_error == 0);
    au_shutdown();
}

// ffaudio watches SDL's event queue for device changes but takes nothing from it: the application's events stay
static void test_sdl_events_left_alone(void) {
    REQUIRE(tc_init_player(0) == 0);
    const Uint32 type = SDL_RegisterEvents(1);
    REQUIRE(type != (Uint32)-1);
    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    REQUIRE(SDL_PushEvent(&ev) == 1);
    au_play_audio(fx("s44_stereo.flac"), NULL);
    tc_sleep_ms(300);   // dozens of control thread iterations
    CHECK(SDL_PeepEvents(&ev, 1, SDL_GETEVENT, type, type) == 1);
    au_shutdown();
}

/* ---------- events by polling ---------- */

static int poll_start(void) {
    const AuConfig config = { .app_name = "ffaudio-tests", .volume = 0.5f };   // no on_event: events are queued
    return au_init(&config);
}

// Wait for an event of this type (and handle, when > 0), counting the other events seen meanwhile
static bool poll_until(AU_EVENT type, int32_t handle, double seconds, AuEvent *out, int *others) {
    const Uint32 deadline = SDL_GetTicks() + (Uint32)(seconds * 1000);
    AuEvent e;
    while ((Sint32)(deadline - SDL_GetTicks()) > 0) {
        if (!au_wait_event(&e, 0.1)) continue;
        if (e.type == type && (handle <= 0 || e.handle == handle)) {
            if (out) *out = e;
            return true;
        }
        if (others) (*others)++;
    }
    return false;
}

static void test_poll_events(void) {
    REQUIRE(poll_start() == AU_OK);
    AuEvent e;
    CHECK(poll_until(AU_EVENT_DEVICE, 0, 5, &e, NULL));
    CHECK(e.data.device.ok && e.data.device.sample_rate > 0 && e.data.device.channels > 0 && e.data.device.name);
    printf("   device: \"%s\", %d Hz, %d channels\n", e.data.device.name ? e.data.device.name : "",
           e.data.device.sample_rate, e.data.device.channels);

    const int32_t h = au_play_audio(fx("s44_stereo.flac"), NULL);
    CHECK(poll_until(AU_EVENT_TRACK_STARTED, h, 5, NULL, NULL));
    CHECK(poll_until(AU_EVENT_TRACK_ENDED, h, 5, &e, NULL));
    CHECK(e.data.ended.reason == AU_END_FINISHED);

    // Nothing more: the wait gives up after its time
    while (au_poll_event(&e)) {}
    const Uint32 t0 = SDL_GetTicks();
    CHECK(!au_wait_event(&e, 0.2));
    CHECK(SDL_GetTicks() - t0 >= 150);
    au_shutdown();
    CHECK(!au_poll_event(&e));
}

// More events than the queue holds, with nobody polling: log events go first, the next poll reports the overflow, and
// every track's end is still there
static void test_poll_overflow(void) {
    REQUIRE(poll_start() == AU_OK);
    REQUIRE(poll_until(AU_EVENT_DEVICE, 0, 5, NULL, NULL));
    const int tracks = 3000;   // each: an error log and AU_EVENT_TRACK_ENDED, so about 6000 events for 4096 places
    for (int i = 0; i < tracks; i++) au_play_audio(fx("no_such_file.flac"), NULL);
    // Let the control thread get through them: the end events stop coming in once all are done
    SDL_Delay(500);
    for (int i = 0; i < 100; i++) SDL_Delay(100);
    AuEvent e;
    REQUIRE(au_poll_event(&e));
    CHECK(e.type == AU_EVENT_OVERFLOW && e.data.overflow.dropped > 0);
    const int dropped = e.type == AU_EVENT_OVERFLOW ? e.data.overflow.dropped : 0;
    int ended = 0, logs = 0, total = 0;
    while (au_poll_event(&e)) {
        total++;
        if (e.type == AU_EVENT_TRACK_ENDED) ended++;
        if (e.type == AU_EVENT_LOG) logs++;
    }
    printf("   %d dropped; then %d events: %d track ends, %d logs\n", dropped, total, ended, logs);
    CHECK(total <= 4096);
    CHECK(ended == tracks);
    au_shutdown();
}

// A thread waiting without a time limit wakes up when the player shuts down
static int wait_forever(void *arg) {
    AuEvent e;
    while (au_wait_event(&e, -1)) {}
    SDL_AtomicSet((SDL_atomic_t *)arg, 1);
    return 0;
}

static void test_wait_ends_with_shutdown(void) {
    REQUIRE(poll_start() == AU_OK);
    SDL_atomic_t done;
    SDL_AtomicSet(&done, 0);
    SDL_Thread *th = SDL_CreateThread(wait_forever, "tc_wait", &done);
    REQUIRE(th);
    SDL_Delay(300);
    CHECK(SDL_AtomicGet(&done) == 0);
    au_shutdown();
    SDL_WaitThread(th, NULL);
    CHECK(SDL_AtomicGet(&done) == 1);
}

int main(int argc, char **argv) {
    tc_setup(argc, argv);
    RUN_TEST(test_calls_before_init);
    RUN_TEST(test_init_null_config);
    RUN_TEST(test_shutdown_while_playing);
    RUN_TEST(test_reinit);
    RUN_TEST(test_device_name_and_no_spurious_changes);
    RUN_TEST(test_fade_without_frames_cuts);
    RUN_TEST(test_sdl_events_left_alone);
    RUN_TEST(test_poll_events);
    RUN_TEST(test_poll_overflow);
    RUN_TEST(test_wait_ends_with_shutdown);
    return tc_finish("test_lifecycle");
}
