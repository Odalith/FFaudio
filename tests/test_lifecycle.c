/*
 * Lifecycle tests: calls before init, shutdown while playing, init again after shutdown.
 */
#include "test_common.h"
#include <math.h>

static void test_calls_before_init(void) {
    /* None of these may crash when the player does not exist, and each says so */
    CHECK(au_play(fx("s44_stereo.flac"), NULL) == AU_ERR_NOT_INITIALIZED);
    CHECK(au_queue(fx("s44_stereo.flac"), NULL) == AU_ERR_NOT_INITIALIZED);
    CHECK(au_skip() == AU_ERR_NOT_INITIALIZED);
    CHECK(au_clear_queue() == AU_ERR_NOT_INITIALIZED);
    CHECK(au_stop() == AU_ERR_NOT_INITIALIZED);
    CHECK(au_pause(true) == AU_ERR_NOT_INITIALIZED);
    CHECK(au_seek(0.01) == AU_ERR_NOT_INITIALIZED);
    CHECK(au_seek_fraction(0.1) == AU_ERR_NOT_INITIALIZED);
    CHECK(au_set_volume(0.1f) == AU_ERR_NOT_INITIALIZED);
    CHECK(au_set_muted(true) == AU_ERR_NOT_INITIALIZED);
    CHECK(au_set_loop_count(2) == AU_ERR_NOT_INITIALIZED);
    CHECK(au_open_device() == AU_ERR_NOT_INITIALIZED);
    CHECK(tc_status().position < 0);
    CHECK(tc_status().duration < 0);
    CHECK(au_set_device(NULL) == AU_ERR_NOT_INITIALIZED);
    int count = -1;
    CHECK(au_list_devices(&count) == NULL && count == 0);
    CHECK(au_shutdown() == AU_ERR_NOT_INITIALIZED);
}

// Values out of range are refused, and change nothing (au_set_volume(2.0f) used to do nothing without a word)
static void test_invalid_values(void) {
    REQUIRE(tc_init_player(0) == 0);
    CHECK(au_set_volume(2.0f) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_set_volume(NAN) == AU_ERR_INVALID_ARGUMENT);
    CHECK(tc_status().volume == 0.5f);
    CHECK(au_seek(-1) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_seek(INFINITY) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_seek_fraction(1.5) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_set_loop_count(-2) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_set_loop_count(-1) == AU_OK);
    CHECK(au_set_loop_count(0) == AU_OK);
    CHECK(au_set_muted(false) == AU_OK);
    CHECK(au_pause(false) == AU_OK);
    au_shutdown();
}

// Everything given to au_init() is optional; a second au_init() is refused
static void test_init_without_arguments(void) {
    CHECK(au_init(NULL, NULL, NULL) == AU_OK);
    CHECK(au_init(NULL, NULL, NULL) == AU_ERR_ALREADY_INITIALIZED);
    au_shutdown();
}

static void test_shutdown_while_playing(void) {
    REQUIRE(tc_init_player(0) == 0);
    au_play(fx("long_6s.flac"), NULL);
    au_queue(fx("s44_stereo.flac"), NULL);
    tc_sleep_ms(300);
    au_shutdown();
}

