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

    DLL_EXPORT void shutdown();

    DLL_EXPORT int initialize(const char* app_name, const int initial_volume, const int loop_count, const NotifyOfError callback, const NotifyOfEndOfFile callback2, const NotifyOfRestart callback3);

    DLL_EXPORT int configure_audio_device(const char* audio_device, int audio_device_index, bool use_default);

    DLL_EXPORT void play_audio(const char *filename, const char * loudnorm_settings, const char * crossfeed_setting);

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

#ifdef __cplusplus
} // extern "C"
#endif


#endif //FFAUDIO_H
