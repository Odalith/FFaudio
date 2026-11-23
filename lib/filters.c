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

#include <ctype.h>
#include <locale.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavfilter/avfilter.h>
#include <libavutil/bprint.h>

#include "ffaudio_filters.h"
#include "numbers.h"

#define NAME_MAX_LEN 32

struct AuFilterBuilder {
    AVBPrint bp;                 // the chain so far
    char error[256];
    char name[NAME_MAX_LEN + 1]; // instance name of the add call in progress ("" = none)
};

static const double default_freqs[AU_EQ_BANDS] = { 31.25, 62.5, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };
// One octave wide around each centre frequency (width = f / sqrt(2))
static const double default_widths[AU_EQ_BANDS] = { 22.1, 44.2, 88.4, 176.8, 353.6, 707.1, 1414.2, 2828.4, 5656.9, 11313.7 };

#define TONE_BASS_HZ 100.0
#define TONE_TREBLE_HZ 10000.0
#define TONE_SLOPE_Q 0.5
#define MONO_WIDTH (1.0 / 64) // stereotools' smallest side level; below it the output is mono

//+-----------------------------------------------------+
//| Helpers                                             |
//+-----------------------------------------------------+

static int vfail(AuFilterBuilder *fb, const int code, const char *fmt, va_list args) {
    vsnprintf(fb->error, sizeof(fb->error), fmt, args);
    return code;
}

static int fail(AuFilterBuilder *fb, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const int r = vfail(fb, AU_ERR_INVALID_ARGUMENT, fmt, args);
    va_end(args);
    return r;
}

// The end of every au_fb_add_* call: the instance name is used once
static int finish(AuFilterBuilder *fb, const int ret) {
    fb->name[0] = '\0';
    return ret;
}

// AU_OK, or AU_ERR_UNSUPPORTED (with the error text) when a filter is not in this FFmpeg build
static int require(AuFilterBuilder *fb, const char *filter) {
    if (avfilter_get_by_name(filter)) return AU_OK;
    snprintf(fb->error, sizeof(fb->error), "the %s filter is not available in this FFmpeg build", filter);
    return AU_ERR_UNSUPPORTED;
}

// "%.6g" in the C locale, whatever the application set
static const char *num(char out[32], const double v) {
    return ff_format_number(out, 32, v, 6, 'g');
}

static bool in_range(const double v, const double lo, const double hi) {
    return isfinite(v) && v >= lo && v <= hi;
}

static double db_to_linear(const double db) {
    return pow(10.0, db / 20.0);
}

// "<filter>", "<filter>@<name>" or "<filter>@<name>_<index>" (index >= 0)
static const char *tag(const AuFilterBuilder *fb, char out[96], const char *filter, const int index) {
    if (!fb->name[0]) snprintf(out, 96, "%s", filter);
    else if (index < 0) snprintf(out, 96, "%s@%s", filter, fb->name);
    else snprintf(out, 96, "%s@%s_%d", filter, fb->name, index);
    return out;
}

static bool named(const AuFilterBuilder *fb) {
    return fb->name[0] != '\0';
}

// Append a filter to the chain (with a ',' separator); the chain is unchanged when memory runs out
static int append(AuFilterBuilder *fb, const char *filter) {
    const unsigned old = fb->bp.len;
    if (fb->bp.len) av_bprint_chars(&fb->bp, ',', 1);
    av_bprintf(&fb->bp, "%s", filter);
    if (!av_bprint_is_complete(&fb->bp)) {
        fb->bp.len = old;
        if (fb->bp.size) fb->bp.str[old] = '\0';
        return AU_ERR_NO_MEMORY;
    }
    fb->error[0] = '\0';
    return AU_OK;
}

// A string for a helper to build everything in before touching the chain
static void str_init(AVBPrint *s) {
    av_bprint_init(s, 0, AV_BPRINT_SIZE_UNLIMITED);
}

