/*
* Copyright (c) 2025 Odalith
 *
 * ffaudio is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * ffaudio is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFaudio; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * Interactive player for listening tests on real hardware.
 *
 *   manual_player <music-dir | URL> [-u URL]... [-x crossfade-seconds] [-f "filter string"] [-n count] [-s] [-c channels]
 *                 [-d full|norm|"filters"] [-r track|album] [-g] [-G cache-file]
 *                 [-e preset|g1,...,g10] [-X] [-b default|chu|jan] [-P ParametricEQ.txt] [-L] [-C gentle|night]
 *
 *   -g  use the ReplayGain cache (track mode unless -r is given); files it does not know are measured and
 *       stored, in the background for queued tracks. -G uses another cache file (implies -g)
 *   -e  equalizer: bass, treble, v-shape, vocal, or 10 gains in dB for 31 Hz..16 kHz (e.g. 4,3,0,0,0,0,0,0,2,3)
 *   -X  headphone crossfeed;  -b  bs2b with the default, Chu Moy or Jan Meier profile (both need stereo output)
 *   -P  parametric EQ from an EqualizerAPO / AutoEq ParametricEQ.txt file (headphone correction)
 *   -L  limiter at -1 dBFS;  -C  compressor: gentle (levelling) or night (low-volume listening)
 *
 * Plays the files under <music-dir> (shuffled with -s), queueing the next one from on_prepare_next.
 * Keys (no Enter needed in a terminal): n = next, p = pause/resume, s = stop, m = mute, + / - = volume,
 * > / < = seek +/-10 s, [ / ] = slower / faster, backslash = speed keeps / changes the pitch,
 * e = next equalizer preset, a = parametric EQ (-P) on/off, 1 / 2 = bass -/+, 3 / 4 = treble -/+,
 * l / r = balance left / right, w = width, o = mono, c = swap channels, x = crossfeed on/off,
 * b = next bs2b profile, C = compressor off/gentle/night, L = limiter on/off, f = show the filters,
 * space = show position, h = help, q = quit
 */

#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include <ftw.h>
#include <signal.h>
#include <pthread.h>
#include <termios.h>
#include <sys/stat.h>
#include <math.h>

#include "../lib/ffaudio.h"

static AuStatus status(void) {
    AuStatus s;
    au_get_status(&s);
    return s;
}
#include "../lib/ffaudio_loudness.h"
#include "../lib/ffaudio_filters.h"

static char **files = NULL;
static size_t file_count = 0, file_cap = 0;
static size_t next_index = 0;
static size_t max_tracks = (size_t)-1;
static const char *filter = NULL;

// Handle of the track queued with au_queue() that has not started yet (0 = none).
// Written from callbacks (control thread) and read by the key loop.
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static int32_t queued_handle = 0;

static void set_queued(const int32_t handle) {
    pthread_mutex_lock(&queue_lock);
    queued_handle = handle;
    pthread_mutex_unlock(&queue_lock);
}

static void clear_queued_if(const int32_t handle) {
    pthread_mutex_lock(&queue_lock);
    if (queued_handle == handle) queued_handle = 0;
    pthread_mutex_unlock(&queue_lock);
}

static int32_t get_queued(void) {
    pthread_mutex_lock(&queue_lock);
    const int32_t h = queued_handle;
    pthread_mutex_unlock(&queue_lock);
    return h;
}

//+-----------------------------------------------------+
//| Terminal: read single keys without waiting for Enter |
//+-----------------------------------------------------+

static struct termios saved_termios;
static bool termios_saved = false;

static void restore_terminal(void) {
    if (termios_saved) tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
}

static void on_signal(int sig) {
    restore_terminal();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void enable_key_mode(void) {
    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &saved_termios) != 0) return;
    termios_saved = true;
    atexit(restore_terminal);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    struct termios raw = saved_termios;
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);  // keep ISIG so Ctrl-C still works
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

static void print_help(void) {
    printf("keys: n=next  p=pause/resume  s=stop  m=mute  +/-=volume  >/<=seek 10s  [/]=speed  \\=pitch mode\n"
           "      e=equalizer preset  a=parametric EQ  1/2=bass -/+  3/4=treble -/+  l/r=balance  w=width  o=mono\n"
           "      c=swap channels  x=crossfeed  b=bs2b profile  C=compressor  L=limiter  f=show filters\n"
           "      space=position  h=help  q=quit\n");
}

