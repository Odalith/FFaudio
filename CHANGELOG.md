# Changelog

## 2.1.1 (unreleased)

### Fixed
- A track replaced with a fade too short for one sample, or before `au_configure_audio_device()` (two
  `au_play_audio()` calls were enough), was never ended: its threads and file stayed open until shutdown and its
  `on_eof` never came. Such fades are now cuts.
- ffaudio emptied SDL's event queue, so applications that also use SDL lost their keyboard, mouse and window
  events. It now only watches for audio device events and leaves the queue alone; it pumps the queue only when
  SDL video is not initialized (with video, the application's event loop does it).
- A callback that caused many notifications at once (e.g. changing the device from `on_eof`) could make ffaudio free
  memory it had already freed.
- Rare races: the last frames of a track could be cut when they arrived just as the mixer found the queue empty, and
  `au_set_loop_count(0)` at the moment a loop started could turn into looping forever.
- Tracks are destroyed without waiting up to 100 ms for a decoder that waits for an output device, and
  `au_seek_percent()` on a track still opening is picked up at once.
- Measuring a file whose read failed partway (a disk error, a download that broke off after its reconnects) returned
  the loudness of the part that was read, and the scan queue stored it as the file's. It now fails with
  `AU_ERR_FILE`.
- Measuring a file with an unspecified channel layout (e.g. a WAV without a channel mask) rebuilt the sample
  converter for every decoded frame.
- Measurements of network files stop right away on `au_gain_scan_stop()` and `au_gain_cache_cancel_requests()`, also
  while connecting or waiting to reconnect, and reconnect after network errors like playback does.
- A track waiting for its measurement (`measure_missing`) starts as soon as the measurement fails or is cancelled,
  instead of after `measure_wait_ms`.
- `au_gain_cache_request_album()` skips albums with a network track unless `measure_streams` is set (it ignored that
  setting), and does not queue an album again while it is queued or being measured; an urgent request moves it to
  the front.
- A request made while `au_gain_scan_stop()` was running could leave the queue without workers for good.
- Pausing faded out with `1 - curve(t)` instead of `curve(1 - t)`, the shape crossfades use and `ffaudio.h`
  describes: equal-power pause fades dipped too early, and logarithmic and exponential ones had each other's shape.
- After a measurement that did not change a track's gain, the next gain change (e.g. a new preamp) took 2 s
  instead of 10 ms.
- `on_prepare_next` comes again after the queued track was removed with `au_clear_next()` or after a seek back;
  it used to come only once per track.
- `au_wait_loop()` could read the player while `au_shutdown()` on another thread freed it.
- `au_media_is_live()` is true only for network streams without a duration, as the player treats them; it was also
  true for local files with an unknown length, and for files whose length only the audio stream gave.
- A local file named like a demuxer (`mp3`, `hls`, ...) was opened with that demuxer instead of being probed.
- Files whose timestamps start late (MPEG-TS and so HLS, which start at 1.4 s or more) reported positions that
  included that start, while seeks and the duration count from 0: `au_get_audio_play_time()` was too large, end
  crossfades and `on_prepare_next` came that much early, and a device change jumped ahead by it.

### Documentation
- The ReplayGain cache file is not safe to write from several programs at the same time, as the readme said:
  compacting replaces the file and can lose another writer's changes. One writer at a time.

## 2.1.0 (unreleased)

### Added
- Network streams: `http://` and `https://` URLs in `au_play_audio()`, `au_set_next()`, `au_probe()` and loudness
  analysis, for files on web and music servers (seeking with range requests), internet radio and HLS.
  `au_set_network()` sets the user agent and timeouts for all of them (also before `au_initialize()`),
  `PlayAudioConfig.http_headers` adds request headers per track, `au_is_network_url()` tells URLs from paths.
- Live streams and buffering: network tracks buffer `buffer_seconds` before they start and after the network
  fell behind (the position holds, a short fade-in on resume) and read ahead up to `max_buffer_seconds` (10 s);
  `on_buffering` reports it. Live streams have no duration and ignore seeks (`au_media_is_live()`,
  `au_media_is_seekable()`). Dropped connections are re-opened (`reconnect_attempts`): files continue with a range
  request, live streams pick up again; an unreachable server still fails within the timeout.
- "Now playing": `on_stream_metadata(handle, title, artist)` for Icecast/SHOUTcast titles ("Artist - Title" is
  split) and tags that change inside Ogg/Opus streams, reported when that audio is heard; `au_get_track_info()`
  follows, and the station's ICY headers (`icy-name`, ...) become tags. Latin-1 titles are converted to UTF-8.
- Protocol whitelists: local paths can only open local files and URLs only http, https, tls, tcp, crypto and
  httpproxy, so playlists (HLS or otherwise) can not reach other protocols or local files.
- Release archives: FFmpeg with the network protocols and the HLS demuxer; https through the system's GnuTLS
  on Linux and schannel on Windows.
- The readme's "Memory use" covers streaming on Linux: with glibc's default settings, the read-ahead buffers make a
  long-running player grow (240 MB after 2 hours in the soak test, still rising); fixed malloc thresholds
  (`GLIBC_TUNABLES` or `mallopt()`) kept it flat at about 80 MB.

## 2.0.1 (2026-11-20)

### Changed
- Far fewer threads: every track ran its decoder with a frame thread per CPU core and three filter graphs with a
  thread pool each (up to ~37 threads per track, 48-96 for the player on 12 cores). Playback now decodes and
  filters on the track's own threads; loudness analysis uses at most 4 decoder threads (as fast as one per core
  for hi-res FLAC). Found with the new soak test (`tests/soak.c`).
