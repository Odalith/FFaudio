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
 * Loudness analysis (EBU R128 / ITU-R BS.1770-4, ReplayGain 2.0), independent of playback.
 *
 * Measures files and returns the values; it never modifies them. Store the results in your own library
 * database and pass them to playback with PlayAudioConfig.replaygain (see au_loudness_to_replaygain()).
 *
 * - No au_initialize() needed, and it does not affect playback: scan while music plays.
 * - A scanner is used by one thread at a time; run several scanners in parallel to scan faster.
 * - Album values: scan every track of the album with the same scanner, then call au_loudness_album().
 *
 *   AuLoudnessScanner *s = au_loudness_scanner_create(NULL);
 *   AuLoudness track[12], album;
 *   for (int i = 0; i < n; i++)
 *       au_loudness_scan_file(s, files[i], &track[i]);
 *   au_loudness_album(s, &album);
 *   AuReplayGainInfo rg = au_loudness_to_replaygain(&track[0], &album);  // store, then use for playback
 *   au_loudness_scanner_free(s);
 */

#ifndef FFAUDIO_LOUDNESS_H
#define FFAUDIO_LOUDNESS_H

#include <stdbool.h>
#include <stdint.h>
#include "ffaudio.h"

#ifdef __cplusplus
extern "C" {
#endif

    typedef struct AuLoudness {
        bool silent;                  // nothing above the -70 LUFS gate: loudness and gains are not meaningful
        double integrated_lufs;       // integrated (programme) loudness, gated, in LUFS
        double loudness_range_lu;     // loudness range (EBU Tech 3342), in LU
        double true_peak;             // linear, 4x oversampled (1.0 = 0 dBTP)
        double sample_peak;           // linear, largest absolute sample value (1.0 = full scale)
        double duration;              // seconds of audio measured
        double replaygain_gain_db;    // ReplayGain 2.0 gain: -18 LUFS - integrated (REPLAYGAIN_*_GAIN)
        double r128_gain_db;          // R128 gain: -23 LUFS - integrated
        int32_t r128_gain_q78;        // r128_gain_db * 256, rounded and clamped, as written in R128_*_GAIN tags
    } AuLoudness;

    // Called while a file is scanned; fraction is 0..1 (or < 0 when the duration is unknown).
    // Return false to cancel the scan of this file.
    typedef bool (*AuLoudnessProgress)(double fraction, void *user);

    typedef struct AuLoudnessConfig {
        bool skip_true_peak;          // only measure the sample peak (true peak costs ~40% of the scan time)
        AuLoudnessProgress progress;  // optional
        void *progress_user;
    } AuLoudnessConfig;

    typedef struct AuLoudnessScanner AuLoudnessScanner;

    // config may be NULL (defaults: true peak on, no progress callback)
    DLL_EXPORT AuLoudnessScanner *au_loudness_scanner_create(const AuLoudnessConfig *config);
    DLL_EXPORT void au_loudness_scanner_free(AuLoudnessScanner *scanner);

    // Forget the scanned tracks (start a new album)
    DLL_EXPORT void au_loudness_scanner_reset(AuLoudnessScanner *scanner);

    // Measure one file and add it to the scanner's album. A file that fails or is cancelled is not added.
    // Returns AU_OK, AU_ERR_FILE (can not be opened/decoded), AU_ERR_CANCELLED, AU_ERR_INVALID_ARGUMENT
    // or AU_ERR_NO_MEMORY. out may be NULL if only the album values are wanted.
    DLL_EXPORT int au_loudness_scan_file(AuLoudnessScanner *scanner, const char *filename, AuLoudness *out);

    // Album values over every file scanned since the scanner was created or reset. Album loudness is
    // measured over all tracks together (not an average of the track values); peaks are the maximum.
    // AU_ERR_NOTHING_TO_DO when no file has been scanned.
    DLL_EXPORT int au_loudness_album(const AuLoudnessScanner *scanner, AuLoudness *out);

    // Number of files in the current album
    DLL_EXPORT int au_loudness_track_count(const AuLoudnessScanner *scanner);

    // Measure a single file without keeping a scanner around (track values only)
    DLL_EXPORT int au_loudness_analyze(const char *filename, const AuLoudnessConfig *config, AuLoudness *out);

    // ReplayGain values for PlayAudioConfig.replaygain. album may be NULL. Peaks are the true peaks
    // (the sample peaks when true peak was skipped). Silent results give no gain.
    DLL_EXPORT AuReplayGainInfo au_loudness_to_replaygain(const AuLoudness *track, const AuLoudness *album);

#ifdef __cplusplus
} // extern "C"
#endif

#endif //FFAUDIO_LOUDNESS_H
