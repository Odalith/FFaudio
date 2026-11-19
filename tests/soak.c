/*
 * Long-running soak test (Linux): plays files from music folders for hours while exercising the whole API at
 * random - track changes, skips, seeks, pause, stop, crossfades, speed, filters and filter commands, ReplayGain
 * with the cache and background measurement, probing, loudness analysis and player restarts - and samples the
 * process' memory, threads and file descriptors into a CSV. At the end it prints how much each grew per hour after
 * the warm-up, which is what a leak looks like.
 *
 *   soak [options] <dir>...
 *     -t hours     how long to run (default 8; fractions allowed)
 *     -x scale     SDL disk driver time scale: 0.1 plays 10x faster than real time (default 0.1).
 *                  0 = the real default audio device in real time
 *     -s seed      random seed (default: time); the seed is printed, so a run can be repeated
 *     -o file      CSV output (default soak.csv)
 *     -i seconds   sampling interval (default 30)
 *     -r minutes   shut down and re-initialize the player this often (default 30, 0 = never)
 *     -j percent   share of picks that are non-audio files from the folders (default 3)
 *     -c file      ReplayGain cache file (default soak-replaygain.cache; never the user's own cache)
 *     -m           call malloc_trim(0) after every sample (glibc), to see how much of the growth is memory that
 *                  malloc has freed but kept
 *     -v           print every ffaudio log message
 *
 * Ctrl-C stops it cleanly (a second Ctrl-C aborts), so sanitizer builds still print their leak report.
 * Media files are only read.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <ftw.h>
#include <malloc.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "../lib/ffaudio.h"
#include "../lib/ffaudio_filters.h"
#include "../lib/ffaudio_loudness.h"

//+-----------------------------------------------------+
//| Files                                               |
//+-----------------------------------------------------+

typedef struct List {
    char **items;
    size_t n, cap;
} List;

static List audio, junk;

static void list_add(List *l, const char *s) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 1024;
        char **n = realloc(l->items, l->cap * sizeof(char *));
        if (!n) { perror("realloc"); exit(1); }
        l->items = n;
    }
    l->items[l->n++] = strdup(s);
}

static bool is_audio(const char *path) {
    static const char *ext[] = { "flac", "mp3", "m4a", "aac", "ogg", "oga", "opus", "wav", "aiff", "aif", "wma",
                                 "wv", "ape", "dsf", "dff", "mpc", "tta", "mka", "ac3", "dts", "caf", "alac", "m4b" };
    const char *dot = strrchr(path, '.');
    if (!dot) return false;
    for (size_t i = 0; i < sizeof(ext) / sizeof(ext[0]); i++)
        if (!strcasecmp(dot + 1, ext[i])) return true;
    return false;
}

static int collect(const char *path, const struct stat *sb, int type, struct FTW *ftw) {
    (void)sb; (void)ftw;
    if (type == FTW_F) list_add(is_audio(path) ? &audio : &junk, path);
    return 0;
}

//+-----------------------------------------------------+
//| Random numbers (one generator per thread)           |
//+-----------------------------------------------------+

static uint64_t base_seed;
static __thread uint64_t rng_state;

static uint64_t rnd(void) {
    if (!rng_state) rng_state = base_seed ^ ((uint64_t)(uintptr_t)&rng_state * 0x9E3779B97F4A7C15ull) ^ 1;
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}
static int rnd_int(const int n) { return n > 0 ? (int)(rnd() % (uint64_t)n) : 0; }
static double rnd_real(const double lo, const double hi) { return lo + (hi - lo) * (double)(rnd() >> 11) / 9007199254740992.0; }
static bool chance(const double p) { return rnd_real(0, 1) < p; }

static int junk_percent = 3;

static const char *pick_file(void) {
    if (junk.n && rnd_int(100) < junk_percent) return junk.items[rnd_int((int)junk.n)];
    return audio.items[rnd_int((int)audio.n)];
}

static const char *pick_audio(void) { return audio.items[rnd_int((int)audio.n)]; }

//+-----------------------------------------------------+
//| Counters                                            |
//+-----------------------------------------------------+

static atomic_long n_started, n_ended, n_ended_error, n_restarts, n_logs_error, n_logs_warning, n_actions,
    n_analyses, n_analysis_failed, n_probes, n_reinits, n_prepare_next, n_cached;
static atomic_int stop_requested;
static atomic_long heartbeat;          // seconds since start, written by the main loop
static atomic_long last_started_at;    // when a track last started
static const char *_Atomic last_action = "start";
static bool verbose, trim;

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
static double t0;
static long elapsed_s(void) { return (long)(now_s() - t0); }

//+-----------------------------------------------------+
//| Callbacks (control thread)                          |
//+-----------------------------------------------------+

static void on_log(const char *msg, int64_t req, enum AU_LOG_LEVEL level, void *ud) {
    (void)req; (void)ud;
    if (level >= AU_LOG_ERROR) atomic_fetch_add(&n_logs_error, 1);
    else if (level == AU_LOG_WARNING) atomic_fetch_add(&n_logs_warning, 1);
    if (verbose) fprintf(stderr, "[%6ld s] log %d: %s\n", elapsed_s(), (int)level, msg);
}

static void on_eof(bool skip, bool error, int32_t handle, void *ud) {
    (void)skip; (void)handle; (void)ud;
    atomic_fetch_add(&n_ended, 1);
    if (error) atomic_fetch_add(&n_ended_error, 1);
}

static void on_started(int32_t handle, void *ud) {
    (void)ud;
    atomic_fetch_add(&n_started, 1);
    atomic_store(&last_started_at, elapsed_s());
    // An application looks at the track's details when it starts
    AuMediaInfo *info = au_get_track_info(handle);
    if (info) {
        const char *k, *v;
        for (int i = 0; au_media_tag_at(info, i, &k, &v); i++) {}
        au_media_info_free(info);
    }
}

static void on_restart(int32_t handle, double pos, bool looped, int32_t left, void *ud) {
    (void)handle; (void)pos; (void)looped; (void)left; (void)ud;
    atomic_fetch_add(&n_restarts, 1);
}

static void on_prepare_next(int32_t current, void *ud) {
    (void)current; (void)ud;
    atomic_fetch_add(&n_prepare_next, 1);
    if (chance(0.9)) au_set_next(pick_file(), NULL);   // sometimes nothing is queued and playback just ends
}

static void on_cached(const char *path, int result, const AuReplayGainInfo *rg, void *user) {
    (void)path; (void)rg; (void)user;
    if (result == AU_OK) atomic_fetch_add(&n_cached, 1);
}

static void on_devices_changed(void *ud) {
    (void)ud;
    int n = 0;
    char **devs = NULL;
    if (au_get_audio_devices(&n, &devs) == AU_OK) {
        for (int i = 0; i < n; i++) free(devs[i]);
        free(devs);
    }
}

//+-----------------------------------------------------+
//| Random settings                                     |
//+-----------------------------------------------------+

static AuFade random_fade(const double max_seconds) {
    static const AU_FADE_CURVE curves[] = { AU_FADE_LINEAR, AU_FADE_EQUAL_POWER, AU_FADE_S_CURVE,
                                            AU_FADE_LOGARITHMIC, AU_FADE_EXPONENTIAL };
    AuFade f = { .seconds = chance(0.3) ? 0 : rnd_real(0.05, max_seconds), .curve = curves[rnd_int(5)] };
    return f;
}

static void random_crossfade(void) {
    const CrossfadeConfig xf = { .on_track_end = random_fade(8), .on_skip = random_fade(3), .on_play = random_fade(3),
                                 .on_stop = random_fade(1), .on_pause = random_fade(0.5),
                                 .min_track_seconds = chance(0.5) ? 0 : rnd_real(1, 20) };
    au_set_crossfade(&xf);
}

static void random_replaygain(void) {
    const AuReplayGainConfig rg = {
        .mode = (AU_REPLAYGAIN_MODE)rnd_int(3),
        .preamp_db = chance(0.5) ? 0 : rnd_real(-6, 6),
        .fallback_gain_db = chance(0.5) ? 0 : -6,
        .prevent_clipping = chance(0.5),
        .measure_missing = chance(0.5),
        .measure_wait_ms = chance(0.5) ? 0 : rnd_int(1500),
    };
    au_set_replaygain(&rg);
}

static int output_channels(void) {
    int rate = 0, ch = 0;
    return au_get_output_spec(&rate, &ch) == AU_OK && ch > 0 ? ch : 2;
}

static void random_filters(void) {
    if (chance(0.2)) {
        au_set_global_filtergraph(NULL);
        return;
    }
    const int ch = output_channels();
    AuFilterBuilder *fb = au_fb_create();
    if (!fb) return;
    if (chance(0.4)) {
        AuEqualizer eq = au_equalizer_defaults();
        for (int i = 0; i < AU_EQ_BANDS; i++) eq.gain_db[i] = chance(0.5) ? 0 : rnd_real(-8, 8);
        eq.preamp_db = -6;
        if (chance(0.5)) au_fb_set_name(fb, "geq");
        au_fb_add_equalizer(fb, ch, &eq);
    }
    if (chance(0.3)) {
        AuParametricEq peq = { .preamp_db = -4, .band_count = 1 + rnd_int(10) };
        for (int i = 0; i < peq.band_count; i++)
            peq.bands[i] = (AuPeqBand){ (AU_PEQ_TYPE)rnd_int(5), rnd_real(30, 15000), rnd_real(0.3, 5), rnd_real(-8, 8) };
        if (chance(0.5)) au_fb_set_name(fb, "peq");
        au_fb_add_parametric_eq(fb, &peq);
    }
    if (chance(0.3)) au_fb_add_tone(fb, rnd_real(-6, 6), rnd_real(-6, 6));
    if (ch == 2) {
        if (chance(0.4)) {
            AuStereo st = au_stereo_defaults();
            st.balance = chance(0.5) ? 0 : rnd_real(-1, 1);
            st.width = chance(0.2) ? 0 : rnd_real(0.5, 2);
            st.swap = chance(0.2);
            au_fb_set_name(fb, "st");
            au_fb_add_stereo(fb, &st);
        }
        if (chance(0.3)) au_fb_add_crossfeed(fb, NULL);
        if (chance(0.2) && au_filter_available("bs2b")) au_fb_add_bs2b_profile(fb, (AU_BS2B_PROFILE)rnd_int(3));
    }
    if (chance(0.25)) {
        const AuCompressor c = chance(0.5) ? au_compressor_defaults() : au_compressor_night();
        au_fb_add_compressor(fb, &c);
    }
    if (chance(0.4)) au_fb_add_limiter(fb, NULL);
    const char *s = au_fb_peek(fb);
    au_set_global_filtergraph(s[0] ? s : NULL);
    au_fb_free(fb);
}

static void random_command(void) {
    char arg[32];
    switch (rnd_int(3)) {
        case 0:
            snprintf(arg, sizeof(arg), "%.2f", rnd_real(-1, 1));
            au_send_filter_command(0, "stereotools@st", "balance_out", arg);
            break;
        case 1:
            snprintf(arg, sizeof(arg), "%.1f", rnd_real(-6, 6));
            au_send_filter_command(0, "equalizer@peq_0", "g", arg);
            break;
        default:
            snprintf(arg, sizeof(arg), "%d|f=1000|w=500|g=%d", rnd_int(10), rnd_int(13) - 6);
            au_send_filter_command(0, "anequalizer@geq", "change", arg);
            break;
    }
}

static void play_random(const bool queue) {
    static const char *track_filters[] = { "volume=0.8", "atempo=1.05", "highpass=f=40", "aecho=0.8:0.5:40:0.2" };
    const AuFade fade = random_fade(2);
    const AuReplayGainInfo rg = { .has_track_gain = true, .track_gain_db = rnd_real(-12, 3) };
    const PlayAudioConfig cfg = {
        .skip_seconds = chance(0.2) ? rnd_real(0, 400) : 0,  // sometimes past the end
        .play_duration = chance(0.15) ? rnd_real(1, 60) : 0,
        .av_filtergraph = chance(0.15) ? track_filters[rnd_int(4)] : NULL,
        .fade_in = chance(0.15) ? &fade : NULL,
        .replaygain = chance(0.1) ? &rg : NULL,
    };
    const PlayAudioConfig *c = chance(0.5) ? &cfg : NULL;
    if (queue) au_set_next(pick_file(), c);
    else au_play_audio(pick_file(), c);
}

//+-----------------------------------------------------+
//| Player setup                                        |
//+-----------------------------------------------------+

static const char *cache_file = "soak-replaygain.cache";

static void init_player(void) {
    const InitializeConfig config = {
        .app_name = "ffaudio soak",
        .initial_volume = 30,
        .prepare_next_seconds = rnd_real(2, 15),
        .on_log = on_log,
        .on_eof = on_eof,
        .on_restart = on_restart,
        .on_prepare_next = on_prepare_next,
        .on_track_started = on_started,
        .on_audio_devices_changed = on_devices_changed,
    };
    if (au_initialize(&config) != AU_OK || au_configure_audio_device(NULL) != AU_OK) {
        fprintf(stderr, "could not start the player\n");
        exit(1);
    }
    random_crossfade();
    random_replaygain();
    random_filters();
}

//+-----------------------------------------------------+
//| Side threads                                        |
//+-----------------------------------------------------+

// Loudness analysis and probing, independent of the player, like a library scanner in an application
static void *analysis_thread(void *arg) {
    (void)arg;
    while (!atomic_load(&stop_requested)) {
        const char *f = pick_file();
        if (chance(0.5)) {
            int err;
            AuMediaInfo *info = au_probe(f, &err);
            au_media_info_free(info);
            atomic_fetch_add(&n_probes, 1);
        }
        else {
            const AuLoudnessConfig cfg = { .skip_true_peak = chance(0.5) };
            AuLoudness l;
            if (au_loudness_analyze(f, &cfg, &l) == AU_OK) atomic_fetch_add(&n_analyses, 1);
            else atomic_fetch_add(&n_analysis_failed, 1);
        }
        usleep((useconds_t)rnd_int(2000) * 1000);
    }
    return NULL;
}

// The main loop calls blocking API functions (e.g. au_get_track_info): if one of them stops returning, that is a
// deadlock, and the watchdog says where it was before it aborts
static void *watchdog_thread(void *arg) {
    (void)arg;
    while (!atomic_load(&stop_requested)) {
        sleep(5);
        const long behind = elapsed_s() - atomic_load(&heartbeat);
        if (behind > 120) {
            fprintf(stderr, "\nWATCHDOG: the main loop has not moved for %ld s (last action: %s)\n", behind,
                    atomic_load(&last_action));
            abort();
        }
    }
    return NULL;
}

//+-----------------------------------------------------+
//| Sampling                                            |
//+-----------------------------------------------------+

typedef struct Sample {
    double t;
    long rss_kb, anon_kb, heap_kb, heap_free_kb, heap_total_kb, threads, fds;
} Sample;

static Sample *samples;
static size_t n_samples, cap_samples;
static size_t n_fit;   // samples taken while running (the last one is taken after the shutdown)

static long status_value(const char *status, const char *key) {
    const char *p = strstr(status, key);
    return p ? atol(p + strlen(key)) : -1;
}

static Sample take_sample(void) {
    Sample s = { .t = now_s() - t0 };
    char buf[8192] = "";
    FILE *f = fopen("/proc/self/status", "r");
    if (f) {
        const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = '\0';
        fclose(f);
    }
    s.rss_kb = status_value(buf, "VmRSS:");
    s.anon_kb = status_value(buf, "RssAnon:");
    s.threads = status_value(buf, "Threads:");
#if defined(__GLIBC__) && (__GLIBC__ > 2 || __GLIBC_MINOR__ >= 33)
    const struct mallinfo2 mi = mallinfo2();
    s.heap_kb = (long)((mi.uordblks + mi.hblkhd) / 1024);   // bytes handed out by malloc, mmapped blocks included
    s.heap_free_kb = (long)(mi.fordblks / 1024);             // freed, but kept by malloc's arenas
    s.heap_total_kb = (long)((mi.arena + mi.hblkhd) / 1024); // everything malloc took from the system
#else
    s.heap_kb = s.heap_free_kb = s.heap_total_kb = -1;
#endif
    s.fds = 0;
    DIR *d = opendir("/proc/self/fd");
    if (d) {
        for (struct dirent *e; (e = readdir(d));)
            if (e->d_name[0] != '.') s.fds++;
        closedir(d);
    }
    return s;
}

static FILE *csv;

static void record(void) {
    const Sample s = take_sample();
    if (n_samples == cap_samples) {
        cap_samples = cap_samples ? cap_samples * 2 : 1024;
        Sample *n = realloc(samples, cap_samples * sizeof(Sample));
        if (!n) return;
        samples = n;
    }
    samples[n_samples++] = s;
    fprintf(csv, "%.0f,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%d,%d\n", s.t, s.rss_kb,
            s.anon_kb, s.heap_kb, s.heap_free_kb, s.heap_total_kb, s.threads, s.fds, atomic_load(&n_started), atomic_load(&n_ended), atomic_load(&n_ended_error),
            atomic_load(&n_actions), atomic_load(&n_analyses), atomic_load(&n_probes), atomic_load(&n_reinits),
            atomic_load(&n_logs_error), atomic_load(&n_cached), au_gain_cache_count(), au_gain_cache_pending());
    fflush(csv);
    printf("[%6.0f s] rss %6ld KB  heap %6ld KB (held %6ld KB)  threads %3ld  fds %3ld  tracks %ld (%ld failed)  actions %ld  "
           "analyses %ld  probes %ld  restarts %ld\n", s.t, s.rss_kb, s.heap_kb, s.heap_total_kb, s.threads, s.fds,
           atomic_load(&n_started), atomic_load(&n_ended_error), atomic_load(&n_actions), atomic_load(&n_analyses),
           atomic_load(&n_probes), atomic_load(&n_reinits));
    fflush(stdout);
}

// Least-squares growth per hour of one column, over the samples after the warm-up
static double slope_per_hour(const size_t from, long (*get)(const Sample *)) {
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    const size_t n = n_fit - from;
    for (size_t i = from; i < n_fit; i++) {
        const double x = samples[i].t / 3600.0, y = (double)get(&samples[i]);
        sx += x; sy += y; sxx += x * x; sxy += x * y;
    }
    const double den = (double)n * sxx - sx * sx;
    return n > 2 && den != 0 ? ((double)n * sxy - sx * sy) / den : 0;
}
static long get_rss(const Sample *s) { return s->rss_kb; }
static long get_anon(const Sample *s) { return s->anon_kb; }
static long get_heap(const Sample *s) { return s->heap_kb; }
static long get_heap_total(const Sample *s) { return s->heap_total_kb; }
static long get_threads(const Sample *s) { return s->threads; }
static long get_fds(const Sample *s) { return s->fds; }

static void summary(void) {
    if (n_fit < 4) return;
    // Warm-up: the first 20 % (at least the first 10 minutes), while caches and the decoder pool fill
    size_t from = n_fit / 5;
    while (from < n_fit - 3 && samples[from].t < 600) from++;
    const Sample *a = &samples[from], *z = &samples[n_fit - 1];
    printf("\n== soak summary: %.1f h, %ld tracks started (%ld ended with errors), %ld actions, %ld analyses, "
           "%ld probes, %ld player restarts\n", z->t / 3600, atomic_load(&n_started), atomic_load(&n_ended_error),
           atomic_load(&n_actions), atomic_load(&n_analyses), atomic_load(&n_probes), atomic_load(&n_reinits));
    printf("   after the warm-up (from %.0f s, %zu samples)    start -> end      growth per hour (fit)\n", a->t,
           n_fit - from);
    printf("   RSS                %8ld -> %8ld KB    %+9.0f KB/h\n", a->rss_kb, z->rss_kb, slope_per_hour(from, get_rss));
    printf("   anonymous RSS      %8ld -> %8ld KB    %+9.0f KB/h\n", a->anon_kb, z->anon_kb, slope_per_hour(from, get_anon));
    printf("   malloc in use      %8ld -> %8ld KB    %+9.0f KB/h\n", a->heap_kb, z->heap_kb, slope_per_hour(from, get_heap));
    printf("   malloc arenas      %8ld -> %8ld KB    %+9.0f KB/h  (in use + freed but kept)\n", a->heap_total_kb,
           z->heap_total_kb, slope_per_hour(from, get_heap_total));
    printf("   threads            %8ld -> %8ld       %+9.1f /h\n", a->threads, z->threads, slope_per_hour(from, get_threads));
    printf("   file descriptors   %8ld -> %8ld       %+9.1f /h\n", a->fds, z->fds, slope_per_hour(from, get_fds));
    if (n_samples > n_fit) {
        const Sample *e = &samples[n_samples - 1];
        printf("   after shutdown: malloc in use %ld KB (arenas %ld KB), RSS %ld KB, %ld threads, %ld fds\n",
               e->heap_kb, e->heap_total_kb, e->rss_kb, e->threads, e->fds);
    }
}

//+-----------------------------------------------------+
//| Main loop                                           |
//+-----------------------------------------------------+

static void on_signal(int sig) {
    if (atomic_exchange(&stop_requested, 1)) {
        signal(sig, SIG_DFL);
        raise(sig);
    }
}

#define ACTION(name) atomic_store(&last_action, name)

static void one_action(void) {
    const int r = rnd_int(1000);
    atomic_fetch_add(&n_actions, 1);
    if (r < 120)      { ACTION("play");          play_random(false); }
    else if (r < 200) { ACTION("set_next");      play_random(true); }
    else if (r < 300) { ACTION("skip");          au_skip_to_next(); }
    else if (r < 380) { ACTION("seek");          chance(0.5) ? au_seek_percent(rnd_real(0, 1.1)) : au_seek_time(rnd_int(400000)); }
    else if (r < 420) { ACTION("pause");         au_pause_audio(true); usleep((useconds_t)rnd_int(1500) * 1000); au_pause_audio(false); }
    else if (r < 440) { ACTION("stop");          au_stop_audio(); if (chance(0.8)) play_random(false); }
    else if (r < 460) { ACTION("clear_next");    au_clear_next(); }
    else if (r < 500) { ACTION("volume");        au_set_audio_volume(rnd_int(101)); }
    else if (r < 510) { ACTION("mute");          au_mute_audio(true); usleep(200000); au_mute_audio(false); }
    else if (r < 560) { ACTION("speed");         au_set_playback_speed(chance(0.4) ? 1.0 : rnd_real(0.25, 4), chance(0.7)); }
    else if (r < 620) { ACTION("filters");       random_filters(); }
    else if (r < 680) { ACTION("command");       random_command(); }
    else if (r < 700) { ACTION("track_filter");  au_update_filtergraph(0, chance(0.5) ? NULL : "volume=0.7"); }
    else if (r < 720) { ACTION("crossfade");     random_crossfade(); }
    else if (r < 740) { ACTION("replaygain");    random_replaygain(); }
    else if (r < 750) { ACTION("downmix");       au_set_downmix(chance(0.5) ? AU_DOWNMIX_FULL_LEVEL : AU_DOWNMIX_NORMALIZED, NULL); }
    else if (r < 760) { ACTION("loop");          au_set_loop_count(chance(0.7) ? 0 : rnd_int(3)); }
    else if (r < 800) {
        ACTION("prefetch");
        const char *paths[10];
        for (int i = 0; i < 10; i++) paths[i] = pick_audio();
        au_gain_cache_request(paths, 10, chance(0.2) ? AU_SCAN_URGENT : AU_SCAN_PREFETCH);
        if (chance(0.1)) au_gain_cache_cancel_requests();
    }
    else if (r < 820) {
        ACTION("album");
        const char *paths[4] = { pick_audio(), pick_audio(), pick_audio(), pick_audio() };
        au_gain_cache_request_album(paths, 4, AU_SCAN_PREFETCH);
    }
    else if (r < 860) {
        ACTION("track_info");
        AuMediaInfo *info = au_get_track_info(0);
        au_media_info_free(info);
        double g;
        au_get_track_gain_db(0, &g);
        au_get_audio_play_time();
        au_get_audio_duration();
    }
    else if (r < 870) {
        ACTION("devices");
        on_devices_changed(NULL);
        char name[256];
        au_get_audio_device_name(name, sizeof(name));
    }
    else if (r < 880) {
        ACTION("burst");  // many changes at once, faster than any track can start
        for (int i = 0; i < 20; i++) {
            switch (rnd_int(4)) {
                case 0: play_random(false); break;
                case 1: play_random(true); break;
                case 2: au_skip_to_next(); break;
                default: au_seek_percent(rnd_real(0, 1)); break;
            }
        }
    }
    else              { ACTION("idle"); }
}

int main(int argc, char **argv) {
    double hours = 8, scale = 0.1;
    int interval = 30, reinit_minutes = 30;
    const char *csv_path = "soak.csv";
    base_seed = (uint64_t)time(NULL);
    int opt;
    while ((opt = getopt(argc, argv, "t:x:s:o:i:r:j:c:mv")) != -1) {
        switch (opt) {
            case 't': hours = atof(optarg); break;
            case 'x': scale = atof(optarg); break;
            case 's': base_seed = strtoull(optarg, NULL, 10); break;
            case 'o': csv_path = optarg; break;
            case 'i': interval = atoi(optarg) > 0 ? atoi(optarg) : 30; break;
            case 'r': reinit_minutes = atoi(optarg); break;
            case 'j': junk_percent = atoi(optarg); break;
            case 'c': cache_file = optarg; break;
            case 'v': verbose = true; break;
            case 'm': trim = true; break;
            default:
                fprintf(stderr, "usage: %s [-t hours] [-x scale] [-s seed] [-o csv] [-i s] [-r min] [-j %%] [-c cache] [-v] <dir>...\n", argv[0]);
                return 2;
        }
    }
    if (optind >= argc) { fprintf(stderr, "no folders given\n"); return 2; }
    for (int i = optind; i < argc; i++)
        if (nftw(argv[i], collect, 32, FTW_PHYS) != 0) fprintf(stderr, "could not read %s\n", argv[i]);
    if (!audio.n) { fprintf(stderr, "no audio files found\n"); return 1; }

    if (scale > 0) {
        char ts[32];
        snprintf(ts, sizeof(ts), "%g", scale);
        setenv("SDL_AUDIODRIVER", "disk", 1);
        setenv("SDL_AUDIO_DISK_OUTPUT_FILE", "/dev/null", 1);   // SDL3 (sdl2-compat)
        setenv("SDL_DISKAUDIOFILE", "/dev/null", 1);            // SDL2
        setenv("SDL_AUDIO_DISK_TIMESCALE", ts, 1);
    }
    csv = fopen(csv_path, "w");
    if (!csv) { perror(csv_path); return 1; }
    fprintf(csv, "seconds,rss_kb,rss_anon_kb,heap_kb,heap_free_kb,heap_total_kb,threads,fds,tracks_started,tracks_ended,tracks_failed,actions,"
                 "analyses,probes,reinits,log_errors,gains_cached,gain_cache_entries,pending_scans\n");
    printf("ffaudio %s soak: %zu audio + %zu other files, %.2f h, seed %llu, %s, CSV %s\n", au_get_version(), audio.n,
           junk.n, hours, (unsigned long long)base_seed, scale > 0 ? "disk driver (fast)" : "real device", csv_path);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    t0 = now_s();
    remove(cache_file);
    au_gain_cache_set_file(cache_file);   // never the user's own cache
    const AuGainScanConfig scan = { .threads = 2, .on_cached = on_cached };
    au_gain_scan_configure(&scan);
    init_player();
    play_random(false);

    pthread_t analyser, watchdog;
    pthread_create(&analyser, NULL, analysis_thread, NULL);
    pthread_create(&watchdog, NULL, watchdog_thread, NULL);

    double next_sample = 0, next_reinit = reinit_minutes > 0 ? reinit_minutes * 60.0 : 1e18;
    record();
    while (!atomic_load(&stop_requested) && now_s() - t0 < hours * 3600) {
        atomic_store(&heartbeat, elapsed_s());
        one_action();
        // Keep something playing: an application would start the next track when playback stopped
        if (au_get_current_handle() == 0 && chance(0.5)) play_random(false);
        usleep((useconds_t)(50 + rnd_int(1500)) * 1000);

        const double t = now_s() - t0;
        if (t >= next_reinit) {
            ACTION("reinit");
            au_shutdown();
            init_player();
            play_random(false);
            atomic_fetch_add(&n_reinits, 1);
            next_reinit += reinit_minutes * 60.0;
        }
        if (t >= next_sample) {
#ifdef __GLIBC__
            if (trim) malloc_trim(0);
#endif
            record();
            next_sample = t + interval;
        }
        static long last_note;
        if (elapsed_s() - atomic_load(&last_started_at) > 600 && elapsed_s() - last_note >= 60) {
            last_note = elapsed_s();
            fprintf(stderr, "[%6ld s] note: no track has started for %ld s\n", elapsed_s(),
                    elapsed_s() - atomic_load(&last_started_at));
        }
    }

    ACTION("shutdown");
    record();
    n_fit = n_samples;
    atomic_store(&stop_requested, 1);
    au_shutdown();
    pthread_join(analyser, NULL);
    au_gain_scan_stop();
    record();
    pthread_join(watchdog, NULL);
    summary();
    fclose(csv);
    for (size_t i = 0; i < audio.n; i++) free(audio.items[i]);
    for (size_t i = 0; i < junk.n; i++) free(junk.items[i]);
    free(audio.items);
    free(junk.items);
    free(samples);
    return 0;
}