static int collect(const char *path, const struct stat *sb, int type, struct FTW *ftw) {
    (void)sb; (void)ftw;
    if (type != FTW_F) return 0;
    if (file_count == file_cap) {
        file_cap = file_cap ? file_cap * 2 : 128;
        char **n = realloc(files, file_cap * sizeof(char *));
        if (!n) return -1;
        files = n;
    }
    files[file_count++] = strdup(path);
    return 0;
}

// next_index is used from the key loop and from callbacks
static pthread_mutex_t files_lock = PTHREAD_MUTEX_INITIALIZER;
static bool use_cache = false;

static const char *take_next(void) {
    pthread_mutex_lock(&files_lock);
    const char *f = (next_index >= file_count || next_index >= max_tracks) ? NULL : files[next_index++];
    pthread_mutex_unlock(&files_lock);
    return f;
}

static void prefetch_upcoming(void);

static AuPlayConfig play_config(void) {
    return (AuPlayConfig){ .filters = filter };
}

static void on_log(const char *msg, int64_t req, enum AU_LOG_LEVEL level, void *ud) {
    (void)req; (void)ud;
    static const char *names[] = { "info", "warning", "error", "fatal" };
    fprintf(level >= AU_LOG_ERROR ? stderr : stdout, "[%s] %s\n", names[level & 3], msg);
}

static void on_eof(bool skip, bool error, int32_t handle, void *ud) {
    (void)ud;
    clear_queued_if(handle); // a queued track that failed or was cleared
    printf("  ended #%d%s%s\n", handle, skip ? " (skipped)" : "", error ? " (error)" : "");
}

static void on_started(int32_t handle, void *ud) {
    (void)ud;
    clear_queued_if(handle);
    const AuMediaInfo *info = au_track_info(handle);
    double gain = 0;
    const bool has_gain = au_track_gain_db(handle, &gain) == AU_OK && gain != 0;
    bool cached = false;
    if (info) {
        const char *artist = au_media_tag(info, "artist"), *title = au_media_tag(info, "title");
        if (!artist) artist = au_media_tag(info, "icy-name");   // a stream: its station
        cached = au_gain_cache_get(info->path, NULL);
        printf("> now playing #%d: %s - %s  [%s %d Hz %d ch%s]", handle, artist ? artist : "?",
               title ? title : info->path, info->codec, info->sample_rate,
               info->channels, !isnan(info->replaygain.track_gain_db) ? ", tagged" : "");
        au_media_info_release(info);
    }
    else {
        printf("> now playing #%d", handle);
    }
    if (has_gain) printf("  gain %+.2f dB%s", gain, cached ? " (from cache)" : "");
    printf("\n");
    prefetch_upcoming();
}

static void on_duration(int32_t handle, double duration, void *ud) {
    (void)ud;
    printf("  #%d duration %.1f s\n", handle, duration);
}

//+-----------------------------------------------------+
//| ReplayGain cache (-g): measure uncached files       |
//+-----------------------------------------------------+

#define PREFETCH_COUNT 10

// ffaudio measures on its own worker threads; this only reports the results
static void on_cached(const char *path, int result, const AuReplayGain *rg, void *user) {
    (void)user;
    if (result == AU_OK) printf("  measured %s: %+.2f dB\n", path, rg->track_gain_db);
    else if (result != AU_ERR_CANCELLED) printf("  could not measure %s (%d)\n", path, result);
}

// Ask ffaudio to measure the next files of the playlist in the background; cached ones are skipped, so this
// can be called whenever the position changes. Tracks started out of order are measured by the player itself
// (measure_missing), which holds their start for up to 500 ms.
static void prefetch_upcoming(void) {
    if (!use_cache) return;
    const char *upcoming[PREFETCH_COUNT];
    int n = 0;
    pthread_mutex_lock(&files_lock);
    for (size_t i = next_index; i < file_count && i < max_tracks && n < PREFETCH_COUNT; i++)
        upcoming[n++] = files[i];
    pthread_mutex_unlock(&files_lock);
    au_gain_cache_request(upcoming, n, AU_SCAN_PREFETCH);
}

