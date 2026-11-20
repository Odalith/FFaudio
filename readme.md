<img width="1815" height="150" alt="banner" src="https://github.com/user-attachments/assets/46466334-645f-4585-a24c-55f315cc892c" />

⚠️ This project is in beta since 2.0.0. The API may still change between versions.

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
- **ReplayGain / R128**: tags are applied automatically (track or album mode, preamp, fallback gain for untagged
  files, clipping prevention); R128 gain tags (the Opus standard, read from any format) are converted to the
  ReplayGain reference
- **Loudness analysis**: measure files (EBU R128 / ITU-R BS.1770-4 integrated loudness, loudness range, true
  peak) per track and per album, independent of playback, and get ReplayGain/R128 gains back. Read only:
  ffaudio never modifies your files
- **Metadata**: tags (normalized names across formats), track/disc numbers and format details, for playing
  tracks or by probing a file without playing it
- **Filters**: any FFmpeg filtergraph per track and/or globally, changeable while playing, with helpers for
  graphic and parametric EQ (AutoEq / EqualizerAPO profiles), bass/treble, balance, width, mono, channel swap,
  crossfeed, bs2b, compressor and limiter
- Volume with smooth ramps (no zipper noise), mute, pause, seek by time or percentage, looping
- Playback speed 0.25x to 4x, keeping the pitch or tape-style, changeable while playing without gaps
- Callbacks for track start/end, prepare-next, restarts (seek/loop), duration, device changes and logs
- Follows the system default device, or uses a chosen one and falls back to the default if it disappears;
  `on_audio_devices_changed` reports outputs that are plugged in or removed
- Surround files up to 7.1 play on multichannel outputs with SDL's speaker order, or are mixed down at full level
  (like mpv), normalized (never clips), or with your own filters (e.g. `sofalizer` for headphones)
- Safe to drive from any thread, including from inside its own callbacks
- Linux tested; Windows builds with MSYS2 (see CI); macOS, Android and iOS untested
- LGPL-2.1

