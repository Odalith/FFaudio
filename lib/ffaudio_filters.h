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
 * Helpers that build FFmpeg filter strings for AuPlayConfig.filters, au_set_filters() and au_set_track_filters().
 *
 *   AuFilterBuilder *fb = au_fb_create();
 *   AuEqualizer eq = au_equalizer_defaults();
 *   eq.gain_db[0] = 3.0;                      // +3 dB at 31 Hz
 *   au_fb_add_equalizer(fb, "eq", &eq);       // named "eq": its bands can be changed while playing
 *   au_fb_add_crossfeed(fb, NULL, NULL);      // no name, default settings
 *   char *filters = au_fb_build(fb);          // "equalizer@eq_0=...,crossfeed=..."
 *   au_set_filters(filters);
 *   au_free(filters);
 *   au_fb_free(fb);
 *
 * Every au_fb_add_* function returns AU_OK or a negative AU_RESULT and leaves the builder unchanged on error;
 * au_fb_last_error() explains what was wrong. Numbers are written independently of the C locale, so a comma
 * decimal separator can not break the strings.
 */

#ifndef FFAUDIO_FILTERS_H
#define FFAUDIO_FILTERS_H

#include <stdbool.h>
#include "ffaudio.h"

#ifdef __cplusplus
extern "C" {
#endif

    typedef struct AuFilterBuilder AuFilterBuilder;

    FFAUDIO_API AuFilterBuilder *au_fb_create(void);
    FFAUDIO_API void au_fb_free(AuFilterBuilder *fb);
    FFAUDIO_API void au_fb_clear(AuFilterBuilder *fb);

    // The filter string so far, or NULL when nothing was added. Free it with au_free().
    FFAUDIO_API char *au_fb_build(const AuFilterBuilder *fb);

    // The filter string so far ("" when empty), owned by the builder
    FFAUDIO_API const char *au_fb_peek(const AuFilterBuilder *fb);

    // Why the last au_fb_add_* call failed ("" when it did not)
    FFAUDIO_API const char *au_fb_last_error(const AuFilterBuilder *fb);

    // au_free() (declared in ffaudio.h) frees the strings au_fb_build() returns

    // Append any filter (or filter chain) as-is, e.g. "atempo=1.25"
    FFAUDIO_API int au_fb_add(AuFilterBuilder *fb, const char *filter);

    // Whether this FFmpeg build has a filter. Most are always there; bs2b needs FFmpeg built with libbs2b
    // (sofalizer, rubberband and others also depend on build options). The au_fb_add_* helpers check this
    // themselves and return AU_ERR_UNSUPPORTED.
    FFAUDIO_API bool au_filter_available(const char *name);

    //+-----------------------------------------------------+
    //| Changing filters while playing                      |
    //+-----------------------------------------------------+
    // Every au_fb_add_* helper takes an instance name for the filters it adds (letters, digits and '_', up to 32
    // characters; NULL or "" = none), so au_send_filter_command() can change them while playing without rebuilding
    // the chain. The command target is "<filter>@<name>"; each helper below lists the filters it adds, e.g.
    //
    //   au_fb_add_stereo(fb, "st", &stereo);                             // stereotools@st
    //   ...
    //   au_send_filter_command(0, "stereotools@st", "balance_out", "-0.3");
    //
    // With a name, helpers also add the filters they would otherwise leave out because they change nothing (0 dB
    // bands, balance 0, ...), so they are there to be changed. au_fb_add() takes no name.
    //
    // A command changes the tracks that are playing (handle 0) right away; tracks that start later are built from
    // the filter string. To keep a change, also set an updated string (au_set_filters()).

    //+-----------------------------------------------------+
    //| Gain                                                |
    //+-----------------------------------------------------+

    // Constant gain in dB (-60..+30). For loudness normalization use au_set_replaygain() instead: it reads the
    // file's ReplayGain/R128 tags and applies the gain in the mixer without a filter.
    // Filter: volume@<name>, command "volume" (e.g. "-3dB").
    FFAUDIO_API int au_fb_add_volume_db(AuFilterBuilder *fb, const char *name, double gain_db);

    //+-----------------------------------------------------+
    //| Equalizer                                           |
    //+-----------------------------------------------------+

    #define AU_EQ_BANDS 10

    // 10-band equalizer: one peaking biquad (FFmpeg equalizer) per band, applied to every channel.
    typedef struct AuEqualizer {
        double gain_db[AU_EQ_BANDS];       // -30..+30 dB; 0 = band not used
        double frequency_hz[AU_EQ_BANDS];  // centre frequencies, see au_equalizer_defaults()
        double width_hz[AU_EQ_BANDS];      // band widths in Hz
        // Gain applied before the equalizer to leave headroom for boosts, e.g. -max(gain_db). 0 = none.
        double preamp_db;
    } AuEqualizer;

    // Octave bands at 31.25, 62.5, 125, 250, 500, 1k, 2k, 4k, 8k and 16k Hz, all gains 0
    FFAUDIO_API AuEqualizer au_equalizer_defaults(void);

    // Adds nothing when every gain is 0 (unless named). Filters: volume@<name> (preamp, when not 0) and one per band,
    // equalizer@<name>_<band index> (0..9, the band's place in AuEqualizer); each takes the commands f (Hz),
    // w (width in Hz) and g (dB), e.g. ("equalizer@eq_3", "g", "-2.5").
    FFAUDIO_API int au_fb_add_equalizer(AuFilterBuilder *fb, const char *name, const AuEqualizer *eq);

    //+-----------------------------------------------------+
    //| Parametric equalizer (headphone correction)         |
    //+-----------------------------------------------------+

    #define AU_PEQ_MAX_BANDS 32

    typedef enum AU_PEQ_TYPE {
        AU_PEQ_PEAK = 0,       // bell around frequency_hz
        AU_PEQ_LOW_SHELF,      // everything below frequency_hz
        AU_PEQ_HIGH_SHELF,     // everything above frequency_hz
        AU_PEQ_LOW_PASS,       // cut above frequency_hz (gain_db not used)
        AU_PEQ_HIGH_PASS,      // cut below frequency_hz (gain_db not used)
    } AU_PEQ_TYPE;

    typedef struct AuPeqBand {
        AU_PEQ_TYPE type;
        double frequency_hz;   // 1..96000, and below half the output sample rate
        double q;              // 0.025..40 (0.7071 = Butterworth for pass filters and shelves)
        double gain_db;        // -30..+30 for peaks and shelves
    } AuPeqBand;

    // The same biquad filters as EqualizerAPO and AutoEq (RBJ cookbook, Q for every type), computed in double
    // precision, applied to every channel.
    typedef struct AuParametricEq {
        double preamp_db;      // -30..+30; AutoEq profiles give one that leaves room for the boosts
        int band_count;        // 0..AU_PEQ_MAX_BANDS
        AuPeqBand bands[AU_PEQ_MAX_BANDS];
    } AuParametricEq;

    // Filters: volume@<name> (preamp, when not 0) and one per band, <filter>@<name>_<band index>, where the filter
    // is equalizer, lowshelf, highshelf, lowpass or highpass. Each takes the commands f, w (Q) and g (dB), e.g.
    // ("equalizer@hp_3", "g", "-2.5").
    FFAUDIO_API int au_fb_add_parametric_eq(AuFilterBuilder *fb, const char *name, const AuParametricEq *eq);

    // Read EqualizerAPO text, e.g. the "ParametricEQ.txt" files AutoEq publishes for most headphones:
    //
    //   Preamp: -6.4 dB
    //   Filter 1: ON LSC Fc 105 Hz Gain 5.6 dB Q 0.70
    //   Filter 2: ON PK Fc 1960 Hz Gain -3.1 dB Q 1.85
    //
    // Types: PK/PEQ (peak), LSC/LS (low shelf), HSC/HS (high shelf), LP/LPQ, HP/HPQ. Q may also be given as
    // "BW Oct <octaves>"; a missing Q is 0.7071. OFF filters, comments (#) and blank lines are skipped; several
    // Preamp lines add up. Anything else (per-channel sections, GraphicEQ, other filter types) is an error, so a
    // profile is never applied half right. On error, `error` (may be NULL) says which line and why.
    FFAUDIO_API int au_parametric_eq_parse(const char *text, AuParametricEq *out, char *error, size_t error_size);

    //+-----------------------------------------------------+
    //| Tone, stereo image, dynamics                        |
    //+-----------------------------------------------------+

    // Bass and treble controls: shelves at 100 Hz and 10 kHz, -20..+20 dB each. A 0 dB control is left out
    // (unless named). Filters: bass@<name> and treble@<name>, command "g" (dB).
    FFAUDIO_API int au_fb_add_tone(AuFilterBuilder *fb, const char *name, double bass_db, double treble_db);

    // Stereo output only (like crossfeed and bs2b)
    typedef struct AuStereo {
        // -1 (left only) .. +1 (right only), 0 = centre. Turns the other side down, never boosts.
        double balance;
        // 1 = unchanged, 0 = mono ((L+R)/2 on both sides, for one earbud or one-sided hearing), up to 4 = wider.
        // It scales the difference between the channels: above 1 can push peaks over 0 dBFS, add a limiter.
        double width;
        bool swap;             // swap left and right
    } AuStereo;

    // balance 0, width 1, no swap
    FFAUDIO_API AuStereo au_stereo_defaults(void);

    // Adds nothing for the defaults (unless named). Filter: stereotools@<name>; commands balance_out (-1..1),
    // slev (width, 0.015625..64) and mode ("lr>lr", "lr>l+r" = mono, "lr>rl" = swap).
    FFAUDIO_API int au_fb_add_stereo(AuFilterBuilder *fb, const char *name, const AuStereo *stereo);

    typedef struct AuLimiter {
        double ceiling_db;     // -24..0, default -1: peaks above this are turned down smoothly
        double attack_ms;      // 0.1..80, default 5 (also the delay the limiter adds)
        double release_ms;     // 1..8000, default 50
    } AuLimiter;

    FFAUDIO_API AuLimiter au_limiter_defaults(void);

    // A look-ahead peak limiter: a safety net after equalizer boosts, widening or compression. It does not raise
    // quiet audio. It runs before ffaudio's volume and ReplayGain stage, so with ReplayGain boosts also enable
    // AuReplayGainConfig.prevent_clipping. config NULL = defaults. Filter: alimiter@<name>; commands limit
    // (linear 0.0625..1), attack, release.
    FFAUDIO_API int au_fb_add_limiter(AuFilterBuilder *fb, const char *name, const AuLimiter *config);

    typedef struct AuCompressor {
        double threshold_db;   // -60..0: compression starts above this level
        double ratio;          // 1..20: 4 = 4 dB over the threshold become 1 dB
        double attack_ms;      // 0.01..2000
        double release_ms;     // 0.01..9000
        double knee_db;        // 0..18: width of the soft transition around the threshold
        double makeup_db;      // 0..36: gain after compression
    } AuCompressor;

    // Gentle levelling for mixed playlists: -18 dB, 2:1, 20 ms / 250 ms, 6 dB knee, +3 dB makeup
    FFAUDIO_API AuCompressor au_compressor_defaults(void);
    // Night mode, quiet passages up and loud ones down for low-volume listening: -32 dB, 4:1, 10 ms / 300 ms,
    // 8 dB knee, +9 dB makeup. Best followed by a limiter.
    FFAUDIO_API AuCompressor au_compressor_night(void);

    // Dynamic range compressor (all channels linked). config NULL = au_compressor_defaults(). Filter:
    // acompressor@<name>; commands threshold and makeup (linear), ratio, attack, release, knee (linear 1..8).
    FFAUDIO_API int au_fb_add_compressor(AuFilterBuilder *fb, const char *name, const AuCompressor *config);

    //+-----------------------------------------------------+
    //| Restoration                                         |
    //+-----------------------------------------------------+
    // For damaged sources: clicks and crackle (vinyl and shellac transfers) and clipped masters. Both work on short
    // windows with an autoregressive model of the signal and add about the window's length as delay. On music each
    // costs about 5 % of one core per track. adeclip's cost grows steeply with the share of clipped samples: at tens
    // of percent (far beyond any release) it falls behind real time. Since 3.1.

    typedef struct AuDeclick {
        double window_ms;            // 10..100, default 55
        double overlap_percent;      // 50..95, default 75
        // 1..100, default 2: how far from the model a sample has to be to count as a click; lower finds more
        double threshold;
        double burst;                // 0..10, default 2: clicks closer than this are repaired as one
    } AuDeclick;
    FFAUDIO_API AuDeclick au_declick_defaults(void);
    // Remove clicks (FFmpeg adeclick). config NULL = defaults. Filter: adeclick@<name> (no commands).
    FFAUDIO_API int au_fb_add_declick(AuFilterBuilder *fb, const char *name, const AuDeclick *config);

    typedef struct AuDeclip {
        double window_ms;            // 10..100, default 55
        double overlap_percent;      // 50..95, default 75
        // 1..100, default 10: how close to the peak level a sample has to be to count as clipped
        double threshold;
    } AuDeclip;
    FFAUDIO_API AuDeclip au_declip_defaults(void);
    // Rebuild clipped peaks (FFmpeg adeclip). Use it with au_set_source_filters(): it finds clipping by the flat
    // tops at the stored sample values, which resampling to the device rate smooths away (then it does nothing).
    // Its output can exceed full scale, so put a limiter after it (au_set_filters()). config NULL = defaults.
    // Filter: adeclip@<name>.
    FFAUDIO_API int au_fb_add_declip(AuFilterBuilder *fb, const char *name, const AuDeclip *config);

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

    FFAUDIO_API AuCrossfeed au_crossfeed_defaults(void);

    // FFmpeg crossfeed (stereo output only). config NULL = defaults. Filter: crossfeed@<name>; commands strength,
    // range, slope, level_in, level_out.
    FFAUDIO_API int au_fb_add_crossfeed(AuFilterBuilder *fb, const char *name, const AuCrossfeed *config);

    typedef enum AU_BS2B_PROFILE {
        AU_BS2B_DEFAULT = 0,   // fcut 700 Hz, feed 4.5 dB
        AU_BS2B_CHU_MOY,       // fcut 700 Hz, feed 6.0 dB
        AU_BS2B_JAN_MEIER,     // fcut 650 Hz, feed 9.5 dB
    } AU_BS2B_PROFILE;

    // Bauer stereo-to-binaural (stereo output only; needs FFmpeg built with libbs2b). Filter: bs2b@<name>
    FFAUDIO_API int au_fb_add_bs2b_profile(AuFilterBuilder *fb, const char *name, AU_BS2B_PROFILE profile);

    // fcut_hz: 300..2000, feed_tenth_db: 10..150 (1.0..15.0 dB)
    FFAUDIO_API int au_fb_add_bs2b(AuFilterBuilder *fb, const char *name, int fcut_hz, int feed_tenth_db);

#ifdef __cplusplus
} // extern "C"
#endif

#endif //FFAUDIO_FILTERS_H