static int appendf(AuFilterBuilder *fb, const char *fmt, ...) {
    AVBPrint s;
    str_init(&s);
    va_list args;
    va_start(args, fmt);
    av_vbprintf(&s, fmt, args);
    va_end(args);
    const int ret = av_bprint_is_complete(&s) ? append(fb, s.str) : AU_ERR_NO_MEMORY;
    av_bprint_finalize(&s, NULL);
    return ret;
}

// Append what a helper built (nothing when empty) and free it
static int append_str(AuFilterBuilder *fb, AVBPrint *s) {
    int ret = AU_OK;
    if (!av_bprint_is_complete(s)) ret = AU_ERR_NO_MEMORY;
    else if (s->len) ret = append(fb, s->str);
    av_bprint_finalize(s, NULL);
    return ret;
}

//+-----------------------------------------------------+
//| Builder                                             |
//+-----------------------------------------------------+

AuFilterBuilder *au_fb_create(void) {
    AuFilterBuilder *fb = calloc(1, sizeof(AuFilterBuilder));
    if (fb) av_bprint_init(&fb->bp, 0, AV_BPRINT_SIZE_UNLIMITED);
    return fb;
}

void au_fb_free(AuFilterBuilder *fb) {
    if (!fb) return;
    av_bprint_finalize(&fb->bp, NULL);
    free(fb);
}

void au_fb_clear(AuFilterBuilder *fb) {
    if (!fb) return;
    av_bprint_clear(&fb->bp);
    fb->error[0] = '\0';
    fb->name[0] = '\0';
}

char *au_fb_build(const AuFilterBuilder *fb) {
    if (!fb || !fb->bp.len) return NULL;
    char *s = malloc(fb->bp.len + 1); // au_free() frees it, with ffaudio's C runtime
    if (s) memcpy(s, fb->bp.str, fb->bp.len + 1);
    return s;
}

const char *au_fb_peek(const AuFilterBuilder *fb) {
    return (fb && fb->bp.len) ? fb->bp.str : "";
}

const char *au_fb_last_error(const AuFilterBuilder *fb) {
    return fb ? fb->error : "no builder";
}

void au_free(void *ptr) {
    free(ptr);
}

int au_fb_add(AuFilterBuilder *fb, const char *filter) {
    if (!fb) return AU_ERR_INVALID_ARGUMENT;
    if (!filter || !filter[0]) return finish(fb, fail(fb, "empty filter"));
    return finish(fb, append(fb, filter));
}

bool au_filter_available(const char *name) {
    return name && name[0] && avfilter_get_by_name(name) != NULL;
}

// The start of every au_fb_add_*() call with a name: check it and use it for the filters of this call (finish()
// clears it). NULL or "" = no name.
static int begin(AuFilterBuilder *fb, const char *name) {
    fb->name[0] = '\0';
    if (!name || !name[0]) return AU_OK;
    const size_t n = strlen(name);
    if (n > NAME_MAX_LEN) return fail(fb, "filter names are at most %d characters", NAME_MAX_LEN);
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)name[i]) && name[i] != '_')
            return fail(fb, "filter names may only contain letters, digits and '_'");
    memcpy(fb->name, name, n + 1);
    return AU_OK;
}

//+-----------------------------------------------------+
//| Gain                                                |
//+-----------------------------------------------------+

