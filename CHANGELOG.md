# Changelog

## 3.1.0 (unreleased)

### Added
- Embedded pictures (cover art): `au_media_picture(path, type, &err)` returns an `AuPicture` (the image as stored,
  its MIME type, picture type, size and description) from FLAC, ID3v2 (MP3, AIFF, WAV), MP4/M4A, APE and Ogg/Opus
  files, the front cover or a given `AU_PICTURE_TYPE`; `au_picture_release()` frees it. Nothing is decoded, and
  pictures are only read when asked for. `AuMediaInfo.picture_count` (a new last field) says how many there are.
- Playlist files (`ffaudio_playlist.h`): `au_playlist_read()` reads an `.m3u`, `.m3u8` or `.pls` file or URL (an
  internet radio station's `.pls`, say) and `au_playlist_parse()` text already in memory; both return an
  `AuPlaylistFile` of entries (path, title, duration) for `au_play()` / `au_queue()`, and play nothing themselves.
  Relative entries are resolved against the playlist's folder or URL, `file://` URIs become paths, and text that is
  not UTF-8 is read as Latin-1. An HLS playlist is a stream, so it gives one entry: itself. A playlist from a URL may
  only list URLs (local paths in it are left out), like the protocol rules for streams. Works without `au_init()`.
- Upmixing: `au_set_upmix(mode, custom)` decides how files with fewer channels than a surround output play.
  `AU_UPMIX_NONE` (the default, as before) keeps each channel on its own speaker; `AU_UPMIX_SURROUND` spreads mono
  and stereo over every speaker with FFmpeg's `surround` filter; `AU_UPMIX_CUSTOM` takes your own filters (a `pan`
  matrix, `surround` with other options). Stereo and mono outputs are not affected.
- Source filters: `au_set_source_filters()` runs filters on every track's decoded audio before the conversion to the
  output format, at the file's own rate and layout, for repairs that need the samples as stored.
- Restoration helpers: `au_fb_add_declick()` (FFmpeg `adeclick`, for vinyl and shellac transfers) and
  `au_fb_add_declip()` (`adeclip`, rebuilds clipped peaks), with `AuDeclick` / `AuDeclip` settings. Declip belongs in
  `au_set_source_filters()`: after resampling to the device rate it finds nothing to repair.

### Fixed
- Chained Ogg files (parts one after another, e.g. a recorded Ogg radio stream or concatenated Vorbis/Opus/FLAC
  files) reported only their last part's duration, as FFmpeg does. ffaudio now adds the parts up, for `au_probe()`,
  the player and `AU_EVENT_TRACK_OPENED`. FFmpeg can not seek past the first part of such a file, so it is reported
  as not seekable (`AuMediaInfo.seekable` false) and seeks are ignored, as for live streams. Network files keep
  FFmpeg's duration (finding the parts would need a request per page).

## 3.0.0 (2026-11-23)

The API is new; `docs/api-3.0.md` describes it and has a migration table from 2.x.

### Changed
- Events: one `AuEvent` (a type, the track's handle and a union) replaces the ten callbacks. It goes to
  `AuConfig.on_event` on ffaudio's control thread, or, without a callback, into a queue for `au_poll_event()` /
  `au_wait_event()` on the application's own thread (4096 events; log events are dropped first when it is full, and
  `AU_EVENT_OVERFLOW` says how many). A track's end is `AU_EVENT_TRACK_ENDED` with `AU_END_FINISHED`, `_SKIPPED` or
  `_ERROR`.
- `au_init(&AuConfig)` replaces `au_initialize()` and opens the output device itself (`AuConfig.device`, NULL = the
  system default). `au_set_device(name)` replaces `au_configure_audio_device()` and returns at once; the result comes
  as `AU_EVENT_DEVICE` (opened, or not; `fell_back` when the chosen device failed and the default was opened). The
  device index is gone: indexes change as devices come and go. `au_list_devices()`, `au_free_devices()` and
  `au_get_device_name()` replace `au_get_audio_devices()`, `au_free_audio_devices()` and `au_get_audio_device_name()`.
- `AuConfig.volume` is a linear gain 0..1 (was `initial_volume` 0..100); `loop_count` was `initial_loop_count`.
- `delegates.h` is gone; everything is in `ffaudio.h`.
- The export macro is `FFAUDIO_API` (was `DLL_EXPORT`, a name other libraries use too). The shared library's version is
  3.0 (`libffaudio.so.3.0`), so 2.x programs never load it by mistake.
