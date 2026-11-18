/*
 * Output tests with SDL's "disk" audio driver: the mixed output is written to a file and checked
 * sample by sample (gapless joins, crossfade length, fades, volume ramps, filters).
 */
#include "test_common.h"
#include <math.h>
#define FFA_MIN(a, b) ((a) < (b) ? (a) : (b))

static char out_path[1024];

typedef struct Capture {
    float *samples;   // interleaved
    size_t frames;
    int channels;
    int rate;
} Capture;

static void start_capture(const char *name) {
    snprintf(out_path, sizeof(out_path), "%s/capture_%s.raw", tc_fixture_dir, name);
    remove(out_path);
#ifdef _WIN32
    char env[1100];
    snprintf(env, sizeof(env), "SDL_DISKAUDIOFILE=%s", out_path);
    _putenv(env);
    snprintf(env, sizeof(env), "SDL_AUDIO_DISK_OUTPUT_FILE=%s", out_path);
    _putenv(env);
#else
    setenv("SDL_AUDIODRIVER", "disk", 1);
    setenv("SDL_DISKAUDIOFILE", out_path, 1);           // SDL2
    setenv("SDL_AUDIO_DISK_OUTPUT_FILE", out_path, 1);  // SDL3 (sdl2-compat)
#endif
    tc_reset_events();
    REQUIRE(tc_init_player(0) == 0);
}

static Capture stop_capture(void) {
    Capture c = {0};
    int rate = 0, channels = 0;
    au_get_output_spec(&rate, &channels);
    au_shutdown();

    FILE *f = fopen(out_path, "rb");
    REQUIRE(f);
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    c.channels = channels;
    c.rate = rate;
    c.frames = (size_t)size / (sizeof(float) * (size_t)channels);
    c.samples = malloc((size_t)size + 1);
    REQUIRE(c.samples);
    REQUIRE(fread(c.samples, 1, (size_t)size, f) == (size_t)size);
    fclose(f);
    if (!getenv("FFAUDIO_KEEP_CAPTURE")) remove(out_path);
    return c;
}

static float frame_peak(const Capture *c, size_t i) {
    float p = 0;
    for (int ch = 0; ch < c->channels; ch++) p = fmaxf(p, fabsf(c->samples[i * c->channels + ch]));
    return p;
}

// First and last frame that is not (near) silent
static void audible_range(const Capture *c, size_t *first, size_t *last) {
    *first = 0; *last = 0;
    size_t i;
    for (i = 0; i < c->frames && frame_peak(c, i) < 1e-3f; i++) {}
    *first = i;
    for (i = c->frames; i > *first && frame_peak(c, i - 1) < 1e-3f; i--) {}
    *last = i;
}

// Longest run of near-silent frames between first and last
static size_t longest_silence(const Capture *c, size_t first, size_t last) {
    size_t run = 0, best = 0;
    for (size_t i = first; i < last; i++) {
        if (frame_peak(c, i) < 1e-3f) { if (++run > best) best = run; }
        else run = 0;
    }
    return best;
}

// Largest sample-to-sample jump between first and last (a click shows up as a large jump)
static float largest_jump(const Capture *c, size_t first, size_t last) {
    float best = 0;
    for (size_t i = first + 1; i < last; i++)
        best = fmaxf(best, fabsf(c->samples[i * c->channels] - c->samples[(i - 1) * c->channels]));
    return best;
}

// Peak level of the first audible half second (before any fade or volume change)
static float steady_peak(const Capture *c, size_t first, size_t last) {
    float p = 0;
    const size_t end = FFA_MIN(last, first + (size_t)c->rate / 2);
    for (size_t i = first; i < end; i++) p = fmaxf(p, frame_peak(c, i));
    return p;
}

// Largest legitimate per-sample change of a 440 Hz sine at that level, with 50 % margin.
// A click or a hard gain step shows up as a much larger jump.
static float max_sine_step(const Capture *c, size_t first, size_t last) {
    return 1.5f * steady_peak(c, first, last) * 2.0f * (float)M_PI * 440.0f / (float)c->rate;
}

static void wait_until_idle(int eofs, uint32_t timeout_ms) {
    CHECK(TC_WAIT_FIELD(eof, eofs, timeout_ms));
    tc_sleep_ms(300); // let the tail reach the device
}

/* ---------- tests ---------- */