int au_fb_add_volume_db(AuFilterBuilder *fb, const char *name, const double gain_db) {
    if (!fb) return AU_ERR_INVALID_ARGUMENT;
    const int named_ok = begin(fb, name);
    if (named_ok != AU_OK) return finish(fb, named_ok);
    if (!in_range(gain_db, -60.0, 30.0)) return finish(fb, fail(fb, "gain must be between -60 and +30 dB"));
    char g[32], t[96];
    return finish(fb, appendf(fb, "%s=%sdB", tag(fb, t, "volume", -1), num(g, gain_db)));
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

static int add_equalizer(AuFilterBuilder *fb, const AuEqualizer *eq) {
    if (!eq) return fail(fb, "no equalizer settings");
    if (!in_range(eq->preamp_db, -30.0, 30.0)) return fail(fb, "preamp must be between -30 and +30 dB");
    const int r = require(fb, "equalizer");
    if (r != AU_OK) return r;

    // With a name every band is added (0 dB included) so commands can change any of them
    const bool all = named(fb);
    for (int b = 0; b < AU_EQ_BANDS; b++) {
        if (!in_range(eq->gain_db[b], -30.0, 30.0)) return fail(fb, "band %d: gain must be between -30 and +30 dB", b);
        if (eq->gain_db[b] == 0 && !all) continue;
        if (!in_range(eq->frequency_hz[b], 1.0, 96000.0)) return fail(fb, "band %d: frequency must be between 1 and 96000 Hz", b);
        if (!in_range(eq->width_hz[b], 0.1, 96000.0)) return fail(fb, "band %d: width must be between 0.1 and 96000 Hz", b);
    }

    // Built into a separate string first, so the chain is unchanged on error. One peaking biquad per band, its width
    // in Hz, in double precision like the parametric EQ; it works on any channel count.
    AVBPrint s;
    str_init(&s);
    char t[96], f[32], w[32], g[32];
    if (eq->preamp_db != 0) av_bprintf(&s, "%s=%sdB", tag(fb, t, "volume", -1), num(g, eq->preamp_db));
    for (int b = 0; b < AU_EQ_BANDS; b++) {
        if (eq->gain_db[b] == 0 && !all) continue;
        av_bprintf(&s, "%s%s=f=%s:t=h:w=%s:g=%s:r=f64", s.len ? "," : "", tag(fb, t, "equalizer", b),
                   num(f, eq->frequency_hz[b]), num(w, eq->width_hz[b]), num(g, eq->gain_db[b]));
    }
    return append_str(fb, &s);
}

int au_fb_add_equalizer(AuFilterBuilder *fb, const char *name, const AuEqualizer *eq) {
    if (!fb) return AU_ERR_INVALID_ARGUMENT;
    const int named_ok = begin(fb, name);
    if (named_ok != AU_OK) return finish(fb, named_ok);
    return finish(fb, add_equalizer(fb, eq));
}

//+-----------------------------------------------------+
//| Parametric equalizer                                |
//+-----------------------------------------------------+

static const char *peq_filter(const AU_PEQ_TYPE type) {
    switch (type) {
        case AU_PEQ_PEAK:       return "equalizer";
        case AU_PEQ_LOW_SHELF:  return "lowshelf";
        case AU_PEQ_HIGH_SHELF: return "highshelf";
        case AU_PEQ_LOW_PASS:   return "lowpass";
        case AU_PEQ_HIGH_PASS:  return "highpass";
        default:                return NULL;
    }
}

static bool peq_has_gain(const AU_PEQ_TYPE type) {
    return type == AU_PEQ_PEAK || type == AU_PEQ_LOW_SHELF || type == AU_PEQ_HIGH_SHELF;
}

// Range check of one band; message in err
static bool peq_band_ok(const AuPeqBand *b, char *err, const size_t size) {
    if (!peq_filter(b->type)) { snprintf(err, size, "unknown filter type %d", (int)b->type); return false; }
    if (!in_range(b->frequency_hz, 1.0, 96000.0)) { snprintf(err, size, "frequency must be between 1 and 96000 Hz"); return false; }
    if (!in_range(b->q, 0.025, 40.0)) { snprintf(err, size, "Q must be between 0.025 and 40"); return false; }
    if (peq_has_gain(b->type) && !in_range(b->gain_db, -30.0, 30.0)) {
        snprintf(err, size, "gain must be between -30 and +30 dB");
        return false;
    }
    return true;
}

static int add_parametric_eq(AuFilterBuilder *fb, const AuParametricEq *eq) {
    if (!eq) return fail(fb, "no equalizer settings");
    if (eq->band_count < 0 || eq->band_count > AU_PEQ_MAX_BANDS)
        return fail(fb, "band_count must be between 0 and %d", AU_PEQ_MAX_BANDS);
    if (!in_range(eq->preamp_db, -30.0, 30.0)) return fail(fb, "preamp must be between -30 and +30 dB");
    char err[128];
    for (int i = 0; i < eq->band_count; i++) {
        if (!peq_band_ok(&eq->bands[i], err, sizeof(err))) return fail(fb, "band %d: %s", i, err);
        const int r = require(fb, peq_filter(eq->bands[i].type));
        if (r != AU_OK) return r;
    }

    const bool all = named(fb);
    AVBPrint s;
    str_init(&s);
    char t[96], f[32], q[32], g[32];
    if (eq->preamp_db != 0 || (all && eq->band_count))
        av_bprintf(&s, "%s=%sdB", tag(fb, t, "volume", -1), num(g, eq->preamp_db));
    for (int i = 0; i < eq->band_count; i++) {
        const AuPeqBand *b = &eq->bands[i];
        if (peq_has_gain(b->type) && b->gain_db == 0 && !all) continue; // a 0 dB peak or shelf does nothing
        av_bprintf(&s, "%s%s=f=%s:t=q:w=%s", s.len ? "," : "", tag(fb, t, peq_filter(b->type), i),
                num(f, b->frequency_hz), num(q, b->q));
        if (peq_has_gain(b->type)) av_bprintf(&s, ":g=%s", num(g, b->gain_db));
        av_bprintf(&s, ":r=f64"); // double precision: float biquads are noisy at low frequencies
    }
    return append_str(fb, &s);
}

int au_fb_add_parametric_eq(AuFilterBuilder *fb, const char *name, const AuParametricEq *eq) {
    if (!fb) return AU_ERR_INVALID_ARGUMENT;
    const int named_ok = begin(fb, name);
    if (named_ok != AU_OK) return finish(fb, named_ok);
    return finish(fb, add_parametric_eq(fb, eq));
}

// Next whitespace-separated token of a line: [*p, return value)
static const char *next_token(const char **p, const char *line_end) {
    while (*p < line_end && isspace((unsigned char)**p)) (*p)++;
    const char *start = *p;
    while (*p < line_end && !isspace((unsigned char)**p)) (*p)++;
    return start;
}

static bool token_is(const char *tok, const char *tok_end, const char *word) {
    const size_t n = strlen(word);
    if ((size_t)(tok_end - tok) != n) return false;
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)tok[i]) != tolower((unsigned char)word[i])) return false;
    return true;
}

