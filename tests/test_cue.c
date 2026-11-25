/*
 * Cue sheets and chapters (ffaudio_playlist.h): parsing, the sources au_cue_read() reads, and the playlist reader
 * refusing cue sheets.
 */
#include <math.h>
#include "test_common.h"
#include "../lib/ffaudio_playlist.h"

#define CHECK_STR(a, b) do { \
    const char *a_ = (a), *b_ = (b); \
    if (!a_ || strcmp(a_, b_) != 0) { \
        fprintf(stderr, "CHECK FAILED %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, a_ ? a_ : "(null)", b_); \
        tc_failures++; \
    } } while (0)
#define CHECK_NEAR(a, b) CHECK(fabs((a) - (b)) < 1e-9)

static bool file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static void test_parse_sheet(void) {
    const char *text =
        "REM GENRE \"Progressive Rock\"\r\n"
        "REM DATE 1973\r\n"
        "REM REPLAYGAIN_ALBUM_GAIN -6.20 dB\r\n"
        "REM REPLAYGAIN_ALBUM_PEAK 0.988\r\n"
        "PERFORMER \"The Band\"\r\n"
        "TITLE \"Live, Somewhere\"\r\n"
        "FILE \"disc one.flac\" WAVE\r\n"
        "  TRACK 01 AUDIO\r\n"
        "    TITLE Opening Words\r\n"           // unquoted, several words
        "    ISRC USABC7300001\r\n"
        "    REM REPLAYGAIN_TRACK_GAIN -7.5 dB\r\n"
        "    REM REPLAYGAIN_TRACK_PEAK 0.9\r\n"
        "    INDEX 01 00:00:00\r\n"
        "  TRACK 02 AUDIO\r\n"
        "    TITLE \"Caf\xe9\"\r\n"              // Latin-1
        "    PERFORMER \"Guest\"\r\n"
        "    SONGWRITER \"Writer\"\r\n"
        "    PREGAP 00:02:00\r\n"
        "    INDEX 00 00:01:50\r\n"
        "    INDEX 01 00:02:00\r\n"
        "  TRACK 03 MODE1/2352\r\n"            // data: skipped
        "    INDEX 01 00:05:00\r\n"
        "FILE \"/abs/disc two.wav\" WAVE\r\n"
        "  TRACK 04 AUDIO\r\n"
        "    INDEX 01 00:00:00\r\n"
        "  TRACK 05 AUDIO\r\n"
        "    INDEX 01 61:00:74\r\n";           // minutes past 99 would do too
    int err = 1;
    const AuCueSheet *cue = au_cue_parse(text, strlen(text), "/music/live/album.cue", &err);
    REQUIRE(cue);
    CHECK(err == AU_OK && cue->count == 4);
    CHECK_STR(cue->title, "Live, Somewhere");
    CHECK_STR(cue->performer, "The Band");
    CHECK_STR(cue->genre, "Progressive Rock");
    CHECK_STR(cue->date, "1973");
    const AuCueTrack *t1 = cue->tracks[0], *t2 = cue->tracks[1], *t4 = cue->tracks[2], *t5 = cue->tracks[3];
    CHECK(t1->number == 1 && t2->number == 2 && t4->number == 4 && t5->number == 5);
    CHECK_STR(t1->path, "/music/live/disc one.flac");
    CHECK_STR(t1->title, "Opening Words");
    CHECK_STR(t1->performer, "The Band");             // the sheet's
    CHECK_STR(t1->isrc, "USABC7300001");
    CHECK(fabsf(t1->replaygain.track_gain_db + 7.5f) < 1e-6f && fabsf(t1->replaygain.track_peak - 0.9f) < 1e-6f);
    CHECK(fabsf(t1->replaygain.album_gain_db + 6.2f) < 1e-6f && fabsf(t1->replaygain.album_peak - 0.988f) < 1e-6f);
    CHECK(isnan(t2->replaygain.track_gain_db) && fabsf(t2->replaygain.album_gain_db + 6.2f) < 1e-6f);
    CHECK_NEAR(t1->start_seconds, 0);
    CHECK_NEAR(t1->length_seconds, 2.0);              // to track 2's INDEX 01: the pregap ends track 1
    CHECK_STR(t2->title, "Caf\xc3\xa9");
    CHECK_STR(t2->performer, "Guest");
    CHECK_STR(t2->songwriter, "Writer");
    CHECK_NEAR(t2->start_seconds, 2.0);
    CHECK_NEAR(t2->length_seconds, 0);                // the last of its file: to the end
    CHECK_STR(t4->path, "/abs/disc two.wav");
    CHECK_NEAR(t4->start_seconds, 0);
    CHECK_NEAR(t4->length_seconds, 3660 + 74 / 75.0);
    CHECK_NEAR(t5->start_seconds, 3660 + 74 / 75.0);
    CHECK_NEAR(t5->length_seconds, 0);
    au_cue_release(cue);
}

static void test_not_a_sheet(void) {
    int err = 0;
    CHECK(au_cue_parse("a.mp3\nb.mp3\n", 12, NULL, &err) == NULL && err == AU_ERR_FILE);
    CHECK(au_cue_parse(NULL, 4, NULL, &err) == NULL && err == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_cue_read(NULL, &err) == NULL && err == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_cue_read(fx("no_such.cue"), &err) == NULL && err == AU_ERR_FILE);
    CHECK(au_cue_read(fx("no_such.flac"), &err) == NULL && err == AU_ERR_FILE);
    CHECK(au_cue_read(fx("s44_stereo.flac"), &err) == NULL && err == AU_ERR_NOTHING_TO_DO);
    au_cue_release(NULL);

    // A cue sheet is not a playlist: its lines are not paths
    const char *cue = "FILE \"x.wav\" WAVE\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n";
    CHECK(au_playlist_parse(cue, strlen(cue), "/m/x.cue", &err) == NULL && err == AU_ERR_UNSUPPORTED);
}

// A sheet for a .wav that was encoded to .flac afterwards
static void test_read_file_and_other_extension(void) {
    char path[1200];
    snprintf(path, sizeof(path), "%s/counter.cue", tc_fixture_dir);
    FILE *f = fopen(path, "wb");
    REQUIRE(f);
    fputs("FILE \"counter_48k.ape\" WAVE\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n"
          "  TRACK 02 AUDIO\n    INDEX 01 00:01:10\n", f);
    fclose(f);
    int err = 1;
    const AuCueSheet *cue = au_cue_read(path, &err);
    REQUIRE(cue);
    CHECK(err == AU_OK && cue->count == 2);
    CHECK_STR(cue->tracks[0]->path, fx("counter_48k.flac"));   // .ape is not there; .flac is
    CHECK_NEAR(cue->tracks[0]->length_seconds, 1 + 10 / 75.0);
    au_cue_release(cue);
    remove(path);
}

static void test_embedded(void) {
    if (!file_exists(fx("cue_tag.flac"))) {
        printf("   skipped: no metaflac to make cue_tag.flac / cue_block.flac\n");
        return;
    }
    int err = 1;
    // The CUESHEET tag: its FILE names the original image; the tracks are in this file
    const AuCueSheet *cue = au_cue_read(fx("cue_tag.flac"), &err);
    REQUIRE(cue);
    CHECK(err == AU_OK && cue->count == 3);
    CHECK_STR(cue->title, "Album");
    CHECK_STR(cue->tracks[1]->path, fx("cue_tag.flac"));
    CHECK_STR(cue->tracks[1]->title, "Two");
    CHECK_NEAR(cue->tracks[1]->start_seconds, 2.0);
    CHECK_NEAR(cue->tracks[0]->length_seconds, 2.0);
    CHECK_NEAR(cue->tracks[2]->start_seconds, 4 + 37 / 75.0);
    CHECK_NEAR(cue->tracks[2]->length_seconds, 0);
    au_cue_release(cue);

    // The CUESHEET block: tracks start at INDEX 01 (FFmpeg's chapters would start track 2 at its pregap, 1.667 s)
    cue = au_cue_read(fx("cue_block.flac"), &err);
    REQUIRE(cue);
    CHECK(err == AU_OK && cue->count == 3);
    printf("   CUESHEET block: %.4f %.4f %.4f\n", cue->tracks[0]->start_seconds, cue->tracks[1]->start_seconds,
           cue->tracks[2]->start_seconds);
    CHECK(cue->tracks[1]->number == 2);
    CHECK_NEAR(cue->tracks[1]->start_seconds, 2.0);
    CHECK_NEAR(cue->tracks[2]->start_seconds, 4 + 37 / 75.0);
    CHECK_NEAR(cue->tracks[0]->length_seconds, 2.0);
    au_cue_release(cue);
}

static void test_chapters(void) {
    const char *files[] = { "chapters.m4a", "chapters.mka", "chapters.mp3" };
    for (int i = 0; i < 3; i++) {
        int err = 1;
        const AuCueSheet *cue = au_cue_read(fx(files[i]), &err);
        REQUIRE(cue);
        printf("   %s: %d chapters at %.3f %.3f %.3f, lengths %.3f %.3f %.3f, \"%s\" by \"%s\"\n", files[i], cue->count,
               cue->tracks[0]->start_seconds, cue->tracks[1]->start_seconds, cue->tracks[2]->start_seconds,
               cue->tracks[0]->length_seconds, cue->tracks[1]->length_seconds, cue->tracks[2]->length_seconds,
               cue->title, cue->performer);
        CHECK(err == AU_OK && cue->count == 3);
        CHECK_STR(cue->tracks[1]->title, "Chapter One");
        CHECK_STR(cue->title, "Book");
        CHECK_STR(cue->tracks[0]->performer, "Reader");
        CHECK(fabs(cue->tracks[1]->start_seconds - 2.0) < 0.03 && fabs(cue->tracks[2]->start_seconds - 4.5) < 0.03);
        CHECK(fabs(cue->tracks[0]->length_seconds - 2.0) < 0.03);
        CHECK(cue->tracks[2]->length_seconds == 0);         // the last one: to the end
        CHECK(cue->tracks[1]->number == 2);
        au_cue_release(cue);
    }
}

int main(int argc, char **argv) {
    tc_setup(argc, argv);
    RUN_TEST(test_parse_sheet);
    RUN_TEST(test_not_a_sheet);
    RUN_TEST(test_read_file_and_other_extension);
    RUN_TEST(test_embedded);
    RUN_TEST(test_chapters);
    return tc_finish("test_cue");
}
