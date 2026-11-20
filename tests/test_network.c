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

/* ---------- live streams, buffering, reconnects ---------- */

// live.mp3 is a constant 128 kbit/s MP3: 16000 bytes per second of audio
#define LIVE_BPS 16000

static void test_live_stream(void) {
    au_play_audio(url("/live/live.mp3"), NULL);
    CHECK(TC_WAIT_FIELD(started, 1, 8000));
    tc_sleep_ms(1500);
    CHECK(au_get_audio_duration() < 0);
    AuMediaInfo *info = au_get_track_info(0);
    CHECK(info && au_media_is_live(info) && !au_media_is_seekable(info));
    au_media_info_free(info);

    const double before = au_get_audio_play_time();
    au_seek_time(60000);                              // ignored: it is live
    tc_sleep_ms(800);
    const double after = au_get_audio_play_time();
    printf("   live: %.2f s -> %.2f s across a seek to 60 s\n", before, after);
    CHECK(after >= before && after < before + 2);
    tc_sleep_ms(4000);                                // past the end of the 5 s file: the stream goes on
    CHECK(tc_snapshot().eof == 0);
    au_stop_audio();
    CHECK(TC_WAIT_FIELD(eof, 1, 3000));
    CHECK(tc_snapshot().eof_error == 0);
}

static void test_preroll(void) {
    // 1.5 s of audio must be in before the first sample plays; at twice real time that takes ~0.75 s
    const AuNetworkConfig net = { .buffer_seconds = 1.5 };
    CHECK(au_set_network(&net) == AU_OK);
    char path[64];
    snprintf(path, sizeof(path), "/live/live.mp3?bps=%d", 2 * LIVE_BPS);
    const uint32_t t0 = SDL_GetTicks();
    au_play_audio(url(path), NULL);
    CHECK(TC_WAIT_FIELD(started, 1, 8000));
    const uint32_t took = SDL_GetTicks() - t0;
    const TcEvents e = tc_snapshot();
    printf("   first sample after %u ms; buffering on %d, off %d\n", took, e.buffering_on, e.buffering_off);
    CHECK(took >= 600);
    CHECK(e.buffering_on >= 1 && e.buffering_off >= 1);
    au_stop_audio();
    CHECK(TC_WAIT_FIELD(eof, 1, 3000));
    CHECK(au_set_network(NULL) == AU_OK);
}

static void test_stall(void) {
    // Data at 0.6x real time: playback has to wait again and again, and its position never runs ahead of the data
    const AuNetworkConfig net = { .buffer_seconds = 0.5 };
    CHECK(au_set_network(&net) == AU_OK);
    char path[64];
    snprintf(path, sizeof(path), "/live/live.mp3?bps=%d", LIVE_BPS * 6 / 10);
    au_play_audio(url(path), NULL);
    tc_sleep_ms(6000);
    const double pos = au_get_audio_play_time();
    const TcEvents e = tc_snapshot();
    printf("   after 6 s at 0.6x: position %.2f s (at most 3.6 s of audio arrived); buffering on %d, off %d\n", pos,
           e.buffering_on, e.buffering_off);
    CHECK(e.buffering_on >= 2);                       // the start, and at least one stall
    CHECK(pos > 1.0 && pos < 3.8);
    CHECK(e.eof == 0);
    au_stop_audio();
    CHECK(TC_WAIT_FIELD(eof, 1, 3000));
    CHECK(au_set_network(NULL) == AU_OK);
}

static void test_reconnect(void) {
    // A file whose connection drops halfway continues with a range request
    ts_reset(&srv);
    play_and_wait(url("/files/long_6s.flac?drop=40000"), NULL, 1, 12000);
    printf("   dropped file: %d requests, %d with a range\n", ts_count(&srv, "/files/long_6s.flac"), ts_range_requests(&srv));
    CHECK(tc_snapshot().eof_error == 0);
    CHECK(ts_count(&srv, "/files/long_6s.flac") >= 2 && ts_range_requests(&srv) >= 1);

    // A live stream whose connection drops after ~2.5 s of audio is opened again
    ts_reset(&srv);
    au_play_audio(url("/live/live.mp3?drop=40000"), NULL);
    tc_sleep_ms(6000);
    printf("   dropped live stream: %d requests, eof %d\n", ts_count(&srv, "/live/live.mp3"), tc_snapshot().eof);
    CHECK(tc_snapshot().eof == 1);                   // only the file above has ended
    CHECK(ts_count(&srv, "/live/live.mp3") >= 2);
    au_stop_audio();
    CHECK(TC_WAIT_FIELD(eof, 2, 3000));

    // Without reconnects the drop ends it
    const AuNetworkConfig never = { .reconnect_attempts = -1 };
    CHECK(au_set_network(&never) == AU_OK);
    au_play_audio(url("/live/live.mp3?drop=40000&n=2"), NULL);
    CHECK(TC_WAIT_FIELD(eof, 3, 10000));
    CHECK(au_set_network(NULL) == AU_OK);
}

/* ---------- "now playing" ---------- */

