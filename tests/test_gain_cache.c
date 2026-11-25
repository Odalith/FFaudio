/*
 * ReplayGain cache tests: the store itself, thread safety, get-or-scan, and how the player uses it.
 */
#include "test_common.h"
#include "../lib/ffaudio_loudness.h"
#include <math.h>
#include <locale.h>
#ifdef _WIN32
#include <windows.h>
#include <wchar.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

static AuReplayGain track_gain(const double db) {
    AuReplayGain rg = AU_REPLAYGAIN_NONE;
    rg.track_gain_db = (float)db;
    return rg;
}

/* ---------- the store ---------- */

static void test_basics(void) {
    au_gain_cache_clear();
    CHECK(au_gain_cache_count() == 0);
    CHECK(!au_gain_cache_get("/music/a.flac", NULL));

    AuReplayGain rg = track_gain(-6.5), out;
    CHECK(au_gain_cache_set("/music/a.flac", &rg) == AU_OK);
    CHECK(au_gain_cache_count() == 1);
    CHECK(au_gain_cache_get("/music/a.flac", &out) && !isnan(out.track_gain_db) && out.track_gain_db == -6.5);
    CHECK(au_gain_cache_get("/music/a.flac", NULL));
    CHECK(!au_gain_cache_get("/music/A.flac", NULL));      // exact, case-sensitive match
    CHECK(!au_gain_cache_get("/music/a.flac ", NULL));

    rg = track_gain(-2);
    CHECK(au_gain_cache_set("/music/a.flac", &rg) == AU_OK); // replace
    CHECK(au_gain_cache_count() == 1);
    CHECK(au_gain_cache_get("/music/a.flac", &out) && out.track_gain_db == -2);

    CHECK(au_gain_cache_remove("/music/a.flac"));
    CHECK(!au_gain_cache_remove("/music/a.flac"));
    CHECK(au_gain_cache_count() == 0);

    CHECK(au_gain_cache_set(NULL, &rg) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_gain_cache_set("", &rg) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_gain_cache_set("/x", NULL) == AU_ERR_INVALID_ARGUMENT);
    CHECK(!au_gain_cache_get(NULL, &out));
    CHECK(!au_gain_cache_remove(NULL));
    CHECK(au_gain_cache_foreach(NULL, NULL) == 0);
}

static int visits = 0;
static bool visit_remove_all(const char *path, const AuReplayGain *rg, void *user) {
    (void)rg; (void)user;
    visits++;
    au_gain_cache_remove(path); // using the cache from the visitor must not deadlock
    return true;
}
static bool visit_stop_after_3(const char *path, const AuReplayGain *rg, void *user) {
    (void)path; (void)rg;
    return ++*(int *)user < 3;
}

static void test_foreach(void) {
    au_gain_cache_clear();
    char key[64];
    for (int i = 0; i < 10; i++) {
        snprintf(key, sizeof(key), "/music/%02d.flac", i);
        const AuReplayGain rg = track_gain(-i);
        au_gain_cache_set(key, &rg);
    }
    int n = 0;
    CHECK(au_gain_cache_foreach(visit_stop_after_3, &n) == 3 && n == 3);
    visits = 0;
    CHECK(au_gain_cache_foreach(visit_remove_all, NULL) == 10);
    CHECK(visits == 10);
    CHECK(au_gain_cache_count() == 0);
}

static void test_many_entries(void) {
    au_gain_cache_clear();
    char key[64];
    const int n = 20000;
    for (int i = 0; i < n; i++) {
        snprintf(key, sizeof(key), "/library/artist %d/track %d.flac", i % 97, i);
        const AuReplayGain rg = track_gain(i * 0.001);
        if (au_gain_cache_set(key, &rg) != AU_OK) { CHECK(false); break; }
    }
    CHECK(au_gain_cache_count() == n);
    int bad = 0;
    for (int i = 0; i < n; i += 7) {
        snprintf(key, sizeof(key), "/library/artist %d/track %d.flac", i % 97, i);
        AuReplayGain out;
        if (!au_gain_cache_get(key, &out) || fabs(out.track_gain_db - i * 0.001) > 1e-6) bad++;   // a float
    }
    CHECK(bad == 0);
    au_gain_cache_clear();
    CHECK(au_gain_cache_count() == 0);
}

