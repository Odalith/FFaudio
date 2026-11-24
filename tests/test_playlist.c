/*
 * Playlist files (ffaudio_playlist.h): M3U and PLS parsing, path resolution, encodings, HLS, reading files.
 */
#include "test_common.h"
#include "../lib/ffaudio_playlist.h"

#define CHECK_STR(a, b) do { \
    const char *a_ = (a), *b_ = (b); \
    if (!a_ || strcmp(a_, b_) != 0) { \
        fprintf(stderr, "CHECK FAILED %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, a_ ? a_ : "(null)", b_); \
        tc_failures++; \
    } } while (0)

static const AuPlaylistFile *parse(const char *text, const char *base, int *err) {
    return au_playlist_parse(text, strlen(text), base, err);
}

static void test_m3u_extended(void) {
    const char *text =
        "#EXTM3U\n"
        "#EXTINF:215,Apashe - Majesty\n"
        "songs/majesty.flac\n"
        "#EXTINF:-1 tvg-name=\"Radio, Live\" tvg-logo=\"x.png\",Station One\n"
        "http://radio.example.com/live.mp3\n"
        "# a comment\n"
        "\n"
        "/abs/track.ogg\n"
        "file:///music/with%20space.flac\n";
    int err = 1;
    const AuPlaylistFile *pl = parse(text, "/music/lists/road.m3u8", &err);
    REQUIRE(pl);
    CHECK(err == AU_OK && pl->count == 4);
    CHECK_STR(pl->entries[0].path, "/music/lists/songs/majesty.flac");
    CHECK_STR(pl->entries[0].title, "Apashe - Majesty");
    CHECK(pl->entries[0].duration == 215);
    CHECK_STR(pl->entries[1].path, "http://radio.example.com/live.mp3");
    CHECK_STR(pl->entries[1].title, "Station One");   // the comma inside quotes is an attribute's
    CHECK(pl->entries[1].duration == -1);
    CHECK_STR(pl->entries[2].path, "/abs/track.ogg");
    CHECK_STR(pl->entries[2].title, "");               // no #EXTINF: no title, unknown duration
    CHECK(pl->entries[2].duration == -1);
    CHECK_STR(pl->entries[3].path, "/music/with space.flac");
    au_playlist_file_release(pl);
}

static void test_m3u_plain_crlf_bom(void) {
    const char *text = "\xEF\xBB\xBF" "a.mp3\r\n  b.flac  \r\n\r\nsub/c.wav";
    const AuPlaylistFile *pl = parse(text, "list.m3u", NULL);   // in the current folder
    REQUIRE(pl);
    CHECK(pl->count == 3);
    CHECK_STR(pl->entries[0].path, "a.mp3");
    CHECK_STR(pl->entries[1].path, "b.flac");
    CHECK_STR(pl->entries[2].path, "sub/c.wav");
    au_playlist_file_release(pl);

    pl = parse("x.flac\n../y.flac\n", NULL, NULL);               // no base: as written
    REQUIRE(pl);
    CHECK_STR(pl->entries[1].path, "../y.flac");
    au_playlist_file_release(pl);
}

static void test_latin1(void) {
    // "Café" in Latin-1 (0xE9), as old Windows tools write .m3u files
    const char *text = "#EXTINF:10,Caf\xe9 Band\nCaf\xe9.mp3\n";
    const AuPlaylistFile *pl = parse(text, "/m/x.m3u", NULL);
    REQUIRE(pl && pl->count == 1);
    CHECK_STR(pl->entries[0].title, "Caf\xc3\xa9 Band");
    CHECK_STR(pl->entries[0].path, "/m/Caf\xc3\xa9.mp3");
    au_playlist_file_release(pl);
}

static void test_pls(void) {
    const char *text =
        "[playlist]\n"
        "NumberOfEntries=3\n"
        "File2=http://stream.example.com:8000/high\n"
        "Title2=Second\n"
        "file1=first.ogg\n"            // keys are case-insensitive, entries ordered by number
        "Length1=180.5\n"
        "Title1 = First\n"
        "File5=fifth.mp3\n"            // a gap in the numbers
        "Length2=-1\n"
        "Version=2\n";
    const AuPlaylistFile *pl = parse(text, "http://example.com/radio/list.pls?id=7", NULL);
    REQUIRE(pl);
    CHECK(pl->count == 3);
    CHECK_STR(pl->entries[0].path, "http://example.com/radio/first.ogg");
    CHECK_STR(pl->entries[0].title, "First");
    CHECK(pl->entries[0].duration == 180.5);
    CHECK_STR(pl->entries[1].path, "http://stream.example.com:8000/high");
    CHECK(pl->entries[1].duration == -1);
    CHECK_STR(pl->entries[2].path, "http://example.com/radio/fifth.mp3");
    CHECK_STR(pl->entries[2].title, "");
    au_playlist_file_release(pl);
}

static void test_url_and_windows_paths(void) {
    const AuPlaylistFile *pl = parse("/root.mp3\nrel.mp3\n", "https://host.example/a/b/list.m3u", NULL);
    REQUIRE(pl && pl->count == 2);
    CHECK_STR(pl->entries[0].path, "https://host.example/root.mp3");
    CHECK_STR(pl->entries[1].path, "https://host.example/a/b/rel.mp3");
    au_playlist_file_release(pl);

    pl = parse("rel.mp3\n", "http://host.example", NULL);       // no path in the base
    REQUIRE(pl && pl->count == 1);
    CHECK_STR(pl->entries[0].path, "http://host.example/rel.mp3");
    au_playlist_file_release(pl);

    pl = parse("Album\\01.flac\nD:\\Other\\02.flac\n\\\\nas\\music\\03.flac\n", "C:\\Music\\list.m3u", NULL);
    REQUIRE(pl && pl->count == 3);
    CHECK_STR(pl->entries[0].path, "C:\\Music\\Album\\01.flac");
    CHECK_STR(pl->entries[1].path, "D:\\Other\\02.flac");        // absolute: as written
    CHECK_STR(pl->entries[2].path, "\\\\nas\\music\\03.flac");
    au_playlist_file_release(pl);
}

// A playlist from a URL can not send the application to local files
static void test_url_playlist_lists_only_urls(void) {
    const char *text = "file:///etc/passwd\n/root.mp3\nC:\\Windows\\x.flac\nrel.mp3\nhttps://other.example/s.mp3\n";
    const AuPlaylistFile *pl = parse(text, "http://host.example/lists/a.m3u", NULL);
    REQUIRE(pl);
    CHECK(pl->count == 3);
    CHECK_STR(pl->entries[0].path, "http://host.example/root.mp3");
    CHECK_STR(pl->entries[1].path, "http://host.example/lists/rel.mp3");
    CHECK_STR(pl->entries[2].path, "https://other.example/s.mp3");
    au_playlist_file_release(pl);
    pl = parse(text, "/home/me/a.m3u", NULL);                     // a local playlist may list anything
    REQUIRE(pl);
    CHECK(pl->count == 5);
    au_playlist_file_release(pl);
}

static void test_hls_and_errors(void) {
    const char *hls = "#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXTINF:6.0,\nseg0.ts\n#EXTINF:6.0,\nseg1.ts\n";
    const AuPlaylistFile *pl = parse(hls, "https://cdn.example/live/index.m3u8", NULL);
    REQUIRE(pl);
    CHECK(pl->count == 1);                                        // a stream: the playlist itself
    CHECK_STR(pl->entries[0].path, "https://cdn.example/live/index.m3u8");
    au_playlist_file_release(pl);
    int err = 0;
    CHECK(parse(hls, NULL, &err) == NULL && err == AU_ERR_UNSUPPORTED);

    err = 0;
    pl = parse("", "/x.m3u", &err);                               // empty: no entries, no error
    CHECK(pl && pl->count == 0 && err == AU_OK);
    au_playlist_file_release(pl);
    const char binary[] = { 'f', 'L', 'a', 'C', 0, 0, 0, 34, 1, 2 };
    CHECK(au_playlist_parse(binary, sizeof(binary), "/x.flac", &err) == NULL && err == AU_ERR_FILE);
    CHECK(au_playlist_parse(NULL, 5, NULL, &err) == NULL && err == AU_ERR_INVALID_ARGUMENT);
    au_playlist_file_release(NULL);
}

static void test_read_files(void) {
    char path[1200];
    snprintf(path, sizeof(path), "%s/test playlist.m3u", tc_fixture_dir);
    FILE *f = fopen(path, "wb");
    REQUIRE(f);
    fputs("#EXTM3U\n#EXTINF:1,Sine\ns44_stereo.flac\n", f);
    fclose(f);
    int err = 1;
    const AuPlaylistFile *pl = au_playlist_read(path, &err);
    REQUIRE(pl);
    CHECK(err == AU_OK && pl->count == 1);
    CHECK_STR(pl->entries[0].path, fx("s44_stereo.flac"));        // next to the playlist
    CHECK_STR(pl->entries[0].title, "Sine");

    // What it lists plays
    REQUIRE(tc_init_player(0) == 0);
    CHECK(au_play(pl->entries[0].path, NULL) > 0);
    CHECK(TC_WAIT_FIELD(eof, 1, 5000));
    CHECK(tc_snapshot().eof_error == 0);
    au_shutdown();
    au_playlist_file_release(pl);
    remove(path);

    CHECK(au_playlist_read(fx("no_such_list.m3u"), &err) == NULL && err == AU_ERR_FILE);
    CHECK(au_playlist_read(fx("s44_stereo.flac"), &err) == NULL && err == AU_ERR_FILE);   // not text
    CHECK(au_playlist_read(NULL, &err) == NULL && err == AU_ERR_INVALID_ARGUMENT);
}

int main(int argc, char **argv) {
    tc_setup(argc, argv);
    RUN_TEST(test_m3u_extended);
    RUN_TEST(test_m3u_plain_crlf_bom);
    RUN_TEST(test_latin1);
    RUN_TEST(test_pls);
    RUN_TEST(test_url_and_windows_paths);
    RUN_TEST(test_url_playlist_lists_only_urls);
    RUN_TEST(test_hls_and_errors);
    RUN_TEST(test_read_files);
    return tc_finish("test_playlist");
}
