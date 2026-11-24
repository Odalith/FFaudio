/*
 * Metadata tests: au_probe() without a player, tags, track/disc numbers, ReplayGain / R128 parsing,
 * and au_track_info() for a playing track.
 */
#include "test_common.h"
#include "../lib/ffaudio_loudness.h"
#include <math.h>
#include <libavutil/log.h>

#define CHECK_TAG(info, key, expected) do { \
    const char *v_ = au_media_tag((info), (key)); \
    if (!v_ || strcmp(v_, (expected)) != 0) { \
        fprintf(stderr, "CHECK FAILED %s:%d: tag %s = \"%s\", expected \"%s\"\n", __FILE__, __LINE__, \
                (key), v_ ? v_ : "(null)", (expected)); \
        tc_failures++; \
    } } while (0)

static void check_tagged(const char *file, const char *container, const char *codec, const int bits) {
    int err = 1;
    const AuMediaInfo *info = au_probe(fx(file), &err);
    REQUIRE(info);
    CHECK(err == AU_OK);
    printf("   %s: %s/%s %d Hz %d ch %d bit %.3f s %s, %d tags\n", file, info->container,
           info->codec, info->sample_rate, info->channels,
           info->bits_per_sample, info->duration, info->channel_layout,
           info->tag_count);
    CHECK(strcmp(info->container, container) == 0);
    CHECK(strcmp(info->codec, codec) == 0);
    CHECK(info->sample_rate == 48000);
    CHECK(info->channels == 2);
    CHECK(strcmp(info->channel_layout, "stereo") == 0);
    CHECK(info->bits_per_sample == bits);
    CHECK(fabs(info->duration - 1.0) < 0.06);

    CHECK_TAG(info, "title", "Test Song");
    CHECK_TAG(info, "TITLE", "Test Song");       // case-insensitive
    CHECK_TAG(info, "artist", "Test Artist");
    CHECK_TAG(info, "album", "Test Album");
    CHECK_TAG(info, "album_artist", "Various");
    CHECK_TAG(info, "albumartist", "Various");   // alias
    CHECK_TAG(info, "genre", "Rock");
    CHECK(au_media_tag(info, "date") && strncmp(au_media_tag(info, "date"), "2024", 4) == 0);
    CHECK(au_media_tag(info, "no_such_tag") == NULL);

    CHECK(info->track_number == 3 && info->track_total == 12);
    CHECK(info->disc_number == 1 && info->disc_total == 2);

    const AuReplayGain rg = info->replaygain;
    CHECK(!isnan(rg.track_gain_db) && fabs(rg.track_gain_db + 6.0) < 1e-6);
    CHECK(!isnan(rg.track_peak) && fabs(rg.track_peak - 0.125) < 1e-6);
    CHECK(!isnan(rg.album_gain_db) && fabs(rg.album_gain_db + 3.0) < 1e-6);
    CHECK(!isnan(rg.album_peak) && fabs(rg.album_peak - 0.2) < 1e-6);
    CHECK(!rg.from_r128);

    // Every tag is in the array
    int found_title = 0;
    for (int i = 0; i < info->tag_count; i++) {
        CHECK(info->tags[i].key && info->tags[i].value);
        if (strcmp(info->tags[i].value, "Test Song") == 0) found_title++;
    }
    CHECK(found_title == 1);
    au_media_info_release(info);
}

static void test_probe_flac(void) { check_tagged("rg_tagged.flac", "flac", "flac", 16); }

static void test_probe_mp3(void) { check_tagged("rg_tagged.mp3", "mp3", "mp3", 0); }

static void test_probe_r128(void) {
    const AuMediaInfo *info = au_probe(fx("rg_r128.opus"), NULL);
    REQUIRE(info);
    CHECK(strcmp(info->codec, "opus") == 0);
    CHECK_TAG(info, "title", "Opus Song");
    const AuReplayGain rg = info->replaygain;
    // -2816 / 256 = -11 dB relative to -23 LUFS = -6 dB relative to ReplayGain's -18 LUFS
    CHECK(!isnan(rg.track_gain_db) && fabs(rg.track_gain_db + 6.0) < 1e-6);
    CHECK(rg.from_r128);
    CHECK(isnan(rg.album_gain_db) && isnan(rg.track_peak));
    au_media_info_release(info);
}

