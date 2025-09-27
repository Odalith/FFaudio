// Converted from C++ to C
// Created by malkj on 27/07/25. Converted to C on 24/09/25.

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

#include "ffaudio.h"

#define PLAY_COUNT 100 // -1 == play all
#define SKIP_AFTER_SECONDS -1 // -1 == disable

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

static void play_next(void);

static void* timer_thread_routine(void *arg) {
    int delay_seconds = *(int*)arg;
    free(arg);

    if (skip_next_timer) {
        skip_next_timer = false;
        return NULL;
    }

    sleep((unsigned int)delay_seconds);
    play_next();
    return NULL;
}

static void schedule_after_seconds(int seconds) {
    pthread_t tid;
    int *arg = (int*)malloc(sizeof(int));
    if (!arg) return;
    *arg = seconds;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&tid, &attr, timer_thread_routine, arg) != 0) {
        pthread_attr_destroy(&attr);
        free(arg);
        return;
    }
    pthread_attr_destroy(&attr);
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

    play_audio(song, NULL, NULL);
    ++queue_pos;

#if SKIP_AFTER_SECONDS > 0
    schedule_after_seconds(SKIP_AFTER_SECONDS);
#endif
}

static void eof_callback(bool is_eof_from_skip) {
    printf("EOF\n");

    if (is_eof_from_skip) {
        printf("Skipped\n");
        return;
    }

    if (SKIP_AFTER_SECONDS > 0) {
        skip_next_timer = true;
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
    initialize("Nachtul", 50, 0, error_callback, eof_callback, restart_callback);

    // Find audio devices
    int n;
    char **devs;
    if (get_audio_devices(&n, &devs)==0) {
        for (int i = 0; i < n; ++i) {
            printf("%d %s\n", i, devs[i]);
            free(devs[i]);
        }
    }

    configure_audio_device(NULL, -1, true);


    play_next();

    wait_loop();

    free_queue();
    return 0;
}
