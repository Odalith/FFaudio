/*
 * Loudness analysis tests (ffaudio_loudness.h) against EBU Tech 3341/3342-style reference signals.
 * Expected values were also cross-checked with FFmpeg's ebur128 filter.
 */
#include "test_common.h"
#include "../lib/ffaudio_loudness.h"
#include <math.h>

static double db(const double x) { return x > 0 ? 20.0 * log10(x) : -999.0; }

#define CHECK_NEAR(actual, expected, tol) do { \
    const double a_ = (actual), e_ = (expected); \
    if (!(fabs(a_ - e_) <= (tol))) { \
        fprintf(stderr, "CHECK FAILED %s:%d: %s = %.4f, expected %.4f +- %.3f\n", __FILE__, __LINE__, \
                #actual, a_, e_, (double)(tol)); \
        tc_failures++; \
    } } while (0)

static AuLoudness analyze(const char *file) {
    AuLoudness l;
    memset(&l, 0, sizeof(l));
    const int ret = au_loudness_analyze(fx(file), NULL, &l);
    if (ret != AU_OK) {
        fprintf(stderr, "analyze %s failed: %d\n", file, ret);
        tc_failures++;
    }
    printf("   %-20s I %7.2f LUFS  LRA %5.2f  TP %6.2f dBTP  SP %6.2f dBFS  RG %+6.2f  R128 %d\n", file,
           l.integrated_lufs, l.loudness_range_lu, db(l.true_peak), db(l.sample_peak), l.replaygain_gain_db,
           l.r128_gain_q78);
    return l;
}

static void test_reference_levels(void) {
    AuLoudness l = analyze("ld_m23_48k.flac");
    CHECK(!l.silent);
    CHECK_NEAR(l.integrated_lufs, -23.0, 0.1);
    CHECK_NEAR(l.loudness_range_lu, 0.0, 0.1);
    CHECK_NEAR(l.replaygain_gain_db, 5.0, 0.1);
    CHECK_NEAR(l.r128_gain_db, 0.0, 0.1);
    CHECK_NEAR(l.r128_gain_q78, 0, 26);
    CHECK_NEAR(db(l.sample_peak), -23.0, 0.05);
    CHECK_NEAR(l.duration, 10.0, 0.01);

    l = analyze("ld_m33_48k.flac");
    CHECK_NEAR(l.integrated_lufs, -33.0, 0.1);
    CHECK_NEAR(l.replaygain_gain_db, 15.0, 0.1);
    CHECK_NEAR(l.r128_gain_q78, 2560, 26);

    // K-weighting coefficients are derived for every sample rate
    l = analyze("ld_m23_44k.flac");
    CHECK_NEAR(l.integrated_lufs, -23.0, 0.1);
}

static void test_gating_and_range(void) {
    // Quiet parts 13 LU below the loud part fall under the relative gate (-10 LU)
    AuLoudness l = analyze("ld_gate.flac");
    CHECK_NEAR(l.integrated_lufs, -23.0, 0.1);

    l = analyze("ld_lra10.flac");
    CHECK_NEAR(l.loudness_range_lu, 10.0, 0.5);
}

static void test_peaks(void) {
    // The samples of this signal only reach 0.354 (-9.03 dBFS); the waveform between them goes much higher
    AuLoudness l = analyze("ld_truepeak.wav");
    CHECK_NEAR(db(l.sample_peak), -9.03, 0.05);
    CHECK(db(l.true_peak) > -6.2 && db(l.true_peak) < -5.0);

    const AuLoudnessConfig fast = { .skip_true_peak = true };
    AuLoudness s;
    CHECK(au_loudness_analyze(fx("ld_truepeak.wav"), &fast, &s) == AU_OK);
    CHECK(s.true_peak == s.sample_peak);
    CHECK_NEAR(s.integrated_lufs, l.integrated_lufs, 1e-9);
}

static void test_surround_weights(void) {
    // One front channel: -3 dB compared with the same sine in both stereo channels
    AuLoudness fl = analyze("ld_fl51.flac");
    CHECK_NEAR(fl.integrated_lufs, -26.01, 0.1);
    // A surround channel weighs 1.41 (+1.5 dB)
    AuLoudness sl = analyze("ld_sl51.flac");
    CHECK_NEAR(sl.integrated_lufs - fl.integrated_lufs, 1.49, 0.05);
}

static void test_silence(void) {
    AuLoudness l = analyze("ld_silence.flac");
    CHECK(l.silent);
    CHECK(l.replaygain_gain_db == 0 && l.r128_gain_q78 == 0);
    const AuReplayGainInfo rg = au_loudness_to_replaygain(&l, NULL);
    CHECK(!rg.has_track_gain);
}