static void test_reinit(void) {
    for (int i = 0; i < 3; i++) {
        tc_reset_events();
        const int ret = tc_init_player(0);
        CHECK(ret == 0);
        if (ret != 0) return;
        au_play(fx("s44_stereo.flac"), NULL);
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
    au_play(fx("long_6s.flac"), NULL);
    tc_sleep_ms(4500);
    CHECK(tc_snapshot().devices_changed == 0);
    au_shutdown();
}

// A fade too short for a single frame is a cut. The replaced track must still end and be reported; it used to be left
// in no mixer slot with its threads running, and its end never came. (2.x could also get there by playing before the
// device was opened; the device is open before the first track takes a slot, so only one that can not be opened
// at all would.)
static void test_fade_without_frames_cuts(void) {
    REQUIRE(tc_init_player(0) == 0);
    const AuFade tiny = { .seconds = 1e-6, .curve = AU_FADE_LINEAR };
    const AuCrossfade xf2 = { .on_play = tiny, .on_skip = tiny, .on_stop = tiny };
    CHECK(au_set_crossfade(&xf2) == AU_OK);
    au_play(fx("long_6s.flac"), NULL);
    tc_sleep_ms(200);
    au_play(fx("long_6s.flac"), NULL);   // play over a playing track
    CHECK(TC_WAIT_FIELD(eof, 1, 2000));
    au_queue(fx("long_6s.flac"), NULL);
    tc_sleep_ms(200);
    au_skip();
    CHECK(TC_WAIT_FIELD(eof, 2, 2000));
    tc_sleep_ms(200);
    au_stop();
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
    au_play(fx("s44_stereo.flac"), NULL);
    tc_sleep_ms(300);   // dozens of control thread iterations
    CHECK(SDL_PeepEvents(&ev, 1, SDL_GETEVENT, type, type) == 1);
    au_shutdown();
}

/* ---------- status ---------- */

// au_get_status() has the settings as last set, before the control thread has applied them, and the defaults
// without a player
static void test_status_reflects_settings(void) {
    AuStatus st = tc_status();
    CHECK(st.handle == 0 && st.position == -1 && st.duration == -1 && st.volume == 1.0f && st.speed == 1.0);
    CHECK(st.sample_rate == 0 && st.channels == 0);

    REQUIRE(tc_init_player(0) == 0);
    st = tc_status();
    CHECK(st.sample_rate > 0 && st.channels > 0 && st.volume == 0.5f && !st.paused);
    au_set_volume(0.333f);
    au_set_speed(1.5, false);
    au_set_loop_count(3);
    au_set_muted(true);
    au_pause(true);
    st = tc_status();
    CHECK(st.volume == 0.333f && st.speed == 1.5 && !st.keep_pitch && st.loop_count == 3 && st.muted && st.paused);
    au_set_volume(2.0f);                 // out of range: ignored
    CHECK(tc_status().volume == 0.333f);
    au_set_loop_count(0);
    au_set_speed(1.0, true);
    au_set_muted(false);

    // Playing unpauses, at once and for good: the pause still queued ahead of the play must not show up as paused
    const int32_t h = au_play(fx("long_6s.flac"), NULL);
    int paused_reads = 0;
    for (int i = 0; i < 200; i++) {
        paused_reads += tc_status().paused;
        SDL_Delay(1);
    }
    CHECK(paused_reads == 0);
    CHECK(TC_WAIT_FIELD(started, 1, 5000));
    tc_sleep_ms(300);
    st = tc_status();
    CHECK(st.handle == h && st.position > 0 && st.position < 6 && st.duration > 5.9 && st.duration < 6.1);

    // au_queue() with nothing playing starts the track, which unpauses: the status follows once that is done
    au_stop();
    CHECK(TC_WAIT_FIELD(eof, 1, 5000));
    au_pause(true);
    CHECK(tc_status().paused);
    au_queue(fx("long_6s.flac"), NULL);
    CHECK(TC_WAIT_FIELD(started, 2, 5000));
    CHECK(!tc_status().paused);
    au_shutdown();
}

/* ---------- events by polling ---------- */

static int poll_start(void) {
    const int ret = au_init("ffaudio-tests", NULL, NULL);   // no on_event: events are queued
    if (ret == AU_OK) {
        au_set_volume(0.5f);
        au_open_device();
    }
    return ret;
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

    const int32_t h = au_play(fx("s44_stereo.flac"), NULL);
    CHECK(poll_until(AU_EVENT_TRACK_STARTED, h, 5, NULL, NULL));
    CHECK(poll_until(AU_EVENT_TRACK_ENDED, h, 5, &e, NULL));
    CHECK(e.data.ended.reason == AU_END_FINISHED);
    CHECK(e.data.ended.error == AU_OK && e.data.ended.message == NULL);

    // A failed track says why; the text is the event's own, valid until the next poll
    const int32_t bad = au_play(fx("does_not_exist.flac"), NULL);
    CHECK(poll_until(AU_EVENT_TRACK_ENDED, bad, 5, &e, NULL));
    CHECK(e.data.ended.reason == AU_END_ERROR && e.data.ended.error == AU_ERR_FILE);
    CHECK(e.data.ended.message && strstr(e.data.ended.message, "does_not_exist.flac"));
    printf("   ended: %s\n", e.data.ended.message ? e.data.ended.message : "(none)");
    CHECK(sizeof(AuEvent) == 72);   // part of the ABI: new members go into the padding

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
    // Every track ends (AU_EVENT_TRACK_ENDED), so 5000 of them are more than the 4096 places on their own. Most also
    // log an error, but not all: a track replaced before its open fails is aborted and says nothing (how many depends
    // on how fast threads start, e.g. fewer logs on Windows).
    const int tracks = 5000;
    for (int i = 0; i < tracks; i++) au_play(fx("no_such_file.flac"), NULL);
    // Let the control thread get through them
    for (int i = 0; i < 200; i++) SDL_Delay(100);
    AuEvent e;
    REQUIRE(au_poll_event(&e));
    CHECK(e.type == AU_EVENT_OVERFLOW && e.data.overflow.dropped >= tracks - 4096);
    const int dropped = e.type == AU_EVENT_OVERFLOW ? e.data.overflow.dropped : 0;
    int ended = 0, logs = 0, prepares = 0, other = 0, total = 0;
    while (au_poll_event(&e)) {
        total++;
        if (e.type == AU_EVENT_TRACK_ENDED) ended++;
        else if (e.type == AU_EVENT_LOG) logs++;
        else if (e.type == AU_EVENT_PREPARE_NEXT) prepares++;   // a failed current track asks for the next one
        else other++;
    }
    printf("   %d dropped; then %d events: %d track ends, %d prepare-next, %d logs, %d other\n", dropped, total,
           ended, prepares, logs, other);
    CHECK(total == 4096);                // full
    CHECK(logs == 0);                    // logs went first: there were more non-log events than places
    // How many tracks fail while current (and ask for the next) depends on thread timing; nothing else is expected
    CHECK(ended + prepares + other == total && other <= 16 && ended >= total / 2 && ended <= tracks);
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

// The device opens when it is needed, with the settings made until then: one AU_EVENT_DEVICE, in the format asked for
// (au_init() used to open the default device at once, and every format or device setter opened it again)
static void test_device_opens_lazily(void) {
    REQUIRE(au_init("ffaudio-tests", NULL, NULL) == AU_OK);
    tc_sleep_ms(300);
    AuEvent e;
    int devices = 0;
    while (au_poll_event(&e)) if (e.type == AU_EVENT_DEVICE) devices++;
    CHECK(devices == 0);
    CHECK(tc_status().sample_rate == 0);
    char name[256];
    CHECK(au_get_device_name(name, sizeof(name)) == 0 && name[0] == '\0');

    CHECK(au_set_output_format(1, 22050) == AU_OK);
    CHECK(au_set_device("no such device") == AU_OK);     // recorded; it falls back when the device opens
    const int32_t h = au_play(fx("s44_stereo.flac"), NULL);
    CHECK(poll_until(AU_EVENT_DEVICE, 0, 5, &e, NULL));
    CHECK(e.data.device.ok && e.data.device.fell_back && e.data.device.sample_rate == 22050 &&
          e.data.device.channels == 1);
    int more = 0;
    CHECK(poll_until(AU_EVENT_TRACK_STARTED, h, 5, NULL, NULL));
    while (au_poll_event(&e)) if (e.type == AU_EVENT_DEVICE) more++;
    CHECK(more == 0);
    CHECK(tc_status().sample_rate == 22050 && tc_status().channels == 1);
    au_shutdown();

    // au_open_device() opens it before any track, once
    REQUIRE(au_init("ffaudio-tests", NULL, NULL) == AU_OK);
    CHECK(au_open_device() == AU_OK);
    CHECK(poll_until(AU_EVENT_DEVICE, 0, 5, &e, NULL) && e.data.device.ok);
    CHECK(au_open_device() == AU_OK);
    tc_sleep_ms(200);
    more = 0;
    while (au_poll_event(&e)) if (e.type == AU_EVENT_DEVICE) more++;
    CHECK(more == 0);
    au_shutdown();
    CHECK(au_open_device() == AU_ERR_NOT_INITIALIZED);
}

// A failed au_init() (here: an audio driver that does not exist) leaves nothing behind (ASan checks for leaks): a
// polling application can read why it failed, and the next au_init() works
static void test_failed_init(void) {
    char driver[64];
    snprintf(driver, sizeof(driver), "%s", SDL_getenv("SDL_AUDIODRIVER") ? SDL_getenv("SDL_AUDIODRIVER") : "dummy");
    SDL_setenv("SDL_AUDIODRIVER", "no_such_driver", 1);
    CHECK(poll_start() == AU_ERR_SDL);
    AuEvent e;
    CHECK(au_poll_event(&e) && e.type == AU_EVENT_LOG && e.data.log.level == AU_LOG_FATAL && e.data.log.message);
    printf("   failed: %s\n", e.data.log.message ? e.data.log.message : "(none)");
    SDL_setenv("SDL_AUDIODRIVER", driver, 1);
    REQUIRE(tc_init_player(0) == AU_OK);
    au_shutdown();
}

int main(int argc, char **argv) {
    tc_setup(argc, argv);
    RUN_TEST(test_calls_before_init);
    RUN_TEST(test_invalid_values);
    RUN_TEST(test_init_without_arguments);
    RUN_TEST(test_shutdown_while_playing);
    RUN_TEST(test_reinit);
    RUN_TEST(test_device_name_and_no_spurious_changes);
    RUN_TEST(test_fade_without_frames_cuts);
    RUN_TEST(test_sdl_events_left_alone);
    RUN_TEST(test_status_reflects_settings);
    RUN_TEST(test_poll_events);
    RUN_TEST(test_poll_overflow);
    RUN_TEST(test_wait_ends_with_shutdown);
    RUN_TEST(test_failed_init);
    RUN_TEST(test_device_opens_lazily);
    return tc_finish("test_lifecycle");
}
