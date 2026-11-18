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
    CHECK(au_fb_add_volume_db(fb, -6.5) == AU_OK);
    CHECK_STR(au_fb_peek(fb), "volume=-6.5dB");
    CHECK(au_fb_add_volume_db(fb, 99) == AU_ERR_INVALID_ARGUMENT);
    CHECK_STR(au_fb_peek(fb), "volume=-6.5dB");   // unchanged on error
    CHECK(strlen(au_fb_last_error(fb)) > 0);
    au_fb_clear(fb);

    // ReplayGain +4 dB with a 0.9 peak and clipping prevention: limited to -20*log10(0.9) = 0.915 dB
    CHECK(au_fb_add_replaygain(fb, 4.0, 0.9, 0.0, true) == AU_OK);
    CHECK_STR(au_fb_peek(fb), "volume=0.91515dB");
    au_fb_clear(fb);
    CHECK(au_fb_add_replaygain(fb, -7.0, 0.9, 2.0, true) == AU_OK);
    CHECK_STR(au_fb_peek(fb), "volume=-5dB");
    au_fb_free(fb);
}

static void test_equalizer(void) {
    AuFilterBuilder *fb = au_fb_create();
    AuEqualizer eq = au_equalizer_defaults();

    // All bands at 0 dB adds nothing
    CHECK(au_fb_add_equalizer(fb, 2, &eq) == AU_OK);
    CHECK(au_fb_build(fb) == NULL);

    eq.gain_db[0] = 2.0;
    eq.gain_db[5] = -1.5;
    CHECK(au_fb_add_equalizer(fb, 2, &eq) == AU_OK);
    CHECK_STR(au_fb_peek(fb),
              "anequalizer=c0 f=31.25 w=22.1 g=2|c0 f=1000 w=707.1 g=-1.5|c1 f=31.25 w=22.1 g=2|c1 f=1000 w=707.1 g=-1.5");
    CHECK(au_validate_filtergraph(au_fb_peek(fb)) == AU_OK);
    au_fb_clear(fb);

    eq.preamp_db = -2.0;
    CHECK(au_fb_add_equalizer(fb, 1, &eq) == AU_OK);
    CHECK_STR(au_fb_peek(fb), "volume=-2dB,anequalizer=c0 f=31.25 w=22.1 g=2|c0 f=1000 w=707.1 g=-1.5");
    CHECK(au_validate_filtergraph(au_fb_peek(fb)) == AU_OK);

    eq.gain_db[3] = 45;
    CHECK(au_fb_add_equalizer(fb, 2, &eq) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_fb_add_equalizer(fb, 0, &eq) == AU_ERR_INVALID_ARGUMENT);
    au_fb_free(fb);
}

static void test_headphones(void) {
    AuFilterBuilder *fb = au_fb_create();
    CHECK(au_fb_add_crossfeed(fb, NULL) == AU_OK);
    CHECK_STR(au_fb_peek(fb), "crossfeed=strength=0.2:range=0.5:slope=0.5:level_in=0.9:level_out=1");
    CHECK(au_validate_filtergraph(au_fb_peek(fb)) == AU_OK);

    AuCrossfeed bad = au_crossfeed_defaults();
    bad.slope = 0;
    CHECK(au_fb_add_crossfeed(fb, &bad) == AU_ERR_INVALID_ARGUMENT);

    CHECK(au_fb_add_bs2b_profile(fb, AU_BS2B_JAN_MEIER) == AU_OK);
    CHECK(au_fb_add_bs2b(fb, 700, 45) == AU_OK);
    CHECK(au_fb_add_bs2b(fb, 100, 45) == AU_ERR_INVALID_ARGUMENT);
    CHECK_STR(au_fb_peek(fb), "crossfeed=strength=0.2:range=0.5:slope=0.5:level_in=0.9:level_out=1,"
                              "bs2b=profile=jmeier,bs2b=fcut=700:feed=45");
    CHECK(au_validate_filtergraph(au_fb_peek(fb)) == AU_OK);
    au_fb_free(fb);
}

static void test_loudnorm(void) {
    AuFilterBuilder *fb = au_fb_create();
    AuLoudnorm l = au_loudnorm_defaults();
    l.target_i = -16;
    l.target_lra = 11;
    l.target_tp = -1.5;
    l.measured_i = -20.5;
    l.measured_lra = 5.2;
    l.measured_tp = -6.1;
    l.measured_thresh = -31.0;
    l.offset = -0.5;
    // gain = -16 - -20.5 + -0.5 = 4 dB; peak -6.1 + 4.5 = -1.6 <= -1.5
    CHECK(au_fb_add_loudnorm_linear(fb, &l) == AU_OK);
    CHECK_STR(au_fb_peek(fb), "volume=4dB");

    // Would push the true peak above the target: refused, like loudnorm's linear mode
    l.measured_tp = -2.0;
    CHECK(au_fb_add_loudnorm_linear(fb, &l) == AU_ERR_INVALID_ARGUMENT);
    // Not measured at all
    AuLoudnorm none = au_loudnorm_defaults();
    CHECK(au_fb_add_loudnorm_linear(fb, &none) == AU_ERR_INVALID_ARGUMENT);
    CHECK_STR(au_fb_peek(fb), "volume=4dB");

    CHECK(au_fb_add_loudnorm_dynamic(fb, -16, 11, -1.5) == AU_OK);
    CHECK_STR(au_fb_peek(fb), "volume=4dB,loudnorm=I=-16:LRA=11:TP=-1.5");
    CHECK(au_validate_filtergraph(au_fb_peek(fb)) == AU_OK);
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
    CHECK(au_fb_add_volume_db(fb, -6.5) == AU_OK);
    CHECK_STR(au_fb_peek(fb), "volume=-6.5dB");
    au_fb_free(fb);
    setlocale(LC_NUMERIC, "C");
}

static void test_raw_and_validate(void) {
    AuFilterBuilder *fb = au_fb_create();
    CHECK(au_fb_add(fb, "atempo=1.25") == AU_OK);
    CHECK(au_fb_add(fb, "") == AU_ERR_INVALID_ARGUMENT);
    char *s = au_fb_build(fb);
    CHECK_STR(s, "atempo=1.25");
    CHECK(au_validate_filtergraph(s) == AU_OK);
    au_free(s);
    au_fb_free(fb);

    CHECK(au_validate_filtergraph("no_such_filter") == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_validate_filtergraph(NULL) == AU_OK);
    CHECK(au_validate_filtergraph("") == AU_OK);
}

int main(int argc, char **argv) {
    tc_setup(argc, argv);
    RUN_TEST(test_empty_builder);
    RUN_TEST(test_gain);
    RUN_TEST(test_equalizer);
    RUN_TEST(test_headphones);
    RUN_TEST(test_loudnorm);
    RUN_TEST(test_locale_independent);
    RUN_TEST(test_raw_and_validate);
    return tc_finish("test_filters");
}
