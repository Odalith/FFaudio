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
 * database and pass them to playback with AuPlayConfig.replaygain (see au_loudness_to_replaygain()).
 *
 * - No au_init() needed, and it does not affect playback: scan while music plays.
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
        double true_peak;             // linear (1.0 = 0 dBTP): 4x interpolated below 96 kHz, 2x below 192 kHz
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
        bool skip_true_peak;          // only measure the sample peak (the true peak adds about 15% to a scan)
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

    // Measure a single file without keeping a scanner around (track values only). URLs work too (it downloads the
    // file); a live stream (no duration) returns AU_ERR_UNSUPPORTED, since it never ends.
    DLL_EXPORT int au_loudness_analyze(const char *filename, const AuLoudnessConfig *config, AuLoudness *out);

    // ReplayGain values for AuPlayConfig.replaygain. album may be NULL. Peaks are the true peaks
    // (the sample peaks when true peak was skipped). Silent results give no gain.
    DLL_EXPORT AuReplayGainInfo au_loudness_to_replaygain(const AuLoudness *track, const AuLoudness *album);

    // The ReplayGain cache entry for `path` (see au_gain_cache_set() in ffaudio.h): returned straight away when
    // cached, otherwise the file is measured (track values only; it blocks for the scan, typically well under a
    // second per song) and the result is stored, which also updates the track if it is already queued or playing.
    // out may be NULL. Returns AU_OK or an error from au_loudness_analyze(); nothing is stored on error.
    // Avoid calling it from player callbacks: they run on the player's control thread, which would be held up.
    DLL_EXPORT int au_gain_cache_scan(const char *path, const AuLoudnessConfig *config, AuReplayGainInfo *out);

    //+-----------------------------------------------------+
    //| Background measurement queue                        |
    //+-----------------------------------------------------+
    // ffaudio measures requested files on its own low-priority worker threads and stores the results in the
    // ReplayGain cache; queued and playing tracks pick them up. Nothing blocks the caller, and requests for
    // paths that are cached, queued or being measured are skipped, so it is fine to re-send a playlist window
    // whenever the position changes:
    //
    //   AU_EVENT_TRACK_STARTED: au_gain_cache_request(next_ten_paths, 10, AU_SCAN_PREFETCH);
    //
    // With AuReplayGainConfig.measure_missing the player also measures a starting track that has no value at
    // all, and holds its start for up to measure_wait_ms. Works without au_init(); paths are only read.

    typedef enum AU_SCAN_PRIORITY {
        AU_SCAN_PREFETCH = 0,     // in request order, after urgent work
        AU_SCAN_URGENT,           // before everything else, newest first (e.g. a track the user picked)
    } AU_SCAN_PRIORITY;

    // A requested file was measured and stored (result AU_OK, rg set), could not be measured (AU_ERR_FILE, ...,
    // rg NULL) or was cancelled while being measured (AU_ERR_CANCELLED). Runs on a worker thread: keep it short.
    typedef void (*NotifyOfGainCached)(const char *path, int result, const AuReplayGainInfo *rg, void *user);

    typedef struct AuGainScanConfig {
        int threads;                  // worker threads, 1..16; 0 = default (2, or 1 on single-core machines)
        bool skip_true_peak;          // faster; the sample peak is stored as the peak
        NotifyOfGainCached on_cached; // optional
        void *user;
    } AuGainScanConfig;

    // Optional; NULL restores the defaults. Can be called at any time.
    DLL_EXPORT int au_gain_scan_configure(const AuGainScanConfig *config);

    // Measure these files (track values). Returns the number of paths queued (skipped ones not counted).
    // Skipped: paths already cached, already queued, and files that could not be measured before (not audio,
    // unreadable) and have not changed since (same size and modification time). Those failures are remembered
    // until au_gain_scan_stop(), so a playlist can be requested again and again without re-reading bad files.
    DLL_EXPORT int au_gain_cache_request(const char *const *paths, int count, AU_SCAN_PRIORITY priority);

    // Measure these files as one album: each gets its track values and the album values. Skipped only when
    // every path already has album values in the cache. Returns the number of paths queued.
    DLL_EXPORT int au_gain_cache_request_album(const char *const *paths, int count, AU_SCAN_PRIORITY priority);

    // Drop every queued request and stop the measurement in progress (the player's own requests stay)
    DLL_EXPORT void au_gain_cache_cancel_requests(void);

    // Paths queued or being measured
    DLL_EXPORT int au_gain_cache_pending(void);

    // Cancel everything (including the player's requests) and stop the worker threads; they start again on
    // the next request. Call it before unloading the library, or let the process exit.
    DLL_EXPORT void au_gain_scan_stop(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif //FFAUDIO_LOUDNESS_H
