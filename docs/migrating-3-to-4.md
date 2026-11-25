# Migrating from ffaudio 3.x to 4.0

4.0 is a major release: source and binary compatibility with 3.x are broken on purpose, once, to settle the API.
The library is `libffaudio.so.4`, and `find_package(ffaudio 4)` asks for it. The headers are the reference; this
page lists what changed and why.

## The rules 4.0 follows

- **Init takes only what is fixed for the player's lifetime.** `au_init(app_name, on_event, user_data)`. Every
  setting that can change has exactly one setter, usable at any time after `au_init()`, also before anything plays.
  A lifetime-fixed option needed later will come as a hint set before `au_init()`, not as a struct.
- **No growing structs the caller allocates.** Structs are small, finished value groups (`AuFade`, `AuReplayGain`,
  `AuLoudnessOptions`, ...) that stay as they are for all of 4.x. Options that may grow are an opaque object with
  typed setters (`AuPlayOptions`, like `AuFilterBuilder`). Lists the library hands out are arrays of pointers, so the
  element type can gain fields.
- **Every call says whether it worked.** Setters and controls return an `AU_RESULT`; nothing ignores a bad value
  without a word.
- **Names:** runtime settings are `Au*Settings`, per-call options `Au*Options`, callback types end in `Fn`.

