/*
 * Output tests with SDL's "disk" audio driver: the mixed output is written to a file and checked
 * sample by sample (gapless joins, crossfade length, fades, volume ramps, filters).
 */
#include "test_common.h"
#include "../lib/ffaudio_playlist.h"
#include "../lib/ffaudio_meters.h"
#include <locale.h>
#include <math.h>
#ifndef _WIN32
#include <sys/stat.h> // mkfifo
#endif
#include "../lib/ffaudio_filters.h"
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
    _putenv("SDL_AUDIODRIVER=disk");
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
    tc_output_channels = 0;
}

static void start_capture_channels(const char *name, const int channels) {
    tc_output_channels = channels;
    start_capture(name);
}

static Capture stop_capture(void) {
    Capture c = {0};
    int rate = 0, channels = 0;
    rate = tc_status().sample_rate;
    channels = tc_status().channels;
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
    au_play(fx("join_a.flac"), NULL);
    au_queue(fx("join_b.flac"), NULL);
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
    au_play(fx("join_a.flac"), NULL);
    au_queue(fx("join_c_44k.flac"), NULL);
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
    const AuCrossfade xf = { .on_track_end = { .seconds = 0.4, .curve = AU_FADE_EQUAL_POWER } };
    au_set_crossfade(&xf);
    au_play(fx("join_a.flac"), NULL);
    au_queue(fx("join_b.flac"), NULL);
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
    const AuCrossfade xf = { .on_stop = { .seconds = 0.3, .curve = AU_FADE_LINEAR } };
    au_set_crossfade(&xf);
    au_play(fx("long_6s.flac"), NULL);
    tc_sleep_ms(800);
    au_stop();
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
    au_play(fx("long_6s.flac"), NULL);
    tc_sleep_ms(600);
    au_set_volume(1.0f);
    tc_sleep_ms(300);
    au_set_volume(0.0f);
    tc_sleep_ms(300);
    au_set_volume(0.5f);
    tc_sleep_ms(300);
    au_stop();
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

// volume=0.5 halves the level: compare against the same file without the filter
static void check_user_filter(const char *name) {
    start_capture(name);
    const AuPlayConfig plain = {0};
    const AuPlayConfig halved = { .filters = "volume=0.5" };
    au_play(fx("join_a.flac"), &plain);
    au_queue(fx("join_b.flac"), &halved);
    wait_until_idle(2, 6000);
    Capture c = stop_capture();

    size_t first, last;
    audible_range(&c, &first, &last);
    float p1 = 0, p2 = 0;
    const size_t mid = first + (size_t)c.rate; // join point
    for (size_t i = first + c.rate / 10; i < mid - c.rate / 10; i++) p1 = fmaxf(p1, frame_peak(&c, i));
    for (size_t i = mid + c.rate / 10; i < last - c.rate / 10; i++) p2 = fmaxf(p2, frame_peak(&c, i));
    printf("   %s: peak %.3f then %.3f\n", name, p1, p2);
    CHECK(p1 > 0 && fabsf(p2 / p1 - 0.5f) < 0.02f);
    free(c.samples);
}

static void test_user_filter(void) {
    check_user_filter("filter");
}

// An application with a comma-decimal locale (GTK applications set the user's): FFmpeg reads "0.5" with strtod(),
// so the tracks' threads must not use that locale
static void test_user_filter_comma_locale(void) {
    const char *locales[] = { "de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "nl_NL.UTF-8" };
    const char *set = NULL;
    for (size_t i = 0; i < sizeof(locales) / sizeof(locales[0]) && !set; i++)
        set = setlocale(LC_NUMERIC, locales[i]);
    if (!set) {
        printf("   (no comma-decimal locale installed, skipped)\n");
        return;
    }
    CHECK(au_validate_filters("volume=0.5") == AU_OK);   // parsed on this thread
    check_user_filter("filter, comma locale");

    // The downmix check (this thread) and a speed stage with "tempo=0.866025404" (the track's thread)
    start_capture("speed, comma locale");
    CHECK(au_set_downmix(AU_DOWNMIX_CUSTOM, "pan=stereo|c0=0.5*FL+0.5*FC|c1=0.5*FR+0.5*FC") == AU_OK);
    CHECK(au_set_downmix(AU_DOWNMIX_FULL_LEVEL, NULL) == AU_OK);
    CHECK(au_set_speed(1.5, true) == AU_OK);
    au_play(fx("join_a.flac"), NULL);
    CHECK(TC_WAIT_FIELD(eof, 1, 5000));
    CHECK(tc_snapshot().eof_error == 0);
    Capture c = stop_capture();
    size_t first, last;
    audible_range(&c, &first, &last);
    const double seconds = (double)(last - first) / c.rate;
    printf("   1 s at 1.5x: %.3f s audible\n", seconds);
    CHECK(seconds > 0.6 && seconds < 0.75);
    free(c.samples);
    setlocale(LC_NUMERIC, "C");
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
    const AuCrossfade xf = { .on_skip = { .seconds = 0.3, .curve = AU_FADE_LINEAR } };
    au_set_crossfade(&xf);
    au_play(fx("long_6s.flac"), NULL);
    au_queue(fx("join_b.flac"), NULL);
    tc_sleep_ms(800);
    au_skip();
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
    const AuCrossfade xf = { .on_play = { .seconds = 0.3, .curve = AU_FADE_S_CURVE } };
    au_set_crossfade(&xf);
    au_play(fx("long_6s.flac"), NULL);
    tc_sleep_ms(800);
    au_play(fx("join_b.flac"), NULL);
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
    const AuCrossfade xf = { .on_pause = { .seconds = 0.2, .curve = AU_FADE_S_CURVE } };
    au_set_crossfade(&xf);
    au_play(fx("long_6s.flac"), NULL);
    tc_sleep_ms(600);
    au_pause(true);
    tc_sleep_ms(500);
    CHECK(tc_status().paused);
    au_pause(false);
    tc_sleep_ms(500);
    au_stop();
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
    const AuPlayConfig cfg = { .fade_in = &fade };
    au_play(fx("join_a.flac"), NULL);
    au_queue(fx("join_b.flac"), &cfg);
    wait_until_idle(2, 6000);
    const double s = transition_seconds("override", NULL, NULL);
    CHECK(fabs(s - 1.6) < 0.005);
}

static void test_short_tracks_stay_gapless(void) {
    start_capture("shortrule");
    // 0.6 s fades need tracks of at least 1.2 s (default rule); 1 s tracks play gapless instead
    const AuCrossfade xf = { .on_track_end = { .seconds = 0.6, .curve = AU_FADE_EQUAL_POWER } };
    au_set_crossfade(&xf);
    au_play(fx("join_a.flac"), NULL);
    au_queue(fx("join_b.flac"), NULL);
    wait_until_idle(2, 6000);
    const double s = transition_seconds("short rule", NULL, NULL);
    CHECK(fabs(s - 2.0) < 0.01);
}

static void test_custom_curve(void) {
    start_capture("custom");
    const AuCrossfade xf = { .on_track_end = { .seconds = 0.3, .curve = AU_FADE_CUSTOM, .custom = linear_curve } };
    au_set_crossfade(&xf);
    au_play(fx("join_a.flac"), NULL);
    au_queue(fx("join_b.flac"), NULL);
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

// Pausing fades out with the curve run backwards, curve(1 - t), as a crossfade's fade-out does: halfway through an
// equal-power fade the level is cos(45 deg) = 0.71 of what it was (it used to be 1 - sin(45 deg) = 0.29)
static void test_pause_fade_curve(void) {
    start_capture("pausecurve");
    const AuCrossfade xf = { .on_pause = { .seconds = 0.4, .curve = AU_FADE_EQUAL_POWER } };
    au_set_crossfade(&xf);
    au_play(fx("long_6s.flac"), NULL);
    tc_sleep_ms(1000);
    au_pause(true);
    tc_sleep_ms(800);
    au_stop();
    wait_until_idle(1, 3000);
    Capture c = stop_capture();
    size_t first, last;
    audible_range(&c, &first, &last);   // a paused player outputs silence: the fade ends at the last audible frame
    const float steady = steady_peak(&c, first, last);
    const double mid_s = (double)(last - first) / c.rate - 0.2;
    const float ratio = window_peak(&c, first, mid_s - 0.003, mid_s + 0.003) / steady;
    printf("   pause fade: level %.3f of steady halfway\n", ratio);
    CHECK(fabsf(ratio - 0.707f) < 0.05f);
    free(c.samples);
}

static void test_live_filter_changes(void) {
    start_capture("livefilters");
    au_play(fx("long_6s.flac"), NULL);
    tc_sleep_ms(1000);
    CHECK(au_set_filters("not_a_filter=1") == AU_ERR_INVALID_ARGUMENT); // rejected, nothing changes
    CHECK(au_set_filters("volume=0.5") == AU_OK);
    tc_sleep_ms(1500);
    CHECK(au_send_filter_command(0, "volume", "volume", "0.25") == AU_OK);
    tc_sleep_ms(1500);
    CHECK(au_set_track_filters(0, "volume=2") == AU_OK); // track filter runs before the global one
    tc_sleep_ms(1500);
    au_stop();
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

/* ---------- surround (issue #19: 5.1 files were ~7.7 dB quieter than in mpv) ---------- */

static float channel_peak_db(const Capture *c, const int channel) {
    float p = 0;
    for (size_t i = 0; i < c->frames; i++) p = fmaxf(p, fabsf(c->samples[i * c->channels + channel]));
    return p > 1e-9f ? 20.0f * log10f(p) : -999.0f;
}

// Play one file at volume 100 and return the capture
static Capture capture_file(const char *name, const char *file, const int channels, const AU_DOWNMIX mode,
                            const char *custom) {
    start_capture_channels(name, channels);
    au_set_volume(1.0f);
    if (mode != AU_DOWNMIX_FULL_LEVEL) CHECK(au_set_downmix(mode, custom) == AU_OK);
    au_play(fx(file), NULL);
    wait_until_idle(1, 4000);
    return stop_capture();
}

#define SINE_DB (-18.06f) // lavfi sine amplitude 1/8

static void test_downmix_levels(void) {
    // Front left only: unity gain into the left speaker, like mpv
    Capture c = capture_file("dm_fl", "surround_FL.flac", 2, AU_DOWNMIX_FULL_LEVEL, NULL);
    printf("   FL -> stereo: L %.2f R %.2f dB\n", channel_peak_db(&c, 0), channel_peak_db(&c, 1));
    CHECK(c.channels == 2);
    CHECK(fabsf(channel_peak_db(&c, 0) - SINE_DB) < 0.3f);
    CHECK(channel_peak_db(&c, 1) < -90);
    free(c.samples);

    // Centre: -3 dB into both
    c = capture_file("dm_fc", "surround_FC.flac", 2, AU_DOWNMIX_FULL_LEVEL, NULL);
    printf("   FC -> stereo: L %.2f R %.2f dB\n", channel_peak_db(&c, 0), channel_peak_db(&c, 1));
    CHECK(fabsf(channel_peak_db(&c, 0) - (SINE_DB - 3.01f)) < 0.3f);
    CHECK(fabsf(channel_peak_db(&c, 1) - (SINE_DB - 3.01f)) < 0.3f);
    free(c.samples);

    // Normalized: the same matrix scaled by 1 / (1 + 0.707 + 0.707), about -7.65 dB
    c = capture_file("dm_norm", "surround_FL.flac", 2, AU_DOWNMIX_NORMALIZED, NULL);
    printf("   FL -> stereo normalized: L %.2f dB\n", channel_peak_db(&c, 0));
    CHECK(fabsf(channel_peak_db(&c, 0) - (SINE_DB - 7.65f)) < 0.3f);
    free(c.samples);

    // Custom: LFE (dropped by the standard matrix) mixed into both sides at 0.5 (-6 dB)
    c = capture_file("dm_custom", "surround_LFE.flac", 2, AU_DOWNMIX_CUSTOM,
                     "pan=stereo|FL=FL+0.707*FC+0.707*SL+0.5*LFE|FR=FR+0.707*FC+0.707*SR+0.5*LFE");
    printf("   LFE -> stereo custom: L %.2f R %.2f dB\n", channel_peak_db(&c, 0), channel_peak_db(&c, 1));
    CHECK(fabsf(channel_peak_db(&c, 0) - (SINE_DB - 6.02f)) < 0.3f);
    CHECK(fabsf(channel_peak_db(&c, 1) - (SINE_DB - 6.02f)) < 0.3f);
    free(c.samples);
}

static void test_bad_custom_downmix_rejected(void) {
    start_capture("dm_bad");
    CHECK(au_set_downmix(AU_DOWNMIX_CUSTOM, "no_such_filter") == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_set_downmix(AU_DOWNMIX_CUSTOM, NULL) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_set_downmix((AU_DOWNMIX)42, NULL) == AU_ERR_INVALID_ARGUMENT);
    Capture c = stop_capture();
    free(c.samples);
}

static void test_multichannel_routing(void) {
    // 5.1 into a 5.1 output: every channel stays where it is (SDL order FL FR FC LFE SL SR)
    const char *files[] = { "surround_FL.flac", "surround_FC.flac", "surround_LFE.flac", "surround_SL.flac" };
    const int expect_6ch[] = { 0, 2, 3, 4 };
    for (int i = 0; i < 4; i++) {
        Capture c = capture_file("route6", files[i], 6, AU_DOWNMIX_FULL_LEVEL, NULL);
        CHECK(c.channels == 6);
        for (int ch = 0; ch < c.channels; ch++) {
            const float db = channel_peak_db(&c, ch);
            if (ch == expect_6ch[i]) CHECK(fabsf(db - SINE_DB) < 0.3f);
            else CHECK(db < -90);
        }
        free(c.samples);
    }

    // 5-channel output is FL FR LFE BL BR in SDL: LFE must land on channel 2, not the centre
    Capture c = capture_file("route5", "surround_LFE.flac", 5, AU_DOWNMIX_FULL_LEVEL, NULL);
    printf("   LFE -> 5 ch: %.1f %.1f %.1f %.1f %.1f dB\n", channel_peak_db(&c, 0), channel_peak_db(&c, 1),
           channel_peak_db(&c, 2), channel_peak_db(&c, 3), channel_peak_db(&c, 4));
    CHECK(c.channels == 5);
    CHECK(fabsf(channel_peak_db(&c, 2) - SINE_DB) < 0.3f);
    CHECK(channel_peak_db(&c, 0) < -90 && channel_peak_db(&c, 1) < -90);
    free(c.samples);
}

static Capture capture_upmix(const char *file, const int channels, const AU_UPMIX mode, const char *custom) {
    start_capture_channels("upmix", channels);
    au_set_volume(1.0f);
    CHECK(au_set_upmix(mode, custom) == AU_OK);
    au_play(fx(file), NULL);
    wait_until_idle(1, 4000);
    return stop_capture();
}

static void print_channels(const char *what, const Capture *c) {
    printf("   %-24s", what);
    for (int ch = 0; ch < c->channels; ch++) printf(" %7.1f", channel_peak_db(c, ch));
    printf(" dB\n");
}

#define STEREO_DB (SINE_DB - 3.01f) // the stereo fixtures: the mono sine spread over two channels

static void test_upmix(void) {
    // Off: stereo plays on front left and right only
    Capture c = capture_upmix("s44_stereo.flac", 6, AU_UPMIX_NONE, NULL);
    print_channels("stereo -> 5.1 none", &c);
    CHECK(c.channels == 6);
    CHECK(fabsf(channel_peak_db(&c, 0) - STEREO_DB) < 0.3f && fabsf(channel_peak_db(&c, 1) - STEREO_DB) < 0.3f);
    for (int ch = 2; ch < 6; ch++) CHECK(channel_peak_db(&c, ch) < -90);
    free(c.samples);

    // surround: identical left and right is a centred sound, so the centre speaker gets it too
    c = capture_upmix("s44_stereo.flac", 6, AU_UPMIX_SURROUND, NULL);
    print_channels("stereo -> 5.1 surround", &c);
    CHECK(channel_peak_db(&c, 2) > STEREO_DB - 6);
    free(c.samples);
    c = capture_upmix("s44_mono.wav", 6, AU_UPMIX_SURROUND, NULL);
    print_channels("mono -> 5.1 surround", &c);
    CHECK(channel_peak_db(&c, 2) > SINE_DB - 9);
    free(c.samples);

    // Custom: a pan matrix copying the sum of left and right to the centre
    c = capture_upmix("s44_stereo.flac", 6, AU_UPMIX_CUSTOM, "pan=5.1|c0=c0|c1=c1|c2=0.5*c0+0.5*c1");
    print_channels("stereo -> 5.1 custom", &c);
    CHECK(fabsf(channel_peak_db(&c, 2) - STEREO_DB) < 0.3f && fabsf(channel_peak_db(&c, 0) - STEREO_DB) < 0.3f);
    CHECK(channel_peak_db(&c, 4) < -90);
    free(c.samples);

    // A stereo output is not touched
    c = capture_upmix("s44_stereo.flac", 2, AU_UPMIX_SURROUND, NULL);
    print_channels("stereo -> stereo surround", &c);
    CHECK(c.channels == 2 && fabsf(channel_peak_db(&c, 0) - STEREO_DB) < 0.3f);
    free(c.samples);

    start_capture("upmix_bad");
    CHECK(au_set_upmix(AU_UPMIX_CUSTOM, "no_such_filter") == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_set_upmix(AU_UPMIX_CUSTOM, NULL) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_set_upmix((AU_UPMIX)42, NULL) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_set_upmix(AU_UPMIX_NONE, NULL) == AU_OK);
    c = stop_capture();
    free(c.samples);
}

// Play `file` with track filters and return the capture
static Capture capture_with_filters(const char *name, const char *file, const char *filters) {
    start_capture(name);
    au_set_volume(1.0f);
    const AuPlayConfig pc = { .filters = filters };
    au_play(fx(file), &pc);
    wait_until_idle(1, 4000);
    return stop_capture();
}

static void test_restoration_filters(void) {
    AuFilterBuilder *fb = au_fb_create();
    REQUIRE(au_fb_add_declick(fb, NULL, NULL) == AU_OK);
    Capture raw = capture_with_filters("clicks", "clicks.wav", NULL);
    Capture fixed = capture_with_filters("declick", "clicks.wav", au_fb_peek(fb));
    size_t f1, l1, f2, l2;
    audible_range(&raw, &f1, &l1);
    audible_range(&fixed, &f2, &l2);
    // The window's delay and its edges: judge the middle
    const float jr = largest_jump(&raw, f1 + raw.rate / 5, l1 - raw.rate / 5);
    const float jf = largest_jump(&fixed, f2 + fixed.rate / 5, l2 - fixed.rate / 5);
    printf("   clicks: largest jump %.3f, declicked %.3f (a clean sine: %.3f)\n", jr, jf,
           0.125f * 2.0f * (float)M_PI * 440.0f / (float)raw.rate);
    CHECK(jr > 0.5f && jf < 0.1f);
    free(raw.samples);
    free(fixed.samples);

    // Declip as a source filter: on the stored samples, before the 44.1 to 48 kHz conversion
    au_fb_clear(fb);
    REQUIRE(au_fb_add_declip(fb, NULL, NULL) == AU_OK);
    raw = capture_with_filters("clipped", "clipped.wav", NULL);
    start_capture("declip");
    au_set_volume(1.0f);
    CHECK(au_set_source_filters(au_fb_peek(fb)) == AU_OK);
    au_play(fx("clipped.wav"), NULL);
    wait_until_idle(1, 4000);
    fixed = stop_capture();
    audible_range(&raw, &f1, &l1);
    audible_range(&fixed, &f2, &l2);
    printf("   clipped: peak %.2f dB, declipped %.2f dB; audible %zu and %zu frames\n", channel_peak_db(&raw, 0),
           channel_peak_db(&fixed, 0), l1 - f1, l2 - f2);
    // Resampling overshoots a little at the clipped edges; the rebuilt peaks rise well above that
    const float clip_db = 20 * log10f(0.25f);
    CHECK(fabsf(channel_peak_db(&raw, 0) - clip_db) < 0.5f);
    CHECK(channel_peak_db(&fixed, 0) > clip_db + 1.2f);
    CHECK(labs((long)(l2 - f2) - (long)(l1 - f1)) < (long)fixed.rate / 100); // its buffered tail is played
    free(raw.samples);
    free(fixed.samples);
    au_fb_free(fb);

    start_capture("source_bad");
    CHECK(au_set_source_filters("no_such_filter") == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_set_source_filters("") == AU_OK);
    CHECK(au_set_source_filters(NULL) == AU_OK);
    Capture c = stop_capture();
    free(c.samples);
}

/* ---------- sample-exact ranges and seeks ---------- */

#define COUNTER_SCALE 8388608.0f // the counter fixtures hold sample n as (n + 1) / 2^23 (left) and its negative (right)

// The counter value of a frame captured at volume 0.5: the file's sample index + 1 (0 = silence), -1 when the frame is
// not a clean counter value
static long counter_at(const Capture *c, const size_t i) {
    const float l = c->samples[i * c->channels] * 2.0f * COUNTER_SCALE;
    const float r = c->samples[i * c->channels + 1] * 2.0f * COUNTER_SCALE;
    const long k = lrintf(l);
    if (fabsf(l - (float)k) > 1e-3f || fabsf(r + l) > 1e-3f) return -1;
    return k;
}

// Checks the run first..last (every value once, in order) starting at frame *i; advances *i past it
static bool counter_run(const Capture *c, size_t *i, const long first, const long last, const char *what) {
    for (long k = first; k <= last; k++, (*i)++) {
        if (*i >= c->frames) {
            printf("   %s: the capture ends before value %ld\n", what, k);
            return false;
        }
        const long got = counter_at(c, *i);
        if (got != k) {
            printf("   %s: frame %zu is %ld, expected %ld\n", what, *i, got, k);
            return false;
        }
    }
    return true;
}

static size_t skip_silence(const Capture *c, size_t i) {
    while (i < c->frames && counter_at(c, i) == 0) i++;
    return i;
}

// The audible part is exactly the run first..last, `times` times over, and nothing else
static bool counter_exact(const Capture *c, const long first, const long last, const int times, const char *what) {
    size_t i = skip_silence(c, 0);
    for (int rep = 0; rep < times; rep++)
        if (!counter_run(c, &i, first, last, what)) return false;
    i = skip_silence(c, i);
    if (i != c->frames) {
        printf("   %s: %zu frames more after the end (frame %zu is %ld)\n", what, c->frames - i, i, counter_at(c, i));
        return false;
    }
    printf("   %s: values %ld..%ld x%d, every sample exactly once\n", what, first, last, times);
    return true;
}

// Consecutive ranges of one file, each queued when the one before asks for its successor
static const char *range_file;
static const double *range_bounds; // n + 1 boundaries in seconds; the last 0 = to the end of the file
static int range_count, range_next;

static AuPlayConfig range_config(const int i) {
    const double start = range_bounds[i], end = range_bounds[i + 1];
    const AuPlayConfig pc = { .start_seconds = start, .length_seconds = end > 0 ? end - start : 0 };
    return pc;
}

static void queue_next_range(void) {
    SDL_LockMutex(tc_mutex);
    const int i = range_next < range_count ? range_next++ : -1;
    SDL_UnlockMutex(tc_mutex);
    if (i < 0) return;
    const AuPlayConfig pc = range_config(i);
    au_queue(fx(range_file), &pc);
}

static void start_capture_48k(const char *name) {
    tc_output_rate = 48000;
    start_capture_channels(name, 2);
    tc_output_rate = 0;
}

static Capture play_ranges(const char *name, const char *file, const double *bounds, const int n) {
    start_capture_48k(name);
    SDL_LockMutex(tc_mutex);
    range_file = file;
    range_bounds = bounds;
    range_count = n;
    range_next = 1;
    tc_on_prepare_next_hook = queue_next_range;
    SDL_UnlockMutex(tc_mutex);
    const AuPlayConfig first = range_config(0);
    au_play(fx(file), &first);
    wait_until_idle(n, 10000);
    SDL_LockMutex(tc_mutex);
    tc_on_prepare_next_hook = NULL;
    SDL_UnlockMutex(tc_mutex);
    return stop_capture();
}

static void test_exact_ranges(void) {
    // Boundaries between samples, on samples, and far from any packet boundary
    static const double bounds[] = { 0, 0.123456789, 1.0 / 3.0, 1.0000104, 2.5, 0 };
    const char *files[] = { "counter_48k.wav", "counter_48k.flac" };
    for (int f = 0; f < 2; f++) {
        Capture c = play_ranges("ranges", files[f], bounds, 5);
        CHECK(c.rate == 48000);
        CHECK(counter_exact(&c, 1, 192000, 1, files[f]));
        CHECK(tc_snapshot().eof_error == 0);
        free(c.samples);
    }
}

static void test_exact_range_loop(void) {
    start_capture_48k("range_loop");
    au_set_loop_count(2);                       // three times in all
    const AuPlayConfig pc = { .start_seconds = 1.0 / 3.0, .length_seconds = 0.25 };
    au_play(fx("counter_48k.flac"), &pc);
    wait_until_idle(1, 6000);
    Capture c = stop_capture();
    CHECK(counter_exact(&c, 16001, 28000, 3, "range 1/3 + 0.25 looped twice"));
    CHECK(tc_snapshot().restart_loop == 2);
    free(c.samples);
}

// Plays `pc` of the counter, seeks to `to` after 0.3 s, and checks: a run from first_k, then exactly from_k..last_k
static void check_exact_seek(const char *what, const AuPlayConfig *pc, const double to, const long first_k,
                             const long from_k, const long last_k) {
    start_capture_48k("seek");
    au_play(fx("counter_48k.wav"), pc);
    CHECK(TC_WAIT_FIELD(started, 1, 3000));
    tc_sleep_ms(300);
    au_seek(to);
    wait_until_idle(1, 6000);
    Capture c = stop_capture();
    size_t i = skip_silence(&c, 0);
    long k = first_k;
    while (i < c.frames && counter_at(&c, i) == k) { i++; k++; }  // before the seek
    printf("   %s: played %ld..%ld, then %ld\n", what, first_k, k - 1, i < c.frames ? counter_at(&c, i) : 0);
    CHECK(k > first_k);
    CHECK(counter_run(&c, &i, from_k, last_k, what));
    CHECK(skip_silence(&c, i) == c.frames);
    CHECK(TC_WAIT_FIELD(restart, 1, 100));
    free(c.samples);
}

static void test_exact_seek(void) {
    check_exact_seek("seek to 1.5 s", NULL, 1.5, 1, 72001, 192000);
    // In a range, seconds count from its start: 0.5 s + 1.0 s is sample 72000
    const AuPlayConfig pc = { .start_seconds = 0.5, .length_seconds = 2.0 };
    check_exact_seek("range 0.5 + 2, seek to 1 s", &pc, 1.0, 24001, 72001, 120000);
}

// Lossy files: ranges and seeks start from the decoder's preroll, so they match playing the whole file to within the
// codec's noise, at the exact sample. `w` is the whole file, `x` part of it that should equal w from w_at on.
static int first_loud(const Capture *c) {
    for (size_t i = 0; i < c->frames; i++)
        if (fabsf(c->samples[i * c->channels]) > 0.1f) return (int)i;
    return -1;
}

// The offset (within +-search) at which x best matches w from w_at, and the largest difference there
static int best_offset(const Capture *w, const int w_at, const Capture *x, const int x_at, const int search,
                       float *max_err) {
    int best = 0;
    double best_e = INFINITY;
    for (int d = -search; d <= search; d++) {
        double e = 0;
        for (int i = 0; i < 4096; i++) {
            const long a = (long)w_at + d + i, b = (long)x_at + i;
            if (a < 0 || a >= (long)w->frames || b >= (long)x->frames) { e = INFINITY; break; }
            const double diff = w->samples[a * w->channels] - x->samples[b * x->channels];
            e += diff * diff;
        }
        if (e < best_e) { best_e = e; best = d; }
    }
    float m = 0;
    for (long i = 0; x_at + i < (long)x->frames && w_at + best + i < (long)w->frames; i++)
        m = fmaxf(m, fabsf(w->samples[(w_at + best + i) * w->channels] - x->samples[(x_at + i) * x->channels]));
    *max_err = m;
    return best;
}

static void test_lossy_ranges(void) {
    static const double bounds[] = { 0, 1.0 / 3.0, 1.0000104, 0 };
    const char *files[] = { "chirp_48k.mp3", "chirp_48k.opus" };
    for (int f = 0; f < 2; f++) {
        start_capture_48k("lossy_whole");
        au_play(fx(files[f]), NULL);
        wait_until_idle(1, 8000);
        Capture w = stop_capture();
        const int w0 = first_loud(&w);
        REQUIRE(w0 >= 0);

        Capture j = play_ranges("lossy_join", files[f], bounds, 3);
        float err_j = 0;
        const int dj = best_offset(&w, w0, &j, first_loud(&j), 200, &err_j);
        size_t fj, lj, fw, lw;
        audible_range(&j, &fj, &lj);
        audible_range(&w, &fw, &lw);

        start_capture_48k("lossy_range");
        const AuPlayConfig pc = { .start_seconds = 1.5 };
        au_play(fx(files[f]), &pc);
        wait_until_idle(1, 8000);
        Capture r = stop_capture();
        size_t fr, lr;
        audible_range(&r, &fr, &lr);
        float err_r = 0;
        const int dr = best_offset(&w, w0 + 72000, &r, (int)fr, 2000, &err_r);

        printf("   %s: joined ranges: offset %d, largest difference %.5f, %zu vs %zu frames; "
               "from 1.5 s: offset %d, largest difference %.5f\n", files[f], dj, err_j, lj - fj, lw - fw, dr, err_r);
        CHECK(dj == 0 && err_j < 0.02f);
        CHECK(labs((long)(lj - fj) - (long)(lw - fw)) <= 1);
        CHECK(dr == 0 && err_r < 0.02f);
        free(w.samples);
        free(j.samples);
        free(r.samples);
    }
}

// A 44.1 kHz file on a 48 kHz device: each range has its own resampler, so the join is not bit-exact, but nothing
// is lost or doubled and there is no gap or click
static void test_resampled_ranges(void) {
    static const double bounds[] = { 0, 1.0 / 3.0, 2.5, 0 };
    start_capture_48k("rs_whole");
    au_play(fx("long_6s.flac"), NULL);
    wait_until_idle(1, 10000);
    Capture w = stop_capture();
    Capture j = play_ranges("rs_join", "long_6s.flac", bounds, 3);
    size_t fw, lw, fj, lj;
    audible_range(&w, &fw, &lw);
    audible_range(&j, &fj, &lj);
    const float jump = largest_jump(&j, fj, lj), step = max_sine_step(&j, fj, lj);
    printf("   44.1 kHz ranges on 48 kHz: %zu frames (whole file %zu), longest silence %zu, largest step %.4f (max %.4f)\n",
           lj - fj, lw - fw, longest_silence(&j, fj, lj), jump, step);
    // And it is what playing the whole file gives, to within the resampler's rounding
    float err = 0;
    const int d = best_offset(&w, (int)fw, &j, (int)fj, 50, &err);
    printf("   compared with the whole file: offset %d, largest difference %.6f\n", d, err);
    CHECK(labs((long)(lj - fj) - (long)(lw - fw)) <= 2);
    CHECK(longest_silence(&j, fj, lj) < 2);
    CHECK(jump <= step);
    CHECK(d == 0 && err < 1e-4f);
    free(w.samples);
    free(j.samples);
}

// A cue sheet over the counter file, its tracks played one after another: exactly the whole file
static void test_cue_playback(void) {
    char path[1200];
    snprintf(path, sizeof(path), "%s/counter_play.cue", tc_fixture_dir);
    FILE *f = fopen(path, "wb");
    REQUIRE(f);
    fputs("FILE \"counter_48k.flac\" WAVE\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n"
          "  TRACK 02 AUDIO\n    INDEX 01 00:01:10\n  TRACK 03 AUDIO\n    INDEX 00 00:02:00\n    INDEX 01 00:02:37\n"
          "  TRACK 04 AUDIO\n    INDEX 01 00:03:74\n", f);
    fclose(f);
    const AuCueSheet *cue = au_cue_read(path, NULL);
    remove(path);
    REQUIRE(cue && cue->count == 4);
    static double bounds[5];
    for (int i = 0; i < 4; i++) {
        bounds[i] = cue->tracks[i]->start_seconds;
        if (i < 3) CHECK(fabs(cue->tracks[i]->start_seconds + cue->tracks[i]->length_seconds -
                              cue->tracks[i + 1]->start_seconds) < 1e-12);
    }
    bounds[4] = 0;
    CHECK(cue->tracks[3]->length_seconds == 0);
    au_cue_release(cue);
    Capture c = play_ranges("cue", "counter_48k.flac", bounds, 4);
    CHECK(counter_exact(&c, 1, 192000, 1, "cue sheet tracks"));
    free(c.samples);
}

/* ---------- meters ---------- */

static AuLevels levels_now(void) {
    AuLevels lv;
    CHECK(au_get_levels(0.05, &lv) == AU_OK);
    return lv;
}

static void test_levels(void) {
    start_capture_48k("levels");
    AuLevels lv;
    CHECK(au_get_levels(0, &lv) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_get_levels(0.6, &lv) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_get_levels(0.05, NULL) == AU_ERR_INVALID_ARGUMENT);
    lv = levels_now();                          // the first call starts the meters: nothing plays yet
    CHECK(lv.peak[0] == 0 && lv.rms[0] == 0);

    au_set_volume(0.2f);                        // the meters show the music, not the volume
    au_play(fx("sine_1k_half.wav"), NULL);
    CHECK(TC_WAIT_FIELD(started, 1, 3000));
    tc_sleep_ms(400);
    lv = levels_now();
    printf("   playing at volume 0.2: %d ch at %d Hz, window %.3f s, peak %.4f %.4f, rms %.4f %.4f\n", lv.channels,
           lv.sample_rate, lv.window_seconds, lv.peak[0], lv.peak[1], lv.rms[0], lv.rms[1]);
    CHECK(lv.channels == 2 && lv.sample_rate == 48000 && fabs(lv.window_seconds - 0.05) < 1e-3);
    for (int c = 0; c < 2; c++) {
        CHECK(fabsf(lv.peak[c] - 0.5f) < 0.005f);
        CHECK(fabsf(lv.rms[c] - 0.35355f) < 0.005f);
    }

    au_pause(true);
    tc_sleep_ms(400);
    lv = levels_now();
    printf("   paused: peak %.6f\n", lv.peak[0]);
    CHECK(lv.peak[0] < 1e-6f);
    au_pause(false);
    tc_sleep_ms(400);
    lv = levels_now();
    printf("   resumed: peak %.4f\n", lv.peak[0]);
    CHECK(fabsf(lv.peak[0] - 0.5f) < 0.005f);

    au_stop();
    CHECK(TC_WAIT_FIELD(eof, 1, 3000));
    tc_sleep_ms(400);
    lv = levels_now();
    printf("   stopped: peak %.6f\n", lv.peak[0]);
    CHECK(lv.peak[0] < 1e-6f);
    Capture c = stop_capture();
    free(c.samples);
}

static void test_spectrum(void) {
    CHECK(au_spectrum_create(100) == NULL && au_spectrum_create(128) == NULL && au_spectrum_create(32768) == NULL);
    au_spectrum_free(NULL);
    AuSpectrum *sp = au_spectrum_create(2048);
    REQUIRE(sp);
    static float mags[2048 / 2 + 1], one[2048 / 2 + 1], bands[32];
    int rate = 0;
    CHECK(au_spectrum_read(sp, 0, mags, &rate) == AU_ERR_NOT_INITIALIZED);   // no player

    start_capture_48k("spectrum");
    CHECK(au_spectrum_read(sp, 9, mags, &rate) == AU_ERR_INVALID_ARGUMENT);
    au_set_volume(0.2f);
    au_play(fx("sine_1k_half.wav"), NULL);
    CHECK(TC_WAIT_FIELD(started, 1, 3000));
    tc_sleep_ms(400);
    CHECK(au_spectrum_read(sp, -1, mags, &rate) == AU_OK);
    CHECK(au_spectrum_read(sp, 0, one, NULL) == AU_OK);
    int peak = 0;
    for (int k = 1; k <= 1024; k++)
        if (mags[k] > mags[peak]) peak = k;
    const double peak_hz = peak * (double)rate / 2048;
    printf("   1 kHz at 0.5: peak bin %d (%.1f Hz) = %.4f (channel 0: %.4f), 10 kHz bin %.6f\n", peak, peak_hz,
           mags[peak], one[peak], mags[427]);
    CHECK(rate == 48000 && abs(peak - 43) <= 1);
    CHECK(mags[peak] > 0.4f && mags[peak] < 0.52f);          // Hann: up to 1.4 dB less between two bins
    CHECK(fabsf(one[peak] - mags[peak]) < 0.01f);             // both channels are the same
    CHECK(mags[427] < 1e-3f);                                 // far from the tone: nothing
    CHECK(au_spectrum_bands(mags, 2048, rate, 30, 16000, bands, 32) == AU_OK);
    int top = 0;
    for (int b = 1; b < 32; b++)
        if (bands[b] > bands[top]) top = b;
    const double lo = 30 * pow(16000.0 / 30, top / 32.0), hi = 30 * pow(16000.0 / 30, (top + 1) / 32.0);
    printf("   loudest of 32 bands: %d (%.0f..%.0f Hz) = %.4f\n", top, lo, hi, bands[top]);
    CHECK(lo <= 1000 && hi > 1000 && bands[top] == mags[peak]);
    CHECK(au_spectrum_bands(mags, 2048, rate, 0, 16000, bands, 32) == AU_ERR_INVALID_ARGUMENT);

    au_pause(true);
    tc_sleep_ms(400);
    CHECK(au_spectrum_read(sp, -1, mags, &rate) == AU_OK);
    float most = 0;
    for (int k = 0; k <= 1024; k++) most = fmaxf(most, mags[k]);
    printf("   paused: largest bin %.6f\n", most);
    CHECK(most < 1e-6f);
    au_stop();
    CHECK(TC_WAIT_FIELD(eof, 1, 3000));
    Capture c = stop_capture();
    free(c.samples);
    au_spectrum_free(sp);
}

/* ---------- ReplayGain ---------- */

// Play `file` at volume 100 with the given ReplayGain settings, return the left channel peak in dB
static float replaygain_level(const char *file, const AuReplayGainConfig *cfg, const AuReplayGain *override) {
    start_capture("replaygain");
    au_set_volume(1.0f);
    if (cfg) CHECK(au_set_replaygain(cfg) == AU_OK);
    const AuPlayConfig pc = { .replaygain = override };
    au_play(fx(file), override ? &pc : NULL);
    wait_until_idle(1, 4000);
    Capture c = stop_capture();
    const float db = channel_peak_db(&c, 0);
    free(c.samples);
    return db;
}

#define CHECK_LEVEL(actual, expected, tol) do { \
    const float a_ = (actual), e_ = (expected); \
    printf("   %-44s %7.2f dB (expected %.2f)\n", #actual, a_, e_); \
    CHECK(fabsf(a_ - e_) < (tol)); } while (0)

static void test_replaygain_levels(void) {
    const AuReplayGainConfig track = { .mode = AU_REPLAYGAIN_TRACK };
    const AuReplayGainConfig album = { .mode = AU_REPLAYGAIN_ALBUM };
    const AuReplayGainConfig preamp = { .mode = AU_REPLAYGAIN_TRACK, .preamp_db = 2 };
    const AuReplayGainConfig fallback = { .mode = AU_REPLAYGAIN_TRACK, .fallback_gain_db = -6 };
    const AuReplayGainConfig safe = { .mode = AU_REPLAYGAIN_TRACK, .prevent_clipping = true };

    CHECK_LEVEL(replaygain_level("rg_tagged.flac", NULL, NULL), SINE_DB, 0.1f);            // off by default
    CHECK_LEVEL(replaygain_level("rg_tagged.flac", &track, NULL), SINE_DB - 6, 0.1f);
    CHECK_LEVEL(replaygain_level("rg_tagged.flac", &album, NULL), SINE_DB - 3, 0.1f);
    CHECK_LEVEL(replaygain_level("rg_tagged.flac", &preamp, NULL), SINE_DB - 4, 0.1f);
    // Lossy codecs change the peak a little: compare with the same file without ReplayGain
    const float mp3_off = replaygain_level("rg_tagged.mp3", NULL, NULL);
    CHECK_LEVEL(replaygain_level("rg_tagged.mp3", &track, NULL), mp3_off - 6, 0.1f);
    const float opus_off = replaygain_level("rg_r128.opus", NULL, NULL);
    CHECK_LEVEL(replaygain_level("rg_r128.opus", &track, NULL), opus_off - 6, 0.1f);
    CHECK_LEVEL(replaygain_level("rg_untagged.flac", &fallback, NULL), SINE_DB - 6, 0.1f);
    CHECK_LEVEL(replaygain_level("rg_untagged.flac", &track, NULL), SINE_DB, 0.1f);       // no tags, no fallback
    // +12 dB would push the 0.5 peak past full scale: limited to +6.02 dB
    CHECK_LEVEL(replaygain_level("rg_hot.flac", &track, NULL), SINE_DB + 12, 0.1f);
    CHECK_LEVEL(replaygain_level("rg_hot.flac", &safe, NULL), SINE_DB + 6.02f, 0.1f);

    // Values from the application instead of the tags
    AuReplayGain mine = AU_REPLAYGAIN_NONE;
    mine.track_gain_db = -10;
    CHECK_LEVEL(replaygain_level("rg_untagged.flac", &track, &mine), SINE_DB - 10, 0.1f);
    CHECK_LEVEL(replaygain_level("rg_tagged.flac", &track, &mine), SINE_DB - 10, 0.1f);   // override wins
}

#ifndef _WIN32
// Opening a FIFO blocks until a writer appears: the track is the mixer's current one, but its file is not open
// and its gain is not known yet, for as long as the test wants
static char fifo_path[1100];
static int feed_fifo(void *arg) {
    SDL_Delay(300);
    FILE *in = fopen((const char *)arg, "rb");
    FILE *out = fopen(fifo_path, "wb");
    if (in && out) {
        char buf[16384];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
    }
    if (in) fclose(in);
    if (out) fclose(out);
    return 0;
}
#endif

// A late value that leaves the gain as it was (a measurement that agrees) must not make the next change slow: it used
// to leave the 2 s ramp armed for whatever changed next, here a preamp change that should take 10 ms
static void test_gain_change_after_unchanged_late_value(void) {
    CHECK(au_gain_cache_set_file("") == AU_OK); // memory only
    start_capture("late_unchanged");
    au_set_volume(1.0f);
    const AuReplayGainConfig cfg = { .mode = AU_REPLAYGAIN_TRACK }; // no tags, nothing cached: the fallback, 0 dB
    CHECK(au_set_replaygain(&cfg) == AU_OK);
    au_play(fx("long_6s.flac"), NULL);
    tc_sleep_ms(800);
    AuReplayGain same = AU_REPLAYGAIN_NONE;
    same.track_gain_db = 0;
    CHECK(au_gain_cache_set(fx("long_6s.flac"), &same) == AU_OK);
    tc_sleep_ms(300);
    const AuReplayGainConfig quieter = { .mode = AU_REPLAYGAIN_TRACK, .preamp_db = -6 };
    CHECK(au_set_replaygain(&quieter) == AU_OK);
    tc_sleep_ms(1500);
    au_stop();
    wait_until_idle(1, 3000);
    Capture c = stop_capture();
    au_gain_cache_remove(fx("long_6s.flac"));

    size_t first, last;
    audible_range(&c, &first, &last);
    // The preamp changed about 1.1 s in: 0.6 s later a 10 ms ramp is long done, a 2 s one under a third of the way
    const float ratio = window_peak(&c, first, 1.7, 1.8) / steady_peak(&c, first, last);
    printf("   level after the preamp change: %.3f (-6 dB = 0.501)\n", ratio);
    CHECK(fabsf(ratio - 0.501f) < 0.03f);
    free(c.samples);
}

static void test_replaygain_from_first_sample(void) {
    // The mixer runs for a track before its file is open (here: a FIFO nobody writes to for 300 ms). The gain
    // it starts with must still be the track's real one: it used to be taken as 0 dB at that point and then
    // ramped to the real gain over 10 ms, so ReplayGain-reduced tracks started too loud. Windows CI caught it
    // with ordinary files, whose opening is sometimes slower than the first audio callback.
#ifdef _WIN32
    printf("   (needs a FIFO, skipped on Windows)\n");
#else
    snprintf(fifo_path, sizeof(fifo_path), "%s/rg_fifo.flac", tc_fixture_dir);
    remove(fifo_path);
    REQUIRE(mkfifo(fifo_path, 0600) == 0);
    char src[1100];
    snprintf(src, sizeof(src), "%s", fx("rg_tagged.flac")); // track gain -6 dB

    start_capture("replaygain_first_sample");
    au_set_volume(1.0f);
    const AuReplayGainConfig cfg = { .mode = AU_REPLAYGAIN_TRACK };
    CHECK(au_set_replaygain(&cfg) == AU_OK);
    SDL_Thread *writer = SDL_CreateThread(feed_fifo, "fifo", src);
    au_play(fifo_path, NULL);
    wait_until_idle(1, 6000);
    SDL_WaitThread(writer, NULL);
    Capture c = stop_capture();
    remove(fifo_path);

    size_t first, last;
    audible_range(&c, &first, &last);
    const float start_db = 20 * log10f(window_peak(&c, first, 0.0, 0.02));
    const float steady_db = 20 * log10f(window_peak(&c, first, 0.3, 0.7));
    printf("   first 20 ms peak %.2f dB, steady %.2f dB (expected %.2f)\n", start_db, steady_db, SINE_DB - 6);
    CHECK(fabsf(steady_db - (SINE_DB - 6)) < 0.1f);
    CHECK(fabsf(start_db - steady_db) < 0.05f);   // right from the first sample
    free(c.samples);
#endif
}

static void test_replaygain_live_change(void) {
    // Switching ReplayGain on while playing ramps the level down instead of jumping
    start_capture("replaygain_live");
    au_set_volume(1.0f);
    au_play(fx("rg_long.flac"), NULL);
    tc_sleep_ms(1000);
    const AuReplayGainConfig track = { .mode = AU_REPLAYGAIN_TRACK };
    CHECK(au_set_replaygain(&track) == AU_OK);
    wait_until_idle(1, 5000);
    Capture c = stop_capture();

    size_t first, last;
    audible_range(&c, &first, &last);
    const float before = 20.0f * log10f(window_peak(&c, first, 0.2, 0.8));
    const float after = 20.0f * log10f(window_peak(&c, first, 2.0, 2.8));
    const float jump = largest_jump(&c, first, last) / max_sine_step(&c, first, last);
    printf("   live: %.2f dB -> %.2f dB, jump ratio %.2f\n", before, after, jump);
    CHECK(fabsf(before - SINE_DB) < 0.1f);
    CHECK(fabsf(after - (SINE_DB - 6)) < 0.1f);
    CHECK(jump < 1.0f);
    free(c.samples);
}

/* ---------- playback speed ---------- */

// Frequency of the (sine) capture from its zero crossings
static double capture_frequency(const Capture *c, size_t first, size_t last) {
    size_t crossings = 0;
    for (size_t i = first + 1; i < last; i++) {
        const float a = c->samples[(i - 1) * c->channels], b = c->samples[i * c->channels];
        if ((a < 0) != (b < 0)) crossings++;
    }
    return crossings / 2.0 / ((double)(last - first) / c->rate);
}

typedef struct SpeedResult {
    double seconds, frequency;
    size_t silence;
} SpeedResult;

static SpeedResult play_at_speed(const char *name, const char *file, const double speed, const bool keep_pitch) {
    start_capture(name);
    CHECK(au_set_speed(speed, keep_pitch) == AU_OK);
    au_play(fx(file), NULL);
    wait_until_idle(1, 15000);
    Capture c = stop_capture();
    size_t first, last;
    audible_range(&c, &first, &last);
    const SpeedResult r = { (double)(last - first) / c.rate, capture_frequency(&c, first, last),
                            longest_silence(&c, first, last) };
    printf("   %-24s %.3f s, %.1f Hz, longest silence %zu\n", name, r.seconds, r.frequency, r.silence);
    free(c.samples);
    return r;
}

static void test_speed(void) {
    // 6 s of 440 Hz at 2x keeping the pitch: 3 s, still 440 Hz
    SpeedResult r = play_at_speed("2x keep pitch", "long_6s.flac", 2.0, true);
    CHECK(fabs(r.seconds - 3.0) < 0.1);
    CHECK(fabs(r.frequency - 440) < 5);
    CHECK(r.silence < 4);

    // Tape-style: 3 s at 880 Hz
    r = play_at_speed("2x tape", "long_6s.flac", 2.0, false);
    CHECK(fabs(r.seconds - 3.0) < 0.1);
    CHECK(fabs(r.frequency - 880) < 5);

    // Half speed: 1 s becomes 2 s
    r = play_at_speed("0.5x keep pitch", "join_a.flac", 0.5, true);
    CHECK(fabs(r.seconds - 2.0) < 0.1);
    CHECK(fabs(r.frequency - 440) < 5);

    CHECK(au_init(NULL) != AU_OK); // (no player: the setter below must refuse)
    CHECK(au_set_speed(2.0, true) == AU_ERR_NOT_INITIALIZED);
}

static void test_speed_limits_and_position(void) {
    start_capture("speed_position");
    CHECK(au_set_speed(5.0, true) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_set_speed(0.1, true) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_set_speed(2.0, true) == AU_OK);
    CHECK(tc_status().speed == 2.0);
    au_play(fx("long_6s.flac"), NULL);
    tc_sleep_ms(1000);
    const double pos = tc_status().position;
    printf("   after 1 s at 2x the position is %.2f s of %.2f s\n", pos, tc_status().duration);
    CHECK(pos > 1.6 && pos < 2.4);                    // file time advances twice as fast
    CHECK(fabs(tc_status().duration - 6.0) < 0.1); // duration stays in file time
    au_stop();
    wait_until_idle(1, 3000);
    Capture c = stop_capture();
    free(c.samples);
}

static void test_speed_gapless_and_crossfade(void) {
    // Gapless at 2x: two 1 s tracks join into 1 s without a gap
    start_capture("speed_gapless");
    au_set_speed(2.0, true);
    au_play(fx("join_a.flac"), NULL);
    au_queue(fx("join_b.flac"), NULL);
    wait_until_idle(2, 6000);
    size_t silence;
    double s = transition_seconds("gapless at 2x", &silence, NULL);
    CHECK(fabs(s - 1.0) < 0.05);
    CHECK(silence < 4);

    // A 0.25 s crossfade at 2x: 3 s + 0.5 s - 0.25 s (fade lengths are wall clock time)
    start_capture("speed_crossfade");
    au_set_speed(2.0, true);
    const AuCrossfade xf = { .on_track_end = { .seconds = 0.25, .curve = AU_FADE_EQUAL_POWER },
                                 .min_track_seconds = 0.4 };
    au_set_crossfade(&xf);
    au_play(fx("long_6s.flac"), NULL);
    au_queue(fx("join_b.flac"), NULL);
    wait_until_idle(2, 8000);
    s = transition_seconds("crossfade at 2x", &silence, NULL);
    CHECK(fabs(s - 3.25) < 0.08);
    CHECK(silence < 4);
}

static void test_speed_change_while_playing(void) {
    start_capture("speed_change");
    au_play(fx("long_6s.flac"), NULL);
    tc_sleep_ms(1000);
    CHECK(au_set_speed(2.0, true) == AU_OK);  // 1x -> 2x: a speed stage is inserted
    tc_sleep_ms(1000);
    CHECK(au_set_speed(1.5, true) == AU_OK);  // 2x -> 1.5x: a command to the running filters
    tc_sleep_ms(500);
    CHECK(au_set_speed(1.0, true) == AU_OK);  // back to normal: the stage plays out and goes away
    wait_until_idle(1, 10000);
    Capture c = stop_capture();
    size_t first, last;
    audible_range(&c, &first, &last);
    const double seconds = (double)(last - first) / c.rate;
    printf("   speed changes: %.2f s audible, longest silence %zu, %.1f Hz\n", seconds,
           longest_silence(&c, first, last), capture_frequency(&c, first, last));
    CHECK(seconds > 3.5 && seconds < 5.5);            // shorter than 6 s, longer than all-2x
    CHECK(longest_silence(&c, first, last) < 4);      // no gap at any of the changes
    CHECK(fabs(capture_frequency(&c, first, last) - 440) < 5);
    free(c.samples);
}

/* ---------- builder filters in playback: limiter ceiling, a live command to a named filter ---------- */

static float channel_window_peak_db(const Capture *c, const int channel, size_t first, double from_s, double to_s) {
    float p = 0;
    const size_t a = first + (size_t)(from_s * c->rate), b = first + (size_t)(to_s * c->rate);
    for (size_t i = a; i < b && i < c->frames; i++) p = fmaxf(p, fabsf(c->samples[i * c->channels + channel]));
    return p > 1e-9f ? 20.0f * log10f(p) : -999.0f;
}

static void test_limiter_and_named_command(void) {
    // +15 dB into a -6 dB limiter: the sine (-18 dB) would peak at -3 dB without it
    AuFilterBuilder *fb = au_fb_create();
    au_fb_add_volume_db(fb, NULL, 15);
    AuLimiter lim = au_limiter_defaults();
    lim.ceiling_db = -6;
    CHECK(au_fb_add_limiter(fb, NULL, &lim) == AU_OK);
    const AuStereo st = au_stereo_defaults();
    CHECK(au_fb_add_stereo(fb, "st", &st) == AU_OK); // named, so it is there to be changed
    printf("   filters: %s\n", au_fb_peek(fb));

    start_capture("named_filters");
    au_set_volume(1.0f);
    CHECK(au_set_filters(au_fb_peek(fb)) == AU_OK);
    au_fb_free(fb);
    au_play(fx("long_6s.flac"), NULL);
    tc_sleep_ms(1500);
    CHECK(au_send_filter_command(0, "stereotools@st", "balance_out", "1") == AU_OK); // right only
    tc_sleep_ms(1500);
    au_stop();
    wait_until_idle(1, 3000);
    Capture c = stop_capture();

    size_t first, last;
    audible_range(&c, &first, &last);
    const float l0 = channel_window_peak_db(&c, 0, first, 0.2, 1.0), r0 = channel_window_peak_db(&c, 1, first, 0.2, 1.0);
    const float l1 = channel_window_peak_db(&c, 0, first, 2.2, 2.6), r1 = channel_window_peak_db(&c, 1, first, 2.2, 2.6);
    const float all = fmaxf(channel_window_peak_db(&c, 0, first, 0, 3.5), channel_window_peak_db(&c, 1, first, 0, 3.5));
    printf("   limited: L %.2f R %.2f dB (overall peak %.2f); after balance 1: L %.1f R %.2f dB\n", l0, r0, all, l1, r1);
    CHECK(fabsf(l0 - -6.0f) < 0.3f && fabsf(r0 - -6.0f) < 0.3f);
    CHECK(all < -5.8f);           // never over the ceiling, also at the start
    CHECK(l1 < -60.0f);           // the command reached the playing track
    CHECK(fabsf(r1 - -6.0f) < 0.3f);
    free(c.samples);
}

static void test_bad_filter_fails_cleanly(void) {
    start_capture("badfilter");
    const AuPlayConfig bad = { .filters = "this_filter_does_not_exist=1" };
    au_play(fx("join_a.flac"), &bad);
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
    RUN_TEST(test_user_filter_comma_locale);
    RUN_TEST(test_crossfade_on_skip);
    RUN_TEST(test_crossfade_on_play);
    RUN_TEST(test_pause_fade);
    RUN_TEST(test_pause_fade_curve);
    RUN_TEST(test_per_track_fade_override);
    RUN_TEST(test_short_tracks_stay_gapless);
    RUN_TEST(test_custom_curve);
    RUN_TEST(test_live_filter_changes);
    RUN_TEST(test_downmix_levels);
    RUN_TEST(test_bad_custom_downmix_rejected);
    RUN_TEST(test_multichannel_routing);
    RUN_TEST(test_upmix);
    RUN_TEST(test_exact_ranges);
    RUN_TEST(test_exact_range_loop);
    RUN_TEST(test_exact_seek);
    RUN_TEST(test_lossy_ranges);
    RUN_TEST(test_resampled_ranges);
    RUN_TEST(test_cue_playback);
    RUN_TEST(test_levels);
    RUN_TEST(test_spectrum);
    RUN_TEST(test_restoration_filters);
    RUN_TEST(test_replaygain_levels);
    RUN_TEST(test_replaygain_from_first_sample);
    RUN_TEST(test_gain_change_after_unchanged_late_value);
    RUN_TEST(test_replaygain_live_change);
    RUN_TEST(test_speed);
    RUN_TEST(test_speed_limits_and_position);
    RUN_TEST(test_speed_gapless_and_crossfade);
    RUN_TEST(test_speed_change_while_playing);
    RUN_TEST(test_limiter_and_named_command);
    RUN_TEST(test_bad_filter_fails_cleanly);
    return tc_finish("test_output");
}