static int hammer(void *arg) {
    const int id = (int)(intptr_t)arg;
    char key[64];
    for (int i = 0; i < 3000; i++) {
        snprintf(key, sizeof(key), "/shared/%d.flac", i % 50);
        const AuReplayGain rg = track_gain(id);
        switch ((i + id) % 4) {
            case 0: case 1: au_gain_cache_set(key, &rg); break;
            case 2: au_gain_cache_get(key, NULL); break;
            case 3: au_gain_cache_remove(key); break;
        }
        if (i % 500 == 0) au_gain_cache_count();
    }
    return 0;
}

static bool count_visitor(const char *path, const AuReplayGain *rg, void *user) {
    (void)path; (void)rg;
    (*(int *)user)++;
    return true;
}

static void test_threads(void) {
    au_gain_cache_clear();
    SDL_Thread *t[4];
    for (int i = 0; i < 4; i++) t[i] = SDL_CreateThread(hammer, "cache_hammer", (void *)(intptr_t)i);
    for (int i = 0; i < 20; i++) {
        int n = 0;
        au_gain_cache_foreach(count_visitor, &n);
        CHECK(n <= 50);
    }
    for (int i = 0; i < 4; i++) SDL_WaitThread(t[i], NULL);
    CHECK(au_gain_cache_count() <= 50);
    au_gain_cache_clear();
}

/* ---------- get-or-scan ---------- */

static void test_scan(void) {
    au_gain_cache_clear();
    AuReplayGain rg;
    CHECK(au_gain_cache_scan(fx("ld_m23_48k.flac"), NULL, &rg) == AU_OK);
    CHECK(!isnan(rg.track_gain_db) && fabs(rg.track_gain_db - 5.0) < 0.1); // -23 LUFS -> +5 dB re -18 LUFS
    CHECK(!isnan(rg.track_peak) && fabs(rg.track_peak - 0.0708) < 0.002);
    CHECK(isnan(rg.album_gain_db));
    CHECK(au_gain_cache_count() == 1);
    CHECK(au_gain_cache_get(fx("ld_m23_48k.flac"), NULL));

    // Cached: returned as stored, without measuring again
    const AuReplayGain mine = track_gain(-1.25);
    au_gain_cache_set(fx("ld_m23_48k.flac"), &mine);
    CHECK(au_gain_cache_scan(fx("ld_m23_48k.flac"), NULL, &rg) == AU_OK && rg.track_gain_db == -1.25);

    CHECK(au_gain_cache_scan(fx("text.mp3"), NULL, &rg) == AU_ERR_FILE);
    CHECK(!au_gain_cache_get(fx("text.mp3"), NULL));
    CHECK(au_gain_cache_scan(NULL, NULL, NULL) == AU_ERR_INVALID_ARGUMENT);
    CHECK(au_gain_cache_count() == 1);
    au_gain_cache_clear();
}

/* ---------- the player ---------- */

static double applied_gain(const int32_t handle) {
    double db = -999;
    for (int i = 0; i < 100; i++) { // the file has to be open before the gain is known
        if (au_track_gain_db(handle, &db) == AU_OK) break;
        tc_sleep_ms(10);
    }
    return db;
}

static void start_player(void) {
    tc_reset_events();
    REQUIRE(tc_init_player(0) == 0);
    const AuReplayGainSettings cfg = { .mode = AU_REPLAYGAIN_TRACK };
    REQUIRE(au_set_replaygain(&cfg) == AU_OK);
}

static void test_player_priority(void) {
    au_gain_cache_clear();
    start_player();

    // Untagged file: the cache supplies the gain
    AuReplayGain rg = track_gain(-10);
    au_gain_cache_set(fx("rg_untagged.flac"), &rg);
    int32_t h = au_play(fx("rg_untagged.flac"), NULL);
    CHECK(fabs(applied_gain(h) + 10) < 0.01);

    // Tagged file (-6 dB): the cache wins over the tags
    rg = track_gain(-7);
    au_gain_cache_set(fx("rg_tagged.flac"), &rg);
    h = au_play(fx("rg_tagged.flac"), NULL);
    CHECK(fabs(applied_gain(h) + 7) < 0.01);

    // AuPlayConfig.replaygain wins over the cache
    const AuReplayGain explicit_rg = track_gain(-3);
    const AuPlayConfig cfg = { .replaygain = &explicit_rg };
    h = au_play(fx("rg_tagged.flac"), &cfg);
    CHECK(fabs(applied_gain(h) + 3) < 0.01);

    au_stop();
    tc_sleep_ms(200);
    au_shutdown();
}