static void queue_next_file(void) {
    const char *f = take_next();
    if (!f) { printf("  (no more files)\n"); return; }
    const AuPlayConfig cfg = play_config();
    const int32_t h = au_queue(f, &cfg);
    set_queued(h);
    printf("  queued #%d %s\n", h, f);
}

static void on_prepare_next(int32_t current, void *ud) {
    (void)current; (void)ud;
    queue_next_file();
}

static void on_restart(int32_t handle, double pos, bool looped, int32_t remaining, void *ud) {
    (void)ud;
    printf("  #%d restarted at %.2f%s (loops left %d)\n", handle, pos, looped ? " by looping" : "", remaining);
}

static void on_devices_changed(void *ud) {
    (void)ud;
    int n = 0;
    char **devs = NULL;
    printf("  output devices changed:\n");
    if ((devs = au_list_devices(&n))) {
        for (int i = 0; i < n; i++) printf("    %d: %s\n", i, devs[i]);
        au_free_devices(devs, n);
    }
}

//+-----------------------------------------------------+
//| Effects: equalizer, crossfeed, bs2b (global filters) |
//+-----------------------------------------------------+

typedef struct EqPreset {
    const char *name;
    double gain_db[AU_EQ_BANDS]; // 31, 62, 125, 250, 500, 1k, 2k, 4k, 8k, 16k Hz
} EqPreset;

static EqPreset eq_presets[] = {
    { "off",     { 0 } },
    { "bass",    { 6, 5, 4, 2, 0, 0, 0, 0, 0, 0 } },
    { "treble",  { 0, 0, 0, 0, 0, 0, 1, 3, 5, 6 } },
    { "v-shape", { 5, 4, 2, 0, -1, -2, -1, 1, 3, 4 } },
    { "vocal",   { -2, -2, -1, 0, 2, 3, 3, 2, 0, -1 } },
    { "custom",  { 0 } },  // from -e g1,g2,...
};
#define EQ_PRESETS (int)(sizeof(eq_presets) / sizeof(eq_presets[0]))

static const char *bs2b_names[] = { "default (700 Hz, 4.5 dB)", "Chu Moy (700 Hz, 6 dB)", "Jan Meier (650 Hz, 9.5 dB)" };

// Changed by the key loop, read when the output device changes (control thread)
static pthread_mutex_t fx_lock = PTHREAD_MUTEX_INITIALIZER;
static int eq_index = 0;          // into eq_presets
static bool have_custom_eq = false;
static bool crossfeed_on = false;
static int bs2b_mode = -1;        // -1 off, else AU_BS2B_PROFILE
static AuParametricEq peq;        // -P file
static bool have_peq = false, peq_on = false;
static double bass_db = 0, treble_db = 0;
static double balance = 0, width = 1;
static bool mono = false, swap_lr = false;
static bool limiter_on = false;
static int compressor_mode = 0;   // 0 off, 1 gentle, 2 night
static const char *compressor_names[] = { "off", "gentle", "night" };
static char last_filters[8192] = "";

#define ADD_DESC(...) snprintf(desc + strlen(desc), sizeof(desc) - strlen(desc), __VA_ARGS__)