- `au_free()` is declared in `ffaudio.h` (it was in `ffaudio_filters.h`), as the general way to free memory
  ffaudio returns.
- The readme explains memory use on Linux (glibc keeping freed memory per thread) and what applications can do.

### Added
- `au_free_audio_devices()` frees the list from `au_get_audio_devices()`. Freeing it with the application's own
  `free()` breaks on Windows when the application and ffaudio use different C runtimes.

## 2.0.0 (2026-11-19)

The first release since 1.0.4: a rework of the playback core for stability, gapless playback and crossfading.
The API changed in incompatible ways; see "API changes".

### Fixed
- Surround files mixed down to stereo were ~7.7 dB quieter than in mpv (#19): with integer output formats
  libswresample scaled the downmix matrix down to avoid clipping. The downmix level is now set explicitly
  (full level by default, see `au_set_downmix`).
- 5-channel outputs used FFmpeg's 5.0 order (FL FR FC BL BR) instead of SDL's 4.1 order (FL FR LFE BL BR), so the
  centre played on the LFE channel.
- Heap corruption ("corrupted double-linked list") and use-after-free when tracks were replaced, skipped or
  failed while another was queued: read threads freed their own track while the audio callback still used it,
  and a queued track could be overwritten and orphaned with its threads still running.
- Hangs: lost wake-ups in `abort_track`, a reconfigure mutex that was never created, and an audio callback that
  blocked forever when a track's decoder stopped.
- Nothing played on FFmpeg 8: `abuffersink` options were set after the filter was initialized.
- A failed filter reconfiguration freed the filters twice.
- Audio device open failures were never detected (unsigned return compared with `< 0`).
- API calls before `au_initialize()` crashed; `au_initialize()` could not be called again after `au_shutdown()`.
- `au_pause_audio(value)` toggled instead of using `value`; volume and mute only applied to the current track.
- Position reporting used a hardware buffer size of 0; global per-file state (byte seeking, infinite buffering,
  loop counts, resampler settings) leaked from one track into the next.
- The library installed SDL signal handlers, taking over the host's Ctrl-C / SIGTERM.
- `ffaudio.pc` listed FFmpeg and SDL2 in `Requires.private` for shared builds, so pkg-config failed without their
  development files although they are only needed for static linking.
- Files that are not audio (e.g. encrypted cache files that FFmpeg mistakes for a raw VVC or JPEG stream) made
  FFmpeg's video decoders print errors while probing. Files whose container has no audio stream now fail before
  any decoder is opened, in playback, `au_probe()` and loudness analysis.
- The measurement queue re-read files that can not be measured on every request; they are now remembered
  (until they change or `au_gain_scan_stop()`), and the player no longer waits for them to be measured.

### Added
- Sample-accurate gapless handover inside the audio callback, with resampler and filter tails flushed at EOF.
- Crossfades and fades for track end, skip, play, stop and pause, with curves (linear, equal power, s-curve,
  logarithmic, exponential, custom) and per-track overrides (`au_set_crossfade`, `PlayAudioConfig.fade_in`).
- 32-bit float mixing at the device rate and layout, soxr resampling when available, TPDF dither for integer
  devices, ramped volume/mute/pause.
- Filter-string builder (`ffaudio_filters.h`): graphic equalizer, parametric equalizer
  (`au_fb_add_parametric_eq`, with `au_parametric_eq_parse()` for EqualizerAPO / AutoEq `ParametricEQ.txt`
  headphone profiles), bass/treble (`au_fb_add_tone`), balance/width/mono/channel swap (`au_fb_add_stereo`),
  compressor with gentle and night presets, look-ahead limiter, crossfeed, bs2b and plain gain. Instance names
  (`au_fb_set_name`) make the filters addressable by `au_send_filter_command`; `au_filter_available()` and
  `AU_ERR_UNSUPPORTED` for filters missing from the FFmpeg build. (Loudness normalization is built in, see
  `au_set_replaygain()`.)
- Global filters and live filter changes: `au_set_global_filtergraph`, `au_update_filtergraph`,
  `au_send_filter_command`, `au_validate_filtergraph`.
- `on_track_started` and `on_audio_device_update` callbacks; `on_prepare_next` now fires
  `prepare_next_seconds` (default 10) before the end of the current track.
- `au_get_current_handle`, `au_get_output_spec`, `au_is_paused`, `au_is_muted`, `au_get_version`.
- Playback speed: `au_set_playback_speed()` (0.25x..4x, keeping the pitch with atempo or tape-style) and
  `au_get_playback_speed()`. Positions stay in file time; changes while playing are gapless.
- Device notifications: `on_audio_devices_changed` when outputs are added or removed, and
  `au_get_audio_device_name()` for the device in use.
- Automatic ReplayGain: `au_set_replaygain()` with track/album mode, preamp, fallback gain for untagged files
  and clipping prevention; ReplayGain 2.0 tags in every format FFmpeg reads, and R128 gain tags (the Opus
  standard, accepted in any format, converted from -23 to the -18 LUFS reference). Gains change with a short ramp; `PlayAudioConfig.replaygain` supplies values from
  the application; `au_get_track_gain_db()` reports the applied gain.
- ReplayGain cache: a persistent, per-user, thread-safe path -> `AuReplayGainInfo` store in the platform's user
  data directory (`au_gain_cache_set/get/remove/clear/count/foreach/reload/set_file/get_file`, and
  `au_gain_cache_scan()` to measure and store on a miss). Append-only file, safe to share between programs. The
  player uses cached values ahead of the file's tags (an explicit `PlayAudioConfig.replaygain` still wins), and
  changes reach queued and playing tracks. No file tracking; media files are never touched.
- Background measurement queue: `au_gain_cache_request()` / `au_gain_cache_request_album()` with prefetch and
  urgent priority, de-duplication, `au_gain_cache_cancel_requests()`, `au_gain_cache_pending()`,
  `au_gain_scan_configure()` (worker threads, `on_cached` callback on the worker thread) and `au_gain_scan_stop()`.
  `AuReplayGainConfig.measure_missing` measures starting tracks without any value and holds their start for up to
  `measure_wait_ms` (default 500 ms); values that arrive later fade in over 2 s.
- Loudness analysis (`ffaudio_loudness.h`), independent of playback and read only: ITU-R BS.1770-4 / EBU R128
  integrated loudness, loudness range, true and sample peak per file, and album values gated over all tracks;
  ReplayGain 2.0 and R128 gains, progress/cancel callback, `au_loudness_to_replaygain()` for
  `PlayAudioConfig.replaygain`. `tests/loudness_scan.c` is a command-line example.
- `AU_ERR_FILE` and `AU_ERR_CANCELLED` result codes; `au_probe()` reports unreadable files as `AU_ERR_FILE`.
- Metadata: `au_probe()` (no player needed) and `au_get_track_info()` return an `AuMediaInfo` with format
  details, tags (case-insensitive, common aliases), track/disc numbers and ReplayGain values.
- `au_set_downmix()`: full level (default, like mpv), normalized (never clips) or custom filters on the original
  channels (e.g. `sofalizer` for headphones). `InitializeConfig.output_channels` / `output_sample_rate` override
  the device's format.
- Headless test suite (CTest) with output capture, sanitizer builds, CI on Linux and Windows (MSYS2), install
  rules, a pkg-config file and a CMake package (`find_package(ffaudio)`, target `ffaudio::ffaudio`).
- Release archives for Linux x86_64 and Windows x86_64, built by `.github/workflows/release.yml` from a version tag:
  ffaudio with an LGPL FFmpeg 9.0.2 trimmed for audio playback, soxr and bs2b (`scripts/build-deps.sh`,
  `scripts/package.sh`), tested before packaging. `FFAUDIO_RELOCATABLE` makes libffaudio look for its libraries
  next to itself. Tested with FFmpeg 8.1 and 9.0.

### API changes
- Log levels are `AU_LOG_INFO`, `AU_LOG_WARNING`, `AU_LOG_ERROR`, `AU_LOG_FATAL` (`ERROR` clashed with `<windows.h>`).
- Every callback takes a trailing `void *user_data` (`InitializeConfig.user_data`); `NotifyOfRestart`,
  `NotifyOfDurationUpdate` and `NotifyOfPrepareNext` also receive the track handle.
- `au_play_audio()` and `au_set_next()` return the track handle; functions return `AU_RESULT` codes.
- `PlayAudioConfig.av_filtergraph` replaces `av_filtergraph_override`, `loudnorm_settings` and `crossfeed_setting`;
  `au_set_equalizer()` and `EqualizerConfig` are replaced by the filter builder.
- Playback functions are asynchronous (queued to the control thread) and never block.
- `au_play_audio()` resumes a paused player.
