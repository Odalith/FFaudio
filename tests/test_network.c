/*
 * Network streams against a local HTTP server (tests/http_server.h): playback, seeking with range requests,
 * probing, errors and timeouts, headers and user agent, redirects, the protocol whitelists (also through HLS
 * playlists), and that network files are not measured unless asked to.
 */
#include "test_common.h"
#include "http_server.h"
#include "../lib/ffaudio_loudness.h"
#ifndef _WIN32
#include <limits.h>
#endif

static TsServer srv;

// http://127.0.0.1:<port><path>, in a ring of buffers like fx()
static const char *url(const char *path) {
    static char bufs[8][700];
    static int next;
    char *b = bufs[next++ % 8];
    snprintf(b, sizeof(bufs[0]), "http://127.0.0.1:%d%s", srv.port, path);
    return b;
}

static void write_file(const char *name, const char *text) {
    FILE *f = fopen(fx(name), "wb");
    REQUIRE(f);
    fputs(text, f);
    fclose(f);
}

// file:///<absolute path of a fixture>
static void file_url(const char *name, char *out, const size_t size) {
    char abs[1100];
#ifdef _WIN32
    if (!_fullpath(abs, fx(name), sizeof(abs))) snprintf(abs, sizeof(abs), "%s", fx(name));
    for (char *p = abs; *p; p++) if (*p == '\\') *p = '/';
    snprintf(out, size, "file:///%s", abs);
#else
    if (!realpath(fx(name), abs)) snprintf(abs, sizeof(abs), "%s", fx(name));
    snprintf(out, size, "file://%s", abs);
#endif
}

static void play_and_wait(const char *what, const PlayAudioConfig *cfg, const int eofs, const uint32_t ms) {
    au_play_audio(what, cfg);
    if (!TC_WAIT_FIELD(eof, eofs, ms)) {
        fprintf(stderr, "no end of %s within %u ms\n", what, ms);
        CHECK(false);
    }
}

static void test_url_detection(void) {
    CHECK(au_is_network_url("http://example.com/a.mp3"));
    CHECK(au_is_network_url("HTTPS://example.com/a.flac"));
    CHECK(au_is_network_url("crypto+http://example.com/k"));
    CHECK(!au_is_network_url("file:///music/a.flac"));
    CHECK(!au_is_network_url("/music/a.flac"));
    CHECK(!au_is_network_url("C://music/a.flac"));   // a Windows drive, not a scheme
    CHECK(!au_is_network_url("C:\\music\\a.flac"));
    CHECK(!au_is_network_url("song: live.mp3"));
    CHECK(!au_is_network_url(NULL));
}

static void test_play_and_probe(void) {
    play_and_wait(url("/files/s44_stereo.flac"), NULL, 1, 6000);
    CHECK(tc_snapshot().eof_error == 0);
    char ua[256];
    ts_user_agent(&srv, ua, sizeof(ua));
    printf("   user agent: %s\n", ua);
    CHECK(!strncmp(ua, "ffaudio/", 8));

    int err = 0;
    AuMediaInfo *info = au_probe(url("/files/long_6s.flac"), &err);
    CHECK(info != NULL);
    if (info) {
        CHECK(fabs(au_media_duration(info) - 6.0) < 0.1);
        CHECK(!strcmp(au_media_codec(info), "flac"));
        au_media_info_free(info);
    }
    // Other formats over HTTP
    play_and_wait(url("/files/s48_stereo.mp3"), NULL, 2, 6000);
    play_and_wait(url("/redirect/s48_stereo.ogg"), NULL, 3, 6000);
    CHECK(tc_snapshot().eof_error == 0);
}

static void test_seek(void) {
    ts_reset(&srv);
    au_play_audio(url("/files/long_6s.flac"), NULL);
    tc_sleep_ms(600);
    au_seek_time(4000);
    tc_sleep_ms(500);
    const double t = au_get_audio_play_time();
    printf("   after seeking to 4 s: %.2f s, %d range requests\n", t, ts_range_requests(&srv));
    CHECK(t > 3.9 && t < 5.5);
    CHECK(ts_range_requests(&srv) >= 1);
    CHECK(TC_WAIT_FIELD(eof, 1, 4000));   // ~1.5 s left
    CHECK(tc_snapshot().eof_error == 0);
}

static void test_errors_and_timeouts(void) {
    play_and_wait(url("/status/404"), NULL, 1, 5000);
    CHECK(tc_snapshot().eof_error == 1);

    char refused[64];
    snprintf(refused, sizeof(refused), "http://127.0.0.1:%d/files/s44_stereo.flac", ts_closed_port());
    play_and_wait(refused, NULL, 2, 5000);
    CHECK(tc_snapshot().eof_error == 2);

    // A server that never answers: the timeout ends the track
    const AuNetworkConfig quick = { .timeout_seconds = 1 };
    CHECK(au_set_network(&quick) == AU_OK);
    const uint32_t t0 = SDL_GetTicks();
    play_and_wait(url("/hang"), NULL, 3, 6000);
    const uint32_t took = SDL_GetTicks() - t0;
    printf("   no answer: ended after %u ms (timeout 1 s)\n", took);
    CHECK(tc_snapshot().eof_error == 3);
    CHECK(took >= 900 && took < 5000);

    // Stopping a track that is still connecting does not wait for the timeout
    const AuNetworkConfig slow = { .timeout_seconds = 60 };
    CHECK(au_set_network(&slow) == AU_OK);
    au_play_audio(url("/hang"), NULL);
    tc_sleep_ms(300);
    const uint32_t t1 = SDL_GetTicks();
    au_stop_audio();
    CHECK(TC_WAIT_FIELD(eof, 4, 3000));
    printf("   stopped while connecting: %u ms\n", SDL_GetTicks() - t1);
    CHECK(au_set_network(NULL) == AU_OK);

    const AuNetworkConfig bad = { .timeout_seconds = -1 };
    CHECK(au_set_network(&bad) == AU_ERR_INVALID_ARGUMENT);
}