static void test_album(void) {
    AuLoudnessScanner *s = au_loudness_scanner_create(NULL);
    REQUIRE(s);
    AuLoudness album;
    CHECK(au_loudness_album(s, &album) == AU_ERR_NOTHING_TO_DO);

    AuLoudness a, b;
    CHECK(au_loudness_scan_file(s, fx("ld_m23_48k.flac"), &a) == AU_OK);
    CHECK(au_loudness_scan_file(s, fx("ld_m33_48k.flac"), &b) == AU_OK);
    CHECK(au_loudness_scan_file(s, fx("text.mp3"), NULL) == AU_ERR_FILE);   // not added
    CHECK(au_loudness_track_count(s) == 2);
    CHECK(au_loudness_album(s, &album) == AU_OK);
    printf("   album: I %.2f LUFS, TP %.2f dBTP, %.1f s\n", album.integrated_lufs, db(album.true_peak), album.duration);

    // Gated over both tracks together: 10*log10((10^-2.3 + 10^-3.3) / 2) = -25.60, not the -28 average
    CHECK_NEAR(album.integrated_lufs, -25.60, 0.1);
    CHECK_NEAR(album.duration, 20.0, 0.02);
    CHECK(album.true_peak == fmax(a.true_peak, b.true_peak));

    const AuReplayGainInfo rg = au_loudness_to_replaygain(&b, &album);
    CHECK(rg.has_track_gain && rg.has_album_gain && rg.has_track_peak && rg.has_album_peak);
    CHECK_NEAR(rg.track_gain_db, b.replaygain_gain_db, 1e-9);
    CHECK_NEAR(rg.album_gain_db, -18.0 - album.integrated_lufs, 1e-9);
    CHECK_NEAR(rg.track_peak, b.true_peak, 1e-12);

    au_loudness_scanner_reset(s);
    CHECK(au_loudness_track_count(s) == 0);
    CHECK(au_loudness_album(s, &album) == AU_ERR_NOTHING_TO_DO);
    au_loudness_scanner_free(s);
}

static int progress_calls = 0;
static double last_fraction = -1;
static bool fractions_increase = true;
static int cancel_after = -1;

static bool on_progress(double fraction, void *user) {
    (void)user;
    if (fraction < last_fraction) fractions_increase = false;
    last_fraction = fraction;
    return ++progress_calls != cancel_after;
}

static void test_progress_and_cancel(void) {
    const AuLoudnessConfig cfg = { .progress = on_progress };
    AuLoudnessScanner *s = au_loudness_scanner_create(&cfg);
    REQUIRE(s);
    CHECK(au_loudness_scan_file(s, fx("ld_gate.flac"), NULL) == AU_OK);
    printf("   progress: %d calls, last %.2f\n", progress_calls, last_fraction);
    CHECK(progress_calls >= 10);
    CHECK(fractions_increase);
    CHECK(last_fraction == 1.0);

    // Cancelling leaves the album untouched
    progress_calls = 0;
    last_fraction = -1;
    cancel_after = 3;
    CHECK(au_loudness_scan_file(s, fx("ld_gate.flac"), NULL) == AU_ERR_CANCELLED);
    CHECK(au_loudness_track_count(s) == 1);
    cancel_after = -1;
    au_loudness_scanner_free(s);
}

static void test_errors(void) {
    AuLoudness l;
    CHECK(au_loudness_analyze(fx("does_not_exist.flac"), NULL, &l) == AU_ERR_FILE);
    CHECK(au_loudness_analyze(fx("video_only.mkv"), NULL, &l) == AU_ERR_FILE);
    CHECK(au_loudness_analyze(fx("empty.flac"), NULL, &l) == AU_ERR_FILE);
    CHECK(au_loudness_analyze(NULL, NULL, &l) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_loudness_analyze(fx("ld_m23_48k.flac"), NULL, NULL) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_loudness_scan_file(NULL, fx("ld_m23_48k.flac"), &l) == AU_ERR_INVALID_ARGUMENT);
    au_loudness_scanner_free(NULL);
}

static void test_scan_while_playing(void) {
    // Analysis is independent of the player: results are identical while music plays
    const AuLoudness before = analyze("ld_gate.flac");
    REQUIRE(tc_init_player(0) == 0);
    au_play_audio(fx("long_6s.flac"), NULL);
    tc_sleep_ms(300);
    const AuLoudness during = analyze("ld_gate.flac");
    CHECK(during.integrated_lufs == before.integrated_lufs);
    CHECK(during.true_peak == before.true_peak);
    CHECK(TC_WAIT_FIELD(eof, 1, 8000));
    CHECK(tc_snapshot().eof_error == 0);
    au_shutdown();
}

int main(int argc, char **argv) {
    tc_setup(argc, argv);
    RUN_TEST(test_reference_levels);
    RUN_TEST(test_gating_and_range);
    RUN_TEST(test_peaks);
    RUN_TEST(test_surround_weights);
    RUN_TEST(test_silence);
    RUN_TEST(test_album);
    RUN_TEST(test_progress_and_cancel);
    RUN_TEST(test_errors);
    RUN_TEST(test_scan_while_playing);
    return tc_finish("test_loudness");
}
