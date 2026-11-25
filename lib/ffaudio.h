/*
* Copyright (c) 2003 Fabrice Bellard, 2025 Odalith
 *
 * This file was part of FFmpeg, particularly FFplay.
 *
 * ffaudio is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * ffaudio is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFaudio; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef FFAUDIO_H
#define FFAUDIO_H

#if defined(_WIN32)
    #if defined(FFAUDIO_BUILDING)
        #define FFAUDIO_API __declspec(dllexport)
    #else
        #define FFAUDIO_API __declspec(dllimport)
    #endif
#else
    #define FFAUDIO_API __attribute__((visibility("default")))
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <math.h>

#define FFAUDIO_VERSION_MAJOR 4
#define FFAUDIO_VERSION_MINOR 0
#define FFAUDIO_VERSION_PATCH 0

#ifdef __cplusplus
extern "C" {
#endif

    // Return codes. Functions returning int return AU_OK (0) on success or a negative AU_RESULT.
    typedef enum AU_RESULT {
        AU_OK = 0,
        AU_ERR_NOT_INITIALIZED = -1,
        AU_ERR_INVALID_ARGUMENT = -2,
        AU_ERR_ALREADY_INITIALIZED = -3,
        AU_ERR_NO_MEMORY = -4,
        AU_ERR_SDL = -5,
        AU_ERR_DEVICE = -6,
        AU_ERR_WRONG_THREAD = -7,
        AU_ERR_NOTHING_TO_DO = -8,
        AU_ERR_FILE = -9,              // a file could not be read (missing, no permission, an I/O or network error)
        AU_ERR_CANCELLED = -10,
        // not available in this build (e.g. an FFmpeg filter that was not compiled in), or no audio that can be played
        // in the file (not a media file, no audio stream, a codec this FFmpeg does not have)
        AU_ERR_UNSUPPORTED = -11,
        AU_ERR_NOT_SAVED = -12,        // done in memory, but the ReplayGain cache file could not be written. Since 4.0.
    } AU_RESULT;

    typedef enum AU_LOG_LEVEL {
        AU_LOG_INFO = 0,
        AU_LOG_WARNING = 1,
        AU_LOG_ERROR = 2,
        AU_LOG_FATAL = 3,
    } AU_LOG_LEVEL;

    //+-----------------------------------------------------+
    //| Events                                              |
    //+-----------------------------------------------------+
    // Everything the player reports comes as an AuEvent: to the on_event given to au_init(), on ffaudio's control
    // thread, or, without one, queued for au_poll_event() / au_wait_event() on a thread of the application's choosing.
    // No function waits for the control thread, so every function may be called from on_event (except
    // au_shutdown()); what such a call changes is reported by later events.

    typedef enum AU_EVENT {
        AU_EVENT_LOG = 0,          // data.log: a message from the library, or from FFmpeg ("FFmpeg <part>: ...", since 3.2)
        AU_EVENT_DEVICE,           // data.device: the output device was opened (ok), or could not be (!ok)
        AU_EVENT_DEVICES_CHANGED,  // an output device was added or removed: au_list_devices() has the new list
        AU_EVENT_TRACK_OPENED,     // data.opened: the track is open; au_track_info() has its details now
        AU_EVENT_TRACK_STARTED,    // the track became the audible one (start, gapless/crossfade transition, skip)
        AU_EVENT_TRACK_ENDED,      // data.ended: the track left the player; exactly once for every handle
        AU_EVENT_RESTARTED,        // data.restarted: the current track restarted, after a seek or a loop
        AU_EVENT_DURATION,         // data.duration: the duration of the track became known or changed
        AU_EVENT_PREPARE_NEXT,     // the current track nears its end and nothing is queued (also when a queued
                                   // track fails to open): a good moment for au_queue()
        AU_EVENT_BUFFERING,        // data.buffering: a network track waits for data, or plays again
        AU_EVENT_STREAM_METADATA,  // data.metadata: the stream's "now playing" changed, when that part is heard
        AU_EVENT_OVERFLOW,         // au_poll_event() only: data.overflow.dropped events were dropped (queue full)
    } AU_EVENT;

    typedef enum AU_END {
        AU_END_FINISHED = 0,       // played to the end
        AU_END_SKIPPED,            // replaced, skipped, cleared or stopped
        AU_END_ERROR,              // could not be opened, or decoding failed
    } AU_END;

    typedef struct AuEvent {
        AU_EVENT type;
        int32_t handle;            // the track the event is about; 0 for player-wide events
        union {
            // request: the number of API calls made when it was logged
            struct { AU_LOG_LEVEL level; const char *message; int64_t request; } log;
            // name: the device now in use (for the system default, what it resolves to when SDL can tell);
            // fell_back: the chosen device could not be opened, the system default was opened instead.
            // !ok: no device could be opened (name: the one asked for, NULL = the system default)
            struct { bool ok; bool fell_back; const char *name; int32_t sample_rate, channels; } device;
            // duration: seconds, -1 = unknown; live: a stream with no end (no duration, no seeking)
            struct { double duration; bool live, seekable; } opened;
            // error (AU_END_ERROR only, otherwise AU_OK): why, as an AU_RESULT (AU_ERR_FILE, AU_ERR_UNSUPPORTED,
            // AU_ERR_DEVICE, AU_ERR_NO_MEMORY, ...); message: the text that was logged for it (NULL without an error).
            // Since 4.0.
            struct { AU_END reason; int32_t error; const char *message; } ended;
            // position in seconds; from_loop: a loop started (loops_remaining after it), otherwise a seek
            struct { double position; bool from_loop; int32_t loops_remaining; } restarted;
            struct { double seconds; } duration;
            // active: waiting for data (the position holds); about every 500 ms while it lasts, with what is queued
            struct { bool active; double buffered_seconds; } buffering;
            // Icecast "Artist - Title" is split at " - "; artist is NULL when there is none
            struct { const char *title, *artist; } metadata;
            struct { int32_t dropped; } overflow;
            // Fixes the size of AuEvent (72 bytes) for the major version, so events may carry more in 4.x
            uint8_t padding[64];
        } data;
    } AuEvent;

    // Strings in the event are valid until the callback returns
    typedef void (*AuEventFn)(const AuEvent *event, void *user_data);

    // The oldest queued event, when au_init() had no on_event. false = none. Its strings are valid until the next
    // au_poll_event() / au_wait_event() call or au_shutdown(). The queue holds 4096 events; when it is full, log events
    // are dropped first, then the oldest, and the next poll returns AU_EVENT_OVERFLOW with the number dropped.
    FFAUDIO_API bool au_poll_event(AuEvent *out);

    // au_poll_event() that waits up to timeout_seconds (< 0 = no limit) for an event, for a thread that only handles
    // events. false on timeout, and once au_shutdown() has been called.
    FFAUDIO_API bool au_wait_event(AuEvent *out, double timeout_seconds);

    //+-----------------------------------------------------+
    //| Fades and crossfades                                |
    //+-----------------------------------------------------+

    // A zeroed AuFade has the default curve, equal power. (Before 4.0 the zero value was linear, and other values
    // than these fell back to linear; they are refused now.)
    typedef enum AU_FADE_CURVE {
        AU_FADE_EQUAL_POWER = 0,  // sin/cos; constant loudness for uncorrelated material. The default
        AU_FADE_LINEAR = 1,       // gain changes linearly; a crossfade dips ~3 dB in the middle
        AU_FADE_S_CURVE = 2,      // smoothstep; gentle start and end
        AU_FADE_LOGARITHMIC = 3,  // fast rise, slow finish
        AU_FADE_EXPONENTIAL = 4,  // slow rise, fast finish
        AU_FADE_CUSTOM = 5,       // use AuFade.custom (which must be set)
    } AU_FADE_CURVE;

    // t goes 0 -> 1 over the fade; return the gain (0..1) of the track fading IN.
    // The track fading out uses curve(1 - t). Called on ffaudio's control thread while the fade is set up
    // (au_set_crossfade(), or the track with its own fade-in, au_play_options_set_fade_in(), being queued): the curve
    // is sampled at 257 points, which are used from then on, so custom_user only has to live until that call has been
    // processed.
    typedef float (*AuFadeCurveFn)(float t, void *custom_user);

    // How a fade happens. seconds <= 0 means no fade (cut, or gapless at a natural track end).
    typedef struct AuFade {
        double seconds;
        AU_FADE_CURVE curve;
        AuFadeCurveFn custom;
        void *custom_user;
    } AuFade;

    // When fades happen. Every member defaults to "no fade" when zeroed.
    typedef struct AuCrossfade {
        AuFade on_track_end;   // the current track nears its end and a track is queued with au_queue()
        AuFade on_skip;        // au_skip()
        AuFade on_play;        // au_play() while another track is playing
        AuFade on_stop;        // au_stop(): fade out, then stop
        AuFade on_pause;       // au_pause(true) fades out, au_pause(false) fades back in
        // Tracks (current or queued) shorter than this many seconds are not crossfaded at their end;
        // they play gapless instead. 0 = 2 * on_track_end.seconds. An end crossfade is also never
        // longer than half of either track. Tracks of unknown length always play gapless.
        double min_track_seconds;
    } AuCrossfade;

    //+-----------------------------------------------------+
    //| ReplayGain                                          |
    //+-----------------------------------------------------+

    // Loudness values of a file. Gains are in dB relative to the ReplayGain 2.0 reference (-18 LUFS);
    // R128_TRACK_GAIN / R128_ALBUM_GAIN tags (the standard for Opus, RFC 7845, but read from any format; relative
    // to -23 LUFS) are used when ReplayGain tags are missing and converted to that reference (+5 dB).
    // NAN marks a missing value: isnan(rg.album_gain_db)
    typedef struct AuReplayGain {
        float track_gain_db;
        // linear, 1.0 = full scale: the true peak when ffaudio measured it (the sample peak with skip_true_peak), the
        // tag's value (normally a sample peak) when it came from tags
        float track_peak;
        float album_gain_db;
        float album_peak;            // as track_peak
        bool from_r128;              // the gains came from R128_TRACK_GAIN / R128_ALBUM_GAIN
    } AuReplayGain;

    // No values at all (a C compound literal; C++ has no compound literals, so it gets a temporary)
#ifdef __cplusplus
    #define AU_REPLAYGAIN_NONE (AuReplayGain{ NAN, NAN, NAN, NAN, false })
#else
    #define AU_REPLAYGAIN_NONE ((AuReplayGain){ NAN, NAN, NAN, NAN, false })
#endif

    typedef enum AU_REPLAYGAIN_MODE {
        AU_REPLAYGAIN_OFF = 0,
        AU_REPLAYGAIN_TRACK,         // track gain; album gain if the track has none
        AU_REPLAYGAIN_ALBUM,         // album gain; track gain if the track has none
    } AU_REPLAYGAIN_MODE;

    typedef struct AuReplayGainSettings {
        AU_REPLAYGAIN_MODE mode;
        double preamp_db;            // added to tagged gains, -15..+15 dB
        double fallback_gain_db;     // used for files without ReplayGain/R128 tags, -30..+15 dB (e.g. -6)
        bool prevent_clipping;       // lower the gain so the tagged peak does not exceed full scale
        // Measure a starting track that has no value (no au_play_options_set_replaygain(), no cache entry, no
        // ReplayGain/R128 tags) with the background queue and store it in the ReplayGain cache
        // (ffaudio_loudness.h). Off by default because it writes to the per-user cache file.
        bool measure_missing;
        // How long such a track waits for its measurement before it starts at fallback_gain_db; the value is
        // then faded in over 2 s when it arrives. 0 = 0.5 s, < 0 = do not wait. At most 10.
        double measure_wait_seconds;
    } AuReplayGainSettings;

    //+-----------------------------------------------------+
    //| Options for one track                               |
    //+-----------------------------------------------------+

    // The options of a track to play, for au_play() and au_queue(), which copy them: one object can serve many tracks
    // (the same headers for a whole server, a new range for each track of a cue sheet) and be freed at any time.
    // An object is used by one thread at a time. Each setter returns AU_OK, or AU_ERR_INVALID_ARGUMENT (also for a
    // NULL object) or AU_ERR_NO_MEMORY, and leaves the object unchanged on error. Since 4.0 (AuPlayConfig before).
    typedef struct AuPlayOptions AuPlayOptions;

    // A new object with every option at its default; NULL without memory
    FFAUDIO_API AuPlayOptions *au_play_options_create(void);
    // NULL is fine
    FFAUDIO_API void au_play_options_free(AuPlayOptions *options);

    // A range of the file to play as the track: start this far into the file (0 = from the start), play this long and
    // then end the track (0 = to the end). Both finite and >= 0. Exact to the sample at the file's rate: consecutive
    // ranges of one file (cue sheet tracks) join with nothing missing or doubled. The track's position, duration,
    // seeks and events then count from the range's start.
    FFAUDIO_API int au_play_options_set_range(AuPlayOptions *options, double start_seconds, double length_seconds);

    // This track's own filter graph (NULL or "" = none), after the conversion to the output format (float, the
    // device's sample rate and channel layout: AuStatus.sample_rate / .channels) and before the mixer. A new graph is
    // built for each track. Checked like au_validate_filters(). Filters: https://ffmpeg.org/ffmpeg-filters.html
    FFAUDIO_API int au_play_options_set_filters(AuPlayOptions *options, const char *filters);

    // The fade into this track (copied), instead of the AuCrossfade setting (on_play for au_play(), on_track_end or
    // on_skip for au_queue()). NULL = the AuCrossfade setting. A fade needs finite seconds and a known curve.
    FFAUDIO_API int au_play_options_set_fade_in(AuPlayOptions *options, const AuFade *fade);

    // ReplayGain values from your own library database (copied). NULL = the ReplayGain cache, then the file's tags.
    FFAUDIO_API int au_play_options_set_replaygain(AuPlayOptions *options, const AuReplayGain *replaygain);

    // Network streams only: extra HTTP request headers, one "Name: value" per line, e.g.
    // "Authorization: Bearer abc\nX-Client: myplayer", for servers that want a token in a header. NULL = none.
    FFAUDIO_API int au_play_options_set_http_headers(AuPlayOptions *options, const char *headers);

    //+-----------------------------------------------------+
    //| Lifetime                                            |
    //+-----------------------------------------------------+

    // Start the player. Only what is fixed for its lifetime is given here: app_name (shown by the system's sound
    // settings; may be NULL), on_event (NULL = events are queued for au_poll_event()) and user_data (passed to
    // on_event). Every setting has its own setter, usable right after au_init(), before anything plays: volume 1,
    // no looping, prepare-next 10 s, the system default device in the format it prefers until they are called.
    // No output device is opened yet: that happens when the first track is played or queued, with the settings made
    // until then, or earlier with au_open_device(); AU_EVENT_DEVICE reports it. Until au_shutdown(), FFmpeg's own
    // error messages come as AU_EVENT_LOG warnings instead of going to stderr (it sets av_log_set_callback(); an
    // application that wants FFmpeg's messages itself sets its own callback after au_init()).
    FFAUDIO_API int au_init(const char *app_name, AuEventFn on_event, void *user_data);

    // Open the output device now, with the device and format chosen so far, instead of when the first track plays
    // (which then starts without waiting for it). AU_EVENT_DEVICE reports the result. Nothing happens when a device is
    // open already. Since 4.0.
    FFAUDIO_API int au_open_device(void);

    // Stops playback and frees everything; au_init() may be called again afterwards. AU_ERR_WRONG_THREAD from on_event
    // (it would wait for the thread making the call), AU_ERR_NOT_INITIALIZED without a player.
    FFAUDIO_API int au_shutdown(void);

    // Choose the output device: a name from au_list_devices(), or NULL for the system default (followed when it
    // changes). Before a device is open this only records the choice; an open device is switched at once. Returns at
    // once; AU_EVENT_DEVICE reports the result of an open. A chosen device that can not be opened, or that disappears
    // later, falls back to the system default (fell_back). Choosing the device in use does nothing.
    FFAUDIO_API int au_set_device(const char *name);

    // Ask for an output format: channels 1..8 (e.g. 2 to always mix down to stereo) and/or a sample rate
    // 8000..384000 Hz; 0 = what the device prefers. SDL converts when the device differs. Before a device is open
    // this only records the choice; an open device is opened again in the new format and playback continues, and
    // AU_EVENT_DEVICE reports the result. Asking for the format already asked for does nothing. Returns
    // AU_ERR_INVALID_ARGUMENT for values out of range. Since 3.1.
    FFAUDIO_API int au_set_output_format(int channels, int sample_rate);

    // The names of the output devices, *count of them; NULL when there are none. Free with au_free_devices().
    FFAUDIO_API char **au_list_devices(int *count);

    // Free a list from au_list_devices() (NULL is fine). Use this rather than free(): on Windows the application and
    // ffaudio can use different C runtimes (e.g. MSVC with /MT), and free() on memory that ffaudio allocated then
    // corrupts the heap.
    FFAUDIO_API void au_free_devices(char **devices, int count);

    // Name of the output device in use (for the system default, the device it currently resolves to when SDL can
    // tell, otherwise "System default"). "" when no device is open. Returns the length, or AU_ERR_INVALID_ARGUMENT
    // if buf is too small.
    FFAUDIO_API int au_get_device_name(char *buf, size_t size);

    //+-----------------------------------------------------+
    //| Playback                                            |
    //+-----------------------------------------------------+
    // These return at once: the change is queued and done in order on the control thread; events report what follows.
    // Each returns AU_OK, or AU_ERR_NOT_INITIALIZED without a player, AU_ERR_INVALID_ARGUMENT for a value out of
    // range (nothing changes then) or AU_ERR_NO_MEMORY.

    // Play a file or URL, replacing the current track (a track queued with au_queue() stays queued). options may be
    // NULL (defaults); they are copied. Returns the track's handle (> 0) at once, or a negative AU_RESULT; a file
    // that can not be opened ends with AU_END_ERROR.
    FFAUDIO_API int32_t au_play(const char *path, const AuPlayOptions *options);

    // Queue a file to play after the current one (gapless, or crossfaded per AuCrossfade.on_track_end), replacing
    // a queued one. If nothing is playing it starts right away. Returns the handle (> 0) or a negative AU_RESULT.
    FFAUDIO_API int32_t au_queue(const char *path, const AuPlayOptions *options);

    // End the current track now and start the queued one (crossfaded per AuCrossfade.on_skip). Without a queued
    // track this stops playback.
    FFAUDIO_API int au_skip(void);

    // Remove the queued track
    FFAUDIO_API int au_clear_queue(void);

    // Stop everything (fades out per AuCrossfade.on_stop) and clear the queue
    FFAUDIO_API int au_stop(void);

    // Pause (fading out per AuCrossfade.on_pause) or resume
    FFAUDIO_API int au_pause(bool paused);

    // Seek the current track to this many seconds (>= 0) from its start (of its range, au_play_options_set_range()),
    // exact to the sample. A position past the end ends the track (or loops it).
    FFAUDIO_API int au_seek(double seconds);

    // Seek the current track to this fraction (0..1) of its duration; also before the duration is known
    FFAUDIO_API int au_seek_fraction(double fraction);

    // Set when and how fades and crossfades happen. NULL disables them all (gapless + hard cuts).
    // AU_ERR_INVALID_ARGUMENT, and nothing changes, when a fade's seconds are not finite or its curve is unknown.
    FFAUDIO_API int au_set_crossfade(const AuCrossfade *config);

    // Playback speed for all tracks, 0.25..4 (1 = normal). keep_pitch: change the tempo only (atempo); false:
    // tape-style, the pitch follows the speed. Takes effect within about half a second (the audio already decoded
    // plays out first). Positions, durations and seeking stay in file time; crossfade lengths and
    // prepare_next_seconds stay in wall clock time.
    FFAUDIO_API int au_set_speed(double speed, bool keep_pitch);

    // Linear gain 0..1 for everything played (AU_ERR_INVALID_ARGUMENT for anything else)
    FFAUDIO_API int au_set_volume(float gain);

    FFAUDIO_API int au_set_muted(bool muted);

    // Applies to the current, the queued and future tracks. -1 = infinite, 0 = none (AU_ERR_INVALID_ARGUMENT below -1).
    FFAUDIO_API int au_set_loop_count(int count);

    // How long before the end of the current track AU_EVENT_PREPARE_NEXT comes, in seconds as heard (the crossfade
    // into the next track is added). Default 10. Also applies to the current track when its event has not come yet.
    // Returns AU_ERR_INVALID_ARGUMENT unless seconds is finite and > 0. Since 3.1.
    FFAUDIO_API int au_set_prepare_next(double seconds);

    //+-----------------------------------------------------+
    //| Network streams                                     |
    //+-----------------------------------------------------+
    // au_play(), au_queue() and au_probe() also take http:// and https:// URLs: files on a web server or a
    // music server (seekable when the server supports range requests), internet radio (Icecast/SHOUTcast) and HLS
    // (.m3u8). Local paths may only use local files, and URLs only network protocols (http, https, tls, tcp,
    // crypto, httpproxy): a playlist can not make FFmpeg read local files or other protocols. Opening a URL takes
    // a moment; stopping or skipping a track that is still connecting interrupts it.

    typedef struct AuNetworkSettings {
        const char *user_agent;       // NULL = "ffaudio/<version>"
        double timeout_seconds;       // connecting and every read; 0 = 10 s. A dead server ends the track with an error
        double buffer_seconds;        // live streams: audio buffered before starting and after a stall; 0 = 2 s
        double max_buffer_seconds;    // most audio read ahead from the network; 0 = 10 s. More rides out longer
                                      // stalls, but costs memory (on Linux glibc keeps it afterwards, per thread)
        int reconnect_attempts;       // after a dropped connection, with growing delays; 0 = 5, -1 = never
    } AuNetworkSettings;

    // Settings for every network stream (player, au_probe(), loudness analysis); NULL restores the defaults. Can be
    // called at any time, also before au_init(); tracks that are already open keep their settings.
    FFAUDIO_API int au_set_network(const AuNetworkSettings *config);

    // true for http(s):// and other URLs with a scheme other than file://
    FFAUDIO_API bool au_is_network_url(const char *path);

    //+-----------------------------------------------------+
    //| Filters                                             |
    //+-----------------------------------------------------+
    // Filter strings use FFmpeg's filtergraph syntax (https://ffmpeg.org/ffmpeg-filters.html) and run
    // at the output format: float samples at the device rate and channel layout. ffaudio_filters.h has
    // helpers that build common strings (equalizer, crossfeed, bs2b, gain).
    // Changes take effect within about half a second (the audio already decoded plays out first).

    // Filters applied to every track, after the track's own (au_play_options_set_filters()). NULL or "" removes them.
    // Returns AU_ERR_INVALID_ARGUMENT (and changes nothing) if the string does not parse.
    FFAUDIO_API int au_set_filters(const char *filters);

    // Replace the filters of one track (handle from au_play()/au_queue(), 0 = the current track).
    FFAUDIO_API int au_set_track_filters(int32_t handle, const char *filters);

    // Filters applied to every track's decoded audio before the conversion to the output format: at the file's own
    // sample rate, channel layout and sample format. For repairs that need the samples as they are stored, above
    // all au_fb_add_declip() (resampling smooths away the flat clipped tops it looks for). They should keep the
    // channel layout, and take no au_send_filter_command(). Applies to playing tracks right away, like the downmix.
    // NULL or "" removes them. Returns AU_ERR_INVALID_ARGUMENT (and changes nothing) if the string does not parse.
    // Since 3.1.
    FFAUDIO_API int au_set_source_filters(const char *filters);

    // Send a runtime command to a filter, see avfilter_graph_send_command() and each filter's
    // "Commands" section, e.g. ("volume", "volume", "0.5") or ("equalizer@eq_3", "g", "-6").
    // `target` is a filter name or instance name ("all" for every filter). handle 0 = every live track.
    FFAUDIO_API int au_send_filter_command(int32_t handle, const char *target, const char *command, const char *arg);

    // Check a filter string against the current output format (or 48 kHz stereo before a device is open)
    FFAUDIO_API int au_validate_filters(const char *filters);

    //+-----------------------------------------------------+
    //| Downmixing                                          |
    //+-----------------------------------------------------+
    // How files with more channels than the output (e.g. 5.1 on stereo speakers or headphones) are mixed down.
    // Files with as many or fewer channels than the output are not affected.

    typedef enum AU_DOWNMIX {
        // FFmpeg's standard matrix (ITU-R BS.775: centre and surrounds at -3 dB, LFE dropped) at full level,
        // the same as mpv. Loud passages where several channels peak together can clip.
        AU_DOWNMIX_FULL_LEVEL = 0,
        // The same matrix scaled down so the result can never clip (about 7.7 dB quieter for 5.1 to stereo)
        AU_DOWNMIX_NORMALIZED,
        // Your own filters run on the original channels before the conversion to the output layout,
        // e.g. "sofalizer=sofa=/path/to/hrtf.sofa" for virtual surround on headphones (FFmpeg built with
        // libmysofa), "surround"/"pan" matrices, or "aformat=channel_layouts=stereo" with custom options.
        AU_DOWNMIX_CUSTOM,
    } AU_DOWNMIX;

    // custom_filters is only used with AU_DOWNMIX_CUSTOM. Applies to playing tracks right away.
    // Returns AU_ERR_INVALID_ARGUMENT if the custom filters do not accept a 7.1 input.
    FFAUDIO_API int au_set_downmix(AU_DOWNMIX mode, const char *custom_filters);

    // How files with fewer channels than a surround output (more than 2 channels, e.g. stereo on 5.1 speakers) are
    // spread out. Outputs with 1 or 2 channels are not affected. Since 3.1.
    typedef enum AU_UPMIX {
        // Each channel goes to its own speaker; the others stay silent (a stereo file plays on front left and right)
        AU_UPMIX_NONE = 0,
        // FFmpeg's surround filter: mono and stereo files are spread over every speaker by where each sound comes
        // from (centre dialogue and vocals to the centre, ambience to the surrounds, bass to the LFE). Other layouts
        // keep AU_UPMIX_NONE.
        AU_UPMIX_SURROUND,
        // Your own filters on the original channels before the conversion to the output layout, e.g.
        // "surround=chl_out=5.1:lfe=0" with options of your choice, or a "pan" matrix
        AU_UPMIX_CUSTOM,
    } AU_UPMIX;

    // custom_filters is only used with AU_UPMIX_CUSTOM. Applies to playing tracks right away. Returns
    // AU_ERR_INVALID_ARGUMENT if the custom filters do not accept a stereo input.
    FFAUDIO_API int au_set_upmix(AU_UPMIX mode, const char *custom_filters);

    //+-----------------------------------------------------+
    //| ReplayGain                                          |
    //+-----------------------------------------------------+

    // Apply ReplayGain/R128 tags automatically. NULL = off (the default). Applies to playing tracks
    // right away (with a short ramp) and to every later track.
    FFAUDIO_API int au_set_replaygain(const AuReplayGainSettings *config);

    // The ReplayGain gain ffaudio applies to a track (handle 0 = the current track), in dB. Never waits.
    // AU_ERR_NOTHING_TO_DO when the track is unknown, has ended, or is not open yet (AU_EVENT_TRACK_OPENED).
    FFAUDIO_API int au_track_gain_db(int32_t handle, double *out_db);

    //+-----------------------------------------------------+
    //| ReplayGain cache                                    |
    //+-----------------------------------------------------+
    // A persistent, per-user store of ReplayGain values by path, for values your application generated (see
    // ffaudio_loudness.h) or keeps in its own library. It is saved in the platform's user data directory and
    // shared by every program using ffaudio (au_gain_cache_set_file() picks another file):
    //   Linux/BSD: $XDG_DATA_HOME/ffaudio/replaygain.cache (default ~/.local/share/ffaudio/replaygain.cache)
    //   macOS:     ~/Library/Application Support/ffaudio/replaygain.cache
    //   Windows:   %LOCALAPPDATA%\ffaudio\replaygain.cache
    //   others:    SDL_GetPrefPath("ffaudio", "ffaudio") + replaygain.cache (Android, iOS, ...)
    // Each change is appended to the file right away (one small write), and the file is read on first use. Several
    // processes can share the file: writes take a lock (a ".lock" file next to it), so none are lost.
    // Works without au_init() and survives au_shutdown().
    // Media files are never touched.
    //
    // When a track starts, ffaudio looks up its path (exactly as passed to au_play()/au_queue()) and
    // uses a cached entry instead of the file's tags. Order: au_play_options_set_replaygain() > cache > file tags.
    // Setting or removing an entry also updates tracks that are already queued or playing (with a short ramp),
    // so values can arrive after the track was queued:
    //
    //   AU_EVENT_PREPARE_NEXT: path = next song; au_queue(path, NULL);
    //                    if (!au_gain_cache_get(path, NULL)) start a scan in a worker thread
    //   worker thread:   au_gain_cache_scan(path, NULL, NULL);   // measures and stores; applied right away
    //
    // Paths are compared as plain strings (case-sensitive, no normalization); ffaudio never looks at or tracks
    // the files. All functions are thread-safe and may be called from callbacks.

    // Use another cache file (UTF-8 path). NULL = the default location above, "" = memory only (nothing is read
    // or written). Discards the entries in memory and loads the new file. Call it before the first use of the
    // cache if your application wants its own file.
    FFAUDIO_API int au_gain_cache_set_file(const char *path);

    // The cache file in use, "" when memory only. Returns its length, or AU_ERR_INVALID_ARGUMENT if buf is too small.
    FFAUDIO_API int au_gain_cache_get_file(char *buf, size_t size);

    // Read the file again, e.g. to pick up entries another process added since this one loaded it
    FFAUDIO_API int au_gain_cache_reload(void);

    // Store (or replace) the values for a path. Returns AU_ERR_INVALID_ARGUMENT for a NULL/empty path or rg, and
    // AU_ERR_NOT_SAVED when the entry is in memory but could not be written to the cache file (AU_ERR_FILE before 4.0).
    FFAUDIO_API int au_gain_cache_set(const char *path, const AuReplayGain *rg);

    // true and *out filled (out may be NULL) when the path is cached
    FFAUDIO_API bool au_gain_cache_get(const char *path, AuReplayGain *out);

    // AU_OK when an entry was removed, AU_ERR_NOTHING_TO_DO when there was none, AU_ERR_INVALID_ARGUMENT for a
    // NULL/empty path, AU_ERR_NOT_SAVED when it is gone from memory but the cache file could not be written.
    // (A bool before 4.0.)
    FFAUDIO_API int au_gain_cache_remove(const char *path);

    // Remove every entry, also from the cache file: AU_OK, or AU_ERR_NOT_SAVED when the file could not be written
    FFAUDIO_API int au_gain_cache_clear(void);

    FFAUDIO_API int au_gain_cache_count(void);

    // Visit every entry (a snapshot, so the callback may call au_gain_cache_* functions). Return false from the
    // callback to stop (e.g. to export the cache or drop entries for files that no longer exist).
    // Returns the number of entries visited.
    typedef bool (*AuGainCacheVisitorFn)(const char *path, const AuReplayGain *rg, void *user);
    FFAUDIO_API int au_gain_cache_foreach(AuGainCacheVisitorFn visitor, void *user);

    //+-----------------------------------------------------+
    //| Metadata                                            |
    //+-----------------------------------------------------+

    // A finished key/value pair: AuMediaInfo.tags is an array of them, and AuTag never gains fields
    typedef struct AuTag {
        const char *key;
        const char *value;
    } AuTag;

    // Everything about a file, in one immutable block: it never changes after it was made, and every pointer in it
    // stays valid until au_media_info_release(). Strings are UTF-8 and never NULL ("" when unknown); tags stored in
    // another encoding (WAV and AIFF chunks) are read as Windows-1252.
    typedef struct AuMediaInfo {
        const char *path;            // as given to au_probe() / au_play()
        const char *container;       // e.g. "flac", "mp3", "ogg", "mov,mp4,m4a,3gp,3g2,mj2"
        const char *codec;           // e.g. "flac", "mp3", "aac", "opus"
        const char *channel_layout;  // e.g. "stereo", "5.1(side)"
        double duration;             // seconds, -1 when unknown
        int64_t bit_rate;            // bits per second, 0 when unknown
        int sample_rate;
        int channels;
        int bits_per_sample;         // 0 for lossy codecs or unknown
        // A live stream (internet radio, live HLS): no duration and no seeking; it ends only when stopped or on
        // errors
        bool live;
        bool seekable;
        // From "3/12" style tags or separate TRACKTOTAL / DISCTOTAL tags; 0 when unknown
        int track_number, track_total, disc_number, disc_total;
        AuReplayGain replaygain;     // from the ReplayGain / R128 tags
        int tag_count;               // every tag, for display or export: container tags, then the stream's for keys
        const AuTag *tags;           // the container lacks
        // Embedded pictures (cover art): how many there are; au_media_picture() reads one. Since 3.1.
        int picture_count;
    } AuMediaInfo;

    // Read the tags and format of a file without playing it. Works without au_init() and from any thread; it blocks
    // while the file is opened. Release the result with au_media_info_release().
    FFAUDIO_API const AuMediaInfo *au_probe(const char *path, int *out_error);

    // The player's own information about a queued or playing track (handle 0 = the current track), including the
    // "now playing" changes of a stream (each makes a new block). Never waits, and copies nothing. NULL when the
    // track is unknown, has ended, or is not open yet (AU_EVENT_TRACK_OPENED). Release it with
    // au_media_info_release().
    FFAUDIO_API const AuMediaInfo *au_track_info(int32_t handle);

    // Done with it (NULL is fine)
    FFAUDIO_API void au_media_info_release(const AuMediaInfo *info);

    // A tag by name, case-insensitive; NULL when absent. Common names are the same in every format:
    // "title", "artist", "album", "album_artist", "date", "genre", "track", "disc", "composer", "comment".
    // Format-specific spellings of album_artist and date ("ALBUMARTIST", "year", ...) are also found.
    FFAUDIO_API const char *au_media_tag(const AuMediaInfo *info, const char *key);

    // Picture types, as ID3v2 APIC and FLAC PICTURE number them (0..20)
    typedef enum AU_PICTURE_TYPE {
        AU_PICTURE_ANY = -1,         // au_media_picture(): the front cover, or else the first picture
        AU_PICTURE_OTHER = 0,
        AU_PICTURE_FRONT_COVER = 3,
        AU_PICTURE_BACK_COVER = 4,
        AU_PICTURE_LEAFLET = 5,
        AU_PICTURE_MEDIA = 6,        // e.g. the label side of a CD
        AU_PICTURE_ARTIST = 8,
        AU_PICTURE_BAND_LOGO = 19,
    } AU_PICTURE_TYPE;

    // An embedded picture: one immutable block, valid until au_picture_release()
    typedef struct AuPicture {
        const uint8_t *data;         // the image file as stored (JPEG, PNG, ...): ready to save or decode
        size_t size;
        const char *mime;            // "image/jpeg", "image/png", ...; "application/octet-stream" when unknown
        int type;                    // AU_PICTURE_TYPE (0..20); MP4 covers count as the front cover
        int width, height;           // 0 when unknown
        const char *description;     // "" when there is none
    } AuPicture;

    // Read an embedded picture (FLAC, ID3v2 in MP3/AIFF/WAV, MP4/M4A, Ogg/Opus METADATA_BLOCK_PICTURE, APE): the one
    // of this type, or with AU_PICTURE_ANY the front cover or else the first. Pictures are only read when asked for,
    // so probing stays fast. Works without au_init() and from any thread; URLs work too. NULL with *out_error (may be
    // NULL) AU_ERR_NOTHING_TO_DO when there is no such picture, AU_ERR_FILE when the file can not be opened.
    FFAUDIO_API const AuPicture *au_media_picture(const char *path, int type, int *out_error);

    // Done with it (NULL is fine)
    FFAUDIO_API void au_picture_release(const AuPicture *picture);

    //+-----------------------------------------------------+
    //| Status                                              |
    //+-----------------------------------------------------+

    typedef struct AuStatus {
        int32_t handle;                // the audible track, 0 when nothing is playing
        double position;               // seconds into it (into its range), as heard; -1 when nothing is playing
        double duration;               // seconds, -1 when unknown or nothing is playing
        bool buffering;                // the audible track waits for network data (its position holds)
        // The settings as last set, also before the control thread has applied them
        bool paused, muted;
        float volume;
        double speed;
        bool keep_pitch;
        int loop_count;
        int sample_rate, channels;     // the output format every track is converted to; 0 before a device is open
    } AuStatus;

    // A snapshot of the player; never waits. Without a player: nothing playing, the defaults.
    FFAUDIO_API void au_get_status(AuStatus *out);

    // Convenience function to block the calling thread until au_shutdown() is called from another thread
    FFAUDIO_API void au_wait_loop(void);

    // "major.minor.patch"
    FFAUDIO_API const char *au_get_version(void);

    // What an AU_RESULT means, in English ("invalid argument", ...): static text, never NULL ("unknown result" for
    // other values). Since 4.0.
    FFAUDIO_API const char *au_result_string(int result);

#ifdef __cplusplus
} // extern "C"
#endif


#endif //FFAUDIO_H
