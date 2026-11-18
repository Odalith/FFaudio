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
 *   manual_player <music-dir> [-x crossfade-seconds] [-f "filter string"] [-n count] [-s]
 *
 * Plays the files under <music-dir> (shuffled with -s), queueing the next one from on_prepare_next.
 * Commands on stdin: n = skip, p = pause/resume, s = stop, + / - = volume, > = seek +10 s, q = quit
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
#include <sys/stat.h>

#include "../lib/ffaudio.h"

static char **files = NULL;
static size_t file_count = 0, file_cap = 0;
static size_t next_index = 0;
static size_t max_tracks = (size_t)-1;
static const char *filter = NULL;

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
    printf("  ended #%d%s%s\n", handle, skip ? " (skipped)" : "", error ? " (error)" : "");
}

static void on_started(int32_t handle, void *ud) {
    (void)ud;
    printf("> now playing #%d (%.1f s)\n", handle, au_get_audio_duration());
}

static void on_prepare_next(int32_t current, void *ud) {
    (void)current; (void)ud;
    const char *f = take_next();
    if (!f) { printf("  (queue empty)\n"); return; }
    const PlayAudioConfig cfg = play_config();
    const int32_t h = au_set_next(f, &cfg);
    printf("  queued #%d %s\n", h, f);
}

static void on_restart(int32_t handle, double pos, bool looped, int32_t remaining, void *ud) {
    (void)ud;
    printf("  #%d restarted at %.2f%s (loops left %d)\n", handle, pos, looped ? " by looping" : "", remaining);
}

static void on_device(int32_t channels, int32_t rate, void *ud) {
    (void)ud;
    printf("  output: %d Hz, %d channels\n", rate, channels);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <music-dir> [-x crossfade-seconds] [-f filter] [-n count] [-s]\n", argv[0]);
        return 1;
    }
    double crossfade = 0;
    bool shuffle = false;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-x") && i + 1 < argc) crossfade = atof(argv[++i]);
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) filter = argv[++i];
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) max_tracks = (size_t)atol(argv[++i]);
        else if (!strcmp(argv[i], "-s")) shuffle = true;
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
        .on_log = on_log,
        .on_eof = on_eof,
        .on_restart = on_restart,
        .on_prepare_next = on_prepare_next,
        .on_track_started = on_started,
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

    printf("commands: n=skip p=pause s=stop +/-=volume >=seek+10s q=quit\n");
    int c;
    while ((c = getchar()) != EOF && c != 'q') {
        switch (c) {
            case 'n': au_skip_to_next(); break;
            case 'p': au_pause_audio(!au_is_paused()); break;
            case 's': au_stop_audio(); break;
            case '+': au_set_audio_volume(au_get_audio_volume() + 5 > 100 ? 100 : au_get_audio_volume() + 5); break;
            case '-': au_set_audio_volume(au_get_audio_volume() - 5 < 0 ? 0 : au_get_audio_volume() - 5); break;
            case '>': au_seek_time((int64_t)((au_get_audio_play_time() + 10) * 1000)); break;
            case '\n': printf("  %.2f / %.2f s\n", au_get_audio_play_time(), au_get_audio_duration()); break;
            default: break;
        }
    }

    au_shutdown();
    for (size_t i = 0; i < file_count; i++) free(files[i]);
    free(files);
    return 0;
}
