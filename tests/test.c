/*
* Copyright (c) 2003 Fabrice Bellard, 2025 Odalith
 *
 * This file was part of FFmpeg, particularly FFplay.
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

#define _XOPEN_SOURCE 700
#define DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <ftw.h>
#include <sys/stat.h>

#include "../lib/ffaudio.h"

#define PLAY_COUNT 100 // -1 == play all
#define SKIP_AFTER_SECONDS 5 // -1 == disable

static char **queue_files = NULL;
static size_t queue_count = 0;
static size_t queue_capacity = 0;
static size_t queue_pos = 0;
static volatile bool skip_next_timer = false;

static void ensure_capacity(size_t needed) {
    if (queue_capacity >= needed) return;
    size_t new_cap = queue_capacity ? queue_capacity * 2 : 128;
    while (new_cap < needed) new_cap *= 2;
    char **new_arr = (char**)realloc(queue_files, new_cap * sizeof(char*));
    if (!new_arr) {
        perror("realloc");
        exit(1);
    }
    queue_files = new_arr;
    queue_capacity = new_cap;
}

static void free_queue(void) {
    if (!queue_files) return;
    for (size_t i = 0; i < queue_count; ++i) free(queue_files[i]);
    free(queue_files);
    queue_files = NULL;
    queue_count = 0;
    queue_capacity = 0;
}

static int collect_file_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    (void)sb; (void)ftwbuf;
    if (typeflag == FTW_F) {
        ensure_capacity(queue_count + 1);
        queue_files[queue_count] = strdup(fpath);
        if (!queue_files[queue_count]) {
            perror("strdup");
            return -1; // abort nftw
        }
        queue_count++;
    }
    return 0;
}

static void shuffle_queue(void) {
    if (queue_count <= 1) return;
    // Fisher-Yates shuffle
    for (size_t i = queue_count - 1; i > 0; --i) {
        size_t j = (size_t)(rand() % (int)(i + 1));
        char *tmp = queue_files[i];
        queue_files[i] = queue_files[j];
        queue_files[j] = tmp;
    }
}

static void error_callback(const char* message, int request) {
    (void)request;
    fprintf(stdout, "Error: %s\n", message ? message : "(null)");
}

static void play_next(void) {
    if (queue_pos >= queue_count || (PLAY_COUNT > 0 && queue_pos >= (size_t)PLAY_COUNT)) {
        printf("Done\n");
        free_queue();
        exit(0);
    }

    const char *song = queue_files[queue_pos];
    printf("%zu Playing %s\n", queue_pos + 1, song);

    play_audio(song, NULL);
    ++queue_pos;
}

static void eof_callback(bool is_eof_from_skip) {
    printf("EOF\n");

    if (is_eof_from_skip) {
        printf("Skipped\n");
        return;
    }

    play_next();
}

static void restart_callback(void) { /* no-op */ }

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Pass the media directory as an argument\n");
        return 1;
    }

    const char *root = argv[1];

    // Collect files recursively
    if (nftw(root, collect_file_cb, 20, FTW_PHYS) != 0) {
        perror("nftw");
        free_queue();
        return 1;
    }

    if (queue_count == 0) {
        fprintf(stderr, "No files found under: %s\n", root);
        return 1;
    }

    // Shuffle queue
    srand((unsigned int)time(NULL));
    shuffle_queue();

    // Setup
    const InitializeConfig config = {
        .app_name = "Test App",
        .initial_volume = 50,
        .initial_loop_count = 0,
        .on_error = error_callback,
        .on_eof = eof_callback,
        .on_restart = restart_callback
    };

    initialize(&config);

    // Find audio devices
    int n;
    char **devs;
    if (get_audio_devices(&n, &devs)==0) {
        for (int i = 0; i < n; ++i) {
            printf("%d %s\n", i, devs[i]);
            free(devs[i]);
        }
    }


    configure_audio_device(NULL);


    play_next();

    if (SKIP_AFTER_SECONDS > 0) {
        for (int i = 0; i < PLAY_COUNT; ++i) {
            sleep((unsigned int)SKIP_AFTER_SECONDS);

            play_next();
        }
    }
    else {
        wait_loop();
    }


    free_queue();
    shutdown();
    return 0;
}