static void test_headers_and_user_agent(void) {
    play_and_wait(url("/auth/s44_stereo.flac"), NULL, 1, 5000);
    CHECK(tc_snapshot().eof_error == 1);           // 401 without the token

    const PlayAudioConfig with_token = { .http_headers = "X-Other: 1\nX-Test-Token: secret" };
    play_and_wait(url("/auth/s44_stereo.flac"), &with_token, 2, 6000);
    CHECK(tc_snapshot().eof_error == 1);           // played

    const AuNetworkConfig ua = { .user_agent = "soak-test/1" };
    CHECK(au_set_network(&ua) == AU_OK);
    play_and_wait(url("/files/s44_stereo.flac"), NULL, 3, 6000);
    char seen[256];
    ts_user_agent(&srv, seen, sizeof(seen));
    CHECK(!strcmp(seen, "soak-test/1"));
    CHECK(au_set_network(NULL) == AU_OK);
}

static void test_protocol_whitelists(void) {
    // Each case has a control that plays, so a refusal is the whitelist and not a broken playlist
    char seg_file[1200], text[1600];
    file_url("hls_seg.aac", seg_file, sizeof(seg_file));
    const char *head = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:3\n#EXTINF:2.0,\n";

    // A local playlist may use local segments ...
    snprintf(text, sizeof(text), "%shls_seg.aac\n#EXT-X-ENDLIST\n", head);
    write_file("local_local.m3u8", text);
    play_and_wait(fx("local_local.m3u8"), NULL, 1, 8000);
    CHECK(tc_snapshot().eof_error == 0);

    // ... but not reach the network
    ts_reset(&srv);
    snprintf(text, sizeof(text), "%s%s\n#EXT-X-ENDLIST\n", head, url("/files/hls_seg.aac"));
    write_file("local_http.m3u8", text);
    play_and_wait(fx("local_http.m3u8"), NULL, 2, 8000);
    CHECK(tc_snapshot().eof_error == 1);
    CHECK(ts_count(&srv, "/files/hls_seg.aac") == 0);

    // A playlist from the network may use network segments (plain HLS) ...
    snprintf(text, sizeof(text), "%shttp://127.0.0.1:{PORT}/files/hls_seg.aac\n#EXT-X-ENDLIST\n", head);
    write_file("remote_http.m3u8", text);
    play_and_wait(url("/text/remote_http.m3u8"), NULL, 3, 8000);
    CHECK(tc_snapshot().eof_error == 1);
    CHECK(ts_count(&srv, "/files/hls_seg.aac") >= 1);

    // ... but not read local files
    snprintf(text, sizeof(text), "%s%s\n#EXT-X-ENDLIST\n", head, seg_file);
    write_file("remote_file.m3u8", text);
    play_and_wait(url("/text/remote_file.m3u8"), NULL, 4, 8000);
    CHECK(tc_snapshot().eof_error == 2);
    printf("   segment requests: %d\n", ts_count(&srv, "/files/hls_seg.aac"));
}

static void test_no_measuring_by_default(void) {
    char cache[1100];
    snprintf(cache, sizeof(cache), "%s/test_network.cache", tc_fixture_dir);
    remove(cache);
    CHECK(au_gain_cache_set_file(cache) == AU_OK);
    const char *paths[1] = { url("/files/long_6s.flac") };
    CHECK(au_gain_cache_request(paths, 1, AU_SCAN_URGENT) == 0);   // skipped: it would download the file

    const AuNetworkConfig measure = { .measure_streams = true };
    CHECK(au_set_network(&measure) == AU_OK);
    CHECK(au_gain_cache_request(paths, 1, AU_SCAN_URGENT) == 1);
    for (int i = 0; i < 300 && au_gain_cache_pending() > 0; i++) tc_sleep_ms(20);
    AuReplayGainInfo rg;
    CHECK(au_gain_cache_get(paths[0], &rg) && rg.has_track_gain);
    CHECK(au_set_network(NULL) == AU_OK);
    au_gain_scan_stop();
    au_gain_cache_set_file("");
    remove(cache);
}

int main(int argc, char **argv) {
    tc_setup(argc, argv);
    REQUIRE(ts_start(&srv, tc_fixture_dir));
    printf("test server on port %d\n", srv.port);
    REQUIRE(tc_init_player(0) == 0);

    RUN_TEST(test_url_detection);
    RUN_TEST(test_play_and_probe);
    RUN_TEST(test_seek);
    RUN_TEST(test_errors_and_timeouts);
    RUN_TEST(test_headers_and_user_agent);
    RUN_TEST(test_protocol_whitelists);
    RUN_TEST(test_no_measuring_by_default);

    au_shutdown();
    ts_stop(&srv);
    return tc_finish("test_network");
}