- Playback: `au_play()`, `au_queue()`, `au_skip()`, `au_clear_queue()`, `au_stop()`, `au_pause()`, `au_seek(seconds)`,
  `au_seek_fraction(0..1)`, `au_set_speed()`, `au_set_volume(0..1)` and `au_set_muted()` replace `au_play_audio()`,
  `au_set_next()`, `au_skip_to_next()`, `au_clear_next()`, `au_stop_audio()`, `au_pause_audio()`,
  `au_seek_time(ms)`, `au_seek_percent(0..100)`, `au_set_playback_speed()`, `au_set_audio_volume(0..100)` and
  `au_mute_audio()`. `AuPlayConfig` (`start_seconds`, `length_seconds`, `filters`) and `AuCrossfade` replace
  `PlayAudioConfig` (`skip_seconds`, `play_duration`, `av_filtergraph`) and `CrossfadeConfig`.
- `au_get_status()` fills one `AuStatus` snapshot (the audible track, its position and duration, buffering, the
  output format, and the settings as last set, also before the control thread has applied them). It replaces
  `au_get_current_handle()`, `au_get_audio_play_time()`, `au_get_audio_duration()`, `au_get_output_spec()`,
  `au_is_paused()`, `au_is_muted()`, `au_get_audio_volume()`, `au_get_playback_speed()` and `au_get_loop_count()`.
- No function waits for the control thread any more. `au_track_info()` and `au_track_gain_db()` (were
  `au_get_track_info()` and `au_get_track_gain_db()`) read what the track has published instead of asking the control
  thread; they know a track from `AU_EVENT_TRACK_OPENED` (new: its duration and whether it is live or seekable) until
  `AU_EVENT_TRACK_ENDED`. Calling the API from `on_event` is therefore always safe and never blocks the player.
- `AuMediaInfo` is a struct to read directly (path, container, codec, layout, duration, rates, live/seekable,
  track/disc numbers and totals, ReplayGain values, and a `tags` array): one immutable allocation with every string
  in it, shared by reference. `au_probe()` and `au_track_info()` return `const AuMediaInfo *`, released with
  `au_media_info_release()`; `au_track_info()` no longer copies anything, and a stream's "now playing" change makes
  a new block, so one in hand never changes. `au_media_tag()` (with its aliases) stays; the 16 other `au_media_*()`
  getters and `au_media_info_free()` are gone.
- `AuReplayGain` (four floats, NAN = missing, 20 bytes) replaces `AuReplayGainInfo` (doubles and `has_*` flags, 72
  bytes) everywhere: `AuPlayConfig.replaygain`, the ReplayGain cache, the background scan and
  `au_loudness_to_replaygain()`. `AU_REPLAYGAIN_NONE` is the value with nothing set. The cache file's format is
  unchanged. `AuReplayGainConfig.measure_wait_seconds` replaces `measure_wait_ms`.
- Filters: `au_set_filters()`, `au_set_track_filters()` and `au_validate_filters()` replace
  `au_set_global_filtergraph()`, `au_update_filtergraph()` and `au_validate_filtergraph()`. Every `au_fb_add_*()`
  helper takes the instance name as its second argument (NULL = none) instead of `au_fb_set_name()` setting it for
  the next call.
- The graphic equalizer (`au_fb_add_equalizer()`) is one peaking biquad per band (FFmpeg `equalizer`, in double
  precision) instead of `anequalizer`: it no longer takes the output channel count, and each band can be changed
  while playing on its own (`equalizer@<name>_<band>`, commands `f`, `w`, `g`). It sounds slightly different, since
  the bands are cookbook biquads rather than anequalizer's filters.
- Loudness: `AuLoudnessMeter` replaces `AuLoudnessScanner`. `au_loudness_measure()` measures a file into a new meter,
  `au_loudness_merge()` adds one meter to another, `au_loudness_result()` gives the values and
  `au_loudness_file_count()` how many files a meter holds. A meter can be made on any thread and merged later, so an
  album's tracks can be measured in parallel; merging is exact (the same result in any order, and as measuring all
  of it at once). `au_loudness_analyze()` stays for a single file.