## Downloads
[Releases](https://github.com/Odalith/ffAudio/releases) have ready-built archives for Linux x86_64 and Windows
x86_64 (MinGW/UCRT). Each contains ffaudio, an LGPL build of FFmpeg trimmed for audio playback (every audio decoder,
demuxer and audio filter; no encoders, video or network code), soxr and bs2b, with headers, pkg-config and CMake
files (`find_package(ffaudio)`). The Windows archive also has SDL2; on Linux SDL2 comes from the system. The FFmpeg
and soxr sources are attached to each release, and `scripts/build-deps.sh` rebuilds them.

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
`-DFFAUDIO_BUILD_TESTS=OFF`, `-DFFAUDIO_BUILD_MANUAL_PLAYER=OFF`, `-DFFAUDIO_RELOCATABLE=ON` (Linux/macOS: find bundled
libraries next to libffaudio, for archives like the release ones).

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

au_set_playback_speed(1.25, true);     // 0.25..4; true keeps the pitch, false is tape-style
```
Playback functions never block: they are queued and run in order on ffaudio's control thread.
`au_get_audio_play_time()`, `au_get_audio_duration()` and `au_get_current_handle()` describe the audible track.
At other speeds, positions and durations stay in file time, while crossfade lengths and `prepare_next_seconds` stay
in wall clock time; a speed change is heard within about half a second and never leaves a gap.

Devices: `au_get_audio_devices()` lists the outputs (free the list with `au_free_audio_devices()`),
`au_get_audio_device_name()` names the one in use, and
`on_audio_devices_changed` fires when outputs are plugged in or removed. With `au_configure_audio_device(NULL)`
playback follows the system default device; a chosen device that disappears falls back to the default.

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

// Per track
const PlayAudioConfig cfg = { .av_filtergraph = "atempo=1.1" };
au_play_audio("/music/song.flac", &cfg);

// Runtime parameter changes without rebuilding the graph
au_send_filter_command(0, "volume", "volume", "0.5");
```
Any [FFmpeg filter](https://ffmpeg.org/ffmpeg-filters.html) string works; `au_validate_filtergraph()` checks one.
Changes take effect within about half a second (the audio that is already decoded plays out first).

Headphone correction, the stereo image and dynamics:
```c
AuParametricEq peq;
char err[128];
if (au_parametric_eq_parse(autoeq_text, &peq, err, sizeof(err)) == AU_OK)  // AutoEq ParametricEQ.txt
    au_fb_add_parametric_eq(fb, &peq);
au_fb_add_tone(fb, 3.0, -1.0);                    // bass and treble in dB

AuStereo st = au_stereo_defaults();               // stereo outputs only
st.balance = -0.2;                                // or .width = 0 for mono, .swap = true
au_fb_set_name(fb, "st");                         // instance name: "stereotools@st"
au_fb_add_stereo(fb, &st);

const AuCompressor night = au_compressor_night(); // or au_compressor_defaults() for gentle levelling
au_fb_add_compressor(fb, &night);
au_fb_add_limiter(fb, NULL);                      // -1 dBFS ceiling, last in the chain

// later, instantly and without rebuilding:
au_send_filter_command(0, "stereotools@st", "balance_out", "0.3");
```
Named filters can be changed with commands; a command reaches the tracks that are playing, so also set the updated
string for tracks that start later. `au_filter_available()` tells whether this FFmpeg build has a filter (bs2b
needs libbs2b); the helpers return `AU_ERR_UNSUPPORTED` when it does not.

### Network streams
URLs work wherever paths do: files on web and music servers (seekable when the server supports range requests),
Icecast/SHOUTcast internet radio and HLS (`.m3u8`).
```c
const AuNetworkConfig net = { .user_agent = "myplayer/1.0", .timeout_seconds = 15 };
au_set_network(&net);                                   // optional; also applies to au_probe() and analysis

au_play_audio("https://radio.example.com/stream.mp3", NULL);

const PlayAudioConfig cfg = { .http_headers = "Authorization: Bearer abc123" };
au_set_next("https://music.example.com/rest/stream?id=42", &cfg);
```
Network tracks buffer before they start (`AuNetworkConfig.buffer_seconds`, default 2 s) and read ahead up to
`max_buffer_seconds` (10 s). When the network falls behind, the track waits for a full buffer again instead of
stuttering: its position holds, and it fades back in. `on_buffering(handle, buffering, buffered_seconds)` reports
both, for a "buffering..." display. Live streams (internet radio, live HLS) have no duration and ignore seeks;
`au_media_is_live()` and `au_media_is_seekable()` tell them apart. `on_stream_metadata(handle, title, artist)`
reports "now playing" changes (Icecast/SHOUTcast titles, new tags inside Ogg/Opus streams) when they are heard,
not when they are read ahead; `au_get_track_info()` shows the same title and artist, and the station's `icy-name`,
`icy-genre`, ... headers as tags. Latin-1 titles from old servers are converted to UTF-8. A dropped connection is opened again
(`reconnect_attempts`, default 5): a file continues where it stopped, a live stream picks up the broadcast.

Local paths may only open local files and URLs only network protocols, so a playlist can not make FFmpeg read
local files. A server that stops answering ends the track with an error after the timeout; stopping or skipping
a track that is still connecting interrupts it. Network files are not measured for ReplayGain unless
`AuNetworkConfig.measure_streams` is set (it downloads the whole file). The release archives use the system's
GnuTLS for https on Linux and Windows' own TLS.

### 5. ReplayGain and metadata
```c
const AuReplayGainConfig rg = {
    .mode = AU_REPLAYGAIN_ALBUM,    // or AU_REPLAYGAIN_TRACK; each falls back to the other gain
    .preamp_db = 0.0,               // added to tagged gains
    .fallback_gain_db = -6.0,       // files without tags
    .prevent_clipping = true,       // never push the tagged peak past full scale
};
au_set_replaygain(&rg);             // applies to playing tracks with a short ramp

// Gains from your own library database instead of the file's tags
const AuReplayGainInfo mine = { .has_track_gain = true, .track_gain_db = -7.3,
                                .has_track_peak = true, .track_peak = 0.97 };
const PlayAudioConfig cfg = { .replaygain = &mine };
au_play_audio("/music/song.flac", &cfg);

// Tags and format without playing (no au_initialize() needed)
AuMediaInfo *info = au_probe("/music/song.flac", NULL);
if (info) {
    int track, tracks;
    au_media_track_number(info, &track, &tracks);
    printf("%s - %s (%d/%d), %s %d Hz\n", au_media_tag(info, "artist"), au_media_tag(info, "title"),
           track, tracks, au_media_codec(info), au_media_sample_rate(info));
    AuReplayGainInfo gain = au_media_replaygain(info);
    au_media_info_free(info);
}

// The same for a playing or queued track (0 = current), e.g. from on_track_started
AuMediaInfo *now = au_get_track_info(0);
```
ReplayGain 2.0 tags (`REPLAYGAIN_TRACK_GAIN`, `..._PEAK`, `REPLAYGAIN_ALBUM_...`) are read from every format FFmpeg
exposes them for (FLAC/Ogg/Opus comments, ID3v2 TXXX, APEv2, MP4 freeform atoms).

`R128_TRACK_GAIN` / `R128_ALBUM_GAIN` are used when a file has no ReplayGain tags. They are the standard for Opus
(RFC 7845, which asks taggers not to write ReplayGain tags there) but are accepted in any format. ReplayGain 2.0 and
EBU R128 measure loudness the same way (ITU-R BS.1770) and only differ in the target level: -18 LUFS versus
-23 LUFS. R128 gains are therefore raised by 5 dB, so Opus files play as loud as the rest of a library; use
`preamp_db = -5` if you prefer the -23 LUFS level. R128 tags carry no peak, so clipping prevention can not apply
to files that only have R128 tags. ReplayGain 1.0 tags (an older measurement) look the same as 2.0 tags and are
used as they are.

The gain is applied in the mixer as a per-track multiplication of the float samples (not a filter), together with
fades and before the master volume, so changing it never rebuilds a filter graph.
Tag names are FFmpeg's normalized ones (`title`, `artist`, `album`, `album_artist`, `date`, `track`, `disc`, ...);
lookups are case-insensitive and also find common alternative spellings.

### 6. Measuring loudness
Files without ReplayGain/R128 tags (most libraries) can be measured with `ffaudio_loudness.h`. It is separate from
playback: no `au_initialize()` needed, it can run in a background thread while music plays, and it only reads
files. Store the results in your own database and hand them back with `PlayAudioConfig.replaygain`.
```c
#include <ffaudio_loudness.h>

AuLoudnessScanner *scanner = au_loudness_scanner_create(NULL);   // one album
AuLoudness track[N], album;
for (int i = 0; i < N; i++)
    au_loudness_scan_file(scanner, album_files[i], &track[i]);    // AU_OK, AU_ERR_FILE, AU_ERR_CANCELLED
au_loudness_album(scanner, &album);                              // gated over all tracks together
au_loudness_scanner_free(scanner);

// track[i].integrated_lufs, .loudness_range_lu, .true_peak, .sample_peak,
//         .replaygain_gain_db (re -18 LUFS), .r128_gain_db / .r128_gain_q78 (re -23 LUFS)

AuReplayGainInfo rg = au_loudness_to_replaygain(&track[0], &album);   // save it with the track...
const PlayAudioConfig cfg = { .replaygain = &rg };                     // ...and use it when playing
au_play_audio(album_files[0], &cfg);
```
The measurement follows ITU-R BS.1770-4 / EBU R128 (K-weighting at any sample rate, 400 ms gating blocks with
the -70 LUFS absolute and -10 LU relative gates, EBU Tech 3342 loudness range, 4x oversampled true peak,
surround channels weighted +1.5 dB and LFE excluded) and agrees with FFmpeg's `ebur128` filter. A progress
callback in `AuLoudnessConfig` reports the position and can cancel a scan; `skip_true_peak` makes it faster.
It runs at a few hundred times real time per thread; use one scanner per thread to scan a library in parallel.
`tests/loudness_scan.c` is a small command-line example (`loudness_scan [-t] [-c] files...`).

#### ReplayGain cache
Instead of passing `PlayAudioConfig.replaygain` every time, put the values in ffaudio's ReplayGain cache: a
persistent key-value store from path to `AuReplayGainInfo`, kept per user and shared by every program that uses
ffaudio, so values measured once are there in every later run.

| Platform | File |
|---|---|
| Linux / BSD | `$XDG_DATA_HOME/ffaudio/replaygain.cache` (default `~/.local/share/ffaudio/replaygain.cache`) |
| macOS | `~/Library/Application Support/ffaudio/replaygain.cache` |
| Windows | `%LOCALAPPDATA%\ffaudio\replaygain.cache` |
| Android, iOS, others | `SDL_GetPrefPath("ffaudio", "ffaudio")` + `replaygain.cache` |

`au_gain_cache_set_file()` selects another file (or `""` for memory only). Every change is appended to the file
right away as one line, so nothing is lost if the program crashes and several programs can use the file at the
same time; it is read on first use, compacted when it has grown with superseded lines, and
`au_gain_cache_reload()` picks up changes from other programs. The folder is only created on the first write.
ffaudio never tracks, checks or modifies the media files: paths are plain strings, and keeping them current is up
to the application (`au_gain_cache_foreach()` + `au_gain_cache_remove()` to drop entries for deleted files).

When a track starts, its path is looked up and a cached entry is used instead of the file's tags
(order: `PlayAudioConfig.replaygain` > cache > tags). Setting or removing an entry also updates tracks that are
already queued or playing, so a value may arrive after the track was queued.

#### Measuring in the background
ffaudio fills the cache itself on low-priority worker threads; the application only says what is coming:
```c
const AuReplayGainConfig rg = {
    .mode = AU_REPLAYGAIN_TRACK,
    .fallback_gain_db = -6,
    .measure_missing = true,      // a starting track with no value at all is measured right away
    .measure_wait_ms = 0,         // ...and its start waits up to 500 ms (the default) for the result
};
au_set_replaygain(&rg);

static void on_track_started(int32_t handle, void *user) {
    // Keep the next part of the playlist measured; cached, queued and running paths are skipped,
    // so re-sending the window every time is cheap
    au_gain_cache_request(next_ten_paths, 10, AU_SCAN_PREFETCH);
}

// A whole album, so album gains are stored as well
au_gain_cache_request_album(album_paths, n, AU_SCAN_PREFETCH);
```
- **Prefetch** covers the normal flow and skips within the window: the values are in the cache before the
  tracks start.
- **Misses** (a track picked out of order, a new playlist): with `measure_missing` the player moves that file to
  the front of the queue and holds its start until the value is there, at most `measure_wait_ms`. Most songs are
  measured in well under that. If the wait runs out, the track starts at `fallback_gain_db` and the measured
  value is faded in over 2 s instead of jumping.
- `AU_SCAN_URGENT` puts requests before everything else (newest first); `au_gain_cache_cancel_requests()` drops
  the application's queued requests when the playlist changes; `au_gain_cache_pending()` reports progress.
- `au_gain_scan_configure()` sets the number of worker threads (default 2) and an optional `on_cached`
  callback, which runs on a worker thread. `au_gain_scan_stop()` stops them; they start again on demand.
- `au_gain_cache_scan()` is the synchronous version for tools and batch jobs.

The cache and the queue work without `au_initialize()`, survive `au_shutdown()`, are thread-safe, and paths are
compared as exact strings (pass them the same way every time, e.g. always absolute).

### 7. Surround files and the output format
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

### 8. Shut down
```c
au_shutdown();   // not from inside a callback; au_initialize() may be called again afterwards
```

## Callbacks and threads
- All callbacks run one at a time on ffaudio's control thread, never on the audio thread.
- Calling any `au_*` function from a callback is fine (except `au_shutdown`); it is queued and runs right after
  the callback returns.
- `on_eof` is called exactly once for every handle returned by `au_play_audio()`/`au_set_next()`.
- Custom fade curves (`AuFadeCurveFn`) are the exception: they run on the audio thread and must be fast.

## Memory use
Free what ffaudio returns with ffaudio's functions (`au_media_info_free()`, `au_free_audio_devices()`,
`au_fb_free()`, `au_free()`, ...), never with your own `free()`: on Windows a program and a DLL can use different C
runtimes, and freeing across them corrupts the heap.

On Linux, a process using ffaudio can appear to grow for a while and then level off. That growth is not a leak.
glibc's `malloc` keeps memory that has been freed, so it can be reused, instead of returning it to the system, and
it keeps a separate pool ("arena") for each thread that allocates. Every pool holds on to its own peak: decoded
audio, packets and filter buffers that were freed long ago still count as resident memory until they are reused.
Once each pool has seen its peak, the total stops growing.

`tests/soak.c` measures this: it plays a music library for hours at 10x speed while changing tracks, seeking,
switching filters, speed and ReplayGain settings, measuring loudness in the background and restarting the player.
In an 8-hour run (8,900 tracks, 35,000 random actions, 9,400 loudness analyses), the memory ffaudio actually uses
stayed at a few MB, while the process' RSS rose to about 145 MB in the first hour and then moved between 110 and
170 MB, without growing, for the remaining seven (2.0.0, with a thread pool per decoder and filter graph, levelled
off at 300-350 MB). A plain player, with one track and no background measurement, needs less.
AddressSanitizer runs of the same test found no leaks.

Network streams read ahead (`AuNetworkConfig.max_buffer_seconds`, 10 s), and every pool keeps the largest
read-ahead it has held: a hi-res stream over HTTP is several MB per 10 s. In 2-hour soaks with a third of the
tracks streamed, a 10 s-class read-ahead (8 s) levelled off at about 110 MB held while 30 s reached 285 MB and was
still growing; raise it only if your streams need it.

If the resident size matters (a small device, a player that runs for weeks), the application can:
- limit glibc to a few pools: `MALLOC_ARENA_MAX=2` in the environment, or `mallopt(M_ARENA_MAX, 2)` at start-up
  before any threads are created. In the soak test this cut RSS to about 94 MB;
- call `malloc_trim(0)` now and then, e.g. after a track change, to return free pages to the system (a smaller
  effect: about 130 MB);
- use another allocator (jemalloc, mimalloc, tcmalloc) by linking it or with `LD_PRELOAD`.

All of these change the whole process, which is why ffaudio leaves the choice to the application. Windows and
macOS allocators return memory to the system more readily.

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
- `test_loudness`: BS.1770 / EBU R128 measurement against reference signals, album gating, peaks, cancelling
- `test_gain_cache`: the ReplayGain cache (store, persistence, file format, threads, get-or-scan, priority over
  tags, live updates)
- `test_gain_scan`: the background measurement queue (priorities, de-duplication, albums, cancelling, workers)
  and the player's `measure_missing` / `measure_wait_ms`
- `test_metadata`: tags, track/disc numbers and ReplayGain/R128 parsing in several formats
- `test_output`: captures the mixed output and checks gapless joins sample by sample, crossfade lengths,
  fades, volume ramps and live filter changes

All suites are run under AddressSanitizer and ThreadSanitizer (`-DFFAUDIO_SANITIZER=thread`, with
`tests/tsan.supp` for FFmpeg's uninstrumented internals). `manual_player <music-dir> [-x seconds] [-f filters] [-s] [-c channels] [-d full|norm|filters] [-r track|album] [-g] [-G cache-file] [-e preset|gains] [-X] [-b default|chu|jan] [-P ParametricEQ.txt] [-L] [-C gentle|night]`
plays a folder on real hardware for listening tests.

## Roadmap
- [x] ReplayGain / R128 read from tags automatically (track/album mode, preamp, clipping prevention)
- [x] Metadata API and probing a file without playing it
- [ ] Embedded cover art
- [x] Measure R128 / ReplayGain values for files (loudness analysis; read only)
- [x] Playback speed (tempo with and without pitch change)
- [x] Network streams: HTTP(S) files, HLS, Icecast/SHOUTcast with "now playing", buffering, reconnects
- [ ] Cue sheets / gapless playback of track ranges inside one file
- [x] Verified multichannel (5.1/7.1) playback and configurable downmixing
- [x] Device added/removed notifications for the application
- [ ] SDL3 port
- [ ] Release binaries for Linux and Windows; test macOS, Android and iOS
- [ ] Update the C# bindings (FFAudioSharp) to this API

Note: this project is not affiliated with FFmpeg, FFplay, or their authors.
