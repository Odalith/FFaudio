/*
 * Renders a fixed playlist through SDL's disk driver and writes the audio, without the silence before and after it,
 * to a file (float32, interleaved stereo, 44100 Hz). Nothing in the playlist depends on timing: tracks are queued
 * from on_prepare_next and no API call waits for anything. Two runs of the same build give the same samples, so a
 * render from before a change, compared with one after it, shows whether the change altered what is heard.
 *
 * Not a CTest: the reference has to come from the same machine and FFmpeg build (resamplers and lossy decoders
 * differ between versions).
 *
 *   golden_render <fixture-dir> <out.f32> [reference.f32]
 *
 * With a reference, the render is compared with it; the exit status is 0 when they are identical, 3 when not.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "SDL2/SDL.h"
#include "../lib/ffaudio.h"
#include "../lib/ffaudio_loudness.h"

typedef struct Item {
    const char *file;
    const char *filters;
} Item;

static const Item playlist[] = {
    { "join_a.flac", NULL },             // 48 kHz, resampled; gapless into the next
    { "join_b.flac", NULL },
    { "long_6s.flac", NULL },            // crossfades into the next (0.5 s, equal power)
    { "long_6s.flac", NULL },
    { "rg_tagged.flac", NULL },          // ReplayGain -6 dB
    { "s48_51.flac", NULL },             // 5.1, mixed down
    { "s44_mono.wav", NULL },
    { "s44_stereo.flac", "volume=0.5" }, // through the track's own filter graph
    { "s48_stereo.ogg", NULL },          // Vorbis
};
#define N_ITEMS ((int)(sizeof(playlist) / sizeof(playlist[0])))
#define CHANNELS 2
#define RATE 44100

static char paths[N_ITEMS][1024];
static SDL_atomic_t next_item;   // next playlist item to queue
static SDL_atomic_t ended, errors;

static void queue(const int i) {
    const PlayAudioConfig cfg = { .av_filtergraph = playlist[i].filters };
    if (i == 0) au_play_audio(paths[i], &cfg);
    else au_set_next(paths[i], &cfg);
}

static void on_prepare_next(int32_t handle, void *user) {
    (void)handle; (void)user;
    const int i = SDL_AtomicAdd(&next_item, 1);
    if (i < N_ITEMS) queue(i);
}

static void on_eof(bool skip, bool error, int32_t handle, void *user) {
    (void)skip; (void)handle; (void)user;
    if (error) SDL_AtomicAdd(&errors, 1);
    SDL_AtomicAdd(&ended, 1);
}

static void on_log(const char *message, int64_t request, enum AU_LOG_LEVEL level, void *user) {
    (void)request; (void)user;
    if (level >= AU_LOG_WARNING) fprintf(stderr, "[ffaudio %d] %s\n", (int)level, message);
}

static void set_env(const char *name, const char *value) {
#ifdef _WIN32
    char buf[1200];
    snprintf(buf, sizeof(buf), "%s=%s", name, value);
    _putenv(buf);
#else
    setenv(name, value, 1);
#endif
}

static float *read_floats(const char *path, size_t *count) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    float *data = malloc((size_t)size + sizeof(float));
    if (data && fread(data, 1, (size_t)size, f) != (size_t)size) { free(data); data = NULL; }
    fclose(f);
    *count = (size_t)size / sizeof(float);
    return data;
}

static bool silent_frame(const float *s) {
    for (int c = 0; c < CHANNELS; c++)
        if (s[c] != 0.0f) return false;
    return true;
}

static int compare(const float *a, const size_t na, const char *ref_path) {
    size_t nb = 0;
    float *b = read_floats(ref_path, &nb);
    if (!b) { fprintf(stderr, "cannot read %s\n", ref_path); return 2; }
    const size_t n = na < nb ? na : nb;
    size_t differing = 0, first = (size_t)-1, worst_at = 0;
    double worst = 0;
    for (size_t i = 0; i < n; i++) {
        const double d = fabs((double)a[i] - (double)b[i]);
        if (d > 0) {
            differing++;
            if (first == (size_t)-1) first = i;
            if (d > worst) { worst = d; worst_at = i; }
        }
    }
    printf("render %zu frames, reference %zu frames; %zu samples differ", na / CHANNELS, nb / CHANNELS, differing);
    if (differing)
        printf(" (first at %.4f s, largest %.3g at %.4f s)", (double)(first / CHANNELS) / RATE, worst,
               (double)(worst_at / CHANNELS) / RATE);
    printf("\n");
    free(b);
    return na == nb && differing == 0 ? 0 : 3;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <fixture-dir> <out.f32> [reference.f32]\n", argv[0]);
        return 2;
    }
    char capture[1100];
    snprintf(capture, sizeof(capture), "%s.capture", argv[2]);
    remove(capture);
    set_env("SDL_AUDIODRIVER", "disk");
    set_env("SDL_DISKAUDIOFILE", capture);          // SDL2
    set_env("SDL_AUDIO_DISK_OUTPUT_FILE", capture); // SDL3 (sdl2-compat)
    for (int i = 0; i < N_ITEMS; i++) snprintf(paths[i], sizeof(paths[i]), "%s/%s", argv[1], playlist[i].file);

    au_gain_cache_set_file(""); // memory only: no values from (or for) the user's cache
    const InitializeConfig config = {
        .app_name = "ffaudio-golden",
        .initial_volume = 100,
        .prepare_next_seconds = 4,
        .output_channels = CHANNELS,
        .output_sample_rate = RATE,
        .on_log = on_log,
        .on_eof = on_eof,
        .on_prepare_next = on_prepare_next,
    };
    if (au_initialize(&config) != AU_OK || au_configure_audio_device(NULL) != AU_OK) {
        fprintf(stderr, "could not start the player\n");
        return 2;
    }
    const CrossfadeConfig xf = { .on_track_end = { .seconds = 0.5, .curve = AU_FADE_EQUAL_POWER },
                                 .min_track_seconds = 2.0 };
    au_set_crossfade(&xf);
    const AuReplayGainConfig rg = { .mode = AU_REPLAYGAIN_TRACK };
    au_set_replaygain(&rg);
    SDL_AtomicSet(&next_item, 2);
    queue(0);
    queue(1);

    const Uint32 deadline = SDL_GetTicks() + 60000;
    while (SDL_AtomicGet(&ended) < N_ITEMS && (Sint32)(deadline - SDL_GetTicks()) > 0) SDL_Delay(20);
    SDL_Delay(400); // the tail reaches the device
    au_shutdown();
    if (SDL_AtomicGet(&ended) < N_ITEMS || SDL_AtomicGet(&errors)) {
        fprintf(stderr, "%d of %d tracks ended, %d with errors\n", SDL_AtomicGet(&ended), N_ITEMS,
                SDL_AtomicGet(&errors));
        return 2;
    }

    size_t n = 0;
    float *s = read_floats(capture, &n);
    remove(capture);
    if (!s) { fprintf(stderr, "no capture\n"); return 2; }
    size_t first = 0, last = n / CHANNELS;
    while (first < last && silent_frame(s + first * CHANNELS)) first++;
    while (last > first && silent_frame(s + (last - 1) * CHANNELS)) last--;
    const float *audio = s + first * CHANNELS;
    const size_t count = (last - first) * CHANNELS;
    FILE *out = fopen(argv[2], "wb");
    if (!out || fwrite(audio, sizeof(float), count, out) != count) { fprintf(stderr, "cannot write %s\n", argv[2]); return 2; }
    fclose(out);
    printf("%s: %.3f s of audio\n", argv[2], (double)(last - first) / RATE);

    const int ret = argc > 3 ? compare(audio, count, argv[3]) : 0;
    free(s);
    return ret;
}