These choices also make bindings (C#, Python, ...) simpler: no size fields to fill in, no nested pointers to
marshal, a fixed-size event, and a capacity wherever the library writes into an array.

## Starting the player

3.x:
```c
const AuConfig config = { .app_name = "My Player", .on_event = on_event, .volume = 0.8f,
                          .output_channels = 2, .prepare_next_seconds = 5 };
au_init(&config);   // opened the device at once
```
4.0:
```c
au_init("My Player", on_event, NULL);   // every argument may be NULL
au_set_volume(0.8f);
au_set_output_format(2, 0);
au_set_prepare_next(5);
au_open_device();                       // optional: otherwise the first au_play()/au_queue() opens it
```

**The device opens when it is needed**, not in `au_init()`: at the first `au_play()`/`au_queue()`, or at
`au_open_device()`. It opens with the device and format chosen until then, so choosing them never opens it twice,
and there is exactly one `AU_EVENT_DEVICE`. The first track's file opens while the device does, so starting is not
slower. Until a device is open, `au_get_status()` reports `sample_rate` and `channels` 0 and `au_get_device_name()`
gives "". A track that needs a device none can be opened for ends with `AU_END_ERROR` and `AU_ERR_DEVICE`; 3.x left
it waiting.

## Per-track options

3.x:
```c
const AuPlayConfig cfg = { .start_seconds = 30, .length_seconds = 60, .filters = "atempo=1.1" };
au_queue(path, &cfg);
```
4.0:
```c
AuPlayOptions *opts = au_play_options_create();
au_play_options_set_range(opts, 30, 60);
au_play_options_set_filters(opts, "atempo=1.1");   // AU_ERR_INVALID_ARGUMENT for a graph FFmpeg refuses
au_queue(path, opts);                              // copied: change or free opts right away
au_play_options_free(opts);
```
`au_play()` and `au_queue()` copy the object, so one object can serve many tracks: a server's HTTP headers, or one
object whose range is set again for each track of a cue sheet. Each setter checks its value at the call and leaves the
object unchanged when it refuses one (3.x accepted a bad per-track filter string and failed the track later). NULL
still means defaults.

| 3.x `AuPlayConfig` field | 4.0 setter |
|---|---|
| `start_seconds`, `length_seconds` | `au_play_options_set_range(o, start, length)` |
| `filters` | `au_play_options_set_filters(o, filters)` |
| `fade_in` | `au_play_options_set_fade_in(o, &fade)` |
| `replaygain` | `au_play_options_set_replaygain(o, &rg)` |
| `http_headers` | `au_play_options_set_http_headers(o, headers)` |

## Renamed

| 3.x | 4.0 |
|---|---|
| `AuNetworkConfig` | `AuNetworkSettings` |
| `AuReplayGainConfig` | `AuReplayGainSettings` |
| `AuGainScanConfig` | `AuGainScanSettings` |
| `AuLoudnessConfig` | `AuLoudnessOptions` |
| `NotifyOfGainCached` | `AuGainCachedFn` |
| `AuGainCacheVisitor` | `AuGainCacheVisitorFn` |
| `AuLoudnessProgress` | `AuLoudnessProgressFn` |
| `au_gain_scan_configure()` | `au_set_gain_scan()` |
| `au_gain_cache_request()` | `au_gain_scan_request()` |
| `au_gain_cache_request_album()` | `au_gain_scan_request_album()` |
| `au_gain_cache_cancel_requests()` | `au_gain_scan_cancel()` |
| `au_gain_cache_pending()` | `au_gain_scan_pending()` |
| `AuNetworkSettings.measure_streams` | `AuGainScanSettings.measure_streams` (a measurement policy) |

`au_gain_cache_*` now means only the store; `au_gain_scan_*` is the background measurement queue
(`au_gain_cache_scan()` keeps its name: it measures and stores one entry).

## Removed

| 3.x | 4.0 |
|---|---|
| `AuConfig`, `au_init(const AuConfig *)` | `au_init(app_name, on_event, user_data)` and the setters (above) |
| `AuPlayConfig` | `AuPlayOptions` (above) |
| `au_fb_build()` + `au_free()` | `au_fb_peek()`: the builder's string, valid until it changes or is freed; every function that takes filters copies them |
| `au_free()` | nothing else needed it |
| static builds (`-DBUILD_SHARED_LIBS=OFF`, `FFAUDIO_STATIC`) | shared library only |
| the `libffaudio.so.3.0` link for 3.0.0 programs | — |
| building ffaudio with MSVC | GCC or Clang (MinGW on Windows). Applications may still use any compiler, MSVC included |
| `-DFFAUDIO_BUILD_MANUAL_PLAYER` | `-DFFAUDIO_BUILD_TOOLS` |

## Changed signatures

| Function | 3.x | 4.0 |
|---|---|---|
| `au_skip`, `au_clear_queue`, `au_stop`, `au_pause`, `au_seek`, `au_seek_fraction`, `au_set_volume`, `au_set_muted`, `au_set_loop_count` | `void` | `int`: `AU_OK`, `AU_ERR_NOT_INITIALIZED`, `AU_ERR_INVALID_ARGUMENT` or `AU_ERR_NO_MEMORY` |
| `au_shutdown` | `void` | `int`; `AU_ERR_WRONG_THREAD` from `on_event` |
| `au_gain_scan_stop` | `void` | `int`; `AU_ERR_WRONG_THREAD` from `on_cached` |
| `au_gain_cache_remove` | `bool` (true = removed) | `int`: `AU_OK`, `AU_ERR_NOTHING_TO_DO`, `AU_ERR_INVALID_ARGUMENT`, `AU_ERR_NOT_SAVED` |
| `au_gain_cache_clear` | `void` | `int`: `AU_OK` or `AU_ERR_NOT_SAVED` |
| `au_spectrum_read(sp, channel, mags, &rate)` | | `au_spectrum_read(sp, channel, mags, capacity, &rate)` |
| `au_play`, `au_queue` | `const AuPlayConfig *` | `const AuPlayOptions *` |
| `AuPlaylistFile.entries` | `const AuPlaylistEntry *` (`entries[i].path`) | `const AuPlaylistEntry *const *` (`entries[i]->path`) |

**Watch out for `au_gain_cache_remove()`:** `AU_OK` is 0, so 3.x code written as `if (au_gain_cache_remove(p))` still
compiles but now means "it failed". Compare with `AU_OK`.

## Events

- `AuEvent` has a fixed size of 72 bytes (64 bytes of padding in its union, as `SDL_Event` does), so later 4.x
  releases can add data without breaking the ABI. Bindings that declare the struct must declare the padding.
- `data.ended` gains `error` and `message`. For `AU_END_ERROR`, `error` is why: `AU_ERR_FILE` (the file can not be
  read: missing, no permission, an I/O or network error), `AU_ERR_UNSUPPORTED` (nothing playable in it: not a media
  file, no audio stream, a codec this FFmpeg lacks), `AU_ERR_DEVICE`, `AU_ERR_NO_MEMORY`, ...; `message` is the text
  that was logged for it. Both are `AU_OK`/NULL for other reasons.
- `AU_EVENT_PREPARE_NEXT` is not sent while an `au_play()`/`au_queue()` the application already made is still on
  its way, so `au_play(a); au_queue(b);` with a short `a` never asks for a track to put in `b`'s place.

## Other behaviour changes

- **Equal power is the default fade curve.** `AU_FADE_EQUAL_POWER` is 0, so a zeroed or designated-initialized
  `AuFade{ .seconds = 3 }` crossfades with equal power instead of linear (which dips about 3 dB in the middle). Code
  that names its curve is unaffected; code that stored curves as numbers must map them (`AU_FADE_LINEAR` is 1 now).
- **Values out of range are refused:** `au_set_volume()` outside 0..1 or NaN, `au_seek()` below 0 or not finite,
  `au_seek_fraction()` outside 0..1, `au_set_loop_count()` below -1 (3.x clamped it to -1), a fade with seconds that
  are not finite or an unknown curve, `AU_FADE_CUSTOM` without a function (3.x played those as linear).
- **`AU_ERR_NOT_SAVED`** (new): a ReplayGain cache change was made in memory, but the cache file could not be
  written. `au_gain_cache_set()` and `au_gain_cache_scan()` returned `AU_ERR_FILE` for that; the background queue
  reported success. `on_cached` now gets `AU_ERR_NOT_SAVED` with the values.
- **Text that is not UTF-8** (tags, playlists, cue sheets, stream titles) is read as Windows-1252, so its quotes and
  dashes survive (3.x: Latin-1, where they became invisible control characters). The UTF-8 check is strict
  (no overlong forms, surrogates or values above U+10FFFF), and container tags are checked too (3.x copied WAV/AIFF
  tags as raw bytes). Applications that decoded raw legacy bytes themselves (e.g. Shift-JIS WAV tags) now get them
  converted.
- **Filter builder:** NULL settings are the defaults for every helper (3.x: an error for the equalizer, the
  parametric equalizer and stereo).
- **`AU_REPLAYGAIN_NONE`** is valid C++ (3.x used a C compound literal).

## Added

- `au_open_device()`: open the output device now rather than with the first track.
- `AuPlayOptions` and `au_play_options_create/free/set_*()`.
- `au_result_string(result)`: static English text for an `AU_RESULT`.
- `AU_ERR_NOT_SAVED`.

## Build

- SDL2 2.24 or newer is required at configure time (it was needed at run time already).
- `ffaudio::ffaudio` also exists when ffaudio is built as part of another project (`add_subdirectory()`), and tests,
  tools and install rules are off by default then.
- The test fixtures are made by a CTest fixture rather than the build: a missing FFmpeg encoder fails the tests,
  not the build.
