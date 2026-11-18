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

#if defined(FFAUDIO_STATIC)
    #define DLL_EXPORT
#elif defined(_WIN32)
    #if defined(FFAUDIO_BUILDING)
        #define DLL_EXPORT __declspec(dllexport)
    #else
        #define DLL_EXPORT __declspec(dllimport)
    #endif
#else
    #define DLL_EXPORT __attribute__((visibility("default")))
#endif

#include <stdbool.h>
#include <stdint.h>
#include "delegates.h"

#define FFAUDIO_VERSION_MAJOR 0
#define FFAUDIO_VERSION_MINOR 2
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
    } AU_RESULT;

    //+-----------------------------------------------------+
    //| Fades and crossfades                                |
    //+-----------------------------------------------------+

    typedef enum AU_FADE_CURVE {
        AU_FADE_LINEAR = 0,       // gain changes linearly; a crossfade dips ~3 dB in the middle
        AU_FADE_EQUAL_POWER,      // sin/cos; constant loudness for uncorrelated material (default choice)
        AU_FADE_S_CURVE,          // smoothstep; gentle start and end
        AU_FADE_LOGARITHMIC,      // fast rise, slow finish
        AU_FADE_EXPONENTIAL,      // slow rise, fast finish
        AU_FADE_CUSTOM,           // use AuFade.custom
    } AU_FADE_CURVE;

    // t goes 0 -> 1 over the fade; return the gain (0..1) of the track fading IN.
    // The track fading out uses curve(1 - t). Called on the audio thread: it must be fast and must not block.
    typedef float (*AuFadeCurveFn)(float t, void *custom_user);

    // How a fade happens. seconds <= 0 means no fade (cut, or gapless at a natural track end).
    typedef struct AuFade {
        double seconds;
        AU_FADE_CURVE curve;
        AuFadeCurveFn custom;
        void *custom_user;
    } AuFade;

    // When fades happen. Every member defaults to "no fade" when zeroed.
    typedef struct CrossfadeConfig {
        AuFade on_track_end;   // the current track nears its end and a track is queued with au_set_next()
        AuFade on_skip;        // au_skip_to_next()
        AuFade on_play;        // au_play_audio() while another track is playing
        AuFade on_stop;        // au_stop_audio(): fade out, then stop
        AuFade on_pause;       // au_pause_audio(true) fades out, au_pause_audio(false) fades back in
        // Tracks (current or queued) shorter than this many seconds are not crossfaded at their end;
        // they play gapless instead. 0 = 2 * on_track_end.seconds. An end crossfade is also never
        // longer than half of either track. Tracks of unknown length always play gapless.
        double min_track_seconds;
    } CrossfadeConfig;

    //+-----------------------------------------------------+
    //| ReplayGain                                          |
    //+-----------------------------------------------------+

    // Loudness values of a file. Gains are in dB relative to the ReplayGain 2.0 reference (-18 LUFS);
    // R128_TRACK_GAIN / R128_ALBUM_GAIN tags (the standard for Opus, RFC 7845, but read from any format; relative
    // to -23 LUFS) are used when ReplayGain tags are missing and converted to that reference (+5 dB).
    typedef struct AuReplayGainInfo {
        bool has_track_gain;
        double track_gain_db;
        bool has_track_peak;
        double track_peak;           // linear sample peak, 1.0 = full scale
        bool has_album_gain;
        double album_gain_db;
        bool has_album_peak;
        double album_peak;
        bool from_r128;              // the gains came from R128_TRACK_GAIN / R128_ALBUM_GAIN
    } AuReplayGainInfo;

    typedef enum AU_REPLAYGAIN_MODE {
        AU_REPLAYGAIN_OFF = 0,
        AU_REPLAYGAIN_TRACK,         // track gain; album gain if the track has none
        AU_REPLAYGAIN_ALBUM,         // album gain; track gain if the track has none
    } AU_REPLAYGAIN_MODE;

    typedef struct AuReplayGainConfig {
        AU_REPLAYGAIN_MODE mode;
        double preamp_db;            // added to tagged gains, -15..+15 dB
        double fallback_gain_db;     // used for files without ReplayGain/R128 tags, -30..+15 dB (e.g. -6)
        bool prevent_clipping;       // lower the gain so the tagged peak does not exceed full scale
    } AuReplayGainConfig;

    //+-----------------------------------------------------+
    //| Configuration structs                               |
    //+-----------------------------------------------------+

    typedef struct InitializeConfig {
        const char* app_name;
        int initial_volume;                  // 0..100, values < 0 mean 100
        int initial_loop_count;              // -1 for infinite, 0 for none
        // Seconds before the end of the current track at which on_prepare_next fires. 0 = 10 seconds.
        double prepare_next_seconds;
        // Output format overrides; 0 = use what the device prefers. SDL converts when the device differs.
        int output_channels;                 // 1..8, e.g. 2 to always mix down to stereo
        int output_sample_rate;              // 8000..384000 Hz
        void *user_data;                     // passed to every callback
        NotifyOfLog on_log;
        NotifyOfEndOfFile on_eof;
        NotifyOfRestart on_restart;
        NotifyOfDurationUpdate on_duration_update;
        NotifyOfPrepareNext on_prepare_next;
        NotifyOfTrackStarted on_track_started;
        NotifyOfAudioDeviceUpdate on_audio_device_update;
    } InitializeConfig;

    typedef struct AudioDeviceConfig {
        const char* audio_device;    // Device name, see au_get_audio_devices()
        int audio_device_index;      // Index in the enumerated list; see au_get_audio_devices()
    } AudioDeviceConfig;

    typedef struct PlayAudioConfig {
        // Optional; Seek this many seconds before starting playback. <= 0 == plays from the start
        double skip_seconds;
        // Optional; How many seconds to play audio before quiting. <= 0 == plays to the end
        double play_duration;
        // Optional; NULL to disable.
        // Your own audio filtergraph, inserted after the input has been converted to the output
        // format (float, device sample rate and channel layout) and before it reaches the mixer.
        // Channel-dependent filters should therefore use the device channel count, see au_get_output_spec().
        // A new filtergraph is created for each track. Filters: https://ffmpeg.org/ffmpeg-filters.html
        const char* av_filtergraph;
        // Optional; NULL = use the CrossfadeConfig. Overrides the fade into this track
        // (on_play for au_play_audio(), on_track_end/on_skip for au_set_next()).
        const AuFade *fade_in;
        // Optional; NULL = read ReplayGain/R128 values from the file's tags. Use this to supply values
        // from your own library database (e.g. scanned but not written to the file).
        const AuReplayGainInfo *replaygain;
    } PlayAudioConfig;

    //+-----------------------------------------------------+
    //| Lifetime                                            |
    //+-----------------------------------------------------+

    // Call this before anything else.
    DLL_EXPORT int au_initialize(const InitializeConfig* config);

    // Stops playback and frees everything. Must not be called from a callback.
    // au_initialize() may be called again afterwards.
    DLL_EXPORT void au_shutdown(void);

    // To be called after au_initialize(); Pass NULL to use (and follow) the system default device.
    // Blocks until the device is open. Tracks can be started before this; they wait for the device.
    DLL_EXPORT int au_configure_audio_device(const AudioDeviceConfig* custom_config);

    // List the user's audio devices. Free each string and the array with free().
    DLL_EXPORT int au_get_audio_devices(int *out_total, char ***out_devices);

    // The output format every track is converted to (float32 internally).
    DLL_EXPORT int au_get_output_spec(int *out_sample_rate, int *out_channels);

    //+-----------------------------------------------------+
    //| Playback                                            |
    //+-----------------------------------------------------+
    // Playback functions never block: they are queued and executed in order on the control thread.

    // Play a file, replacing the current track (a track queued with au_set_next() stays queued).
    // config can be NULL. Returns the track handle (> 0) or a negative AU_RESULT.
    DLL_EXPORT int32_t au_play_audio(const char *filename, const PlayAudioConfig* config);

    // Queue a file to play after the current one (gapless, or crossfaded per CrossfadeConfig.on_track_end).
    // Replaces a previously queued file. If nothing is playing it starts right away.
    // Returns the track handle (> 0) or a negative AU_RESULT.
    DLL_EXPORT int32_t au_set_next(const char *filename, const PlayAudioConfig* config);

    // End the current track now and start the queued one (crossfaded per CrossfadeConfig.on_skip).
    // Without a queued track this stops playback.
    DLL_EXPORT void au_skip_to_next(void);

    // Remove the queued track.
    DLL_EXPORT void au_clear_next(void);

    // Stop everything (fades out per CrossfadeConfig.on_stop) and clear the queue.
    DLL_EXPORT void au_stop_audio(void);

    DLL_EXPORT void au_pause_audio(const bool value);

    DLL_EXPORT bool au_is_paused(void);

    DLL_EXPORT void au_seek_percent(const double percentPos);

    DLL_EXPORT void au_seek_time(const int64_t milliseconds);

    // Set when and how fades and crossfades happen. NULL disables them all (gapless + hard cuts).
    DLL_EXPORT int au_set_crossfade(const CrossfadeConfig *config);

    //+-----------------------------------------------------+
    //| Filters                                             |
    //+-----------------------------------------------------+
    // Filter strings use FFmpeg's filtergraph syntax (https://ffmpeg.org/ffmpeg-filters.html) and run
    // at the output format: float samples at the device rate and channel layout. ffaudio_filters.h has
    // helpers that build common strings (equalizer, crossfeed, loudness normalization, ...).
    // Changes take effect within about half a second (the audio already decoded plays out first).

    // Filters applied to every track, after the track's own av_filtergraph. NULL or "" removes them.
    // Returns AU_ERR_INVALID_ARGUMENT (and changes nothing) if the string does not parse.
    DLL_EXPORT int au_set_global_filtergraph(const char *filters);

    // Replace the filters of one track (handle from au_play_audio()/au_set_next(), 0 = the current track).
    DLL_EXPORT int au_update_filtergraph(int32_t handle, const char *filters);

    // Send a runtime command to a filter, see avfilter_graph_send_command() and each filter's
    // "Commands" section, e.g. ("volume", "volume", "0.5") or ("anequalizer", "change", "0|f=1000|w=500|g=-6").
    // `target` is a filter name or instance name ("all" for every filter). handle 0 = every live track.
    DLL_EXPORT int au_send_filter_command(int32_t handle, const char *target, const char *command, const char *arg);

    // Check a filter string against the current output format (or 48 kHz stereo before a device is open)
    DLL_EXPORT int au_validate_filtergraph(const char *filters);

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
    DLL_EXPORT int au_set_downmix(AU_DOWNMIX mode, const char *custom_filters);

    //+-----------------------------------------------------+
    //| ReplayGain                                          |
    //+-----------------------------------------------------+

    // Apply ReplayGain/R128 tags automatically. NULL = off (the default). Applies to playing tracks
    // right away (with a short ramp) and to every later track.
    DLL_EXPORT int au_set_replaygain(const AuReplayGainConfig *config);

    // The ReplayGain gain ffaudio applies to a track (handle 0 = the current track), in dB.
    // AU_ERR_NOTHING_TO_DO when the track is unknown or not open yet.
    DLL_EXPORT int au_get_track_gain_db(int32_t handle, double *out_gain_db);

    //+-----------------------------------------------------+
    //| Metadata                                            |
    //+-----------------------------------------------------+

    typedef struct AuMediaInfo AuMediaInfo;

    // Read the tags and format of a file without playing it. Works without au_initialize() and from
    // any thread; it blocks while the file is opened. Free the result with au_media_info_free().
    DLL_EXPORT AuMediaInfo *au_probe(const char *filename, int *out_error);

    // Information about a playing or queued track (handle 0 = the current track); NULL when the track is
    // unknown or not open yet (see on_duration_update). Free the result with au_media_info_free().
    DLL_EXPORT AuMediaInfo *au_get_track_info(int32_t handle);

    DLL_EXPORT void au_media_info_free(AuMediaInfo *info);

    DLL_EXPORT const char *au_media_filename(const AuMediaInfo *info);
    DLL_EXPORT const char *au_media_container(const AuMediaInfo *info);      // e.g. "flac", "mp3", "ogg"
    DLL_EXPORT const char *au_media_codec(const AuMediaInfo *info);          // e.g. "flac", "mp3", "aac", "opus"
    DLL_EXPORT const char *au_media_channel_layout(const AuMediaInfo *info); // e.g. "stereo", "5.1(side)"
    DLL_EXPORT double au_media_duration(const AuMediaInfo *info);            // seconds, -1 when unknown
    DLL_EXPORT int64_t au_media_bit_rate(const AuMediaInfo *info);           // bits per second, 0 when unknown
    DLL_EXPORT int au_media_sample_rate(const AuMediaInfo *info);
    DLL_EXPORT int au_media_channels(const AuMediaInfo *info);
    DLL_EXPORT int au_media_bits_per_sample(const AuMediaInfo *info);        // 0 for lossy codecs / unknown

    // A tag by name, case-insensitive; NULL when absent. Common names are the same in every format:
    // "title", "artist", "album", "album_artist", "date", "genre", "track", "disc", "composer", "comment".
    // Format-specific spellings of album_artist and date ("ALBUMARTIST", "year", ...) are also found.
    DLL_EXPORT const char *au_media_tag(const AuMediaInfo *info, const char *key);

    // All tags, for display or export
    DLL_EXPORT int au_media_tag_count(const AuMediaInfo *info);
    DLL_EXPORT bool au_media_tag_at(const AuMediaInfo *info, int index, const char **key, const char **value);

    // Track / disc numbers from "3/12" style tags or separate TRACKTOTAL / DISCTOTAL tags; 0 when unknown
    DLL_EXPORT void au_media_track_number(const AuMediaInfo *info, int *number, int *total);
    DLL_EXPORT void au_media_disc_number(const AuMediaInfo *info, int *number, int *total);

    // ReplayGain / R128 values from the tags
    DLL_EXPORT AuReplayGainInfo au_media_replaygain(const AuMediaInfo *info);

    //+-----------------------------------------------------+
    //| Volume, looping                                     |
    //+-----------------------------------------------------+

    DLL_EXPORT void au_set_audio_volume(const int volume);

    DLL_EXPORT int au_get_audio_volume(void);

    DLL_EXPORT void au_mute_audio(const bool value);

    DLL_EXPORT bool au_is_muted(void);

    // Applies to the current, the queued and future tracks. -1 = infinite, 0 = none.
    DLL_EXPORT void au_set_loop_count(const int loop_count);

    DLL_EXPORT int au_get_loop_count(void);

    //+-----------------------------------------------------+
    //| Status                                              |
    //+-----------------------------------------------------+

    // Handle of the audible track, 0 when nothing is playing.
    DLL_EXPORT int32_t au_get_current_handle(void);

    // Returns time in seconds, -1 when nothing is playing
    DLL_EXPORT double au_get_audio_play_time(void);

    // Returns time in seconds, -1 when unknown or nothing is playing
    DLL_EXPORT double au_get_audio_duration(void);

    // Convenience function to block the calling thread until au_shutdown() is called from another thread
    DLL_EXPORT void au_wait_loop(void);

    // "major.minor.patch"
    DLL_EXPORT const char *au_get_version(void);

#ifdef __cplusplus
} // extern "C"
#endif


#endif //FFAUDIO_H