static bool token_number(const char *tok, const char *tok_end, double *out) {
    const char *end;
    return tok < tok_end && ff_parse_number(tok, &end, out, false) && end == tok_end; // never ',': "1,000" is not 1
}

#define PARSE_FAIL(...) do { if (error && error_size) snprintf(error, error_size, __VA_ARGS__); return AU_ERR_INVALID_ARGUMENT; } while (0)

int au_parametric_eq_parse(const char *text, AuParametricEq *out, char *error, const size_t error_size) {
    if (error && error_size) error[0] = '\0';
    if (!text || !out) PARSE_FAIL("no text");
    AuParametricEq eq;
    memset(&eq, 0, sizeof(eq));
    if (!strncmp(text, "\xEF\xBB\xBF", 3)) text += 3; // UTF-8 byte order mark

    int line_no = 0;
    for (const char *line = text; *line;) {
        const char *line_end = line;
        while (*line_end && *line_end != '\n') line_end++;
        const char *next_line = *line_end ? line_end + 1 : line_end;
        line_no++;
        while (line_end > line && isspace((unsigned char)line_end[-1])) line_end--; // also drops '\r'
        const char *p = line;
        while (p < line_end && isspace((unsigned char)*p)) p++;

        if (p == line_end || *p == '#') { line = next_line; continue; }

        const char *colon = memchr(p, ':', (size_t)(line_end - p));
        if (!colon) PARSE_FAIL("line %d: expected \"Preamp:\" or \"Filter:\"", line_no);
        const char *kw_end = colon;
        while (kw_end > p && isspace((unsigned char)kw_end[-1])) kw_end--;

        // "Preamp: -6.4 dB"
        if (token_is(p, kw_end, "preamp")) {
            const char *q = colon + 1;
            const char *tok = next_token(&q, line_end);
            double v;
            if (!token_number(tok, q, &v)) PARSE_FAIL("line %d: Preamp needs a number", line_no);
            tok = next_token(&q, line_end);
            if (tok < q && !token_is(tok, q, "db")) PARSE_FAIL("line %d: Preamp is in dB", line_no);
            eq.preamp_db += v;
            if (!in_range(eq.preamp_db, -30.0, 30.0)) PARSE_FAIL("line %d: preamp must be between -30 and +30 dB", line_no);
            line = next_line;
            continue;
        }

        // "Filter:" or "Filter <n>:"
        const char *f = p;
        const char *word = next_token(&f, kw_end);
        if (!token_is(word, f, "filter"))
            PARSE_FAIL("line %d: \"%.*s\" is not supported (only Preamp and Filter lines)", line_no, (int)(kw_end - p), p);
        const char *rest = next_token(&f, kw_end);
        double index;
        if (rest < f && (!token_number(rest, f, &index) || next_token(&f, kw_end) < f))
            PARSE_FAIL("line %d: expected \"Filter <number>:\"", line_no);

        const char *q = colon + 1;
        const char *tok = next_token(&q, line_end);
        if (token_is(tok, q, "off")) { line = next_line; continue; }
        if (!token_is(tok, q, "on")) PARSE_FAIL("line %d: expected ON or OFF", line_no);

        tok = next_token(&q, line_end);
        AuPeqBand band = { .q = 0.7071 };
        if (token_is(tok, q, "pk") || token_is(tok, q, "peq") || token_is(tok, q, "peak")) band.type = AU_PEQ_PEAK;
        else if (token_is(tok, q, "lsc") || token_is(tok, q, "ls") || token_is(tok, q, "lsq")) band.type = AU_PEQ_LOW_SHELF;
        else if (token_is(tok, q, "hsc") || token_is(tok, q, "hs") || token_is(tok, q, "hsq")) band.type = AU_PEQ_HIGH_SHELF;
        else if (token_is(tok, q, "lp") || token_is(tok, q, "lpq")) band.type = AU_PEQ_LOW_PASS;
        else if (token_is(tok, q, "hp") || token_is(tok, q, "hpq")) band.type = AU_PEQ_HIGH_PASS;
        else PARSE_FAIL("line %d: filter type \"%.*s\" is not supported", line_no, (int)(q - tok), tok);

        bool have_fc = false, have_gain = false;
        for (;;) {
            tok = next_token(&q, line_end);
            if (tok == q) break;
            const char *key = tok, *key_end = q;
            if (token_is(key, key_end, "bw")) { // "BW Oct 1.0"
                tok = next_token(&q, line_end);
                if (!token_is(tok, q, "oct")) PARSE_FAIL("line %d: only \"BW Oct\" bandwidths are supported", line_no);
                tok = next_token(&q, line_end);
                double n;
                if (!token_number(tok, q, &n) || n <= 0 || n > 10) PARSE_FAIL("line %d: bad bandwidth", line_no);
                const double p2 = pow(2.0, n);
                band.q = sqrt(p2) / (p2 - 1);
                continue;
            }
            tok = next_token(&q, line_end);
            double v;
            if (!token_number(tok, q, &v))
                PARSE_FAIL("line %d: \"%.*s\" needs a number", line_no, (int)(key_end - key), key);
            const char *save = q;
            const char *unit = next_token(&q, line_end);
            if (token_is(key, key_end, "fc")) {
                band.frequency_hz = v;
                have_fc = true;
                if (unit < q && !token_is(unit, q, "hz")) q = save; // the unit is optional
            }
            else if (token_is(key, key_end, "gain")) {
                band.gain_db = v;
                have_gain = true;
                if (unit < q && !token_is(unit, q, "db")) q = save;
            }
            else if (token_is(key, key_end, "q")) {
                band.q = v;
                q = save;
            }
            else {
                PARSE_FAIL("line %d: unknown parameter \"%.*s\"", line_no, (int)(key_end - key), key);
            }
        }
        if (!have_fc) PARSE_FAIL("line %d: the filter has no Fc", line_no);
        if (peq_has_gain(band.type) && !have_gain) PARSE_FAIL("line %d: the filter has no Gain", line_no);
        char err[128];
        if (!peq_band_ok(&band, err, sizeof(err))) PARSE_FAIL("line %d: %s", line_no, err);
        if (eq.band_count == AU_PEQ_MAX_BANDS) PARSE_FAIL("line %d: more than %d filters", line_no, AU_PEQ_MAX_BANDS);
        eq.bands[eq.band_count++] = band;
        line = next_line;
    }
    *out = eq;
    return AU_OK;
}

