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

#include <locale.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ffaudio_filters.h"

struct AuFilterBuilder {
    char *buf;
    size_t len;
    size_t cap;
    char error[256];
};

static const double default_freqs[AU_EQ_BANDS] = { 31.25, 62.5, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };
// One octave wide around each centre frequency (width = f / sqrt(2))
static const double default_widths[AU_EQ_BANDS] = { 22.1, 44.2, 88.4, 176.8, 353.6, 707.1, 1414.2, 2828.4, 5656.9, 11313.7 };

//+-----------------------------------------------------+
//| Helpers                                             |
//+-----------------------------------------------------+

static int fail(AuFilterBuilder *fb, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(fb->error, sizeof(fb->error), fmt, args);
    va_end(args);
    return AU_ERR_INVALID_ARGUMENT;
}

// Locale independent "%.6g"
static const char *num(char out[32], const double v) {
    snprintf(out, 32, "%.6g", v);
    const struct lconv *lc = localeconv();
    const char dp = (lc && lc->decimal_point && lc->decimal_point[0]) ? lc->decimal_point[0] : '.';
    if (dp != '.')
        for (char *p = out; *p; p++)
            if (*p == dp) *p = '.';
    return out;
}

static bool in_range(const double v, const double lo, const double hi) {
    return isfinite(v) && v >= lo && v <= hi;
}

// Append a filter to the chain (with a ',' separator)
static int append(AuFilterBuilder *fb, const char *filter) {
    const size_t add = strlen(filter);
    const size_t need = fb->len + add + 2;
    if (need > fb->cap) {
        size_t cap = fb->cap ? fb->cap : 128;
        while (cap < need) cap *= 2;
        char *b = realloc(fb->buf, cap);
        if (!b) return AU_ERR_NO_MEMORY;
        fb->buf = b;
        fb->cap = cap;
    }
    if (fb->len) fb->buf[fb->len++] = ',';
    memcpy(fb->buf + fb->len, filter, add + 1);
    fb->len += add;
    fb->error[0] = '\0';
    return AU_OK;
}

static int appendf(AuFilterBuilder *fb, const char *fmt, ...) {
    char tmp[1024];
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    if (n < 0 || n >= (int)sizeof(tmp)) return fail(fb, "filter string too long");
    return append(fb, tmp);
}

//+-----------------------------------------------------+
//| Builder                                             |
//+-----------------------------------------------------+

AuFilterBuilder *au_fb_create(void) {
    return calloc(1, sizeof(AuFilterBuilder));
}

void au_fb_free(AuFilterBuilder *fb) {
    if (!fb) return;
    free(fb->buf);
    free(fb);
}

void au_fb_clear(AuFilterBuilder *fb) {
    if (!fb) return;
    fb->len = 0;
    if (fb->buf) fb->buf[0] = '\0';
    fb->error[0] = '\0';
}

char *au_fb_build(const AuFilterBuilder *fb) {
    if (!fb || !fb->len) return NULL;
    char *s = malloc(fb->len + 1);
    if (s) memcpy(s, fb->buf, fb->len + 1);
    return s;
}

const char *au_fb_peek(const AuFilterBuilder *fb) {
    return (fb && fb->len) ? fb->buf : "";
}

const char *au_fb_last_error(const AuFilterBuilder *fb) {
    return fb ? fb->error : "no builder";
}

void au_free(void *ptr) {
    free(ptr);
}

int au_fb_add(AuFilterBuilder *fb, const char *filter) {
    if (!fb) return AU_ERR_INVALID_ARGUMENT;
    if (!filter || !filter[0]) return fail(fb, "empty filter");
    return append(fb, filter);
}

//+-----------------------------------------------------+
//| Gain                                                |
//+-----------------------------------------------------+

int au_fb_add_volume_db(AuFilterBuilder *fb, const double gain_db) {
    if (!fb) return AU_ERR_INVALID_ARGUMENT;
    if (!in_range(gain_db, -60.0, 30.0)) return fail(fb, "gain must be between -60 and +30 dB");
    char g[32];
    return appendf(fb, "volume=%sdB", num(g, gain_db));
}

//+-----------------------------------------------------+
//| Equalizer                                           |
//+-----------------------------------------------------+

AuEqualizer au_equalizer_defaults(void) {
    AuEqualizer eq;
    memset(&eq, 0, sizeof(eq));
    for (int i = 0; i < AU_EQ_BANDS; i++) {
        eq.frequency_hz[i] = default_freqs[i];
        eq.width_hz[i] = default_widths[i];
    }
    return eq;
}

