/*
 * Smoke test for a release archive (see .github/workflows/release.yml): built against the unpacked archive only,
 * and run with nothing but the archive's libraries (and the system's) on the search path. Measures and plays a
 * file to the end. Uses no SDL or FFmpeg headers itself.
 *
 *   release_smoke <audio-file>
 */
#define _POSIX_C_SOURCE 200809L // nanosleep
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
static void sleep_ms(const int ms) { Sleep((DWORD)ms); }
#else
#include <time.h>
static void sleep_ms(const int ms) {
    const struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
#endif

#include <ffaudio.h>
#include <ffaudio_filters.h>
#include <ffaudio_loudness.h>

static atomic_int ended; // 1 = end of file, 2 = error

static void on_event(const AuEvent *e, void *ud) {
    (void)ud;
    if (e->type == AU_EVENT_TRACK_ENDED) atomic_store(&ended, e->data.ended.reason == AU_END_ERROR ? 2 : 1);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: release_smoke <audio-file>\n");
        return 2;
    }
    printf("ffaudio %s; filters: stereotools %s, alimiter %s, bs2b %s\n", au_get_version(),
           au_filter_available("stereotools") ? "yes" : "NO", au_filter_available("alimiter") ? "yes" : "NO",
           au_filter_available("bs2b") ? "yes" : "no");
    if (!au_filter_available("stereotools") || !au_filter_available("alimiter")) return 1;

    AuLoudness l;
    const int r = au_loudness_analyze(argv[1], NULL, &l);
    if (r != AU_OK) {
        fprintf(stderr, "loudness analysis failed: %d\n", r);
        return 1;
    }
    printf("loudness %.2f LUFS, true peak %.2f dBTP\n", l.integrated_lufs, 20 * log10(l.true_peak));

    const AuConfig cfg = { .on_event = on_event };
    if (au_init(&cfg) != AU_OK) {
        fprintf(stderr, "could not initialize the player\n");
        return 1;
    }
    au_play(argv[1], NULL);
    for (int i = 0; i < 1000 && !atomic_load(&ended); i++) sleep_ms(10);
    const int e = atomic_load(&ended);
    au_shutdown();
    printf("playback: %s\n", e == 1 ? "reached the end" : e == 2 ? "error" : "timed out");
    return e == 1 ? 0 : 1;
}