// Chained Ogg files (parts one after another): FFmpeg reports only the last part's length (2 s, 1 s); ffaudio adds the
// parts up, for au_probe() and for the player
static void test_chained_ogg_duration(void) {
    const struct { const char *file; double seconds; } cases[] = { { "chained.ogg", 4.0 }, { "chained.opus", 3.0 } };
    for (int i = 0; i < 2; i++) {
        const AuMediaInfo *info = au_probe(fx(cases[i].file), NULL);
        REQUIRE(info);
        printf("   %s: %.3f s\n", cases[i].file, info->duration);
        CHECK(fabs(info->duration - cases[i].seconds) < 0.03);
        CHECK(!info->seekable);   // FFmpeg can not seek past the first part
        au_media_info_release(info);
    }
    const AuMediaInfo *single = au_probe(fx("chain_a.ogg"), NULL);   // one part: FFmpeg's value, unchanged
    REQUIRE(single);
    CHECK(fabs(single->duration - 2.0) < 0.03 && single->seekable);
    au_media_info_release(single);

    REQUIRE(tc_init_player(0) == 0);
    au_play(fx("chained.ogg"), NULL);
    CHECK(TC_WAIT_FIELD(opened, 1, 5000));
    CHECK(fabs(tc_snapshot().opened_duration - 4.0) < 0.03);
    au_seek(3.0);                                 // ignored, like on a live stream
    tc_sleep_ms(500);
    CHECK(tc_snapshot().restart == 0);
    au_shutdown();
}

static void test_probe_untagged_and_bad(void) {
    const AuMediaInfo *info = au_probe(fx("rg_untagged.flac"), NULL);
    REQUIRE(info);
    const AuReplayGain rg = info->replaygain;
    CHECK(isnan(rg.track_gain_db) && isnan(rg.album_gain_db) && isnan(rg.track_peak) && isnan(rg.album_peak));
    CHECK(info->track_number == 0 && info->track_total == 0);
    au_media_info_release(info);

    int err = 0;
    CHECK(au_probe(fx("text.mp3"), &err) == NULL && err != AU_OK);
    CHECK(au_probe(fx("video_only.mkv"), &err) == NULL && err != AU_OK);
    CHECK(au_probe(fx("does_not_exist.flac"), &err) == NULL && err != AU_OK);
    CHECK(au_probe(NULL, &err) == NULL && err == AU_ERR_INVALID_ARGUMENT);

    // NULL info is harmless everywhere
    CHECK(au_media_tag(NULL, "title") == NULL);
    au_media_info_release(NULL);
}