static void test_player_live_updates(void) {
    au_gain_cache_clear();
    start_player();

    const int32_t h = au_play(fx("rg_long.flac"), NULL);
    CHECK(fabs(applied_gain(h) + 6) < 0.01);       // from the tags

    AuReplayGain rg = track_gain(-9);
    au_gain_cache_set(fx("rg_long.flac"), &rg);      // arrives while the track plays
    tc_sleep_ms(100);
    CHECK(fabs(applied_gain(h) + 9) < 0.01);

    // A queued track picks up a value that arrives after au_queue()
    const int32_t next = au_queue(fx("rg_untagged.flac"), NULL);
    tc_sleep_ms(100);
    CHECK(fabs(applied_gain(next)) < 0.01);         // no tags, no cache yet
    rg = track_gain(-4.5);
    au_gain_cache_set(fx("rg_untagged.flac"), &rg);
    tc_sleep_ms(100);
    CHECK(fabs(applied_gain(next) + 4.5) < 0.01);

    CHECK(au_gain_cache_remove(fx("rg_long.flac")));
    tc_sleep_ms(100);
    CHECK(fabs(applied_gain(h) + 6) < 0.01);       // back to the tags
    au_gain_cache_clear();
    tc_sleep_ms(100);
    CHECK(fabs(applied_gain(next)) < 0.01);

    au_stop();
    tc_sleep_ms(200);
    au_shutdown();
}

static void test_survives_shutdown(void) {
    au_gain_cache_clear();
    const AuReplayGain rg = track_gain(-8);
    au_gain_cache_set(fx("rg_untagged.flac"), &rg);  // before any player exists
    for (int i = 0; i < 2; i++) {
        start_player();
        const int32_t h = au_play(fx("rg_untagged.flac"), NULL);
        CHECK(fabs(applied_gain(h) + 8) < 0.01);
        au_shutdown();
    }
    CHECK(au_gain_cache_count() == 1);
    au_gain_cache_clear();
}

static SDL_atomic_t keep_setting;
static int set_loop(void *arg) {
    (void)arg;
    int i = 0;
    while (SDL_AtomicGet(&keep_setting)) {
        const AuReplayGain rg = track_gain(-(i++ % 10));
        au_gain_cache_set(fx("rg_untagged.flac"), &rg);
        if (i % 3 == 0) au_gain_cache_remove(fx("rg_untagged.flac"));
    }
    return 0;
}

static void test_updates_during_shutdown(void) {
    // Cache updates from another thread while the player starts and stops must be safe
    au_gain_cache_clear();
    SDL_AtomicSet(&keep_setting, 1);
    SDL_Thread *t = SDL_CreateThread(set_loop, "cache_setter", NULL);
    for (int i = 0; i < 5; i++) {
        start_player();
        au_play(fx("rg_untagged.flac"), NULL);
        tc_sleep_ms(50);
        au_shutdown();
    }
    SDL_AtomicSet(&keep_setting, 0);
    SDL_WaitThread(t, NULL);
    au_gain_cache_clear();
}

/* ---------- persistence ---------- */

static char cache_file[1100];

static int count_lines(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int n = 0, c;
    while ((c = fgetc(f)) != EOF) if (c == '\n') n++;
    fclose(f);
    return n;
}

static void append_text(const char *path, const char *text) {
    FILE *f = fopen(path, "ab");
    REQUIRE(f);
    fputs(text, f);
    fclose(f);
}

static void use_fresh_file(void) {
    remove(cache_file);
    REQUIRE(au_gain_cache_set_file(cache_file) == AU_OK);
    CHECK(au_gain_cache_count() == 0);
}

