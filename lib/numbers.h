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

#ifndef FFAUDIO_NUMBERS_H
#define FFAUDIO_NUMBERS_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Numbers in text, the same whatever locale the application set.
 *
 * FFmpeg reads option values with strtod(): with a comma-decimal LC_NUMERIC (de_DE, fr_FR, ..., which e.g. every
 * GTK application sets at startup) it rejects "volume=0.5", and printf() writes "0,5", which a filter string would
 * split at the comma. So ffaudio's own threads use the C locale for good (ff_thread_use_c_locale(), first thing in
 * each), and the calls that parse filter strings on the application's thread switch to it for their duration.
 */

// For the rest of the calling thread (one of ffaudio's own)
void ff_thread_use_c_locale(void);

// The C locale until ff_c_locale_end(), which restores the thread's previous locale. Cheap: no allocation (on Windows
// one small one, for the previous setting).
typedef struct FfLocale { void *saved; } FfLocale;
FfLocale ff_c_locale_begin(void);
void ff_c_locale_end(FfLocale saved);

// A decimal number: optional sign, digits with '.' (and ',' when comma_too, for tags written as "-6,20 dB"), an
// optional exponent. false when there is none; *end is where it ended. Never reads "1,000" as 1 unless comma_too.
bool ff_parse_number(const char *s, const char **end, double *out, bool comma_too);

// v as printf("%.<digits><conv>") would write it in the C locale; conv is 'f' or 'g'. Returns out.
const char *ff_format_number(char *out, size_t size, double v, int digits, char conv);

#endif //FFAUDIO_NUMBERS_H