// Rebuild the global filter string from the effect settings and the output channel count.
// Order: parametric EQ, graphic EQ, tone, stereo image, crossfeed/bs2b, compressor, limiter (last: the safety net).
static void apply_effects(void) {
    pthread_mutex_lock(&fx_lock);
    int rate = 0, channels = 0;
    rate = status().sample_rate;
    channels = status().channels;
    if (channels <= 0) channels = 2;
    const bool stereo = channels == 2;

    AuFilterBuilder *fb = au_fb_create();
    if (!fb) { pthread_mutex_unlock(&fx_lock); return; }
    char desc[512] = "";
    if (peq_on) {
        if (au_fb_add_parametric_eq(fb, NULL, &peq) != AU_OK) printf("  parametric eq: %s\n", au_fb_last_error(fb));
        ADD_DESC(" peq(%d bands)", peq.band_count);
    }
    if (eq_index > 0) {
        AuEqualizer eq = au_equalizer_defaults();
        double max_gain = 0;
        for (int i = 0; i < AU_EQ_BANDS; i++) {
            eq.gain_db[i] = eq_presets[eq_index].gain_db[i];
            if (eq.gain_db[i] > max_gain) max_gain = eq.gain_db[i];
        }
        eq.preamp_db = -max_gain; // headroom for the boosts
        if (au_fb_add_equalizer(fb, NULL, &eq) != AU_OK)
            printf("  equalizer: %s\n", au_fb_last_error(fb));
        ADD_DESC(" eq=%s", eq_presets[eq_index].name);
    }
    if (bass_db != 0 || treble_db != 0) {
        if (au_fb_add_tone(fb, NULL, bass_db, treble_db) != AU_OK) printf("  tone: %s\n", au_fb_last_error(fb));
        ADD_DESC(" bass=%+.0fdB treble=%+.0fdB", bass_db, treble_db);
    }

    const bool stereo_fx = crossfeed_on || bs2b_mode >= 0 || balance != 0 || width != 1 || mono || swap_lr;
    if (!stereo) {
        if (stereo_fx)
            printf("  balance/width/mono/swap/crossfeed/bs2b need a stereo output (it has %d channels; try -c 2)\n", channels);
    }
    else {
        // Always there and named, so the balance keys can change it instantly with a command
        const AuStereo st = { .balance = balance, .width = mono ? 0 : width, .swap = swap_lr };
        if (au_fb_add_stereo(fb, "st", &st) != AU_OK) printf("  stereo: %s\n", au_fb_last_error(fb));
        if (balance != 0) ADD_DESC(" balance=%+.1f", balance);
        if (mono) ADD_DESC(" mono");
        else if (width != 1) ADD_DESC(" width=%.2g", width);
        if (swap_lr && !mono) ADD_DESC(" swapped");
        if (crossfeed_on) {
            if (au_fb_add_crossfeed(fb, NULL, NULL) != AU_OK) printf("  crossfeed: %s\n", au_fb_last_error(fb));
            ADD_DESC(" crossfeed");
        }
        if (bs2b_mode >= 0) {
            if (au_fb_add_bs2b_profile(fb, NULL, (AU_BS2B_PROFILE)bs2b_mode) != AU_OK)
                printf("  bs2b: %s\n", au_fb_last_error(fb));
            ADD_DESC(" bs2b=%s", bs2b_names[bs2b_mode]);
        }
    }
    if (compressor_mode) {
        const AuCompressor c = compressor_mode == 2 ? au_compressor_night() : au_compressor_defaults();
        if (au_fb_add_compressor(fb, NULL, &c) != AU_OK) printf("  compressor: %s\n", au_fb_last_error(fb));
        ADD_DESC(" compressor=%s", compressor_names[compressor_mode]);
    }
    if (limiter_on) {
        if (au_fb_add_limiter(fb, NULL, NULL) != AU_OK) printf("  limiter: %s\n", au_fb_last_error(fb));
        ADD_DESC(" limiter(-1 dB)");
    }

    const char *filters = au_fb_peek(fb);
    if (au_set_filters(filters[0] ? filters : NULL) != AU_OK) {
        printf("  effects rejected by FFmpeg: \"%s\"\n", filters);
    }
    else {
        snprintf(last_filters, sizeof(last_filters), "%s", filters);
        printf("  effects:%s\n", desc[0] ? desc : " none");
    }
    au_fb_free(fb);
    pthread_mutex_unlock(&fx_lock);
}

// Balance changes right away through a command to the named stereotools filter; the rebuilt string (a moment
// later) keeps the value for the tracks that start afterwards
static void change_balance(const double delta) {
    pthread_mutex_lock(&fx_lock);
    balance += delta;
    if (balance > 1) balance = 1;
    if (balance < -1) balance = -1;
    if (fabs(balance) < 1e-9) balance = 0;
    char arg[32];
    snprintf(arg, sizeof(arg), "%.2f", balance);
    for (char *c = arg; *c; c++) if (*c == ',') *c = '.'; // the command value must not depend on the locale
    const int r = au_send_filter_command(0, "stereotools@st", "balance_out", arg);
    pthread_mutex_unlock(&fx_lock);
    if (r != AU_OK) printf("  (balance command failed: %d)\n", r);
    apply_effects();
}