static void test_persistence(void) {
    use_fresh_file();
    AuReplayGain full = { .track_gain_db = -7.25f, .track_peak = 0.987654f, .album_gain_db = -6.5f,
                          .album_peak = 1.0123f, .from_r128 = true };
    AuReplayGain b = track_gain(-1), c = track_gain(3.5);
    CHECK(au_gain_cache_set("/m/a.flac", &full) == AU_OK);
    CHECK(au_gain_cache_set("/m/b.flac", &b) == AU_OK);
    CHECK(au_gain_cache_set("/m/c.flac", &c) == AU_OK);
    CHECK(au_gain_cache_remove("/m/b.flac"));
    c.track_gain_db = 4.0;
    CHECK(au_gain_cache_set("/m/c.flac", &c) == AU_OK);

    char path[1100];
    CHECK(au_gain_cache_get_file(path, sizeof(path)) == (int)strlen(cache_file));
    CHECK(strcmp(path, cache_file) == 0);
    CHECK(au_gain_cache_get_file(path, 4) == AU_ERR_INVALID_ARGUMENT);

    // "Next run": load the file again
    CHECK(au_gain_cache_reload() == AU_OK);
    CHECK(au_gain_cache_count() == 2);
    AuReplayGain out;
    CHECK(au_gain_cache_get("/m/a.flac", &out));
    CHECK(!isnan(out.track_gain_db) && out.track_gain_db == -7.25);
    CHECK(!isnan(out.track_peak) && fabs(out.track_peak - 0.987654) < 1e-6);
    CHECK(!isnan(out.album_gain_db) && out.album_gain_db == -6.5);
    CHECK(!isnan(out.album_peak) && fabs(out.album_peak - 1.0123) < 1e-6);
    CHECK(out.from_r128);
    CHECK(!au_gain_cache_get("/m/b.flac", NULL));
    CHECK(au_gain_cache_get("/m/c.flac", &out) && out.track_gain_db == 4.0 && isnan(out.track_peak));

    // Clearing is persistent too
    au_gain_cache_clear();
    CHECK(au_gain_cache_reload() == AU_OK);
    CHECK(au_gain_cache_count() == 0);
}

static void test_tricky_paths(void) {
    use_fresh_file();
    const char *paths[] = {
        "/music/with\ttab.flac", "/music/with\nnewline.flac", "C:\\Music\\Windows path.flac",
        "/music/Bj\xc3\xb6rk/J\xc3\xb3ga.flac", "/music/back\\slash\\n.flac", "relative/path.mp3", "\\", " ",
    };
    const int n = (int)(sizeof(paths) / sizeof(paths[0]));
    for (int i = 0; i < n; i++) {
        const AuReplayGain rg = track_gain(-i);
        CHECK(au_gain_cache_set(paths[i], &rg) == AU_OK);
    }
    CHECK(au_gain_cache_reload() == AU_OK);
    CHECK(au_gain_cache_count() == n);
    for (int i = 0; i < n; i++) {
        AuReplayGain out;
        if (!au_gain_cache_get(paths[i], &out) || out.track_gain_db != -i) {
            fprintf(stderr, "path %d did not survive the round trip\n", i);
            CHECK(false);
        }
    }
}

static void test_compaction(void) {
    use_fresh_file();
    for (int i = 0; i < 1000; i++) {
        const AuReplayGain rg = track_gain(-i * 0.01);
        au_gain_cache_set("/m/again.flac", &rg);
    }
    CHECK(count_lines(cache_file) == 1001);
    CHECK(au_gain_cache_reload() == AU_OK);         // loading notices the superseded lines and compacts
    CHECK(count_lines(cache_file) == 2);            // header + one entry
    AuReplayGain out;
    CHECK(au_gain_cache_get("/m/again.flac", &out) && fabs(out.track_gain_db + 9.99) < 1e-6);
}