//+-----------------------------------------------------+
//| Tone, stereo image, dynamics                        |
//+-----------------------------------------------------+

static int add_tone(AuFilterBuilder *fb, const double bass_db, const double treble_db) {
    if (!in_range(bass_db, -20.0, 20.0)) return fail(fb, "bass must be between -20 and +20 dB");
    if (!in_range(treble_db, -20.0, 20.0)) return fail(fb, "treble must be between -20 and +20 dB");
    const bool all = named(fb);
    int r;
    if ((bass_db != 0 || all) && (r = require(fb, "bass")) != AU_OK) return r;
    if ((treble_db != 0 || all) && (r = require(fb, "treble")) != AU_OK) return r;

    AVBPrint s;
    str_init(&s);
    char t[96], f[32], w[32], g[32];
    num(w, TONE_SLOPE_Q);
    if (bass_db != 0 || all)
        av_bprintf(&s, "%s=f=%s:t=q:w=%s:g=%s:r=f64", tag(fb, t, "bass", -1), num(f, TONE_BASS_HZ), w, num(g, bass_db));
    if (treble_db != 0 || all)
        av_bprintf(&s, "%s%s=f=%s:t=q:w=%s:g=%s:r=f64", s.len ? "," : "", tag(fb, t, "treble", -1),
                num(f, TONE_TREBLE_HZ), w, num(g, treble_db));
    return append_str(fb, &s);
}

