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

    // Constant gain in dB (-60..+30). For loudness normalization use au_set_replaygain() instead: it reads the
    // file's ReplayGain/R128 tags and applies the gain in the mixer without a filter.
    DLL_EXPORT int au_fb_add_volume_db(AuFilterBuilder *fb, double gain_db);

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

#ifdef __cplusplus
} // extern "C"
#endif

#endif //FFAUDIO_FILTERS_H
