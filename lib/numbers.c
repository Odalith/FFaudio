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

#ifndef _WIN32
#define _GNU_SOURCE // uselocale(), newlocale() (POSIX 2008)
#endif
#include "numbers.h"

#include <ctype.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <xlocale.h>
#endif
#include "once.h"

#ifdef _WIN32

// The CRT's locale per thread; FFmpeg uses the same CRT (ucrt) in ffaudio's builds
void ff_thread_use_c_locale(void) {
    _configthreadlocale(_ENABLE_PER_THREAD_LOCALE);
    setlocale(LC_NUMERIC, "C");
}

typedef struct WinSaved { int mode; char numeric[64]; } WinSaved;

FfLocale ff_c_locale_begin(void) {
    WinSaved *s = malloc(sizeof(*s)); // rare calls (validating a filter string); NULL = nothing to restore
    if (s) {
        s->mode = _configthreadlocale(_ENABLE_PER_THREAD_LOCALE);
        const char *cur = setlocale(LC_NUMERIC, NULL);
        snprintf(s->numeric, sizeof(s->numeric), "%s", cur ? cur : "C");
        setlocale(LC_NUMERIC, "C");
    }
    return (FfLocale){ s };
}

void ff_c_locale_end(const FfLocale saved) {
    WinSaved *s = saved.saved;
    if (!s) return;
    setlocale(LC_NUMERIC, s->numeric);
    _configthreadlocale(s->mode);
    free(s);
}

#else

static locale_t c_locale = (locale_t)0;
static FfOnce c_locale_once = FF_ONCE_INIT;

static void make_c_locale(void) {
    c_locale = newlocale(LC_ALL_MASK, "C", (locale_t)0); // lives as long as the process
}

static locale_t get_c_locale(void) {
    ff_once(&c_locale_once, make_c_locale);
    return c_locale;
}

void ff_thread_use_c_locale(void) {
    const locale_t c = get_c_locale();
    if (c) uselocale(c);
}

FfLocale ff_c_locale_begin(void) {
    const locale_t c = get_c_locale();
    return (FfLocale){ c ? (void *)uselocale(c) : NULL };
}

void ff_c_locale_end(const FfLocale saved) {
    if (saved.saved) uselocale((locale_t)saved.saved);
}

#endif

bool ff_parse_number(const char *s, const char **end, double *out, const bool comma_too) {
    const char *p = s;
    double sign = 1;
    if (*p == '+' || *p == '-') { if (*p == '-') sign = -1; p++; }
    double v = 0;
    bool digits = false;
    while (isdigit((unsigned char)*p)) { v = v * 10 + (*p - '0'); p++; digits = true; }
    if (*p == '.' || (comma_too && *p == ',')) {
        double scale = 0.1;
        const char *d = p + 1;
        bool frac = false;
        while (isdigit((unsigned char)*d)) { v += (*d - '0') * scale; scale /= 10; d++; frac = true; }
        if (frac || digits) { p = d; digits = true; }
    }
    if (!digits) return false;
    if (*p == 'e' || *p == 'E') {
        const char *e = p + 1;
        int esign = 1, ev = 0;
        if (*e == '+' || *e == '-') { if (*e == '-') esign = -1; e++; }
        if (isdigit((unsigned char)*e)) {
            while (isdigit((unsigned char)*e)) { ev = ev * 10 + (*e - '0'); if (ev > 400) ev = 400; e++; }
            v *= pow(10.0, esign * ev);
            p = e;
        }
    }
    v *= sign;
    if (!isfinite(v)) return false;
    *out = v;
    if (end) *end = p;
    return true;
}

const char *ff_format_number(char *out, const size_t size, const double v, const int digits, const char conv) {
    const FfLocale saved = ff_c_locale_begin();
    snprintf(out, size, conv == 'f' ? "%.*f" : "%.*g", digits, v);
    ff_c_locale_end(saved);
    return out;
}