int au_fb_add_tone(AuFilterBuilder *fb, const char *name, const double bass_db, const double treble_db) {
    if (!fb) return AU_ERR_INVALID_ARGUMENT;
    const int named_ok = begin(fb, name);
    if (named_ok != AU_OK) return finish(fb, named_ok);
    return finish(fb, add_tone(fb, bass_db, treble_db));
}

AuStereo au_stereo_defaults(void) {
    const AuStereo s = { .balance = 0, .width = 1, .swap = false };
    return s;
}

static int add_stereo(AuFilterBuilder *fb, const AuStereo *st) {
    if (!st) return fail(fb, "no stereo settings");
    if (!in_range(st->balance, -1.0, 1.0)) return fail(fb, "balance must be between -1 and 1");
    if (!in_range(st->width, 0.0, 4.0)) return fail(fb, "width must be between 0 (mono) and 4");
    const bool mono = st->width < MONO_WIDTH;
    if (!named(fb) && st->balance == 0 && st->width == 1 && !st->swap) return AU_OK;
    const int r = require(fb, "stereotools");
    if (r != AU_OK) return r;

    // One stereotools instance: the mode (mono / swap) and the side level (width) are applied before the
    // output balance. Mono wins over swap (both sides are the same anyway).
    char t[96], b[32], w[32];
    const char *mode = mono ? "lr>l+r" : st->swap ? "lr>rl" : "lr>lr";
    return appendf(fb, "%s=mode=%s:slev=%s:balance_out=%s:bmode_out=balance", tag(fb, t, "stereotools", -1), mode,
                   num(w, mono ? 1.0 : st->width), num(b, st->balance));
}

int au_fb_add_stereo(AuFilterBuilder *fb, const char *name, const AuStereo *stereo) {
    if (!fb) return AU_ERR_INVALID_ARGUMENT;
    const int named_ok = begin(fb, name);
    if (named_ok != AU_OK) return finish(fb, named_ok);
    return finish(fb, add_stereo(fb, stereo));
}

