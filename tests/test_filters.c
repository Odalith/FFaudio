/*
 * Unit tests for the filter-string builder (ffaudio_filters.h). No audio device needed.
 */
#include "test_common.h"
#include "../lib/ffaudio_filters.h"
#include <locale.h>

#define CHECK_STR(actual, expected) do { \
    const char *a_ = (actual), *e_ = (expected); \
    if (!a_ || strcmp(a_, e_) != 0) { \
        fprintf(stderr, "CHECK FAILED %s:%d:\n  got      \"%s\"\n  expected \"%s\"\n", __FILE__, __LINE__, a_ ? a_ : "(null)", e_); \
        tc_failures++; \
    } } while (0)

static void test_empty_builder(void) {
    AuFilterBuilder *fb = au_fb_create();
    REQUIRE(fb);
    CHECK(au_fb_build(fb) == NULL);
    CHECK_STR(au_fb_peek(fb), "");
    au_fb_free(fb);
}

static void test_gain(void) {
    AuFilterBuilder *fb = au_fb_create();
    CHECK(au_fb_add_volume_db(fb, NULL, -6.5) == AU_OK);
    CHECK_STR(au_fb_peek(fb), "volume=-6.5dB");
    CHECK(au_fb_add_volume_db(fb, NULL, 99) == AU_ERR_INVALID_ARGUMENT);
    CHECK_STR(au_fb_peek(fb), "volume=-6.5dB");   // unchanged on error
    CHECK(strlen(au_fb_last_error(fb)) > 0);
    au_fb_clear(fb);
    CHECK(au_fb_add_volume_db(fb, NULL, 0.91515) == AU_OK);
    CHECK_STR(au_fb_peek(fb), "volume=0.91515dB");
    CHECK(au_validate_filters(au_fb_peek(fb)) == AU_OK);
    au_fb_free(fb);
}

static void test_equalizer(void) {
    AuFilterBuilder *fb = au_fb_create();
    AuEqualizer eq = au_equalizer_defaults();

    // All bands at 0 dB adds nothing
    CHECK(au_fb_add_equalizer(fb, NULL, &eq) == AU_OK);
    CHECK(au_fb_build(fb) == NULL);

    eq.gain_db[0] = 2.0;
    eq.gain_db[5] = -1.5;
    CHECK(au_fb_add_equalizer(fb, NULL, &eq) == AU_OK);   // a biquad per band: any channel count
    CHECK_STR(au_fb_peek(fb), "equalizer=f=31.25:t=h:w=22.1:g=2:r=f64,equalizer=f=1000:t=h:w=707.1:g=-1.5:r=f64");
    CHECK(au_validate_filters(au_fb_peek(fb)) == AU_OK);
    au_fb_clear(fb);

    eq.preamp_db = -2.0;
    CHECK(au_fb_add_equalizer(fb, NULL, &eq) == AU_OK);
    CHECK_STR(au_fb_peek(fb), "volume=-2dB," "equalizer=f=31.25:t=h:w=22.1:g=2:r=f64,equalizer=f=1000:t=h:w=707.1:g=-1.5:r=f64");
    CHECK(au_validate_filters(au_fb_peek(fb)) == AU_OK);

    eq.gain_db[3] = 45;
    CHECK(au_fb_add_equalizer(fb, NULL, &eq) == AU_ERR_INVALID_ARGUMENT);
    au_fb_free(fb);
}

static void test_headphones(void) {
    AuFilterBuilder *fb = au_fb_create();
    CHECK(au_fb_add_crossfeed(fb, NULL, NULL) == AU_OK);
    CHECK_STR(au_fb_peek(fb), "crossfeed=strength=0.2:range=0.5:slope=0.5:level_in=0.9:level_out=1");
    CHECK(au_validate_filters(au_fb_peek(fb)) == AU_OK);

    AuCrossfeed bad = au_crossfeed_defaults();
    bad.slope = 0;
    CHECK(au_fb_add_crossfeed(fb, NULL, &bad) == AU_ERR_INVALID_ARGUMENT);

    CHECK(au_fb_add_bs2b(fb, NULL, 100, 45) == AU_ERR_INVALID_ARGUMENT); // range checks come first
    if (!au_filter_available("bs2b")) {
        // FFmpeg built without libbs2b (e.g. MSYS2's): the helpers say so and add nothing
        printf("   (no bs2b in this FFmpeg build)\n");
        CHECK(au_fb_add_bs2b_profile(fb, NULL, AU_BS2B_JAN_MEIER) == AU_ERR_UNSUPPORTED);
        CHECK(au_fb_add_bs2b(fb, NULL, 700, 45) == AU_ERR_UNSUPPORTED);
        CHECK(strstr(au_fb_last_error(fb), "bs2b") != NULL);
        CHECK_STR(au_fb_peek(fb), "crossfeed=strength=0.2:range=0.5:slope=0.5:level_in=0.9:level_out=1");
        au_fb_free(fb);
        return;
    }
    CHECK(au_fb_add_bs2b_profile(fb, NULL, AU_BS2B_JAN_MEIER) == AU_OK);
    CHECK(au_fb_add_bs2b(fb, NULL, 700, 45) == AU_OK);
    CHECK_STR(au_fb_peek(fb), "crossfeed=strength=0.2:range=0.5:slope=0.5:level_in=0.9:level_out=1,"
                              "bs2b=profile=jmeier,bs2b=fcut=700:feed=45");
    CHECK(au_validate_filters(au_fb_peek(fb)) == AU_OK);
    au_fb_free(fb);
}