static void test_gapless_join(void) {
    start_capture("gapless");
    au_play_audio(fx("join_a.flac"), NULL);
    au_set_next(fx("join_b.flac"), NULL);
    wait_until_idle(2, 6000);
    Capture c = stop_capture();

    size_t first, last;
    audible_range(&c, &first, &last);
    const double seconds = (double)(last - first) / c.rate;
    printf("   gapless: %.4f s audible, longest silence %zu frames, largest jump %.4f (rate %d)\n",
           seconds, longest_silence(&c, first, last), largest_jump(&c, first, last), c.rate);
    CHECK(fabs(seconds - 2.0) < 0.01);
    CHECK(longest_silence(&c, first, last) < 4);
    // A 440 Hz full-scale sine changes by at most 2*pi*440/rate per sample (~0.06 at 48 kHz) times the volume
    CHECK(largest_jump(&c, first, last) < max_sine_step(&c, first, last));
    free(c.samples);
}

static void test_gapless_resampled(void) {
    // Second track at 44.1 kHz: resampled to the output rate, the join must still be seamless
    start_capture("gapless_rs");
    au_play_audio(fx("join_a.flac"), NULL);
    au_set_next(fx("join_c_44k.flac"), NULL);
    wait_until_idle(2, 6000);
    Capture c = stop_capture();

    size_t first, last;
    audible_range(&c, &first, &last);
    const double seconds = (double)(last - first) / c.rate;
    printf("   resampled: %.4f s audible, longest silence %zu, largest jump %.4f\n",
           seconds, longest_silence(&c, first, last), largest_jump(&c, first, last));
    CHECK(fabs(seconds - 2.0) < 0.01);
    CHECK(longest_silence(&c, first, last) < 4);
    CHECK(largest_jump(&c, first, last) < max_sine_step(&c, first, last));
    free(c.samples);
}

static void test_crossfade_at_end(void) {
    start_capture("crossfade");
    const CrossfadeConfig xf = { .on_track_end = { .seconds = 0.4, .curve = AU_FADE_EQUAL_POWER } };
    au_set_crossfade(&xf);
    au_play_audio(fx("join_a.flac"), NULL);
    au_set_next(fx("join_b.flac"), NULL);
    wait_until_idle(2, 6000);
    Capture c = stop_capture();

    size_t first, last;
    audible_range(&c, &first, &last);
    const double seconds = (double)(last - first) / c.rate;
    printf("   crossfade: %.4f s audible (expected ~1.6)\n", seconds);
    CHECK(fabs(seconds - 1.6) < 0.005);
    CHECK(longest_silence(&c, first, last) < 4);
    free(c.samples);
}

static void test_fade_out_on_stop(void) {
    start_capture("stopfade");
    const CrossfadeConfig xf = { .on_stop = { .seconds = 0.3, .curve = AU_FADE_LINEAR } };
    au_set_crossfade(&xf);
    au_play_audio(fx("long_6s.flac"), NULL);
    tc_sleep_ms(800);
    au_stop_audio();
    wait_until_idle(1, 3000);
    Capture c = stop_capture();

    size_t first, last;
    audible_range(&c, &first, &last);
    // The end of the capture must taper: the last 0.3 s of audio gets quieter
    const size_t fade_frames = (size_t)(0.3 * c.rate);
    float early = 0, late = 0;
    for (size_t i = last - fade_frames; i < last - fade_frames + c.rate / 50; i++) early = fmaxf(early, frame_peak(&c, i));
    for (size_t i = last - c.rate / 50; i < last; i++) late = fmaxf(late, frame_peak(&c, i));
    printf("   stop fade: peak at fade start %.3f, at fade end %.3f\n", early, late);
    CHECK(early > 0.5f * steady_peak(&c, first, last));
    CHECK(late < early * 0.25f);
    CHECK(largest_jump(&c, first, last) < max_sine_step(&c, first, last));
    free(c.samples);
}

static void test_volume_ramp(void) {
    start_capture("volume");
    au_play_audio(fx("long_6s.flac"), NULL);
    tc_sleep_ms(600);
    au_set_audio_volume(100);
    tc_sleep_ms(300);
    au_set_audio_volume(0);
    tc_sleep_ms(300);
    au_set_audio_volume(50);
    tc_sleep_ms(300);
    au_stop_audio();
    wait_until_idle(1, 3000);
    Capture c = stop_capture();

    size_t first, last;
    audible_range(&c, &first, &last);
    const float jump = largest_jump(&c, first, last);
    printf("   volume ramp: largest jump %.4f\n", jump);
    // At volume 100 a 440 Hz sine moves at most ~0.058 per sample; a hard volume step would be far larger
    CHECK(jump < 2.0f * max_sine_step(&c, first, last)); // peak doubles at volume 100
    free(c.samples);
}