AuLimiter au_limiter_defaults(void) {
    const AuLimiter l = { .ceiling_db = -1.0, .attack_ms = 5.0, .release_ms = 50.0 };
    return l;
}

static int add_limiter(AuFilterBuilder *fb, const AuLimiter *config) {
    const AuLimiter l = config ? *config : au_limiter_defaults();
    if (!in_range(l.ceiling_db, -24.0, 0.0)) return fail(fb, "limiter ceiling must be between -24 and 0 dB");
    if (!in_range(l.attack_ms, 0.1, 80.0)) return fail(fb, "limiter attack must be between 0.1 and 80 ms");
    if (!in_range(l.release_ms, 1.0, 8000.0)) return fail(fb, "limiter release must be between 1 and 8000 ms");
    const int r = require(fb, "alimiter");
    if (r != AU_OK) return r;
    // level=0: alimiter's "auto level" would otherwise raise everything to the ceiling
    char t[96], c[32], a[32], rel[32];
    return appendf(fb, "%s=limit=%s:attack=%s:release=%s:level=0", tag(fb, t, "alimiter", -1),
                   num(c, fmax(0.0625, db_to_linear(l.ceiling_db))), num(a, l.attack_ms), num(rel, l.release_ms));
}

int au_fb_add_limiter(AuFilterBuilder *fb, const char *name, const AuLimiter *config) {
    if (!fb) return AU_ERR_INVALID_ARGUMENT;
    const int named_ok = begin(fb, name);
    if (named_ok != AU_OK) return finish(fb, named_ok);
    return finish(fb, add_limiter(fb, config));
}

AuCompressor au_compressor_defaults(void) {
    const AuCompressor c = { .threshold_db = -18, .ratio = 2, .attack_ms = 20, .release_ms = 250,
                             .knee_db = 6, .makeup_db = 3 };
    return c;
}

AuCompressor au_compressor_night(void) {
    const AuCompressor c = { .threshold_db = -32, .ratio = 4, .attack_ms = 10, .release_ms = 300,
                             .knee_db = 8, .makeup_db = 9 };
    return c;
}

static int add_compressor(AuFilterBuilder *fb, const AuCompressor *config) {
    const AuCompressor c = config ? *config : au_compressor_defaults();
    if (!in_range(c.threshold_db, -60.0, 0.0)) return fail(fb, "compressor threshold must be between -60 and 0 dB");
    if (!in_range(c.ratio, 1.0, 20.0)) return fail(fb, "compressor ratio must be between 1 and 20");
    if (!in_range(c.attack_ms, 0.01, 2000.0)) return fail(fb, "compressor attack must be between 0.01 and 2000 ms");
    if (!in_range(c.release_ms, 0.01, 9000.0)) return fail(fb, "compressor release must be between 0.01 and 9000 ms");
    if (!in_range(c.knee_db, 0.0, 18.0)) return fail(fb, "compressor knee must be between 0 and 18 dB");
    if (!in_range(c.makeup_db, 0.0, 36.0)) return fail(fb, "compressor makeup must be between 0 and 36 dB");
    const int r = require(fb, "acompressor");
    if (r != AU_OK) return r;
    char t[96], th[32], ra[32], at[32], re[32], kn[32], mk[32];
    return appendf(fb, "%s=threshold=%s:ratio=%s:attack=%s:release=%s:knee=%s:makeup=%s",
                   tag(fb, t, "acompressor", -1), num(th, db_to_linear(c.threshold_db)), num(ra, c.ratio),
                   num(at, c.attack_ms), num(re, c.release_ms), num(kn, fmin(8.0, db_to_linear(c.knee_db))),
                   num(mk, db_to_linear(c.makeup_db)));
}

int au_fb_add_compressor(AuFilterBuilder *fb, const char *name, const AuCompressor *config) {
    if (!fb) return AU_ERR_INVALID_ARGUMENT;
    const int named_ok = begin(fb, name);
    if (named_ok != AU_OK) return finish(fb, named_ok);
    return finish(fb, add_compressor(fb, config));
}

