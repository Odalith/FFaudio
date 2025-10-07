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

#ifdef _WIN32
    #define DLL_EXPORT __declspec(dllexport)
#else
    #define DLL_EXPORT __attribute__((visibility("default")))
#endif

#include <stdbool.h>
#include <stdint.h>
#include "delagates.h"

#ifdef __cplusplus
extern "C" {
#endif

    typedef struct InitializeConfig {
        const char* app_name;
        int initial_volume;          // 0..100
        int initial_loop_count;              // -1 for infinite
        NotifyOfLog on_log;
        NotifyOfEndOfFile on_eof;
        NotifyOfRestart on_restart;
        NotifyOfDurationUpdate on_duration_update;
        NotifyOfPrepareNext on_prepare_next;
    } InitializeConfig;

    typedef struct AudioDeviceConfig {
        const char* audio_device;    // Device name/ID
        int audio_device_index;      // Index in enumerated list; See get_audio_devices()
    } AudioDeviceConfig;

    // Values can be positive (amplification) or negative (attenuation). Measured in Decibels
    typedef struct EqualizerConfig {
        double one_31Hz;
        double two_63Hz;
        double three_125Hz;
        double four_250Hz;
        double five_500Hz;
        double six_1000Hz;
        double seven_2000Hz;
        double eight_4000Hz;
        double nine_8000Hz;
        double ten_16000Hz;
    } EqualizerConfig;

    typedef struct PlayAudioConfig {
        double skip_seconds;            // Optional; Seek this many seconds before starting playback. <= 0 == plays from the start
        double play_time;               // Optional; How many seconds to play audio before quiting. <= 0 == plays to the end
        const char* loudnorm_settings;  // Optional; NULL to disable. Add loudness normalization filter. Ex: "I=-16:TP=-1.5:LRA=11:measured_I=-8.9:measured_LRA=5.2:measured_TP=1.1:measured_thresh=-19.1:offset=-0.8"
        const char* crossfeed_setting;  // Optional; NULL to disable. Add crossfeed filter. Ex: "0.5"
    } PlayAudioConfig;

    DLL_EXPORT void shutdown();

    // Call this before anything else. config can be NULL.
    DLL_EXPORT int initialize(const InitializeConfig* config);

    // To be called after initialize(); Pass NULL to use the default device
    DLL_EXPORT int configure_audio_device(const AudioDeviceConfig* custom_config);

    // Play a file. config can be NULL
    DLL_EXPORT void play_audio(const char *filename, const PlayAudioConfig* config);

    DLL_EXPORT void stop_audio();

    DLL_EXPORT void pause_audio(const bool value);

    DLL_EXPORT void seek_percent(const double percentPos);

    DLL_EXPORT void seek_time(const int64_t milliseconds);

    DLL_EXPORT void set_audio_volume(const int volume);

    DLL_EXPORT int get_audio_volume();

    DLL_EXPORT void mute_audio(const bool value);

    DLL_EXPORT void set_loop_count(const int loop_count);

    DLL_EXPORT int get_loop_count();

    //Returns time in seconds
    DLL_EXPORT double get_audio_play_time();

    //Returns time in seconds
    DLL_EXPORT double get_audio_duration();

    //Convince function to block the main thread
    DLL_EXPORT void wait_loop();

    //List the user's audio devices
    DLL_EXPORT int get_audio_devices(int *out_total, char ***out_devices);

    //Set or update equalizer settings; Persists through tracks
    DLL_EXPORT bool set_equalizer(const EqualizerConfig params);

#ifdef __cplusplus
} // extern "C"
#endif


#endif //FFAUDIO_H
