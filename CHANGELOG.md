# Changelog

## 0.2.0 (2026-11-19)

A rework of the playback core for stability, gapless playback and crossfading. The API changed in
incompatible ways; see "API changes".

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