static void test_icy_now_playing(void) {
    // An unthrottled server: the reader is soon many seconds ahead, but every title must come when it is heard.
    // A block every 8000 bytes = every 0.5 s of audio; song k starts with block 4(k-1), sent after (4k-3) x 0.5 s.
    au_play_audio(url("/live/live.mp3?metaint=8000"), NULL);
    tc_sleep_ms(7500);
    const TcEvents e = tc_snapshot();
    // The first title is read while FFmpeg probes the stream, so it shows from the start (stations send the current
    // title in their first block anyway)
    const double starts[3] = { 0.0, 2.5, 4.5 };
    for (int i = 0; i < e.meta_count && i < 8; i++)
        printf("   now playing at %.2f s: \"%s\" by \"%s\"\n", e.meta_pos[i], e.meta_title[i], e.meta_artist[i]);
    CHECK(e.meta_count >= 3);
    CHECK(!strcmp(e.meta_title[0], "Song 1") && !strcmp(e.meta_artist[0], "Test Artist"));
    CHECK(!strcmp(e.meta_title[1], "Song 2") && !strcmp(e.meta_artist[1], "Caf\xc3\xa9 Band")); // Latin-1 -> UTF-8
    CHECK(!strcmp(e.meta_title[2], "Station Jingle") && e.meta_artist[2][0] == '\0');          // no artist
    for (int i = 0; i < 3 && i < e.meta_count; i++)
        CHECK(e.meta_pos[i] > starts[i] - 0.3 && e.meta_pos[i] < starts[i] + 1.0);              // heard, not read

    AuMediaInfo *info = au_get_track_info(0);
    CHECK(info != NULL);
    if (info) {
        const char *station = au_media_tag(info, "icy-name"), *title = au_media_tag(info, "title");
        printf("   track info: station \"%s\", title \"%s\"\n", station ? station : "", title ? title : "");
        CHECK(station && !strcmp(station, "Test Radio"));
        CHECK(title && e.meta_count <= 8 && !strcmp(title, e.meta_title[e.meta_count - 1]));
        au_media_info_free(info);
    }
    au_stop_audio();
    CHECK(TC_WAIT_FIELD(eof, 1, 3000));
}

static void test_icy_with_file_tags(void) {
    // An MP3 with its own ID3 title (looped by the server, so the tag also turns up mid-stream): only the ICY
    // titles count, each once, in order
    au_play_audio(url("/live/live_id3.mp3?metaint=8000"), NULL);
    tc_sleep_ms(5500);
    const TcEvents e = tc_snapshot();
    for (int i = 0; i < e.meta_count && i < 8; i++)
        printf("   now playing at %.2f s: \"%s\" by \"%s\"\n", e.meta_pos[i], e.meta_title[i], e.meta_artist[i]);
    CHECK(e.meta_count >= 2);
    for (int i = 0; i < e.meta_count && i < 8; i++) CHECK(strcmp(e.meta_title[i], "File Title") != 0);
    CHECK(!strcmp(e.meta_title[0], "Song 1"));
    CHECK(e.meta_count < 2 || !strcmp(e.meta_title[1], "Song 2"));
    au_stop_audio();
    CHECK(TC_WAIT_FIELD(eof, 1, 3000));
}

static void test_no_analysis_of_live_streams(void) {
    // It would wait forever for the end; it must give up right after probing
    const uint32_t t0 = SDL_GetTicks();
    AuLoudness l;
    CHECK(au_loudness_analyze(url("/live/live.mp3"), NULL, &l) == AU_ERR_UNSUPPORTED);
    printf("   refused after %u ms\n", SDL_GetTicks() - t0);
    CHECK(SDL_GetTicks() - t0 < 5000);
    int err = 0;
    AuMediaInfo *info = au_probe(url("/live/live.mp3"), &err);   // probing is fine
    CHECK(info && au_media_is_live(info));
    au_media_info_free(info);
}

static void test_ogg_now_playing(void) {
    // A chained Ogg stream changes its tags when the next part starts (2 s in)
    au_play_audio(url("/files/chained.ogg"), NULL);
    CHECK(TC_WAIT_FIELD(eof, 1, 10000));
    const TcEvents e = tc_snapshot();
    for (int i = 0; i < e.meta_count && i < 8; i++)
        printf("   now playing at %.2f s: \"%s\" by \"%s\"\n", e.meta_pos[i], e.meta_title[i], e.meta_artist[i]);
    CHECK(e.meta_count >= 1);
    const int last = (e.meta_count < 8 ? e.meta_count : 8) - 1;
    CHECK(last >= 0 && !strcmp(e.meta_title[last], "Second Part") && !strcmp(e.meta_artist[last], "Ogg Artist"));
    CHECK(last >= 0 && e.meta_pos[last] > 1.7);
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
    RUN_TEST(test_live_stream);
    RUN_TEST(test_preroll);
    RUN_TEST(test_stall);
    RUN_TEST(test_reconnect);
    RUN_TEST(test_icy_now_playing);
    RUN_TEST(test_ogg_now_playing);
    RUN_TEST(test_icy_with_file_tags);
    RUN_TEST(test_no_analysis_of_live_streams);

    au_shutdown();
    ts_stop(&srv);
    return tc_finish("test_network");
}