static void test_locale_independent(void) {
    const char *locales[] = { "de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "nl_NL.UTF-8" };
    const char *set = NULL;
    for (size_t i = 0; i < sizeof(locales) / sizeof(locales[0]) && !set; i++)
        set = setlocale(LC_NUMERIC, locales[i]);
    if (!set) {
        printf("   (no comma-decimal locale installed, skipped)\n");
        return;
    }
    AuFilterBuilder *fb = au_fb_create();
    CHECK(au_fb_add_volume_db(fb, NULL, -6.5) == AU_OK);
    CHECK_STR(au_fb_peek(fb), "volume=-6.5dB");
    CHECK(au_validate_filters(au_fb_peek(fb)) == AU_OK);   // FFmpeg reads it too (strtod: locale-dependent)
    CHECK(au_validate_filters("volume=0.5,atempo=1.25") == AU_OK);
    au_fb_free(fb);
    setlocale(LC_NUMERIC, "C");
}

static void test_raw_and_validate(void) {
    AuFilterBuilder *fb = au_fb_create();
    CHECK(au_fb_add(fb, "atempo=1.25") == AU_OK);
    CHECK(au_fb_add(fb, "") == AU_ERR_INVALID_ARGUMENT);
    char *s = au_fb_build(fb);
    CHECK_STR(s, "atempo=1.25");
    CHECK(au_validate_filters(s) == AU_OK);
    au_free(s);
    au_fb_free(fb);

    CHECK(au_validate_filters("no_such_filter") == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_validate_filters(NULL) == AU_OK);
    CHECK(au_validate_filters("") == AU_OK);
}

static void test_availability(void) {
    CHECK(au_filter_available("anequalizer"));
    CHECK(au_filter_available("stereotools"));
    CHECK(!au_filter_available("no_such_filter"));
    CHECK(!au_filter_available(NULL));
    CHECK(!au_filter_available(""));
    printf("   bs2b %s in this FFmpeg build\n", au_filter_available("bs2b") ? "is" : "is not");
    AuFilterBuilder *fb = au_fb_create();
    const int r = au_fb_add_bs2b_profile(fb, NULL, AU_BS2B_DEFAULT);
    CHECK(r == (au_filter_available("bs2b") ? AU_OK : AU_ERR_UNSUPPORTED));
    au_fb_free(fb);
}

static void test_names(void) {
    AuFilterBuilder *fb = au_fb_create();
    CHECK(au_fb_add_volume_db(fb, "gain", -3) == AU_OK);
    CHECK(au_fb_add_volume_db(fb, NULL, -1) == AU_OK);   // a name belongs to one call
    CHECK_STR(au_fb_peek(fb), "volume@gain=-3dB,volume=-1dB");
    CHECK(au_validate_filters(au_fb_peek(fb)) == AU_OK);

    CHECK(au_fb_add_volume_db(fb, "bad name", -3) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_fb_add_volume_db(fb, "a,b", -3) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_fb_add_volume_db(fb, "this_name_is_much_longer_than_32_chars", -3) == AU_ERR_INVALID_ARGUMENT);
    CHECK_STR(au_fb_peek(fb), "volume@gain=-3dB,volume=-1dB");   // unchanged
    CHECK(au_fb_add_volume_db(fb, "", 2) == AU_OK);              // "" = no name
    CHECK_STR(au_fb_peek(fb), "volume@gain=-3dB,volume=-1dB,volume=2dB");

    // A failed add leaves no name behind
    au_fb_clear(fb);
    CHECK(au_fb_add_volume_db(fb, "x", 99) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_fb_add_volume_db(fb, NULL, 1) == AU_OK);
    CHECK_STR(au_fb_peek(fb), "volume=1dB");

    // Named: 0 dB bands are kept so they can be changed later, each under its band's index
    au_fb_clear(fb);
    AuEqualizer eq = au_equalizer_defaults();
    CHECK(au_fb_add_equalizer(fb, "geq", &eq) == AU_OK);
    const char *named_eq = "equalizer@geq_0=f=31.25:t=h:w=22.1:g=0:r=f64,equalizer@geq_1=f=62.5:t=h:w=44.2:g=0:r=f64";
    CHECK(strncmp(au_fb_peek(fb), named_eq, strlen(named_eq)) == 0);
    CHECK(strstr(au_fb_peek(fb), "equalizer@geq_9=f=16000:t=h:w=11313.7:g=0:r=f64") != NULL);
    CHECK(au_validate_filters(au_fb_peek(fb)) == AU_OK);
    au_fb_free(fb);
}

static void test_parametric_eq(void) {
    AuFilterBuilder *fb = au_fb_create();
    AuParametricEq eq = { .preamp_db = -6.4, .band_count = 4 };
    eq.bands[0] = (AuPeqBand){ AU_PEQ_LOW_SHELF, 105, 0.7, 5.6 };
    eq.bands[1] = (AuPeqBand){ AU_PEQ_PEAK, 1960, 1.85, -3.1 };
    eq.bands[2] = (AuPeqBand){ AU_PEQ_PEAK, 3000, 2, 0 };          // 0 dB: left out
    eq.bands[3] = (AuPeqBand){ AU_PEQ_HIGH_PASS, 20, 0.7071, 99 }; // gain not used
    CHECK(au_fb_add_parametric_eq(fb, NULL, &eq) == AU_OK);
    CHECK_STR(au_fb_peek(fb), "volume=-6.4dB,lowshelf=f=105:t=q:w=0.7:g=5.6:r=f64,"
                              "equalizer=f=1960:t=q:w=1.85:g=-3.1:r=f64,highpass=f=20:t=q:w=0.7071:r=f64");
    CHECK(au_validate_filters(au_fb_peek(fb)) == AU_OK);

    au_fb_clear(fb);
    CHECK(au_fb_add_parametric_eq(fb, "hp", &eq) == AU_OK);
    CHECK_STR(au_fb_peek(fb), "volume@hp=-6.4dB,lowshelf@hp_0=f=105:t=q:w=0.7:g=5.6:r=f64,"
                              "equalizer@hp_1=f=1960:t=q:w=1.85:g=-3.1:r=f64,equalizer@hp_2=f=3000:t=q:w=2:g=0:r=f64,"
                              "highpass@hp_3=f=20:t=q:w=0.7071:r=f64");
    CHECK(au_validate_filters(au_fb_peek(fb)) == AU_OK);

    const char *before = "volume@hp=-6.4dB";
    eq.bands[1].q = 0;
    CHECK(au_fb_add_parametric_eq(fb, NULL, &eq) == AU_ERR_INVALID_ARGUMENT);
    eq.bands[1].q = 1;
    eq.band_count = AU_PEQ_MAX_BANDS + 1;
    CHECK(au_fb_add_parametric_eq(fb, NULL, &eq) == AU_ERR_INVALID_ARGUMENT);
    CHECK(strncmp(au_fb_peek(fb), before, strlen(before)) == 0); // unchanged
    au_fb_free(fb);
}

static void test_parametric_eq_parse(void) {
    // An AutoEq ParametricEQ.txt with a BOM, CRLF line ends, a comment and the other forms EqualizerAPO accepts
    const char *text =
        "\xEF\xBB\xBFPreamp: -6.4 dB\r\n"
        "# AutoEq\r\n"
        "Filter 1: ON LSC Fc 105 Hz Gain 5.6 dB Q 0.70\r\n"
        "Filter 2: ON PK Fc 1960 Hz Gain -3.1 dB Q 1.85\r\n"
        "Filter 3: OFF PK Fc 3000 Hz Gain 9 dB Q 1\r\n"
        "\r\n"
        "Filter: ON PK Fc 1000 Hz Gain 2 dB BW Oct 1.0\r\n"
        "filter 5: on hsc fc 10000 hz gain -2 db\r\n"
        "Filter 6: ON HP Fc 20 Hz\r\n"
        "Preamp: -0.6 dB\r\n";
    AuParametricEq eq;
    char err[128];
    CHECK(au_parametric_eq_parse(text, &eq, err, sizeof(err)) == AU_OK);
    if (err[0]) printf("   unexpected error: %s\n", err);
    CHECK(fabs(eq.preamp_db - -7.0) < 1e-9);
    CHECK(eq.band_count == 5);
    CHECK(eq.bands[0].type == AU_PEQ_LOW_SHELF && eq.bands[0].frequency_hz == 105 && fabs(eq.bands[0].gain_db - 5.6) < 1e-9
          && fabs(eq.bands[0].q - 0.7) < 1e-9);
    CHECK(eq.bands[1].type == AU_PEQ_PEAK && fabs(eq.bands[1].gain_db - -3.1) < 1e-9 && fabs(eq.bands[1].q - 1.85) < 1e-9);
    CHECK(eq.bands[2].type == AU_PEQ_PEAK && fabs(eq.bands[2].q - 1.4142) < 1e-3);           // 1 octave
    CHECK(eq.bands[3].type == AU_PEQ_HIGH_SHELF && fabs(eq.bands[3].q - 0.7071) < 1e-9);     // default Q
    CHECK(eq.bands[4].type == AU_PEQ_HIGH_PASS && eq.bands[4].frequency_hz == 20);
    AuFilterBuilder *fb = au_fb_create();
    CHECK(au_fb_add_parametric_eq(fb, NULL, &eq) == AU_OK);
    CHECK(au_validate_filters(au_fb_peek(fb)) == AU_OK);
    au_fb_free(fb);

    // Errors name the line, and nothing half-parsed is returned
    struct { const char *text; int line; } bad[] = {
        { "Preamp: -3 dB\nChannel: L\nFilter 1: ON PK Fc 100 Hz Gain 3 dB Q 1\n", 2 },
        { "GraphicEQ: 20 -3; 30 -2\n", 1 },
        { "Filter 1: ON NO Fc 100 Hz\n", 1 },
        { "Filter 1: ON PK Gain 3 dB Q 1\n", 1 },           // no Fc
        { "Filter 1: ON PK Fc 100 Hz Q 1\n", 1 },            // no gain
        { "\n\nFilter 1: ON PK Fc 100 Hz Gain 45 dB Q 1\n", 3 },
        { "Filter 1: ON PK Fc 1,000 Hz Gain 1 dB Q 1\n", 1 }, // "1,000" is not a number
        { "Filter 1: MAYBE PK Fc 100 Hz Gain 1 dB\n", 1 },
        { "Preamp: loud\n", 1 },
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        AuParametricEq out = { .band_count = 7 };
        char msg[128];
        CHECK(au_parametric_eq_parse(bad[i].text, &out, msg, sizeof(msg)) == AU_ERR_INVALID_ARGUMENT);
        char want[16];
        snprintf(want, sizeof(want), "line %d:", bad[i].line);
        if (strncmp(msg, want, strlen(want)) != 0) {
            fprintf(stderr, "CHECK FAILED %s:%d: case %zu: \"%s\"\n", __FILE__, __LINE__, i, msg);
            tc_failures++;
        }
        CHECK(out.band_count == 7); // untouched
    }
    char many[AU_PEQ_MAX_BANDS * 48 + 64] = "";
    for (int i = 0; i <= AU_PEQ_MAX_BANDS; i++)
        snprintf(many + strlen(many), sizeof(many) - strlen(many), "Filter %d: ON PK Fc %d Hz Gain 1 dB Q 1\n", i + 1, 100 + i);
    CHECK(au_parametric_eq_parse(many, &eq, err, sizeof(err)) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_parametric_eq_parse(NULL, &eq, NULL, 0) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_parametric_eq_parse("", &eq, NULL, 0) == AU_OK && eq.band_count == 0);
}

static void test_tone_stereo_dynamics(void) {
    AuFilterBuilder *fb = au_fb_create();
    CHECK(au_fb_add_tone(fb, NULL, 0, 0) == AU_OK);
    CHECK_STR(au_fb_peek(fb), "");
    CHECK(au_fb_add_tone(fb, NULL, 4, -2.5) == AU_OK);
    CHECK_STR(au_fb_peek(fb), "bass=f=100:t=q:w=0.5:g=4:r=f64,treble=f=10000:t=q:w=0.5:g=-2.5:r=f64");
    CHECK(au_fb_add_tone(fb, NULL, 25, 0) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_validate_filters(au_fb_peek(fb)) == AU_OK);

    au_fb_clear(fb);
    AuStereo st = au_stereo_defaults();
    CHECK(au_fb_add_stereo(fb, NULL, &st) == AU_OK);
    CHECK_STR(au_fb_peek(fb), "");                   // nothing to do
    st.balance = -0.25;
    st.width = 1.5;
    CHECK(au_fb_add_stereo(fb, NULL, &st) == AU_OK);
    st = au_stereo_defaults();
    st.width = 0;
    st.swap = true;                                  // mono wins
    CHECK(au_fb_add_stereo(fb, NULL, &st) == AU_OK);
    st = au_stereo_defaults();
    st.swap = true;
    CHECK(au_fb_add_stereo(fb, "st", &st) == AU_OK);
    CHECK_STR(au_fb_peek(fb),
              "stereotools=mode=lr>lr:slev=1.5:balance_out=-0.25:bmode_out=balance,"
              "stereotools=mode=lr>l+r:slev=1:balance_out=0:bmode_out=balance,"
              "stereotools@st=mode=lr>rl:slev=1:balance_out=0:bmode_out=balance");
    CHECK(au_validate_filters(au_fb_peek(fb)) == AU_OK);
    st.balance = 2;
    CHECK(au_fb_add_stereo(fb, NULL, &st) == AU_ERR_INVALID_ARGUMENT);
    st.balance = 0;
    st.width = 5;
    CHECK(au_fb_add_stereo(fb, NULL, &st) == AU_ERR_INVALID_ARGUMENT);

    au_fb_clear(fb);
    CHECK(au_fb_add_limiter(fb, NULL, NULL) == AU_OK);
    CHECK_STR(au_fb_peek(fb), "alimiter=limit=0.891251:attack=5:release=50:level=0");
    AuLimiter lim = au_limiter_defaults();
    lim.ceiling_db = 1;
    CHECK(au_fb_add_limiter(fb, NULL, &lim) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_fb_add_compressor(fb, NULL, NULL) == AU_OK);
    const AuCompressor night = au_compressor_night();
    CHECK(au_fb_add_compressor(fb, NULL, &night) == AU_OK);
    CHECK_STR(au_fb_peek(fb), "alimiter=limit=0.891251:attack=5:release=50:level=0,"
                              "acompressor=threshold=0.125893:ratio=2:attack=20:release=250:knee=1.99526:makeup=1.41254,"
                              "acompressor=threshold=0.0251189:ratio=4:attack=10:release=300:knee=2.51189:makeup=2.81838");
    CHECK(au_validate_filters(au_fb_peek(fb)) == AU_OK);
    AuCompressor c = au_compressor_defaults();
    c.ratio = 0.5;
    CHECK(au_fb_add_compressor(fb, NULL, &c) == AU_ERR_INVALID_ARGUMENT);
    c = au_compressor_defaults();
    c.knee_db = 18;  // the most acompressor takes
    CHECK(au_fb_add_compressor(fb, NULL, &c) == AU_OK);
    CHECK(au_validate_filters(au_fb_peek(fb)) == AU_OK);
    au_fb_free(fb);
}

static void test_parse_locale_independent(void) {
    const char *locales[] = { "de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "nl_NL.UTF-8" };
    const char *set = NULL;
    for (size_t i = 0; i < sizeof(locales) / sizeof(locales[0]) && !set; i++)
        set = setlocale(LC_NUMERIC, locales[i]);
    if (!set) {
        printf("   (no comma-decimal locale installed, skipped)\n");
        return;
    }
    AuParametricEq eq;
    CHECK(au_parametric_eq_parse("Preamp: -6.4 dB\nFilter 1: ON PK Fc 1960 Hz Gain -3.1 dB Q 1.85\n", &eq, NULL, 0) == AU_OK);
    CHECK(fabs(eq.preamp_db - -6.4) < 1e-9 && fabs(eq.bands[0].q - 1.85) < 1e-9);
    AuFilterBuilder *fb = au_fb_create();
    au_fb_add_parametric_eq(fb, NULL, &eq);
    CHECK_STR(au_fb_peek(fb), "volume=-6.4dB,equalizer=f=1960:t=q:w=1.85:g=-3.1:r=f64");
    au_fb_free(fb);
    setlocale(LC_NUMERIC, "C");
}

int main(int argc, char **argv) {
    tc_setup(argc, argv);
    RUN_TEST(test_availability);
    RUN_TEST(test_names);
    RUN_TEST(test_parametric_eq);
    RUN_TEST(test_parametric_eq_parse);
    RUN_TEST(test_tone_stereo_dynamics);
    RUN_TEST(test_parse_locale_independent);
    RUN_TEST(test_empty_builder);
    RUN_TEST(test_gain);
    RUN_TEST(test_equalizer);
    RUN_TEST(test_headphones);
    RUN_TEST(test_locale_independent);
    RUN_TEST(test_raw_and_validate);
    return tc_finish("test_filters");
}