static void test_damaged_and_foreign_lines(void) {
    use_fresh_file();
    const AuReplayGain rg = track_gain(-2);
    CHECK(au_gain_cache_set("/m/ok.flac", &rg) == AU_OK);
    append_text(cache_file, "garbage line\nS\tnot-a-number\t-\t-\t-\t0\t/m/bad.flac\nS\t-1\nR\n\n");
    // Another process appended an entry
    append_text(cache_file, "S\t-3.500000\t0.900000\t-\t-\t0\t/m/other process.flac\n");
    CHECK(au_gain_cache_reload() == AU_OK);
    CHECK(au_gain_cache_count() == 2);
    AuReplayGain out;
    CHECK(au_gain_cache_get("/m/ok.flac", &out) && out.track_gain_db == -2);
    CHECK(au_gain_cache_get("/m/other process.flac", &out) && out.track_gain_db == -3.5f && out.track_peak == 0.9f);
    CHECK(!au_gain_cache_get("/m/bad.flac", NULL));

    // A file that is not a cache file is not used, and not written to either
    remove(cache_file);
    append_text(cache_file, "S\t-3.5\t-\t-\t-\t0\t/m/x.flac\n");
    CHECK(au_gain_cache_reload() == AU_OK);
    CHECK(au_gain_cache_count() == 0);
    CHECK(au_gain_cache_set("/m/y.flac", &rg) == AU_ERR_FILE);  // kept in memory only
    CHECK(au_gain_cache_get("/m/y.flac", NULL));
    CHECK(count_lines(cache_file) == 1);

    // Nor is one of a later format: version 1 records would corrupt it
    remove(cache_file);
    append_text(cache_file, "ffaudio-replaygain-cache 2\nwhatever version 2 writes\n");
    CHECK(au_gain_cache_reload() == AU_OK);
    CHECK(au_gain_cache_count() == 0);
    CHECK(au_gain_cache_set("/m/y.flac", &rg) == AU_ERR_FILE);
    CHECK(count_lines(cache_file) == 2);
    use_fresh_file(); // the tests after this one write to the cache file
}

// Two processes writing one cache file: while this one compacts it again and again, another appends entries of its
// own. Every write takes a file lock and an append follows a file that was replaced, so none of them are lost (they
// used to go to the replaced file, or be dropped by a compaction that had not seen them). On Windows the other
// process is this test again (--cache-child), and it keeps the cache file open between its writes, which the
// compactions here must neither be blocked by for good nor lose entries to.
#define CHILD_ENTRIES 500

// The other process's part: its entries, spread over the parent's compactions. 0 = all written.
static int child_writes(const char *file) {
    au_gain_cache_set_file(file); // its own state, its own lock
    for (int i = 0; i < CHILD_ENTRIES; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/child/%d.flac", i);
        const AuReplayGain rg = track_gain(-i * 0.01);
        if (au_gain_cache_set(path, &rg) != AU_OK) return 1;
#ifdef _WIN32
        Sleep(1);
#else
        usleep(400);
#endif
    }
    return 0;
}

static int compact_round(void) {
    const AuReplayGain rg = track_gain(-2);
    for (int i = 0; i < 300; i++) au_gain_cache_set("/m/superseded.flac", &rg);
    au_gain_cache_reload(); // loading compacts once superseded lines pile up
    return 1;
}

static void test_two_processes(void) {
    use_fresh_file();
    const AuReplayGain old = track_gain(-1);
    for (int i = 0; i < 2000; i++) au_gain_cache_set("/m/superseded.flac", &old);
    fflush(NULL);
    int rounds = 0;
    bool child_ok = false;
#ifdef _WIN32
    wchar_t exe[1024], wfile[1100], cmd[2400];
    GetModuleFileNameW(NULL, exe, 1024);
    MultiByteToWideChar(CP_UTF8, 0, cache_file, -1, wfile, 1100);
    swprintf(cmd, 2400, L"\"%ls\" --cache-child \"%ls\"", exe, wfile);
    STARTUPINFOW si = { .cb = sizeof(si) };
    PROCESS_INFORMATION pi;
    REQUIRE(CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi));
    while (WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT) rounds += compact_round();
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    child_ok = code == 0;
#else
    const pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) _exit(child_writes(cache_file));
    int status = 0;
    while (waitpid(pid, &status, WNOHANG) == 0) rounds += compact_round();
    child_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
    printf("   %d compactions while the other process wrote\n", rounds);
    CHECK(child_ok);
    CHECK(au_gain_cache_reload() == AU_OK);
    int found = 0;
    for (int i = 0; i < CHILD_ENTRIES; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/child/%d.flac", i);
        found += au_gain_cache_get(path, NULL);
    }
    printf("   %d of the other process's %d entries are in the file\n", found, CHILD_ENTRIES);
    CHECK(found == CHILD_ENTRIES);
    CHECK(au_gain_cache_get("/m/superseded.flac", NULL));
}

