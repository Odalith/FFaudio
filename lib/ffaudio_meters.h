/*
* Copyright (c) 2025 Odalith
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

/*
 * Level meters and a spectrum of what is playing, for VU meters and visualizers. They show the music: the mix after
 * ReplayGain, fades and crossfades, before the master volume and mute (turning the volume down does not shrink
 * them), and they fall to silence when paused or when nothing plays. They follow what is heard, the device's
 * latency taken off.
 *
 *   // on a UI timer, 30 to 60 times a second
 *   AuLevels lv;
 *   au_get_levels(0.05, &lv);                    // the last 50 ms
 *   draw_meter(lv.peak[0], lv.rms[0]);          // linear, 1.0 = full scale; the app adds its own ballistics
 *
 *   AuSpectrum *sp = au_spectrum_create(2048);   // once
 *   float mags[2048 / 2 + 1], bands[32];
 *   int rate;
 *   if (au_spectrum_read(sp, -1, mags, &rate) == AU_OK)
 *       au_spectrum_bands(mags, 2048, rate, 30, 16000, bands, 32);
 *
 * The first call turns the meters on (until then, a few ms, they read silence); after that they cost a copy of the
 * mix in the audio callback. Nothing here waits for the audio thread, and any thread may call them. Since 3.2.
 */

#ifndef FFAUDIO_METERS_H
#define FFAUDIO_METERS_H

#include "ffaudio.h"

#ifdef __cplusplus
extern "C" {
#endif

    typedef struct AuLevels {
        int channels;                // of the output (AuStatus.channels), 0 while the meters start
        int sample_rate;
        double window_seconds;       // what was measured
        // Per channel, in SDL's channel order; linear, 1.0 = full scale. Before the volume, so above 1.0 is possible
        float peak[8];
        float rms[8];
    } AuLevels;

    // Peak and RMS of the last window_seconds heard (0 < window_seconds <= 0.5). AU_OK with zeros when nothing plays
    // or while paused; AU_ERR_NOT_INITIALIZED without a player, AU_ERR_INVALID_ARGUMENT otherwise.
    FFAUDIO_API int au_get_levels(double window_seconds, AuLevels *out);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // FFAUDIO_METERS_H