static void test_user_filter(void) {
    // volume=0.5 halves the level: compare against the same file without the filter
    start_capture("filter");
    const PlayAudioConfig plain = {0};
    const PlayAudioConfig halved = { .av_filtergraph = "volume=0.5" };
    au_play_audio(fx("join_a.flac"), &plain);
    au_set_next(fx("join_b.flac"), &halved);
    wait_until_idle(2, 6000);
    Capture c = stop_capture();

    size_t first, last;
    audible_range(&c, &first, &last);
    float p1 = 0, p2 = 0;
    const size_t mid = first + (size_t)c.rate; // join point
    for (size_t i = first + c.rate / 10; i < mid - c.rate / 10; i++) p1 = fmaxf(p1, frame_peak(&c, i));
    for (size_t i = mid + c.rate / 10; i < last - c.rate / 10; i++) p2 = fmaxf(p2, frame_peak(&c, i));
    printf("   filter: peak %.3f then %.3f\n", p1, p2);
    CHECK(fabsf(p2 / p1 - 0.5f) < 0.02f);
    free(c.samples);
}

// Audible length of a two-track transition, plus sanity checks shared by the crossfade tests
static double transition_seconds(const char *name, size_t *silence_out, float *jump_ratio_out) {
    Capture c = stop_capture();
    size_t first, last;
    audible_range(&c, &first, &last);
    const double seconds = (double)(last - first) / c.rate;
    const size_t silence = longest_silence(&c, first, last);
    const float jump_ratio = largest_jump(&c, first, last) / max_sine_step(&c, first, last);
    printf("   %s: %.4f s audible, longest silence %zu, jump ratio %.2f\n", name, seconds, silence, jump_ratio);
    if (silence_out) *silence_out = silence;
    if (jump_ratio_out) *jump_ratio_out = jump_ratio;
    free(c.samples);
    return seconds;
}

static float linear_curve(float t, void *user) { (void)user; return t; }

static void test_crossfade_on_skip(void) {
    start_capture("skipfade");
    const CrossfadeConfig xf = { .on_skip = { .seconds = 0.3, .curve = AU_FADE_LINEAR } };
    au_set_crossfade(&xf);
    au_play_audio(fx("long_6s.flac"), NULL);
    au_set_next(fx("join_b.flac"), NULL);
    tc_sleep_ms(800);
    au_skip_to_next();
    wait_until_idle(2, 5000);
    size_t silence;
    float jump;
    const double s = transition_seconds("skip fade", &silence, &jump);
    // ~0.8 s of the first track, then join_b (1 s) whose first 0.3 s overlap the fade-out
    CHECK(s > 1.6 && s < 2.1);
    CHECK(silence < 4);
    CHECK(jump < 1.0f);
    const TcEvents e = tc_snapshot();
    CHECK(e.eof_skip == 1);
}

static void test_crossfade_on_play(void) {
    start_capture("playfade");
    const CrossfadeConfig xf = { .on_play = { .seconds = 0.3, .curve = AU_FADE_S_CURVE } };
    au_set_crossfade(&xf);
    au_play_audio(fx("long_6s.flac"), NULL);
    tc_sleep_ms(800);
    au_play_audio(fx("join_b.flac"), NULL);
    wait_until_idle(2, 5000);
    size_t silence;
    float jump;
    const double s = transition_seconds("play fade", &silence, &jump);
    CHECK(s > 1.6 && s < 2.1);
    CHECK(silence < 4);
    CHECK(jump < 1.0f);
}

static void test_pause_fade(void) {
    start_capture("pausefade");
    const CrossfadeConfig xf = { .on_pause = { .seconds = 0.2, .curve = AU_FADE_S_CURVE } };
    au_set_crossfade(&xf);
    au_play_audio(fx("long_6s.flac"), NULL);
    tc_sleep_ms(600);
    au_pause_audio(true);
    tc_sleep_ms(500);
    CHECK(au_is_paused());
    au_pause_audio(false);
    tc_sleep_ms(500);
    au_stop_audio();
    wait_until_idle(1, 3000);
    size_t silence;
    float jump;
    transition_seconds("pause fade", &silence, &jump);
    CHECK(silence > (size_t)(0.2 * 44100)); // the pause itself
    CHECK(jump < 1.0f);                      // faded out and back in, no click
}

static void test_per_track_fade_override(void) {
    start_capture("override");
    // No crossfade configured, but the queued track asks for a 0.4 s fade in
    const AuFade fade = { .seconds = 0.4, .curve = AU_FADE_EQUAL_POWER };
    const PlayAudioConfig cfg = { .fade_in = &fade };
    au_play_audio(fx("join_a.flac"), NULL);
    au_set_next(fx("join_b.flac"), &cfg);
    wait_until_idle(2, 6000);
    const double s = transition_seconds("override", NULL, NULL);
    CHECK(fabs(s - 1.6) < 0.005);
}