int au_fb_add_equalizer(AuFilterBuilder *fb, const int channels, const AuEqualizer *eq) {
    if (!fb) return AU_ERR_INVALID_ARGUMENT;
    if (!eq) return fail(fb, "no equalizer settings");
    if (channels < 1 || channels > 8) return fail(fb, "channel count must be between 1 and 8");
    if (!in_range(eq->preamp_db, -30.0, 30.0)) return fail(fb, "preamp must be between -30 and +30 dB");

    bool any = false;
    for (int b = 0; b < AU_EQ_BANDS; b++) {
        if (!in_range(eq->gain_db[b], -30.0, 30.0)) return fail(fb, "band %d: gain must be between -30 and +30 dB", b);
        if (eq->gain_db[b] == 0) continue;
        if (!in_range(eq->frequency_hz[b], 1.0, 96000.0)) return fail(fb, "band %d: frequency must be between 1 and 96000 Hz", b);
        if (!in_range(eq->width_hz[b], 0.1, 96000.0)) return fail(fb, "band %d: width must be between 0.1 and 96000 Hz", b);
        any = true;
    }
    if (!any && eq->preamp_db == 0) return AU_OK;

    // Build into a separate string first so the builder is unchanged on error
    char *s = NULL;
    size_t len = 0, cap = 0;
    #define EQ_APPEND(str) do { \
        const size_t l_ = strlen(str); \
        if (len + l_ + 1 > cap) { \
            size_t c_ = cap ? cap * 2 : 512; while (c_ < len + l_ + 1) c_ *= 2; \
            char *n_ = realloc(s, c_); if (!n_) { free(s); return AU_ERR_NO_MEMORY; } s = n_; cap = c_; \
        } \
        memcpy(s + len, str, l_ + 1); len += l_; \
    } while (0)

    char part[128], f[32], w[32], g[32];
    if (eq->preamp_db != 0) {
        snprintf(part, sizeof(part), "volume=%sdB", num(g, eq->preamp_db));
        EQ_APPEND(part);
    }
    if (any) {
        if (len) EQ_APPEND(",");
        EQ_APPEND("anequalizer=");
        bool first = true;
        for (int c = 0; c < channels; c++) {
            for (int b = 0; b < AU_EQ_BANDS; b++) {
                if (eq->gain_db[b] == 0) continue;
                snprintf(part, sizeof(part), "%sc%d f=%s w=%s g=%s", first ? "" : "|", c,
                         num(f, eq->frequency_hz[b]), num(w, eq->width_hz[b]), num(g, eq->gain_db[b]));
                EQ_APPEND(part);
                first = false;
            }
        }
    }
    #undef EQ_APPEND

    const int ret = append(fb, s);
    free(s);
    return ret;
}

//+-----------------------------------------------------+
//| Headphones                                          |
//+-----------------------------------------------------+

AuCrossfeed au_crossfeed_defaults(void) {
    const AuCrossfeed c = { .strength = 0.2, .range = 0.5, .slope = 0.5, .level_in = 0.9, .level_out = 1.0 };
    return c;
}

int au_fb_add_crossfeed(AuFilterBuilder *fb, const AuCrossfeed *config) {
    if (!fb) return AU_ERR_INVALID_ARGUMENT;
    const AuCrossfeed c = config ? *config : au_crossfeed_defaults();
    if (!in_range(c.strength, 0.0, 1.0)) return fail(fb, "crossfeed strength must be between 0 and 1");
    if (!in_range(c.range, 0.0, 1.0)) return fail(fb, "crossfeed range must be between 0 and 1");
    if (!in_range(c.slope, 0.01, 1.0)) return fail(fb, "crossfeed slope must be between 0.01 and 1");
    if (!in_range(c.level_in, 0.0, 1.0)) return fail(fb, "crossfeed level_in must be between 0 and 1");
    if (!in_range(c.level_out, 0.0, 1.0)) return fail(fb, "crossfeed level_out must be between 0 and 1");
    char a[32], b[32], s[32], li[32], lo[32];
    return appendf(fb, "crossfeed=strength=%s:range=%s:slope=%s:level_in=%s:level_out=%s",
                   num(a, c.strength), num(b, c.range), num(s, c.slope), num(li, c.level_in), num(lo, c.level_out));
}

int au_fb_add_bs2b_profile(AuFilterBuilder *fb, const AU_BS2B_PROFILE profile) {
    if (!fb) return AU_ERR_INVALID_ARGUMENT;
    switch (profile) {
        case AU_BS2B_DEFAULT:   return append(fb, "bs2b=profile=default");
        case AU_BS2B_CHU_MOY:   return append(fb, "bs2b=profile=cmoy");
        case AU_BS2B_JAN_MEIER: return append(fb, "bs2b=profile=jmeier");
        default:                return fail(fb, "unknown bs2b profile %d", (int)profile);
    }
}

int au_fb_add_bs2b(AuFilterBuilder *fb, const int fcut_hz, const int feed_tenth_db) {
    if (!fb) return AU_ERR_INVALID_ARGUMENT;
    if (fcut_hz < 300 || fcut_hz > 2000) return fail(fb, "bs2b fcut must be between 300 and 2000 Hz");
    if (feed_tenth_db < 10 || feed_tenth_db > 150) return fail(fb, "bs2b feed must be between 10 and 150 (tenths of a dB)");
    return appendf(fb, "bs2b=fcut=%d:feed=%d", fcut_hz, feed_tenth_db);
}
