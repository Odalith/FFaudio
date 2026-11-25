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

#include "text_list.h"

#include <ctype.h>
#include <string.h>
#include "ffaudio.h"
#include "libavformat/avio.h"
#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/common.h"
#include "libavutil/mem.h"
#include "metadata.h"
#include "network.h"

//+-----------------------------------------------------+
//| Paths                                               |
//+-----------------------------------------------------+

static bool is_absolute_local(const char *p) {
    return p[0] == '/' || p[0] == '\\' ||
           (isalpha((unsigned char)p[0]) && p[1] == ':' && (p[2] == '/' || p[2] == '\\'));
}

static bool has_scheme(const char *p) {
    const char *s = p;
    if (!isalpha((unsigned char)*s)) return false;
    while (isalnum((unsigned char)*s) || *s == '+' || *s == '-' || *s == '.') s++;
    return s[0] == ':' && s[1] == '/' && s[2] == '/';
}

static int hex(const char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// file:///music/a%20b.flac -> /music/a b.flac (and file:///C:/x -> C:/x)
static char *file_uri_to_path(const char *uri) {
    const char *p = uri + 7;                          // after "file://"
    if (!av_strncasecmp(p, "localhost/", 10)) p += 9;
    if (p[0] == '/' && isalpha((unsigned char)p[1]) && p[2] == ':') p++;   // Windows drive
    char *out = av_malloc(strlen(p) + 1), *o = out;
    if (!out) return NULL;
    for (; *p; p++) {
        if (*p == '%' && hex(p[1]) >= 0 && hex(p[2]) >= 0) {
            *o++ = (char)(hex(p[1]) * 16 + hex(p[2]));
            p += 2;
        }
        else *o++ = *p;
    }
    *o = '\0';
    return out;
}

char *ff_list_resolve(const char *entry, const char *base) {
    if (!av_strncasecmp(entry, "file://", 7)) return file_uri_to_path(entry);
    if (has_scheme(entry) || !base || !base[0]) return av_strdup(entry);
    // From a URL, "/x" is on the same server; a drive or UNC path stays as written
    const bool url_base = has_scheme(base);
    if (is_absolute_local(entry) && !(url_base && entry[0] == '/')) return av_strdup(entry);
    if (url_base) {
        const char *host = strstr(base, "://") + 3;
        const char *path_start = strchr(host, '/');
        size_t end = strcspn(base, "?#");
        if (entry[0] == '/') {                             // only the scheme and host from the base
            const size_t n = path_start && (size_t)(path_start - base) < end ? (size_t)(path_start - base) : end;
            return av_asprintf("%.*s%s", (int)n, base, entry);
        }
        if (!path_start || (size_t)(path_start - base) >= end) return av_asprintf("%.*s/%s", (int)end, base, entry);
        size_t dir = end;
        while (dir > 0 && base[dir - 1] != '/') dir--;
        return av_asprintf("%.*s%s", (int)dir, base, entry);
    }
    size_t dir = strlen(base);
    while (dir > 0 && base[dir - 1] != '/' && base[dir - 1] != '\\') dir--;
    if (dir == 0) return av_strdup(entry);              // the playlist is in the current folder
    return av_asprintf("%.*s%s", (int)dir, base, entry);
}

bool ff_list_allows(const char *base, const char *path) {
    return !(base && au_is_network_url(base) && !au_is_network_url(path));
}

//+-----------------------------------------------------+
//| Text                                                |
//+-----------------------------------------------------+

char *ff_list_text(const char *text, const size_t length, int *err) {
    if (!text && length) { *err = AU_ERR_INVALID_ARGUMENT; return NULL; }
    // Lists are text: NUL bytes near the start mean something else (an audio file, say)
    if (text && memchr(text, '\0', FFMIN(length, 4096))) { *err = AU_ERR_FILE; return NULL; }
    // UTF-8 as it is (without a BOM); anything else is read as Latin-1
    char *raw = av_strndup(text ? text : "", length);
    if (!raw) { *err = AU_ERR_NO_MEMORY; return NULL; }
    const char *start = raw;
    if (strlen(start) >= 3 && !memcmp(start, "\xEF\xBB\xBF", 3)) start += 3;
    char *utf8 = text_to_utf8(start);
    av_free(raw);
    if (!utf8) *err = AU_ERR_NO_MEMORY;
    return utf8;
}

char *ff_list_read(const char *path_or_url, const size_t max_bytes, size_t *length, int *err) {
    // Through FFmpeg's I/O, like media: the same protocol rules, timeouts and user agent, and UTF-8 paths on Windows
    AVDictionary *opts = NULL;
    network_input_options(&opts, path_or_url, NULL);
    AVIOContext *pb = NULL;
    const int r = avio_open2(&pb, path_or_url, AVIO_FLAG_READ, NULL, &opts);
    av_dict_free(&opts);
    if (r < 0) { *err = AU_ERR_FILE; return NULL; }

    AVBPrint text;
    av_bprint_init(&text, 0, AV_BPRINT_SIZE_UNLIMITED);
    uint8_t buf[16384];
    int n;
    bool failed = false;
    while ((n = avio_read(pb, buf, sizeof(buf))) > 0) {
        av_bprint_append_data(&text, (const char *)buf, (unsigned)n);
        if (text.len > max_bytes) { failed = true; break; }   // not a list
    }
    if (n < 0 && n != AVERROR_EOF) failed = true;
    avio_closep(&pb);
    char *out = NULL;
    if (failed || !av_bprint_is_complete(&text)) {
        *err = failed ? AU_ERR_FILE : AU_ERR_NO_MEMORY;
        av_bprint_finalize(&text, NULL);
        return NULL;
    }
    *length = text.len;
    if (av_bprint_finalize(&text, &out) < 0) { *err = AU_ERR_NO_MEMORY; return NULL; }
    return out;
}

bool ff_text_next_line(const char **p, const char *end, const char **ls, const char **le) {
    if (*p >= end) return false;
    const char *s = *p, *e = s;
    while (e < end && *e != '\n') e++;
    *p = e < end ? e + 1 : e;
    while (s < e && isspace((unsigned char)*s)) s++;
    while (e > s && isspace((unsigned char)e[-1])) e--;
    *ls = s;
    *le = e;
    return true;
}

bool ff_text_starts_nocase(const char *s, const char *e, const char *prefix) {
    const size_t n = strlen(prefix);
    return (size_t)(e - s) >= n && !av_strncasecmp(s, prefix, n);
}

char *ff_text_dup(const char *s, const char *e) {
    return av_strndup(s, (size_t)(e - s));
}

