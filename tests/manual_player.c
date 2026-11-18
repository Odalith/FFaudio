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
 *   manual_player <music-dir> [-x crossfade-seconds] [-f "filter string"] [-n count] [-s] [-c channels] [-d full|norm|"filters"] [-r track|album]
 *
 * Plays the files under <music-dir> (shuffled with -s), queueing the next one from on_prepare_next.
 * Keys (no Enter needed in a terminal): n = next, p = pause/resume, s = stop, m = mute, + / - = volume,
 * > / < = seek +/-10 s, space = show position, h = help, q = quit
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

#include "../lib/ffaudio.h"

static char **files = NULL;
static size_t file_count = 0, file_cap = 0;
static size_t next_index = 0;
static size_t max_tracks = (size_t)-1;
static const char *filter = NULL;

// Handle of the track queued with au_set_next() that has not started yet (0 = none).
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
    printf("keys: n=next  p=pause/resume  s=stop  m=mute  +/-=volume  >/<=seek 10s  space=position  h=help  q=quit\n");
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

static const char *take_next(void) {
    if (next_index >= file_count || next_index >= max_tracks) return NULL;
    return files[next_index++];
}

static PlayAudioConfig play_config(void) {
    return (PlayAudioConfig){ .av_filtergraph = filter };
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
    AuMediaInfo *info = au_get_track_info(handle);
    double gain = 0;
    const bool has_gain = au_get_track_gain_db(handle, &gain) == AU_OK && gain != 0;
    if (info) {
        const char *artist = au_media_tag(info, "artist"), *title = au_media_tag(info, "title");
        printf("> now playing #%d: %s - %s  [%s %d Hz %d ch%s]", handle, artist ? artist : "?",
               title ? title : au_media_filename(info), au_media_codec(info), au_media_sample_rate(info),
               au_media_channels(info), au_media_replaygain(info).has_track_gain ? ", ReplayGain" : "");
        au_media_info_free(info);
    }
    else {
        printf("> now playing #%d", handle);
    }
    if (has_gain) printf("  gain %+.2f dB", gain);
    printf("\n");
}

static void on_duration(int32_t handle, double duration, void *ud) {
    (void)ud;
    printf("  #%d duration %.1f s\n", handle, duration);
}

static void queue_next_file(void) {
    const char *f = take_next();
    if (!f) { printf("  (no more files)\n"); return; }
    const PlayAudioConfig cfg = play_config();
    const int32_t h = au_set_next(f, &cfg);
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
    const char *downmix = NULL;
    const char *replaygain = NULL;
static void on_device(int32_t channels, int32_t rate, void *ud) {
    (void)ud;
    printf("  output: %d Hz, %d channels\n", rate, channels);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <music-dir> [-x crossfade-seconds] [-f filter] [-n count] [-s] [-c channels] [-d full|norm|\"downmix filters\"] [-r track|album]\n", argv[0]);
        return 1;
    }
    double crossfade = 0;
    bool shuffle = false;
    int channels = 0;
    const char *downmix = NULL;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-x") && i + 1 < argc) crossfade = atof(argv[++i]);
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) filter = argv[++i];
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) max_tracks = (size_t)atol(argv[++i]);
        else if (!strcmp(argv[i], "-s")) shuffle = true;
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) channels = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) downmix = argv[++i];
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) replaygain = argv[++i];
    }

    if (nftw(argv[1], collect, 20, FTW_PHYS) != 0 || file_count == 0) {
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

    const InitializeConfig config = {
        .app_name = "ffaudio manual player",
        .initial_volume = 60,
        .output_channels = channels,
        .on_log = on_log,
        .on_eof = on_eof,
        .on_restart = on_restart,
        .on_prepare_next = on_prepare_next,
        .on_track_started = on_started,
        .on_duration_update = on_duration,
        .on_audio_device_update = on_device,
    };
    if (au_initialize(&config) != AU_OK) return 1;

    int n = 0;
    char **devs = NULL;
    if (au_get_audio_devices(&n, &devs) == AU_OK) {
        for (int i = 0; i < n; i++) { printf("device %d: %s\n", i, devs[i]); free(devs[i]); }
        free(devs);
    }
    au_configure_audio_device(NULL);

    if (replaygain) {
        const AuReplayGainConfig rg = {
            .mode = !strcmp(replaygain, "album") ? AU_REPLAYGAIN_ALBUM : AU_REPLAYGAIN_TRACK,
            .fallback_gain_db = -6.0,
            .prevent_clipping = true,
        };
        au_set_replaygain(&rg);
        printf("ReplayGain: %s mode, -6 dB for untagged files\n", rg.mode == AU_REPLAYGAIN_ALBUM ? "album" : "track");
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
        const CrossfadeConfig xf = { .on_track_end = fade, .on_skip = quick, .on_play = quick,
                                     .on_stop = quick, .on_pause = { .seconds = 0.2, .curve = AU_FADE_S_CURVE } };
        au_set_crossfade(&xf);
        printf("crossfade: %.1f s\n", crossfade);
    }

    const PlayAudioConfig cfg = play_config();
    au_play_audio(take_next(), &cfg);

    setvbuf(stdout, NULL, _IOLBF, 0);
    enable_key_mode();
    print_help();

    bool paused = false, muted = false;
    int c;
    while ((c = getchar()) != EOF && c != 'q') {
        switch (c) {
            case 'n': {
                // Use the queued track if there is one, otherwise start the next file right away
                if (get_queued()) {
                    printf("  next (queued track)\n");
                    au_skip_to_next();
                }
                else {
                    const char *f = take_next();
                    if (!f) { printf("  (no more files)\n"); break; }
                    const PlayAudioConfig next_cfg = play_config();
                    printf("  next: #%d %s\n", au_play_audio(f, &next_cfg), f);
                }
                if (paused) {
                    au_pause_audio(false); // au_skip_to_next() does not resume on its own
                    paused = false;
                }
                break;
            }
            case 'p':
                paused = !paused;
                au_pause_audio(paused);
                printf("  %s\n", paused ? "paused" : "resumed");
                break;
            case 's':
                au_stop_audio();
                paused = false;
                printf("  stopped (n plays the next file)\n");
                break;
            case 'm':
                muted = !muted;
                au_mute_audio(muted);
                printf("  %s\n", muted ? "muted" : "unmuted");
                break;
            case '+':
            case '=': {
                const int v = au_get_audio_volume() + 5 > 100 ? 100 : au_get_audio_volume() + 5;
                au_set_audio_volume(v);
                printf("  volume %d\n", v);
                break;
            }
            case '-': {
                const int v = au_get_audio_volume() - 5 < 0 ? 0 : au_get_audio_volume() - 5;
                au_set_audio_volume(v);
                printf("  volume %d\n", v);
                break;
            }
            case '>':
            case '<': {
                const double t = au_get_audio_play_time();
                if (t < 0) { printf("  (nothing playing)\n"); break; }
                const double target = c == '>' ? t + 10 : (t > 10 ? t - 10 : 0);
                au_seek_time((int64_t)(target * 1000));
                printf("  seek to %.1f s\n", target);
                break;
            }
            case ' ':
            case '\n':
                printf("  #%d  %.2f / %.2f s\n", au_get_current_handle(), au_get_audio_play_time(), au_get_audio_duration());
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
    au_shutdown();
    for (size_t i = 0; i < file_count; i++) free(files[i]);
    free(files);
    return 0;
}