// The settings and the filter string ffaudio got (for copying into -f or an application)
static void print_effects(void) {
    pthread_mutex_lock(&fx_lock);
    printf("  peq=%s eq=%s bass=%+.0f treble=%+.0f balance=%+.1f width=%.2g%s%s crossfeed=%s bs2b=%s "
           "compressor=%s limiter=%s\n",
           peq_on ? "on" : (have_peq ? "off" : "none"), eq_presets[eq_index].name, bass_db, treble_db, balance, width,
           mono ? " mono" : "", swap_lr ? " swapped" : "", crossfeed_on ? "on" : "off",
           bs2b_mode >= 0 ? bs2b_names[bs2b_mode] : "off", compressor_names[compressor_mode], limiter_on ? "on" : "off");
    printf("  global filters: %s\n", last_filters[0] ? last_filters : "(none)");
    if (filter) printf("  track filters (-f): %s\n", filter);
    pthread_mutex_unlock(&fx_lock);
}

// -P: an EqualizerAPO / AutoEq ParametricEQ.txt file
static bool load_peq(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "-P: can not open %s\n", path); return false; }
    char text[65536];
    const size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = '\0';
    char err[160];
    if (au_parametric_eq_parse(text, &peq, err, sizeof(err)) != AU_OK) {
        fprintf(stderr, "-P %s: %s\n", path, err);
        return false;
    }
    printf("parametric EQ: %d bands, preamp %+.1f dB from %s\n", peq.band_count, peq.preamp_db, path);
    have_peq = peq_on = true;
    return true;
}

// -e: a preset name or 10 comma-separated gains in dB
static bool parse_eq(const char *arg) {
    for (int i = 0; i < EQ_PRESETS - 1; i++)
        if (!strcmp(arg, eq_presets[i].name)) { eq_index = i; return true; }
    double g[AU_EQ_BANDS];
    const char *p = arg;
    for (int i = 0; i < AU_EQ_BANDS; i++) {
        char *end;
        g[i] = strtod(p, &end);
        if (end == p || g[i] < -30 || g[i] > 30) return false;
        p = end;
        if (i < AU_EQ_BANDS - 1) {
            if (*p != ',') return false;
            p++;
        }
    }
    if (*p) return false;
    memcpy(eq_presets[EQ_PRESETS - 1].gain_db, g, sizeof(g));
    have_custom_eq = true;
    eq_index = EQ_PRESETS - 1;
    return true;
}

static bool parse_bs2b(const char *arg) {
    if (!strcmp(arg, "default")) bs2b_mode = AU_BS2B_DEFAULT;
    else if (!strcmp(arg, "chu")) bs2b_mode = AU_BS2B_CHU_MOY;
    else if (!strcmp(arg, "jan")) bs2b_mode = AU_BS2B_JAN_MEIER;
    else return false;
    return true;
}

static void on_buffering(int32_t handle, bool buffering, double seconds, void *ud) {
    (void)ud;
    if (buffering) printf("  #%d buffering... %.1f s\n", handle, seconds);
    else printf("  #%d playing (%.1f s buffered)\n", handle, seconds);
}

static void on_stream_metadata(int32_t handle, const char *title, const char *artist, void *ud) {
    (void)ud;
    printf("> #%d now playing: %s%s%s\n", handle, artist ? artist : "", artist ? " - " : "", title ? title : "");
}

static void on_device(int32_t channels, int32_t rate, void *ud);

static void on_event(const AuEvent *e, void *ud) {
    switch (e->type) {
        case AU_EVENT_LOG: on_log(e->data.log.message, e->data.log.request, e->data.log.level, ud); break;
        case AU_EVENT_TRACK_ENDED:
            on_eof(e->data.ended.reason == AU_END_SKIPPED, e->data.ended.reason == AU_END_ERROR, e->handle, ud);
            break;
        case AU_EVENT_RESTARTED:
            on_restart(e->handle, e->data.restarted.position, e->data.restarted.from_loop,
                       e->data.restarted.loops_remaining, ud);
            break;
        case AU_EVENT_PREPARE_NEXT: on_prepare_next(e->handle, ud); break;
        case AU_EVENT_TRACK_STARTED: on_started(e->handle, ud); break;
        case AU_EVENT_DURATION: on_duration(e->handle, e->data.duration.seconds, ud); break;
        case AU_EVENT_DEVICE:
            if (e->data.device.ok) {
                printf("  device: %s%s\n", e->data.device.name ? e->data.device.name : "?",
                       e->data.device.fell_back ? " (the chosen one failed: system default)" : "");
                on_device(e->data.device.channels, e->data.device.sample_rate, ud);
            }
            else printf("  no output device could be opened\n");
            break;
        case AU_EVENT_DEVICES_CHANGED: on_devices_changed(ud); break;
        case AU_EVENT_BUFFERING:
            on_buffering(e->handle, e->data.buffering.active, e->data.buffering.buffered_seconds, ud);
            break;
        case AU_EVENT_STREAM_METADATA:
            on_stream_metadata(e->handle, e->data.metadata.title, e->data.metadata.artist, ud);
            break;
        default: break;
    }
}

