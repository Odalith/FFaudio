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

// Internal: what the playlist and cue sheet readers share (text files that list other files)

#ifndef FFAUDIO_TEXT_LIST_H
#define FFAUDIO_TEXT_LIST_H

#include <stdbool.h>
#include <stddef.h>

// An entry as a usable path: URLs and absolute paths as they are, file:// URIs as paths, relative entries against
// the list's folder (or URL). NULL = out of memory.
char *ff_list_resolve(const char *entry, const char *base);

// A list read from a URL may only point at URLs (as a stream may not reach local files either)
bool ff_list_allows(const char *base, const char *path);

// The text of a list as UTF-8 (a BOM taken off; text that is not UTF-8 read as Latin-1), to av_free(). NULL with
// *err AU_ERR_FILE when it is not text (NUL bytes near the start), AU_ERR_NO_MEMORY, AU_ERR_INVALID_ARGUMENT.
char *ff_list_text(const char *text, size_t length, int *err);

// Read a whole text file or URL (at most max_bytes) through FFmpeg's I/O, with the stream settings. NULL with *err
// AU_ERR_FILE or AU_ERR_NO_MEMORY; *length gets its size.
char *ff_list_read(const char *path_or_url, size_t max_bytes, size_t *length, int *err);

// Next line of [*p, end): trimmed, without its newline; false at the end
bool ff_text_next_line(const char **p, const char *end, const char **ls, const char **le);

bool ff_text_starts_nocase(const char *s, const char *e, const char *prefix);

char *ff_text_dup(const char *s, const char *e);

// Does this text look like a cue sheet (a FILE line, then a TRACK line)? In cue.c.
bool ff_text_is_cue(const char *text, const char *end);

#endif // FFAUDIO_TEXT_LIST_H