### Fixed
- In applications with a comma-decimal locale (`LC_NUMERIC` such as de_DE or fr_FR, which GTK applications set from
  the user's settings), filter strings with decimals failed: FFmpeg reads option values with `strtod()`, so
  `volume=0.5`, the filter builder's strings, `au_set_downmix()` matrices and `au_validate_filtergraph()` were all
  rejected, and a track with such a filter played silence. ffaudio's threads now use the C locale, and the calls that
  parse filter strings on the application's thread switch to it while they do.
- `au_probe()` now opens inputs like the player does: MPEG-TS programs announced late are found, and network inputs
  get the reconnect settings.

## 2.1.0 (2026-11-20)

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
- ReplayGain measurement of network files only with `AuNetworkConfig.measure_streams` (measuring downloads the
  whole file; albums with a network track are skipped without it). Such measurements stop right away on
  `au_gain_scan_stop()` and `au_gain_cache_cancel_requests()`, also while connecting or waiting to reconnect.
- The readme's "Memory use" covers streaming on Linux: with glibc's default settings, the read-ahead buffers make a
  long-running player grow (240 MB after 2 hours in the soak test, still rising); fixed malloc thresholds
  (`GLIBC_TUNABLES` or `mallopt()`) kept it flat at about 80 MB.

### Changed
- The audio callback no longer takes locks, frees memory or wakes other threads: decoded audio reaches the mixer
  through a lock-free ring per track, and the mixer computes gains per 64-frame block. Seeking a paused or queued
  track no longer leaves it to start with a gap.
- One thread per local track instead of two (network streams keep their reader thread): it reads, decodes and
  filters, waits for events instead of polling every 10 ms, and stops at the end of a file instead of reading it again
  100 times a second until the track is removed.
- The control thread sleeps until a command or a mixer event needs it (or 50 ms while tracks are loaded, 500 ms when
  idle) instead of waking every 5 ms, and never waits for a track's thread to end: one stuck in a DNS lookup used to
  hold up every command and callback. With the change above, a player playing one track wakes about 47 times a
  second instead of 341 (24 instead of 223 when idle), with 4 threads instead of 5.
- Loudness measurement keeps gating and short-term blocks in histograms (0.01 LU bins with exact energy sums)
  instead of lists of every block: the memory no longer grows with the length, and an album is the sum of its tracks.
  The true peak comes from a polyphase interpolator run on the decoded audio instead of a second resampler: scans are
  about a third faster (8.0 s -> 5.0 s for a mixed set of files). Integrated loudness and gains are unchanged;
  true peaks can differ by about 0.1 dB, within EBU Tech 3341's tolerance. Peaks now include the LFE channel, which
  is played and can clip.
- Custom fade curves (`AuFade.custom`) are sampled at 257 points when the fade is set up (`au_set_crossfade()`, or
  queueing a track with `PlayAudioConfig.fade_in`), on ffaudio's control thread, instead of being called on the
  audio thread throughout the fade; `custom_user` only has to live until that call has been processed.

### Fixed
- The position of a chained Ogg stream (how Ogg radio changes songs) went back to 0 at every new part, whose
  timestamps start again; it now goes on. The same applies to the other containers FFmpeg marks as having timestamp
  jumps (MPEG-TS, HLS). Seeks and loops still go back.
- With real SDL2 (as on Ubuntu and Debian; SDL3's sdl2-compat was not affected), a synchronous call from a callback
  (`au_configure_audio_device()`, `au_get_track_info()`, `au_get_track_gain_db()`) hung the player for good: the
  control thread did not recognise itself, so the call waited for the thread that was making it.
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
- Measuring a file whose read failed partway (a disk error, or a download that broke off) returned
  the loudness of the part that was read, and the scan queue stored it as the file's. It now fails with
  `AU_ERR_FILE`.
- Measuring a file with an unspecified channel layout (e.g. a WAV without a channel mask) rebuilt the sample
  converter for every decoded frame.
- A track waiting for its measurement (`measure_missing`) starts as soon as the measurement fails or is cancelled,
  instead of after `measure_wait_ms`.
- `au_gain_cache_request_album()` does not queue an album again while it is queued or being measured; an urgent
  request moves it to the front.
- A request made while `au_gain_scan_stop()` was running could leave the queue without workers for good.
- The files of one `au_gain_cache_request()` were measured by a single worker, whatever `AuGainScanConfig.threads`
  said, and an urgent request for a file inside a queued request moved that whole request. Each file is now its own
  job; duplicate checks no longer compare the path with everything queued, and each worker reuses its scanner.
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
- The ReplayGain cache file was not safe to share between programs, as the readme said: a compaction renamed a new
  file over it, so another program kept appending to the replaced file, and records appended meanwhile were dropped
  (in a test with two processes, 492 of 500 entries were lost). Writes now take a lock (a `.lock` file next to the
  cache), a compaction reads the file again first, and appends follow a replaced file. A file of a later format is
  left alone instead of being read as this one and appended to. Loading reads the file in one go (100000 entries:
  40 ms instead of 74).
- Files whose timestamps start late (MPEG-TS and so HLS, which start at 1.4 s or more) reported positions that
  included that start, while seeks and the duration count from 0: `au_get_audio_play_time()` was too large, end
  crossfades and `on_prepare_next` came that much early, and a device change jumped ahead by it.


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
