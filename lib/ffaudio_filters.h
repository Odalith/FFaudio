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
 * Helpers that build FFmpeg filter strings for PlayAudioConfig.av_filtergraph,
 * au_set_global_filtergraph() and au_update_filtergraph().
 *
 *   AuFilterBuilder *fb = au_fb_create();
 *   AuEqualizer eq = au_equalizer_defaults();
 *   eq.gain_db[0] = 3.0;                      // +3 dB at 31 Hz
 *   au_fb_add_equalizer(fb, channels, &eq);   // channels: see au_get_output_spec()
 *   au_fb_add_crossfeed(fb, NULL);            // NULL = default settings
 *   char *filters = au_fb_build(fb);          // "anequalizer=...,crossfeed=..."
 *   au_set_global_filtergraph(filters);
 *   au_free(filters);
 *   au_fb_free(fb);
 *
 * Every au_fb_add_* function returns AU_OK or a negative AU_RESULT and leaves the builder unchanged
 * on error; au_fb_last_error() explains what was wrong. Numbers are written independently of the
 * C locale, so a comma decimal separator can not break the strings.
 */

#ifndef FFAUDIO_FILTERS_H
#define FFAUDIO_FILTERS_H

#include <stdbool.h>
#include "ffaudio.h"

#ifdef __cplusplus
extern "C" {
#endif

    typedef struct AuFilterBuilder AuFilterBuilder;

    DLL_EXPORT AuFilterBuilder *au_fb_create(void);
    DLL_EXPORT void au_fb_free(AuFilterBuilder *fb);
    DLL_EXPORT void au_fb_clear(AuFilterBuilder *fb);

    // The filter string so far, or NULL when nothing was added. Free it with au_free().
    DLL_EXPORT char *au_fb_build(const AuFilterBuilder *fb);

    // The filter string so far ("" when empty), owned by the builder
    DLL_EXPORT const char *au_fb_peek(const AuFilterBuilder *fb);

    // Why the last au_fb_add_* call failed ("" when it did not)
    DLL_EXPORT const char *au_fb_last_error(const AuFilterBuilder *fb);

    // Free strings returned by ffaudio
    DLL_EXPORT void au_free(void *ptr);

    // Append any filter (or filter chain) as-is, e.g. "atempo=1.25"
    DLL_EXPORT int au_fb_add(AuFilterBuilder *fb, const char *filter);

    //+-----------------------------------------------------+
    //| Gain                                                |
    //+-----------------------------------------------------+

    // Constant gain in dB (-60..+30)
    DLL_EXPORT int au_fb_add_volume_db(AuFilterBuilder *fb, double gain_db);

    // ReplayGain: gain_db (+ preamp_db), limited so that a track with the given peak does not clip.
    // peak is the linear sample peak from the tags (1.0 = full scale), <= 0 if unknown.
    DLL_EXPORT int au_fb_add_replaygain(AuFilterBuilder *fb, double gain_db, double peak, double preamp_db, bool prevent_clipping);

    //+-----------------------------------------------------+
    //| Equalizer                                           |
    //+-----------------------------------------------------+

    #define AU_EQ_BANDS 10

    // 10-band parametric equalizer (FFmpeg anequalizer), applied to every channel.
    typedef struct AuEqualizer {
        double gain_db[AU_EQ_BANDS];       // -30..+30 dB; 0 = band not used
        double frequency_hz[AU_EQ_BANDS];  // centre frequencies, see au_equalizer_defaults()
        double width_hz[AU_EQ_BANDS];      // band widths in Hz
        // Gain applied before the equalizer to leave headroom for boosts, e.g. -max(gain_db). 0 = none.
        double preamp_db;
    } AuEqualizer;

    // Octave bands at 31.25, 62.5, 125, 250, 500, 1k, 2k, 4k, 8k and 16k Hz, all gains 0
    DLL_EXPORT AuEqualizer au_equalizer_defaults(void);

    // channels: the output channel count (au_get_output_spec()), filters run at the output layout.
    // Adds nothing when every gain is 0.
    DLL_EXPORT int au_fb_add_equalizer(AuFilterBuilder *fb, int channels, const AuEqualizer *eq);

    //+-----------------------------------------------------+
    //| Headphones                                          |
    //+-----------------------------------------------------+

    typedef struct AuCrossfeed {
        double strength;     // 0..1, default 0.2
        double range;        // soundstage wideness 0..1, default 0.5
        double slope;        // low shelf slope 0.01..1, default 0.5
        double level_in;     // input gain 0..1, default 0.9
        double level_out;    // output gain 0..1, default 1.0
    } AuCrossfeed;

    DLL_EXPORT AuCrossfeed au_crossfeed_defaults(void);

    // FFmpeg crossfeed (stereo output only). config NULL = defaults.
    DLL_EXPORT int au_fb_add_crossfeed(AuFilterBuilder *fb, const AuCrossfeed *config);

    typedef enum AU_BS2B_PROFILE {
        AU_BS2B_DEFAULT = 0,   // fcut 700 Hz, feed 4.5 dB
        AU_BS2B_CHU_MOY,       // fcut 700 Hz, feed 6.0 dB
        AU_BS2B_JAN_MEIER,     // fcut 650 Hz, feed 9.5 dB
    } AU_BS2B_PROFILE;

    // Bauer stereo-to-binaural (stereo output only)
    DLL_EXPORT int au_fb_add_bs2b_profile(AuFilterBuilder *fb, AU_BS2B_PROFILE profile);

    // fcut_hz: 300..2000, feed_tenth_db: 10..150 (1.0..15.0 dB)
    DLL_EXPORT int au_fb_add_bs2b(AuFilterBuilder *fb, int fcut_hz, int feed_tenth_db);

    //+-----------------------------------------------------+
    //| Loudness                                            |
    //+-----------------------------------------------------+

    // EBU R128 values. The measured_* values come from a first loudnorm pass over the file
    // (ffmpeg -i file -af loudnorm=print_format=json -f null -).
    typedef struct AuLoudnorm {
        double target_i;          // integrated loudness target, -70..-5 LUFS, default -24
        double target_lra;        // loudness range target, 1..50 LU, default 7
        double target_tp;         // max true peak, -9..0 dBTP, default -2
        double offset;            // extra gain, -99..99 dB, default 0
        double measured_i;        // -99..0 (exclusive) LUFS
        double measured_lra;      // 0 (exclusive)..50 LU
        double measured_tp;       // -99..99 (exclusive) dBTP
        double measured_thresh;   // -99..0 LUFS
    } AuLoudnorm;

    // Targets set to the defaults above, measured values to 0
    DLL_EXPORT AuLoudnorm au_loudnorm_defaults(void);

    // Linear normalization: a constant gain of target_i - measured_i + offset, which is exactly what
    // loudnorm's linear mode does, written as a cheap volume filter. Fails (AU_ERR_INVALID_ARGUMENT)
    // if the result would exceed target_tp or the measured LRA exceeds target_lra, like loudnorm would.
    DLL_EXPORT int au_fb_add_loudnorm_linear(AuFilterBuilder *fb, const AuLoudnorm *config);

    // Dynamic (single pass) EBU R128 normalization with FFmpeg loudnorm, for files without measurements.
    // Note: loudnorm works at 192 kHz internally and adds latency; prefer the linear mode when possible.
    DLL_EXPORT int au_fb_add_loudnorm_dynamic(AuFilterBuilder *fb, double target_i, double target_lra, double target_tp);

#ifdef __cplusplus
} // extern "C"
#endif

#endif //FFAUDIO_FILTERS_H
