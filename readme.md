⚠️ This project is in alpha. The API may still change between versions.

### FFaudio is a music playback library for C99, built on FFmpeg & SDL2.
It started as a heavily modified FFplay, so credit for the original design goes to the FFmpeg team and contributors.
The goal: a full-featured, high-quality player engine that covers ~98% of music playback needs.

## Features
- Plays basically any file with an audio stream that FFmpeg can decode
  (tested with mp3, flac, wav, aiff, m4a/aac, wma, ogg/oga and dsf)
- **Gapless playback**: queue the next track with `au_set_next()`; the handover happens inside the audio
  callback on the exact sample, and resampler tails are flushed so nothing is lost at the join
- **Crossfades** with a configurable length and curve for every transition: natural track end, skip,
  play, stop and pause/resume, plus per-track overrides
- **High quality output**: everything is mixed in 32-bit float at the device's own sample rate and channel
  layout, resampled with soxr when available; integer devices get TPDF dither
- **Filters**: any FFmpeg filtergraph per track and/or globally, changeable while playing, with helpers that
  build equalizer, crossfeed, bs2b, ReplayGain and EBU R128 loudness strings
- Volume with smooth ramps (no zipper noise), mute, pause, seek by time or percentage, looping
- Callbacks for track start/end, prepare-next, restarts (seek/loop), duration, device changes and logs
- Follows the system default device, or uses a chosen one and falls back to the default if it disappears
- Surround files up to 7.1 play on multichannel outputs with SDL's speaker order, or are mixed down at full level
  (like mpv), normalized (never clips), or with your own filters (e.g. `sofalizer` for headphones)
- Safe to drive from any thread, including from inside its own callbacks
- Linux tested; Windows builds with MSYS2 (see CI); macOS, Android and iOS untested
- LGPL-2.1

## Building
Requires CMake ≥ 3.20, a C99 compiler, SDL2 ≥ 2.24 and FFmpeg ≥ 6.0 (libavcodec, libavformat, libavfilter,
libswresample, libavutil) with pkg-config.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure   # needs the ffmpeg CLI to generate test files
cmake --install build                        # installs headers, the library, ffaudio.pc and CMake targets
```

Options: `-DBUILD_SHARED_LIBS=OFF` for a static library, `-DFFAUDIO_SANITIZER=address|thread|undefined`,
`-DFFAUDIO_BUILD_TESTS=OFF`, `-DFFAUDIO_BUILD_MANUAL_PLAYER=OFF`.

Consumers: `pkg-config --cflags --libs ffaudio`, or `find_package` the installed `ffaudio::ffaudio` target.

## Usage

### 1. Initialize and open a device
```c
#include <ffaudio.h>

static void on_eof(bool skipped, bool error, int32_t handle, void *user) {
    printf("track %d ended%s%s\n", handle, skipped ? " (skipped)" : "", error ? " (error)" : "");
}

static void on_prepare_next(int32_t current, void *user) {
    // The current track ends soon: queue the next one for a gapless (or crossfaded) transition
    au_set_next(next_file_in_my_playlist(), NULL);
}

const InitializeConfig config = {
    .app_name = "My Player",
    .initial_volume = 80,
    .on_eof = on_eof,
    .on_prepare_next = on_prepare_next,
    // .on_track_started, .on_restart, .on_duration_update, .on_audio_device_update, .on_log, .user_data
};
au_initialize(&config);
au_configure_audio_device(NULL);   // NULL = system default (followed when it changes)
```

### 2. Play
```c
int32_t handle = au_play_audio("/music/song.flac", NULL);

const PlayAudioConfig cfg = { .skip_seconds = 30, .play_duration = 60 };
au_set_next("/music/next.mp3", &cfg);  // starts gaplessly when song.flac ends

au_pause_audio(true);
au_seek_time(90 * 1000);
au_skip_to_next();
au_stop_audio();
```
Playback functions never block: they are queued and run in order on ffaudio's control thread.
`au_get_audio_play_time()`, `au_get_audio_duration()` and `au_get_current_handle()` describe the audible track.

### 3. Crossfades
```c
const CrossfadeConfig xf = {
    .on_track_end = { .seconds = 6.0, .curve = AU_FADE_EQUAL_POWER },
    .on_skip      = { .seconds = 0.8, .curve = AU_FADE_S_CURVE },
    .on_play      = { .seconds = 0.8, .curve = AU_FADE_S_CURVE },
    .on_stop      = { .seconds = 0.5, .curve = AU_FADE_LINEAR },
    .on_pause     = { .seconds = 0.2, .curve = AU_FADE_S_CURVE },
};
au_set_crossfade(&xf);   // NULL = gapless + hard cuts

// One track can ask for something else, e.g. no crossfade inside a live album:
const AuFade none = { 0 };
const PlayAudioConfig gapless = { .fade_in = &none };
au_set_next("/music/live/track2.flac", &gapless);
```
Curves: linear, equal power, s-curve, logarithmic, exponential, or your own `AuFadeCurveFn`.
Tracks shorter than `min_track_seconds` (default: twice the fade) and tracks of unknown length play gapless.
Seeks always cut.

### 4. Filters
Filters run after the input has been converted to the output format, so they always see float samples at the
device's rate and channel count (`au_get_output_spec()`).

```c
#include <ffaudio_filters.h>

int rate, channels;
au_get_output_spec(&rate, &channels);

AuFilterBuilder *fb = au_fb_create();
AuEqualizer eq = au_equalizer_defaults();   // 31 Hz ... 16 kHz octave bands
eq.gain_db[0] = 3.0;
eq.gain_db[9] = -2.0;
eq.preamp_db = -3.0;                        // headroom for the boost
au_fb_add_equalizer(fb, channels, &eq);
au_fb_add_crossfeed(fb, NULL);