static void on_device(int32_t channels, int32_t rate, void *ud) {
    (void)ud;
    printf("  output: %d Hz, %d channels\n", rate, channels);
    apply_effects(); // the equalizer is built for the channel count, and crossfeed/bs2b need stereo
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <music-dir | URL> [-u URL]... [-x crossfade-seconds] [-f filter] [-n count] [-s] [-c channels] [-d full|norm|\"downmix filters\"] [-r track|album] [-g] [-G cache-file] [-e preset|g1,...,g10] [-X] [-b default|chu|jan] [-P ParametricEQ.txt] [-L] [-C gentle|night]\n", argv[0]);
        return 1;
    }
    double crossfade = 0;
    bool shuffle = false;
    int channels = 0;
    const char *downmix = NULL;
    const char *replaygain = NULL;
    const char *cache_file = NULL;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-x") && i + 1 < argc) crossfade = atof(argv[++i]);
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) filter = argv[++i];
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) max_tracks = (size_t)atol(argv[++i]);
        else if (!strcmp(argv[i], "-s")) shuffle = true;
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) channels = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) downmix = argv[++i];
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) replaygain = argv[++i];
        else if (!strcmp(argv[i], "-g")) use_cache = true;
        else if (!strcmp(argv[i], "-G") && i + 1 < argc) { cache_file = argv[++i]; use_cache = true; }
        else if (!strcmp(argv[i], "-e") && i + 1 < argc) {
            if (!parse_eq(argv[++i])) {
                fprintf(stderr, "-e: bass, treble, v-shape, vocal or 10 gains (-30..30) like 4,3,0,0,0,0,0,0,2,3\n");
                return 1;
            }
        }
        else if (!strcmp(argv[i], "-X")) crossfeed_on = true;
        else if (!strcmp(argv[i], "-P") && i + 1 < argc) { if (!load_peq(argv[++i])) return 1; }
        else if (!strcmp(argv[i], "-L")) limiter_on = true;
        else if (!strcmp(argv[i], "-u") && i + 1 < argc) i++;   // streams, added below
        else if (!strcmp(argv[i], "-C") && i + 1 < argc) {
            const char *m = argv[++i];
            compressor_mode = !strcmp(m, "gentle") ? 1 : !strcmp(m, "night") ? 2 : -1;
            if (compressor_mode < 0) { fprintf(stderr, "-C: gentle or night\n"); return 1; }
        }
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) {
            if (!parse_bs2b(argv[++i])) { fprintf(stderr, "-b: default, chu or jan\n"); return 1; }
        }
    }
    if (use_cache && !replaygain) replaygain = "track"; // the cache only matters with ReplayGain on

    // Streams: a URL instead of the folder, and every -u
    for (int i = 1; i < argc; i++) {
        const char *u = i == 1 ? argv[1] : (!strcmp(argv[i - 1], "-u") ? argv[i] : NULL);
        if (u && au_is_network_url(u)) collect(u, NULL, FTW_F, NULL);
    }
    if (!au_is_network_url(argv[1]) && nftw(argv[1], collect, 20, FTW_PHYS) != 0) file_count = 0;
    if (file_count == 0) {
        fprintf(stderr, "No files found under %s\n", argv[1]);
        return 1;
    }
    if (shuffle) {
        srand((unsigned)time(NULL));
        for (size_t i = file_count - 1; i > 0; i--) {
            const size_t j = (size_t)rand() % (i + 1);
            char *t = files[i]; files[i] = files[j]; files[j] = t;
        }
    }

    const AuConfig config = {
        .app_name = "ffaudio manual player",
        .volume = 0.6f,
        .output_channels = channels,
        .on_event = on_event,
    };
    if (au_init(&config) != AU_OK) return 1;

    int n = 0;
    char **devs = au_list_devices(&n);
    for (int i = 0; i < n; i++) printf("device %d: %s\n", i, devs[i]);
    au_free_devices(devs, n);

    if (replaygain) {
        const AuReplayGainConfig rg = {
            .mode = !strcmp(replaygain, "album") ? AU_REPLAYGAIN_ALBUM : AU_REPLAYGAIN_TRACK,
            .fallback_gain_db = -6.0,
            .prevent_clipping = true,
            .measure_missing = use_cache,   // -g: tracks without a value are measured (waits up to 500 ms)
        };
        au_set_replaygain(&rg);
        printf("ReplayGain: %s mode, -6 dB for untagged files\n", rg.mode == AU_REPLAYGAIN_ALBUM ? "album" : "track");
    }

    if (use_cache) {
        if (cache_file && au_gain_cache_set_file(cache_file) != AU_OK)
            fprintf(stderr, "could not use cache file %s\n", cache_file);
        char where[1024] = "";
        au_gain_cache_get_file(where, sizeof(where));
        printf("ReplayGain cache: %s (%d entries); uncached files are measured in the background\n",
               where[0] ? where : "memory only", au_gain_cache_count());
        const AuGainScanConfig scan = { .on_cached = on_cached };
        au_gain_scan_configure(&scan);
    }

    if (downmix) {
        const AU_DOWNMIX mode = !strcmp(downmix, "full") ? AU_DOWNMIX_FULL_LEVEL
                              : !strcmp(downmix, "norm") ? AU_DOWNMIX_NORMALIZED : AU_DOWNMIX_CUSTOM;
        if (au_set_downmix(mode, mode == AU_DOWNMIX_CUSTOM ? downmix : NULL) != AU_OK)
            fprintf(stderr, "invalid downmix \"%s\", using the default\n", downmix);
        else
            printf("downmix: %s\n", downmix);
    }

    if (crossfade > 0) {
        const AuFade fade = { .seconds = crossfade, .curve = AU_FADE_EQUAL_POWER };
        const AuFade quick = { .seconds = 0.4, .curve = AU_FADE_S_CURVE };
        const AuCrossfade xf = { .on_track_end = fade, .on_skip = quick, .on_play = quick,
                                     .on_stop = quick, .on_pause = { .seconds = 0.2, .curve = AU_FADE_S_CURVE } };
        au_set_crossfade(&xf);
        printf("crossfade: %.1f s\n", crossfade);
    }

    const AuPlayConfig cfg = play_config();
    const char *first_file = take_next();
    au_play(first_file, &cfg);

    setvbuf(stdout, NULL, _IOLBF, 0);
    enable_key_mode();
    print_help();

    bool paused = false, muted = false, keep_pitch = true;
    int c;
    while ((c = getchar()) != EOF && c != 'q') {
        switch (c) {
            case 'n': {
                // Use the queued track if there is one, otherwise start the next file right away
                if (get_queued()) {
                    printf("  next (queued track)\n");
                    au_skip();
                }
                else {
                    const char *f = take_next();
                    if (!f) { printf("  (no more files)\n"); break; }
                    const AuPlayConfig next_cfg = play_config();
                    printf("  next: #%d %s\n", au_play(f, &next_cfg), f);
                }
                if (paused) {
                    au_pause(false); // au_skip() does not resume on its own
                    paused = false;
                }
                break;
            }
            case 'p':
                paused = !paused;
                au_pause(paused);
                printf("  %s\n", paused ? "paused" : "resumed");
                break;
            case 's':
                au_stop();
                paused = false;
                printf("  stopped (n plays the next file)\n");
                break;
            case 'm':
                muted = !muted;
                au_set_muted(muted);
                printf("  %s\n", muted ? "muted" : "unmuted");
                break;
            case '+':
            case '=': {
                const float v = fminf(1.0f, status().volume + 0.05f);
                au_set_volume(v);
                printf("  volume %.0f%%\n", v * 100);
                break;
            }
            case '-': {
                const float v = fmaxf(0.0f, status().volume - 0.05f);
                au_set_volume(v);
                printf("  volume %.0f%%\n", v * 100);
                break;
            }
            case '>':
            case '<': {
                const double t = status().position;
                if (t < 0) { printf("  (nothing playing)\n"); break; }
                const double target = c == '>' ? t + 10 : (t > 10 ? t - 10 : 0);
                au_seek(target);
                printf("  seek to %.1f s\n", target);
                break;
            }
            case ' ':
            case '\n':
                printf("  #%d  %.2f / %.2f s\n", status().handle, status().position, status().duration);
                break;
            case '[':
            case ']': {
                double sp = status().speed + (c == ']' ? 0.25 : -0.25);
                sp = sp < 0.25 ? 0.25 : (sp > 4.0 ? 4.0 : sp);
                au_set_speed(sp, keep_pitch);
                printf("  speed %.2fx%s\n", sp, keep_pitch ? "" : " (tape)");
                break;
            }
            case '\\':
                keep_pitch = !keep_pitch;
                au_set_speed(status().speed, keep_pitch);
                printf("  %s\n", keep_pitch ? "speed keeps the pitch" : "speed changes the pitch (tape)");
                break;
            case 'e':
                pthread_mutex_lock(&fx_lock);
                eq_index = (eq_index + 1) % EQ_PRESETS;
                if (eq_index == EQ_PRESETS - 1 && !have_custom_eq) eq_index = 0; // no -e gains given
                pthread_mutex_unlock(&fx_lock);
                apply_effects();
                break;
            case 'x':
                pthread_mutex_lock(&fx_lock);
                crossfeed_on = !crossfeed_on;
                pthread_mutex_unlock(&fx_lock);
                apply_effects();
                break;
            case 'b':
                pthread_mutex_lock(&fx_lock);
                bs2b_mode = bs2b_mode >= AU_BS2B_JAN_MEIER ? -1 : bs2b_mode + 1;
                pthread_mutex_unlock(&fx_lock);
                apply_effects();
                break;
            case 'f':
                print_effects();
                break;
            case 'a':
                pthread_mutex_lock(&fx_lock);
                if (have_peq) peq_on = !peq_on;
                pthread_mutex_unlock(&fx_lock);
                if (have_peq) apply_effects();
                else printf("  (no parametric EQ: start with -P ParametricEQ.txt)\n");
                break;
            case '1':
            case '2':
            case '3':
            case '4': {
                pthread_mutex_lock(&fx_lock);
                double *v = c <= '2' ? &bass_db : &treble_db;
                *v += (c == '2' || c == '4') ? 2 : -2;
                if (*v > 12) *v = 12;
                if (*v < -12) *v = -12;
                pthread_mutex_unlock(&fx_lock);
                apply_effects();
                break;
            }
            case 'l':
            case 'r':
                change_balance(c == 'l' ? -0.1 : 0.1);
                break;
            case 'w': {
                static const double widths[] = { 1, 1.5, 2, 0.5 };
                pthread_mutex_lock(&fx_lock);
                int i = 0;
                while (i < 4 && widths[i] != width) i++;
                width = widths[(i + 1) % 4];
                mono = false;
                pthread_mutex_unlock(&fx_lock);
                apply_effects();
                break;
            }
            case 'o':
                pthread_mutex_lock(&fx_lock);
                mono = !mono;
                pthread_mutex_unlock(&fx_lock);
                apply_effects();
                break;
            case 'c':
                pthread_mutex_lock(&fx_lock);
                swap_lr = !swap_lr;
                pthread_mutex_unlock(&fx_lock);
                apply_effects();
                break;
            case 'C':
                pthread_mutex_lock(&fx_lock);
                compressor_mode = (compressor_mode + 1) % 3;
                pthread_mutex_unlock(&fx_lock);
                apply_effects();
                break;
            case 'L':
                pthread_mutex_lock(&fx_lock);
                limiter_on = !limiter_on;
                pthread_mutex_unlock(&fx_lock);
                apply_effects();
                break;
            case 'h':
            case '?':
                print_help();
                break;
            default:
                break;
        }
    }

    restore_terminal();
    au_gain_scan_stop();
    au_shutdown();
    for (size_t i = 0; i < file_count; i++) free(files[i]);
    free(files);
    return 0;
}
