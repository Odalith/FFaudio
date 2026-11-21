# ffaudio 3.0 API (draft for review)

What the public headers become in 3.0.0. The decisions it follows (2026-11-21):

- **One global player.** No player object; every function works on the one player, as in 2.x.
- **One event type.** `AuEvent` replaces the ten callbacks. It is delivered either to `on_event` on ffaudio's control
  thread, or, when no callback is set, queued for `au_poll_event()` on the application's own thread.
- **Nothing waits for the control thread.** A call that changes something queues the change and returns at once; if
  the change has a result (a device opened, a track's details are known), it comes as an event. Getters read a
  snapshot. So every function may be called from `on_event`, except `au_shutdown()`.
- **Full naming cleanup.** `Au`-prefixed types, short verbs, seconds for every time and position, volume as a float gain.

Marked **[choice]**: details the decisions above leave open, which I filled in. Each says what the alternative is.

## Conventions

- Functions are `au_<verb>` or `au_<thing>_<verb>`; types are `AuName`; enum values are `AU_NAME`.
- Times, positions and durations are `double` seconds, everywhere in the player API. DSP parameters keep their
  usual units (a limiter's attack in ms, an EQ band in Hz and dB).
- Functions that can fail return `int`: `AU_OK` (0) or a negative `AU_RESULT`, unchanged from 2.x.
- The export macro becomes `FFAUDIO_API`. `DLL_EXPORT` is too generic a name for a public header: it collides with
  other libraries' macros.
- `delegates.h` is merged into `ffaudio.h`.

## Lifetime and devices

```c
typedef struct AuConfig {
    const char *app_name;
    float volume;                  // linear gain 0..1; < 0 = 1
    int loop_count;                // -1 = forever, 0 = none
    double prepare_next_seconds;   // AU_EVENT_PREPARE_NEXT this long before the end; 0 = 10
    int output_channels;           // 0 = the device's choice
    int output_sample_rate;        // 0 = the device's choice
    const char *device;            // NULL = follow the system default
    AuEventFn on_event;            // NULL = events are queued for au_poll_event()
    void *user_data;               // passed to on_event
} AuConfig;

int  au_init(const AuConfig *config);    // also starts opening the device (AU_EVENT_DEVICE)
void au_shutdown(void);                  // not from on_event

int  au_set_device(const char *name);    // NULL = follow the system default; result: AU_EVENT_DEVICE
char **au_list_devices(int *count);      // free with au_free_devices()
void au_free_devices(char **devices, int count);
int  au_get_device_name(char *buf, size_t size);
```

- **[choice]** `au_init()` opens the device itself (`AuConfig.device`, default = system default). In 2.x, every
  application called `au_configure_audio_device(NULL)` straight after `au_initialize()`. Alternative: keep a separate
  call.
- The device index from `AudioDeviceConfig` is dropped: indexes change as devices come and go, and the name is what
  2.x already preferred.

## Playback

```c
typedef struct AuPlayConfig {
    double start_seconds;          // 0 = from the start
    double length_seconds;         // 0 = to the end
    const char *filters;           // this track's own filter graph, NULL = none
    const AuFade *fade_in;         // NULL = the AuCrossfade setting
    const AuReplayGain *replaygain; // NULL = cache, then tags
    const char *http_headers;      // network streams: "Name: value" lines
} AuPlayConfig;

int32_t au_play(const char *path, const AuPlayConfig *config);   // handle > 0, at once
int32_t au_queue(const char *path, const AuPlayConfig *config);  // the next track; replaces a queued one
void au_skip(void);
void au_clear_queue(void);
void au_stop(void);
void au_pause(bool paused);
void au_seek(double seconds);
void au_seek_fraction(double fraction);  // 0..1 of the duration
int  au_set_speed(double speed, bool keep_pitch);
void au_set_volume(float gain);          // linear 0..1
void au_set_muted(bool muted);
void au_set_loop_count(int count);
int  au_set_crossfade(const AuCrossfade *config);   // CrossfadeConfig renamed, same members
```

- **[choice]** `au_queue()` instead of `au_next()`, the name in the question. `au_next()` reads like "skip to the next
  track", which is `au_skip()`. Alternative: `au_next()` as agreed, or `au_set_next()` as in 2.x.
- A file that can not be opened still gets its handle; the track then ends with `AU_END_ERROR`.

## Status

```c
typedef struct AuStatus {
    int32_t handle;                // the audible track, 0 = none
    double position;               // seconds, -1 = nothing playing
    double duration;               // seconds, -1 = unknown
    bool paused, muted, buffering;
    float volume;
    double speed;
    bool keep_pitch;
    int loop_count;
    int sample_rate, channels;     // the output format, 0 = no device yet
} AuStatus;

void au_get_status(AuStatus *out);   // one snapshot; never waits
const AuMediaInfo *au_track_info(int32_t handle);   // 0 = current; NULL until AU_EVENT_TRACK_OPENED
int  au_track_gain_db(int32_t handle, double *out_db);
```

- One snapshot replaces nine getters (`au_is_paused()`, `au_get_audio_volume()`, `au_get_audio_play_time()`, ...),
  so the values in it belong together.
- The settings in it (volume, speed, loop count, paused, muted) are the last ones set, even before the control thread
  has applied them. So `au_set_volume(0.5)` followed by `au_get_status()` reads 0.5.

## Events

```c
typedef enum AU_EVENT {
    AU_EVENT_LOG,              // data.log
    AU_EVENT_DEVICE,           // data.device: opened, or failed (ok = false)
    AU_EVENT_DEVICES_CHANGED,  // a device was added or removed: call au_list_devices()
    AU_EVENT_TRACK_OPENED,     // data.opened: au_track_info() has the details now
    AU_EVENT_TRACK_STARTED,    // the track became the audible one
    AU_EVENT_TRACK_ENDED,      // data.ended; exactly once per handle
    AU_EVENT_RESTARTED,        // data.restarted: after a seek or a loop
    AU_EVENT_DURATION,         // data.duration: the duration changed
    AU_EVENT_PREPARE_NEXT,     // the current track nears its end and nothing is queued
    AU_EVENT_BUFFERING,        // data.buffering
    AU_EVENT_STREAM_METADATA,  // data.metadata: "now playing" changed
    AU_EVENT_OVERFLOW,         // au_poll_event() only: events were dropped (data.overflow.dropped)
} AU_EVENT;

typedef enum AU_END { AU_END_FINISHED, AU_END_SKIPPED, AU_END_ERROR } AU_END;

typedef struct AuEvent {
    AU_EVENT type;
    int32_t handle;                // the track, 0 for player-wide events
    union {
        struct { AU_LOG_LEVEL level; const char *message; int64_t request; } log;
        struct { bool ok; bool fell_back; const char *name; int sample_rate, channels; } device;
        struct { double duration; bool live, seekable; } opened;
        struct { AU_END reason; } ended;
        struct { double position; bool from_loop; int loops_remaining; } restarted;
        struct { double seconds; } duration;
        struct { bool active; double buffered_seconds; } buffering;
        struct { const char *title, *artist; } metadata;
        struct { int dropped; } overflow;
    } data;
} AuEvent;

typedef void (*AuEventFn)(const AuEvent *event, void *user_data);

bool au_poll_event(AuEvent *out);                     // false = none waiting
bool au_wait_event(AuEvent *out, double timeout_seconds);  // for a thread that only handles events
```

- The union is named (`data`) because C99 has no anonymous unions. C# maps it with `StructLayout(Explicit)`.
- Strings stay valid during the callback. With polling, they stay valid until the next `au_poll_event()` or
  `au_wait_event()`.
- **[choice]** The poll queue holds 4096 events. When it is full, log events are dropped first, then the oldest of
  the rest, and an `AU_EVENT_OVERFLOW` reports how many were dropped. Alternative: an unbounded queue, which grows
  without limit if the application stops polling.
- The background gain scan keeps its own callback (`AuGainScanConfig.on_cached`, on a worker thread), because it
  works without `au_init()`.

## Media information and ReplayGain

```c
typedef struct AuTag { const char *key, *value; } AuTag;

typedef struct AuMediaInfo {
    const char *path, *container, *codec, *channel_layout;
    double duration;               // seconds, -1 = unknown
    int64_t bit_rate;              // 0 = unknown
    int sample_rate, channels, bits_per_sample;
    bool live, seekable;
    int track_number, track_total, disc_number, disc_total;   // 0 = unknown
    AuReplayGain replaygain;       // from the tags
    int tag_count;
    const AuTag *tags;
} AuMediaInfo;

const AuMediaInfo *au_probe(const char *path, int *out_error);
void au_media_info_release(const AuMediaInfo *info);
const char *au_media_tag(const AuMediaInfo *info, const char *key);   // aliases: "ALBUMARTIST", "year", ...

typedef struct AuReplayGain {
    float track_gain_db, track_peak, album_gain_db, album_peak;   // NAN = missing
    bool from_r128;
} AuReplayGain;
```

- One immutable allocation (header, strings, tags), shared by reference: `au_track_info()` returns the player's own
  copy, with no copy and no wait. It replaces 16 `au_media_*` getters.
- `AuReplayGain` is 20 bytes instead of 72. A missing value is NaN instead of a separate `has_*` flag
  (`isnan(rg.track_gain_db)`).

## Filters

```c
int au_set_filters(const char *filters);                       // was au_set_global_filtergraph
int au_set_track_filters(int32_t handle, const char *filters); // was au_update_filtergraph
int au_send_filter_command(int32_t handle, const char *target, const char *command, const char *arg);
int au_validate_filters(const char *filters);
int au_set_downmix(AU_DOWNMIX mode, const char *custom_filters);

// Builder: the instance name is an argument instead of state
int au_fb_add_equalizer(AuFilterBuilder *fb, const char *name, const AuEqualizer *eq);   // no channel count
int au_fb_add_limiter(AuFilterBuilder *fb, const char *name, const AuLimiter *config);
/* ... every au_fb_add_*() the same way; au_fb_set_name() is gone */
```

- The graphic EQ becomes one `equalizer` biquad per band, like the parametric EQ, instead of `anequalizer`. It no
  longer needs the output channel count, and each band takes its own commands (`equalizer@<name>_<band>`, `g`).
  It sounds slightly different, because the bands are cookbook biquads rather than anequalizer's.
- The builder writes its string with `AVBPrint`.

## Loudness

```c
typedef struct AuLoudnessMeter AuLoudnessMeter;   // one file, or several merged

int  au_loudness_measure(const char *path, const AuLoudnessConfig *config, AuLoudnessMeter **out);
int  au_loudness_merge(AuLoudnessMeter *into, const AuLoudnessMeter *other);   // an album = its tracks merged
int  au_loudness_result(const AuLoudnessMeter *meter, AuLoudness *out);
void au_loudness_meter_free(AuLoudnessMeter *meter);
int  au_loudness_analyze(const char *path, const AuLoudnessConfig *config, AuLoudness *out);   // one file, at once
AuReplayGain au_loudness_to_replaygain(const AuLoudness *track, const AuLoudness *album);
```

- This replaces the scanner object (`create` / `scan_file` / `album` / `reset` / `track_count`). A meter can be made
  on any thread and merged later, so an application can measure an album's tracks in parallel. Stage 2's histograms
  made merging exact.
- The ReplayGain cache and the background scan keep their functions; they now take `AuReplayGain`, and
  `measure_wait_ms` becomes `measure_wait_seconds`.

## Migration from 2.x

| 2.x | 3.0 |
|---|---|
| `au_initialize(&InitializeConfig)` + `au_configure_audio_device(NULL)` | `au_init(&AuConfig)` |
| `au_configure_audio_device(&AudioDeviceConfig)` | `au_set_device(name)` + `AU_EVENT_DEVICE` |
| `au_get_audio_devices` / `au_free_audio_devices` | `au_list_devices` / `au_free_devices` |
| `au_get_output_spec` | `AuStatus.sample_rate`, `.channels` |
| `au_play_audio` / `au_set_next` | `au_play` / `au_queue` |
| `au_skip_to_next` / `au_clear_next` / `au_stop_audio` | `au_skip` / `au_clear_queue` / `au_stop` |
| `au_pause_audio(b)` / `au_is_paused()` | `au_pause(b)` / `AuStatus.paused` |
| `au_seek_time(ms)` / `au_seek_percent(0..100)` | `au_seek(seconds)` / `au_seek_fraction(0..1)` |
| `au_set_playback_speed` / `au_get_playback_speed` | `au_set_speed` / `AuStatus.speed` |
| `au_set_audio_volume(0..100)` / `au_get_audio_volume` | `au_set_volume(0..1)` / `AuStatus.volume` |
| `au_mute_audio` / `au_is_muted` | `au_set_muted` / `AuStatus.muted` |
| `au_get_loop_count` | `AuStatus.loop_count` |
| `au_get_current_handle` / `au_get_audio_play_time` / `au_get_audio_duration` | `AuStatus.handle` / `.position` / `.duration` |
| `au_get_track_info(handle)` (waited) | `au_track_info(handle)` (snapshot) |
| `au_get_track_gain_db` (waited) | `au_track_gain_db` (snapshot) |
| `CrossfadeConfig` / `PlayAudioConfig` | `AuCrossfade` / `AuPlayConfig` |
| `PlayAudioConfig.skip_seconds` / `.play_duration` / `.av_filtergraph` | `.start_seconds` / `.length_seconds` / `.filters` |
| `on_eof(skip, error, handle)` | `AU_EVENT_TRACK_ENDED`, `data.ended.reason` |
| `on_restart` / `on_duration_update` / `on_prepare_next` | `AU_EVENT_RESTARTED` / `AU_EVENT_DURATION` / `AU_EVENT_PREPARE_NEXT` |
| `on_track_started` / `on_buffering` / `on_stream_metadata` | `AU_EVENT_TRACK_STARTED` / `AU_EVENT_BUFFERING` / `AU_EVENT_STREAM_METADATA` |
| `on_audio_device_update` / `on_audio_devices_changed` | `AU_EVENT_DEVICE` / `AU_EVENT_DEVICES_CHANGED` |
| `on_log` | `AU_EVENT_LOG` |
| `au_set_global_filtergraph` / `au_update_filtergraph` / `au_validate_filtergraph` | `au_set_filters` / `au_set_track_filters` / `au_validate_filters` |
| `au_fb_set_name(fb, n); au_fb_add_x(fb, ...)` | `au_fb_add_x(fb, n, ...)` |
| `au_fb_add_equalizer(fb, channels, &eq)` | `au_fb_add_equalizer(fb, name, &eq)` |
| `AuReplayGainInfo` (`has_*` flags, doubles) | `AuReplayGain` (floats, NaN = missing) |
| `AuMediaInfo *` + `au_media_*()` getters + `au_media_info_free` | `const AuMediaInfo *` fields + `au_media_info_release` |
| `AuLoudnessScanner` | `AuLoudnessMeter` (`measure` / `merge` / `result`) |
| `AuReplayGainConfig.measure_wait_ms` | `.measure_wait_seconds` |
| `DLL_EXPORT` | `FFAUDIO_API` |

Unchanged: the ReplayGain cache (`au_gain_cache_*`), the background scan (`au_gain_scan_*`, `au_gain_cache_request*`),
`au_set_network()`, `au_set_replaygain()`, `au_set_downmix()`, `au_send_filter_command()`, `au_set_loop_count()`,
`au_get_version()`, `au_free()`.