static void test_locale(void) {
    const char *locales[] = { "de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8" };
    const char *set = NULL;
    for (size_t i = 0; i < sizeof(locales) / sizeof(locales[0]) && !set; i++)
        set = setlocale(LC_NUMERIC, locales[i]);
    if (!set) { printf("   (no comma-decimal locale installed, skipped)\n"); return; }
    use_fresh_file();
    const AuReplayGain rg = track_gain(-6.5);
    CHECK(au_gain_cache_set("/m/locale.flac", &rg) == AU_OK);
    CHECK(au_gain_cache_reload() == AU_OK);
    AuReplayGain out;
    CHECK(au_gain_cache_get("/m/locale.flac", &out) && out.track_gain_db == -6.5);
    setlocale(LC_NUMERIC, "C");
}

static void test_memory_only_and_default_location(void) {
    CHECK(au_gain_cache_set_file("") == AU_OK);
    char path[1100] = "x";
    CHECK(au_gain_cache_get_file(path, sizeof(path)) == 0 && path[0] == '\0');
    const AuReplayGain rg = track_gain(-1);
    CHECK(au_gain_cache_set("/m/memory.flac", &rg) == AU_OK);
    CHECK(au_gain_cache_reload() == AU_OK);
    CHECK(au_gain_cache_count() == 0);              // nothing was written anywhere

    CHECK(au_gain_cache_set_file(NULL) == AU_OK);
    CHECK(au_gain_cache_get_file(path, sizeof(path)) > 0);
    printf("   default location: %s\n", path);
#ifdef _WIN32
    const char *suffix = "ffaudio\\replaygain.cache";
    const char *base = getenv("LOCALAPPDATA"), *test_dir = "localappdata";
#else
    const char *suffix = "ffaudio/replaygain.cache";
    const char *base = getenv("XDG_DATA_HOME"), *test_dir = "xdg-data";
#endif
    CHECK(strlen(path) > strlen(suffix) && strcmp(path + strlen(path) - strlen(suffix), suffix) == 0);

    // Only write to the default location when CTest pointed it into the build tree
    if (base && strstr(base, test_dir)) {
        CHECK(strncmp(path, base, strlen(base)) == 0);
        remove(path);
        CHECK(au_gain_cache_set("/m/default.flac", &rg) == AU_OK);
        CHECK(count_lines(path) == 2);              // directory created on the first write
        au_gain_cache_clear();
    }
    REQUIRE(au_gain_cache_set_file(cache_file) == AU_OK);
}

int main(int argc, char **argv) {
    // The other process of test_two_processes on Windows
    if (argc == 3 && !strcmp(argv[1], "--cache-child")) return child_writes(argv[2]);
    tc_setup(argc, argv);
    // Never use the real per-user cache from the tests
    snprintf(cache_file, sizeof(cache_file), "%s/test_gain_cache.cache", tc_fixture_dir);
    remove(cache_file);
    REQUIRE(au_gain_cache_set_file(cache_file) == AU_OK);

    RUN_TEST(test_persistence);
    RUN_TEST(test_tricky_paths);
    RUN_TEST(test_compaction);
    RUN_TEST(test_two_processes); // early: fork() (POSIX) while no other thread runs
    RUN_TEST(test_damaged_and_foreign_lines);
    RUN_TEST(test_locale);
    RUN_TEST(test_memory_only_and_default_location);
    RUN_TEST(test_basics);
    RUN_TEST(test_foreach);
    RUN_TEST(test_many_entries);
    RUN_TEST(test_threads);
    RUN_TEST(test_scan);
    RUN_TEST(test_player_priority);
    RUN_TEST(test_player_live_updates);
    RUN_TEST(test_survives_shutdown);
    RUN_TEST(test_updates_during_shutdown);
    return tc_finish("test_gain_cache");
}
