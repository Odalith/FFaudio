/*
 * Output tests with SDL's "disk" audio driver: the mixed output is written to a file and checked
 * sample by sample (gapless joins, crossfade length, fades, volume ramps, filters).
 */
#include "test_common.h"
#include <math.h>
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
    au_set_audio_volume(100);
    if (mode != AU_DOWNMIX_FULL_LEVEL) CHECK(au_set_downmix(mode, custom) == AU_OK);
    au_play_audio(fx(file), NULL);
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

/* ---------- ReplayGain ---------- */

// Play `file` at volume 100 with the given ReplayGain settings, return the left channel peak in dB
static float replaygain_level(const char *file, const AuReplayGainConfig *cfg, const AuReplayGainInfo *override) {
    start_capture("replaygain");
    au_set_audio_volume(100);
    if (cfg) CHECK(au_set_replaygain(cfg) == AU_OK);
    const PlayAudioConfig pc = { .replaygain = override };
    au_play_audio(fx(file), override ? &pc : NULL);
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
    const AuReplayGainInfo mine = { .has_track_gain = true, .track_gain_db = -10 };
    CHECK_LEVEL(replaygain_level("rg_untagged.flac", &track, &mine), SINE_DB - 10, 0.1f);
    CHECK_LEVEL(replaygain_level("rg_tagged.flac", &track, &mine), SINE_DB - 10, 0.1f);   // override wins
}

static void test_replaygain_live_change(void) {
    // Switching ReplayGain on while playing ramps the level down instead of jumping
    start_capture("replaygain_live");
    au_set_audio_volume(100);
    au_play_audio(fx("rg_long.flac"), NULL);
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
    CHECK(au_set_playback_speed(speed, keep_pitch) == AU_OK);
    au_play_audio(fx(file), NULL);
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

    CHECK(au_initialize(NULL) != AU_OK); // (no player: the setter below must refuse)
    CHECK(au_set_playback_speed(2.0, true) == AU_ERR_NOT_INITIALIZED);
}

static void test_speed_limits_and_position(void) {
    start_capture("speed_position");
    CHECK(au_set_playback_speed(5.0, true) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_set_playback_speed(0.1, true) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_set_playback_speed(2.0, true) == AU_OK);
    CHECK(au_get_playback_speed() == 2.0);
    au_play_audio(fx("long_6s.flac"), NULL);
    tc_sleep_ms(1000);
    const double pos = au_get_audio_play_time();
    printf("   after 1 s at 2x the position is %.2f s of %.2f s\n", pos, au_get_audio_duration());
    CHECK(pos > 1.6 && pos < 2.4);                    // file time advances twice as fast
    CHECK(fabs(au_get_audio_duration() - 6.0) < 0.1); // duration stays in file time
    au_stop_audio();
    wait_until_idle(1, 3000);
    Capture c = stop_capture();
    free(c.samples);
}

static void test_speed_gapless_and_crossfade(void) {
    // Gapless at 2x: two 1 s tracks join into 1 s without a gap
    start_capture("speed_gapless");
    au_set_playback_speed(2.0, true);
    au_play_audio(fx("join_a.flac"), NULL);
    au_set_next(fx("join_b.flac"), NULL);
    wait_until_idle(2, 6000);
    size_t silence;
    double s = transition_seconds("gapless at 2x", &silence, NULL);
    CHECK(fabs(s - 1.0) < 0.05);
    CHECK(silence < 4);

    // A 0.25 s crossfade at 2x: 3 s + 0.5 s - 0.25 s (fade lengths are wall clock time)
    start_capture("speed_crossfade");
    au_set_playback_speed(2.0, true);
    const CrossfadeConfig xf = { .on_track_end = { .seconds = 0.25, .curve = AU_FADE_EQUAL_POWER },
                                 .min_track_seconds = 0.4 };
    au_set_crossfade(&xf);
    au_play_audio(fx("long_6s.flac"), NULL);
    au_set_next(fx("join_b.flac"), NULL);
    wait_until_idle(2, 8000);
    s = transition_seconds("crossfade at 2x", &silence, NULL);
    CHECK(fabs(s - 3.25) < 0.08);
    CHECK(silence < 4);
}

static void test_speed_change_while_playing(void) {
    start_capture("speed_change");
    au_play_audio(fx("long_6s.flac"), NULL);
    tc_sleep_ms(1000);
    CHECK(au_set_playback_speed(2.0, true) == AU_OK);  // 1x -> 2x: a speed stage is inserted
    tc_sleep_ms(1000);
    CHECK(au_set_playback_speed(1.5, true) == AU_OK);  // 2x -> 1.5x: a command to the running filters
    tc_sleep_ms(500);
    CHECK(au_set_playback_speed(1.0, true) == AU_OK);  // back to normal: the stage plays out and goes away
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
    au_fb_add_volume_db(fb, 15);
    AuLimiter lim = au_limiter_defaults();
    lim.ceiling_db = -6;
    CHECK(au_fb_add_limiter(fb, &lim) == AU_OK);
    const AuStereo st = au_stereo_defaults();
    CHECK(au_fb_set_name(fb, "st") == AU_OK);
    CHECK(au_fb_add_stereo(fb, &st) == AU_OK); // named, so it is there to be changed
    printf("   filters: %s\n", au_fb_peek(fb));

    start_capture("named_filters");
    au_set_audio_volume(100);
    CHECK(au_set_global_filtergraph(au_fb_peek(fb)) == AU_OK);
    au_fb_free(fb);
    au_play_audio(fx("long_6s.flac"), NULL);
    tc_sleep_ms(1500);
    CHECK(au_send_filter_command(0, "stereotools@st", "balance_out", "1") == AU_OK); // right only
    tc_sleep_ms(1500);
    au_stop_audio();
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
    RUN_TEST(test_downmix_levels);
    RUN_TEST(test_bad_custom_downmix_rejected);
    RUN_TEST(test_multichannel_routing);
    RUN_TEST(test_replaygain_levels);
    RUN_TEST(test_replaygain_live_change);
    RUN_TEST(test_speed);
    RUN_TEST(test_speed_limits_and_position);
    RUN_TEST(test_speed_gapless_and_crossfade);
    RUN_TEST(test_speed_change_while_playing);
    RUN_TEST(test_limiter_and_named_command);
    RUN_TEST(test_bad_filter_fails_cleanly);
    return tc_finish("test_output");
}