static void test_short_tracks_stay_gapless(void) {
    start_capture("shortrule");
    // 0.6 s fades need tracks of at least 1.2 s (default rule); 1 s tracks play gapless instead
    const CrossfadeConfig xf = { .on_track_end = { .seconds = 0.6, .curve = AU_FADE_EQUAL_POWER } };
    au_set_crossfade(&xf);
    au_play_audio(fx("join_a.flac"), NULL);
    au_set_next(fx("join_b.flac"), NULL);
    wait_until_idle(2, 6000);
    const double s = transition_seconds("short rule", NULL, NULL);
    CHECK(fabs(s - 2.0) < 0.01);
}

static void test_custom_curve(void) {
    start_capture("custom");
    const CrossfadeConfig xf = { .on_track_end = { .seconds = 0.3, .curve = AU_FADE_CUSTOM, .custom = linear_curve } };
    au_set_crossfade(&xf);
    au_play_audio(fx("join_a.flac"), NULL);
    au_set_next(fx("join_b.flac"), NULL);
    wait_until_idle(2, 6000);
    size_t silence;
    const double s = transition_seconds("custom curve", &silence, NULL);
    CHECK(fabs(s - 1.7) < 0.005);
    CHECK(silence < 4);
}

static float window_peak(const Capture *c, size_t first, double from_s, double to_s) {
    float p = 0;
    const size_t a = first + (size_t)(from_s * c->rate), b = first + (size_t)(to_s * c->rate);
    for (size_t i = a; i < b && i < c->frames; i++) p = fmaxf(p, frame_peak(c, i));
    return p;
}

static void test_live_filter_changes(void) {
    start_capture("livefilters");
    au_play_audio(fx("long_6s.flac"), NULL);
    tc_sleep_ms(1000);
    CHECK(au_set_global_filtergraph("not_a_filter=1") == AU_ERR_INVALID_ARGUMENT); // rejected, nothing changes
    CHECK(au_set_global_filtergraph("volume=0.5") == AU_OK);
    tc_sleep_ms(1500);
    CHECK(au_send_filter_command(0, "volume", "volume", "0.25") == AU_OK);
    tc_sleep_ms(1500);
    CHECK(au_update_filtergraph(0, "volume=2") == AU_OK); // track filter runs before the global one
    tc_sleep_ms(1500);
    au_stop_audio();
    wait_until_idle(1, 3000);
    Capture c = stop_capture();

    size_t first, last;
    audible_range(&c, &first, &last);
    const float p0 = window_peak(&c, first, 0.2, 0.8);
    const float p1 = window_peak(&c, first, 2.0, 2.4);   // global volume=0.5
    const float p2 = window_peak(&c, first, 3.5, 3.9);   // command: 0.25
    const float p3 = window_peak(&c, first, 5.0, 5.4);   // track volume=2 * global 0.25 (rebuilt: back to 0.5)
    printf("   live filters: %.4f -> %.4f -> %.4f -> %.4f, longest silence %zu\n", p0, p1, p2, p3,
           longest_silence(&c, first, last));
    CHECK(fabsf(p1 / p0 - 0.5f) < 0.02f);
    CHECK(fabsf(p2 / p0 - 0.25f) < 0.02f);
    // Rebuilding the user graph re-applies the global string (volume=0.5), the command value is not kept
    CHECK(fabsf(p3 / p0 - 1.0f) < 0.03f);
    CHECK(longest_silence(&c, first, last) < 6); // quietest section: zero crossings span ~3 frames
    free(c.samples);
}

static void test_bad_filter_fails_cleanly(void) {
    start_capture("badfilter");
    const PlayAudioConfig bad = { .av_filtergraph = "this_filter_does_not_exist=1" };
    au_play_audio(fx("join_a.flac"), &bad);
    CHECK(TC_WAIT_FIELD(eof, 1, 3000));
    CHECK(tc_snapshot().eof_error == 1);
    Capture c = stop_capture();
    free(c.samples);
}

int main(int argc, char **argv) {
    tc_setup(argc, argv);
    RUN_TEST(test_gapless_join);
    RUN_TEST(test_gapless_resampled);
    RUN_TEST(test_crossfade_at_end);
    RUN_TEST(test_fade_out_on_stop);
    RUN_TEST(test_volume_ramp);
    RUN_TEST(test_user_filter);
    RUN_TEST(test_crossfade_on_skip);
    RUN_TEST(test_crossfade_on_play);
    RUN_TEST(test_pause_fade);
    RUN_TEST(test_per_track_fade_override);
    RUN_TEST(test_short_tracks_stay_gapless);
    RUN_TEST(test_custom_curve);
    RUN_TEST(test_live_filter_changes);
    RUN_TEST(test_bad_filter_fails_cleanly);
    return tc_finish("test_output");
}