//+-----------------------------------------------------+
//| Headphones                                          |
//+-----------------------------------------------------+

AuCrossfeed au_crossfeed_defaults(void) {
    const AuCrossfeed c = { .strength = 0.2, .range = 0.5, .slope = 0.5, .level_in = 0.9, .level_out = 1.0 };
    return c;
}

static int add_crossfeed(AuFilterBuilder *fb, const AuCrossfeed *config) {
    const AuCrossfeed c = config ? *config : au_crossfeed_defaults();
    if (!in_range(c.strength, 0.0, 1.0)) return fail(fb, "crossfeed strength must be between 0 and 1");
    if (!in_range(c.range, 0.0, 1.0)) return fail(fb, "crossfeed range must be between 0 and 1");
    if (!in_range(c.slope, 0.01, 1.0)) return fail(fb, "crossfeed slope must be between 0.01 and 1");
    if (!in_range(c.level_in, 0.0, 1.0)) return fail(fb, "crossfeed level_in must be between 0 and 1");
    if (!in_range(c.level_out, 0.0, 1.0)) return fail(fb, "crossfeed level_out must be between 0 and 1");
    const int r = require(fb, "crossfeed");
    if (r != AU_OK) return r;
    char t[96], a[32], b[32], s[32], li[32], lo[32];
    return appendf(fb, "%s=strength=%s:range=%s:slope=%s:level_in=%s:level_out=%s", tag(fb, t, "crossfeed", -1),
                   num(a, c.strength), num(b, c.range), num(s, c.slope), num(li, c.level_in), num(lo, c.level_out));
}

int au_fb_add_crossfeed(AuFilterBuilder *fb, const char *name, const AuCrossfeed *config) {
    if (!fb) return AU_ERR_INVALID_ARGUMENT;
    const int named_ok = begin(fb, name);
    if (named_ok != AU_OK) return finish(fb, named_ok);
    return finish(fb, add_crossfeed(fb, config));
}

static int add_bs2b_profile(AuFilterBuilder *fb, const AU_BS2B_PROFILE profile) {
    const char *name;
    switch (profile) {
        case AU_BS2B_DEFAULT:   name = "default"; break;
        case AU_BS2B_CHU_MOY:   name = "cmoy"; break;
        case AU_BS2B_JAN_MEIER: name = "jmeier"; break;
        default:                return fail(fb, "unknown bs2b profile %d", (int)profile);
    }
    const int r = require(fb, "bs2b");
    if (r != AU_OK) return r;
    char t[96];
    return appendf(fb, "%s=profile=%s", tag(fb, t, "bs2b", -1), name);
}

int au_fb_add_bs2b_profile(AuFilterBuilder *fb, const char *name, const AU_BS2B_PROFILE profile) {
    if (!fb) return AU_ERR_INVALID_ARGUMENT;
    const int named_ok = begin(fb, name);
    if (named_ok != AU_OK) return finish(fb, named_ok);
    return finish(fb, add_bs2b_profile(fb, profile));
}

static int add_bs2b(AuFilterBuilder *fb, const int fcut_hz, const int feed_tenth_db) {
    if (fcut_hz < 300 || fcut_hz > 2000) return fail(fb, "bs2b fcut must be between 300 and 2000 Hz");
    if (feed_tenth_db < 10 || feed_tenth_db > 150) return fail(fb, "bs2b feed must be between 10 and 150 (tenths of a dB)");
    const int r = require(fb, "bs2b");
    if (r != AU_OK) return r;
    char t[96];
    return appendf(fb, "%s=fcut=%d:feed=%d", tag(fb, t, "bs2b", -1), fcut_hz, feed_tenth_db);
}

int au_fb_add_bs2b(AuFilterBuilder *fb, const char *name, const int fcut_hz, const int feed_tenth_db) {
    if (!fb) return AU_ERR_INVALID_ARGUMENT;
    const int named_ok = begin(fb, name);
    if (named_ok != AU_OK) return finish(fb, named_ok);
    return finish(fb, add_bs2b(fb, fcut_hz, feed_tenth_db));
}
