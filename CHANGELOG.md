# Changelog

## 0.2.0 (unreleased)

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

### Added
- Sample-accurate gapless handover inside the audio callback, with resampler and filter tails flushed at EOF.
- Crossfades and fades for track end, skip, play, stop and pause, with curves (linear, equal power, s-curve,
  logarithmic, exponential, custom) and per-track overrides (`au_set_crossfade`, `PlayAudioConfig.fade_in`).
- 32-bit float mixing at the device rate and layout, soxr resampling when available, TPDF dither for integer
  devices, ramped volume/mute/pause.
- Filter-string builder (`ffaudio_filters.h`): equalizer, crossfeed, bs2b, ReplayGain, linear and dynamic
  EBU R128 loudness, plain gain.
- Global filters and live filter changes: `au_set_global_filtergraph`, `au_update_filtergraph`,
  `au_send_filter_command`, `au_validate_filtergraph`.
- `on_track_started` and `on_audio_device_update` callbacks; `on_prepare_next` now fires
  `prepare_next_seconds` (default 10) before the end of the current track.
- `au_get_current_handle`, `au_get_output_spec`, `au_is_paused`, `au_is_muted`, `au_get_version`.
- Automatic ReplayGain: `au_set_replaygain()` with track/album mode, preamp, fallback gain for untagged files
  and clipping prevention; ReplayGain 2.0 tags in every format FFmpeg reads and Opus R128 gains (converted to
  the -18 LUFS reference). Gains change with a short ramp; `PlayAudioConfig.replaygain` supplies values from
  the application; `au_get_track_gain_db()` reports the applied gain.
- Metadata: `au_probe()` (no player needed) and `au_get_track_info()` return an `AuMediaInfo` with format
  details, tags (case-insensitive, common aliases), track/disc numbers and ReplayGain values.
- `au_set_downmix()`: full level (default, like mpv), normalized (never clips) or custom filters on the original
  channels (e.g. `sofalizer` for headphones). `InitializeConfig.output_channels` / `output_sample_rate` override
  the device's format.
- Headless test suite (CTest) with output capture, sanitizer builds, CI, install rules and a pkg-config file.

### API changes
- Log levels are `AU_LOG_INFO`, `AU_LOG_WARNING`, `AU_LOG_ERROR`, `AU_LOG_FATAL` (`ERROR` clashed with `<windows.h>`).
- Every callback takes a trailing `void *user_data` (`InitializeConfig.user_data`); `NotifyOfRestart`,
  `NotifyOfDurationUpdate` and `NotifyOfPrepareNext` also receive the track handle.
- `au_play_audio()` and `au_set_next()` return the track handle; functions return `AU_RESULT` codes.
- `PlayAudioConfig.av_filtergraph` replaces `av_filtergraph_override`, `loudnorm_settings` and `crossfeed_setting`;
  `au_set_equalizer()` and `EqualizerConfig` are replaced by the filter builder.
- Playback functions are asynchronous (queued to the control thread) and never block.
- `au_play_audio()` resumes a paused player.