static void test_track_info_while_playing(void) {
    REQUIRE(tc_init_player(0) == 0);
    CHECK(au_track_info(0) == NULL); // nothing playing
    const int32_t h = au_play(fx("rg_long.flac"), NULL);
    CHECK(TC_WAIT_FIELD(opened, 1, 5000));               // no waiting call: the event says when
    TcEvents e = tc_snapshot();
    CHECK(e.opened_with_info == 1 && fabs(e.opened_duration - 3.0) < 0.06);
    CHECK(TC_WAIT_FIELD(started, 1, 5000));              // handle 0 is the audible track

    const AuMediaInfo *info = au_track_info(0);
    CHECK(info != NULL);
    if (info) {
        CHECK_TAG(info, "title", "Test Song");
        CHECK(fabs(info->duration - 3.0) < 0.06);
        au_media_info_release(info);
    }
    info = au_track_info(h);
    CHECK(info != NULL);
    const AuMediaInfo *same = au_track_info(h);   // the player's own block, shared: no copy
    CHECK(same == info);
    au_media_info_release(same);
    const AuMediaInfo *kept = au_track_info(h);   // outlives the player (checked after au_shutdown())
    au_media_info_release(info);
    CHECK(au_track_info(h + 1000) == NULL);

    double gain = 99;
    CHECK(au_track_gain_db(h, &gain) == AU_OK && gain == 0.0); // ReplayGain is off by default
    const AuReplayGainConfig rg = { .mode = AU_REPLAYGAIN_TRACK };
    CHECK(au_set_replaygain(&rg) == AU_OK);
    tc_sleep_ms(100);
    CHECK(au_track_gain_db(0, &gain) == AU_OK && fabs(gain + 6.0) < 0.01);
    CHECK(au_track_gain_db(h + 1000, &gain) == AU_ERR_NOTHING_TO_DO);

    const AuReplayGainConfig bad = { .mode = AU_REPLAYGAIN_TRACK, .preamp_db = 40 };
    CHECK(au_set_replaygain(&bad) == AU_ERR_INVALID_ARGUMENT);

    au_stop();
    CHECK(TC_WAIT_FIELD(eof, 1, 3000));
    CHECK(au_track_info(h) == NULL);                     // gone with the track
    CHECK(au_track_gain_db(h, &gain) == AU_ERR_NOTHING_TO_DO);
    au_shutdown();
    CHECK(au_track_info(0) == NULL && au_track_gain_db(0, &gain) == AU_ERR_NOT_INITIALIZED);
    CHECK(kept && strstr(kept->path, "rg_long.flac") && kept->tag_count > 0);
    CHECK_TAG(kept, "title", "Test Song");
    au_media_info_release(kept);
}

/* ---------- non-audio data must fail without running video decoders ---------- */

// Messages of FFmpeg decoders (warning and worse), from any thread
static SDL_atomic_t decoder_messages;

static void watch_ffmpeg_log(void *ptr, int level, const char *fmt, va_list vl) {
    if (ptr && level <= AV_LOG_WARNING) {
        const AVClass *avc = *(const AVClass **)ptr;
        const AVClassCategory cat = avc && avc->get_category ? avc->get_category(ptr) : avc ? avc->category : 0;
        if (cat == AV_CLASS_CATEGORY_DECODER) SDL_AtomicIncRef(&decoder_messages);
    }
    av_log_default_callback(ptr, level, fmt, vl);
}

static void test_non_audio_opens_no_decoder(void) {
    // not_audio.bin is damaged JPEG data, detected as a raw video stream like encrypted cache files often are.
    // Probing it used to open the video decoder, which printed warnings ("[vvc] SPS id 9 not available", ...).
    SDL_AtomicSet(&decoder_messages, 0);
    av_log_set_callback(watch_ffmpeg_log);
    av_log_set_level(AV_LOG_WARNING);

    int err = 0;
    CHECK(au_probe(fx("not_audio.bin"), &err) == NULL && err == AU_ERR_FILE);
    AuLoudness l;
    CHECK(au_loudness_analyze(fx("not_audio.bin"), NULL, &l) == AU_ERR_FILE);

    REQUIRE(tc_init_player(0) == 0);
    av_log_set_level(AV_LOG_WARNING); // au_initialize() sets AV_LOG_ERROR
    au_play(fx("not_audio.bin"), NULL);
    CHECK(TC_WAIT_FIELD(eof, 1, 3000));
    CHECK(tc_snapshot().eof_error == 1);
    au_shutdown();

    printf("   decoder messages: %d\n", SDL_AtomicGet(&decoder_messages));
    CHECK(SDL_AtomicGet(&decoder_messages) == 0);
    av_log_set_callback(av_log_default_callback);
    av_log_set_level(AV_LOG_ERROR);
}

int main(int argc, char **argv) {
    tc_setup(argc, argv);
    RUN_TEST(test_probe_flac);
    RUN_TEST(test_probe_mp3);
    RUN_TEST(test_probe_r128);
    RUN_TEST(test_probe_untagged_and_bad);
    RUN_TEST(test_chained_ogg_duration);
    RUN_TEST(test_track_info_while_playing);
    RUN_TEST(test_non_audio_opens_no_decoder);
    return tc_finish("test_metadata");
}