char *filters = au_fb_build(fb);
au_set_global_filtergraph(filters);         // every track, changes apply while playing
au_free(filters);
au_fb_free(fb);

// Per track, e.g. ReplayGain from the file's tags (gain -6.2 dB, peak 0.98)
AuFilterBuilder *rg = au_fb_create();
au_fb_add_replaygain(rg, -6.2, 0.98, 0.0, true);
const PlayAudioConfig cfg = { .av_filtergraph = au_fb_peek(rg) };
au_play_audio("/music/song.flac", &cfg);
au_fb_free(rg);

// Runtime parameter changes without rebuilding the graph
au_send_filter_command(0, "volume", "volume", "0.5");
```
Any [FFmpeg filter](https://ffmpeg.org/ffmpeg-filters.html) string works; `au_validate_filtergraph()` checks one.
Changes take effect within about half a second (the audio that is already decoded plays out first).

### 5. Surround files and the output format
By default the output uses the device's own channel count and sample rate. Files with more channels than the
output are mixed down; files with fewer are spread onto the matching speakers.
```c
// Force stereo (or 6 for 5.1, ...) and/or a sample rate; 0 keeps the device's preference
const InitializeConfig config = { /* ... */ .output_channels = 2, .output_sample_rate = 48000 };

au_set_downmix(AU_DOWNMIX_FULL_LEVEL, NULL);   // default: standard matrix at full level, like mpv
au_set_downmix(AU_DOWNMIX_NORMALIZED, NULL);   // scaled so it can never clip (~7.7 dB quieter for 5.1)
au_set_downmix(AU_DOWNMIX_CUSTOM,              // your own filters on the original channels, e.g. headphones:
               "sofalizer=sofa=/usr/share/sofa/hrtf.sofa");   // (needs FFmpeg built with libmysofa)
au_set_downmix(AU_DOWNMIX_CUSTOM,              // or a matrix that keeps the LFE
               "pan=stereo|FL=FL+0.707*FC+0.707*SL+0.5*LFE|FR=FR+0.707*FC+0.707*SR+0.5*LFE");
```
The standard matrix (ITU-R BS.775) puts the centre and surrounds in at -3 dB and drops the LFE.

### 6. Shut down
```c
au_shutdown();   // not from inside a callback; au_initialize() may be called again afterwards
```

## Callbacks and threads
- All callbacks run one at a time on ffaudio's control thread, never on the audio thread.
- Calling any `au_*` function from a callback is fine (except `au_shutdown`); it is queued and runs right after
  the callback returns.
- `on_eof` is called exactly once for every handle returned by `au_play_audio()`/`au_set_next()`.
- Custom fade curves (`AuFadeCurveFn`) are the exception: they run on the audio thread and must be fast.

## Architecture
```
 app ──au_*()──▶ control thread ──creates/destroys──▶ tracks (one read thread + one decode thread each)
                  │  ▲ events                            │ demux → decode → convert graph → user graph
                  ▼  │                                   ▼
            mixer slots (current / next / outgoing) ◀── decoded float frames (~0.5 s per track)
                  │
                  ▼
            SDL audio callback: gapless handover, crossfades, gain ramps, dither → device
```
- `lib/ffaudio.c`: public API and the control thread; the only place tracks are created, moved and freed.
- `lib/track.c`: per-track read and decode threads, filter graphs, and the non-blocking `track_pull()`.
- `lib/output.c`: device handling and the mixer (SDL callback). It never blocks, allocates or frees.
- `lib/filtergraph.c`: the convert graph (decoder format → output format) and the user graph (filters).
- `lib/filters.c`: the filter-string builder (`ffaudio_filters.h`).
- `lib/*_queue_utils.c`, `lib/decoder_utils.c`: packet/frame queues and the decoder loop, from FFplay.

## Testing
`ctest` runs headless suites with SDL's dummy and disk drivers:
- `test_filters`: filter-string builder
- `test_basic`: formats, broken files, queueing, pause, seek, stop, looping
- `test_lifecycle`: calls before init, shutdown while playing, re-initialization
- `test_stress`: random API sequences, a playlist driven entirely from callbacks, rapid track replacement
  (`FFAUDIO_STRESS_SEED`, `FFAUDIO_STRESS_ITERS`)
- `test_output`: captures the mixed output and checks gapless joins sample by sample, crossfade lengths,
  fades, volume ramps and live filter changes

All suites are run under AddressSanitizer and ThreadSanitizer (`-DFFAUDIO_SANITIZER=thread`, with
`tests/tsan.supp` for FFmpeg's uninstrumented internals). `manual_player <music-dir> [-x seconds] [-f filters] [-s] [-c channels] [-d full|norm|filters]`
plays a folder on real hardware for listening tests.

## Roadmap
- [ ] ReplayGain / R128 read from tags automatically (track/album mode, preamp, clipping prevention)
- [ ] Metadata and embedded cover art API, and probing a file without playing it
- [ ] Playback speed (tempo with and without pitch change)
- [ ] Network streams: HTTP/Icecast with "now playing" metadata, reconnects, buffering state (rtp/rtsp)
- [ ] Cue sheets / gapless playback of track ranges inside one file
- [x] Verified multichannel (5.1/7.1) playback and configurable downmixing
- [ ] Device added/removed notifications for the application
- [ ] SDL3 port
- [ ] Release binaries for Linux and Windows; test macOS, Android and iOS
- [ ] Update the C# bindings (FFAudioSharp) to this API

Note: this project is not affiliated with FFmpeg, FFplay, or their authors.
