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
 * Playlist files (ffaudio_playlist.h): M3U / M3U8 (with or without #EXTINF) and PLS, read and turned into a list of
 * paths for au_play(). Nothing is opened or checked: entries are what the playlist says, resolved to usable paths.
 */

#include "ffaudio_playlist.h"

#include <ctype.h>
#include <string.h>
#include "libavformat/avio.h"
#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/common.h"
#include "libavutil/mem.h"
#include "metadata.h"
#include "network.h"
#include "numbers.h"

#define MAX_PLAYLIST_BYTES (16 * 1024 * 1024)
#define MAX_PLS_ENTRIES 100000

typedef struct Draft {
    struct { char *path, *title; double duration; } *items;
    int n, cap;
    bool oom;
} Draft;

static void draft_add(Draft *d, char *path, const char *title, const double duration) {
    if (d->oom || !path) { if (path) av_free(path); else d->oom = true; return; }
    if (d->n == d->cap) {
        const int cap = d->cap ? d->cap * 2 : 32;
        void *items = av_realloc_array(d->items, cap, sizeof(*d->items));
        if (!items) { av_free(path); d->oom = true; return; }
        d->items = items;
        d->cap = cap;
    }
    char *t = av_strdup(title ? title : "");
    if (!t) { av_free(path); d->oom = true; return; }
    d->items[d->n].path = path;
    d->items[d->n].title = t;
    d->items[d->n].duration = duration;
    d->n++;
}

static void draft_free(Draft *d) {
    for (int i = 0; i < d->n; i++) {
        av_free(d->items[i].path);
        av_free(d->items[i].title);
    }
    av_free(d->items);
}

// One allocation: the AuPlaylistFile, its entries, then every string
static const AuPlaylistFile *freeze(const Draft *d) {
    size_t chars = 0;
    for (int i = 0; i < d->n; i++) chars += strlen(d->items[i].path) + strlen(d->items[i].title) + 2;
    const size_t head = sizeof(AuPlaylistFile) + (size_t)d->n * sizeof(AuPlaylistEntry);
    AuPlaylistFile *pl = av_malloc(head + chars + 1);
    if (!pl) return NULL;
    AuPlaylistEntry *entries = (AuPlaylistEntry *)(pl + 1);
    char *arena = (char *)pl + head;
    for (int i = 0; i < d->n; i++) {
        const size_t lp = strlen(d->items[i].path) + 1, lt = strlen(d->items[i].title) + 1;
        memcpy(arena, d->items[i].path, lp);
        entries[i].path = arena;
        arena += lp;
        memcpy(arena, d->items[i].title, lt);
        entries[i].title = arena;
        arena += lt;
        entries[i].duration = d->items[i].duration;
    }
    pl->count = d->n;
    pl->entries = entries;
    return pl;
}

void au_playlist_file_release(const AuPlaylistFile *playlist) {
    av_free((void *)playlist);
}

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

// An entry as a usable path: URLs and absolute paths as they are, file:// URIs as paths, relative entries against
// the playlist's folder (or URL)
static char *resolve(const char *entry, const char *base) {
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

// Add an entry as written in the playlist. A playlist from a URL may only list URLs: local paths in it (absolute ones,
// file://) are left out, as a stream's playlist may not reach local files either.
static void add_entry(Draft *d, const char *entry, const char *base, const char *title, const double duration) {
    char *path = resolve(entry, base);
    if (!path) { d->oom = true; return; }
    if (base && au_is_network_url(base) && !au_is_network_url(path)) {
        av_free(path);
        return;
    }
    draft_add(d, path, title, duration);
}

//+-----------------------------------------------------+
//| Formats                                             |
//+-----------------------------------------------------+

// Next line of [*p, end): trimmed, without its newline; false at the end
static bool next_line(const char **p, const char *end, const char **ls, const char **le) {
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

static bool starts_with_nocase(const char *s, const char *e, const char *prefix) {
    const size_t n = strlen(prefix);
    return (size_t)(e - s) >= n && !av_strncasecmp(s, prefix, n);
}

static char *dup_range(const char *s, const char *e) {
    return av_strndup(s, (size_t)(e - s));
}

// #EXTINF:<seconds>[ attributes],<title>
static void parse_extinf(const char *s, const char *e, double *duration, char **title) {
    s += 8;
    double d;
    const char *after;
    char *num = dup_range(s, e);
    *duration = num && ff_parse_number(num, &after, &d, false) && d >= 0 ? d : -1;
    av_free(num);
    // The title follows the first comma outside quotes (attributes such as tvg-name="a, b" may have commas)
    bool quoted = false;
    for (const char *c = s; c < e; c++) {
        if (*c == '"') quoted = !quoted;
        else if (*c == ',' && !quoted) {
            c++;
            while (c < e && isspace((unsigned char)*c)) c++;
            av_free(*title);
            *title = dup_range(c, e);
            return;
        }
    }
}

static bool parse_m3u(const char *text, const char *end, const char *base, Draft *d, bool *hls) {
    const char *p = text, *s, *e;
    char *title = NULL;
    double duration = -1;
    while (next_line(&p, end, &s, &e)) {
        if (s == e) continue;
        if (*s == '#') {
            if (starts_with_nocase(s, e, "#EXTINF:")) parse_extinf(s, e, &duration, &title);
            else if (starts_with_nocase(s, e, "#EXT-X-")) *hls = true;
            continue;
        }
        char *entry = dup_range(s, e);
        if (!entry) { d->oom = true; break; }
        add_entry(d, entry, base, title, duration);
        av_free(entry);
        av_freep(&title);
        duration = -1;
    }
    av_free(title);
    return !d->oom;
}

typedef struct PlsItem { char *file, *title; double length; } PlsItem;

static bool parse_pls(const char *text, const char *end, const char *base, Draft *d) {
    PlsItem *items = NULL;
    int n = 0;
    const char *p = text, *s, *e;
    while (next_line(&p, end, &s, &e)) {
        const char *eq = memchr(s, '=', (size_t)(e - s));
        if (!eq) continue;
        const char *key_end = eq;
        while (key_end > s && isspace((unsigned char)key_end[-1])) key_end--;
        const char *value = eq + 1;
        while (value < e && isspace((unsigned char)*value)) value++;
        int which;   // 0 file, 1 title, 2 length
        const char *digits;
        if (starts_with_nocase(s, key_end, "File")) { which = 0; digits = s + 4; }
        else if (starts_with_nocase(s, key_end, "Title")) { which = 1; digits = s + 5; }
        else if (starts_with_nocase(s, key_end, "Length")) { which = 2; digits = s + 6; }
        else continue;
        long index = 0;
        if (digits >= key_end) continue;
        for (const char *c = digits; c < key_end; c++) {
            if (!isdigit((unsigned char)*c)) { index = -1; break; }
            index = index * 10 + (*c - '0');
            if (index > MAX_PLS_ENTRIES) { index = -1; break; }
        }
        if (index < 1) continue;
        if (index > n) {
            PlsItem *grown = av_realloc_array(items, index, sizeof(PlsItem));
            if (!grown) { d->oom = true; break; }
            for (long i = n; i < index; i++) grown[i] = (PlsItem){ NULL, NULL, -1 };
            items = grown;
            n = (int)index;
        }
        PlsItem *it = &items[index - 1];
        if (which == 0) { av_free(it->file); it->file = dup_range(value, e); }
        else if (which == 1) { av_free(it->title); it->title = dup_range(value, e); }
        else {
            char *num = dup_range(value, e);
            double v;
            const char *after;
            it->length = num && ff_parse_number(num, &after, &v, false) && v >= 0 ? v : -1;
            av_free(num);
        }
    }
    for (int i = 0; i < n; i++) {
        if (items[i].file && items[i].file[0] && !d->oom)
            add_entry(d, items[i].file, base, items[i].title, items[i].length);
        av_free(items[i].file);
        av_free(items[i].title);
    }
    av_free(items);
    return !d->oom;
}

const AuPlaylistFile *au_playlist_parse(const char *text, const size_t length, const char *base, int *out_error) {
    int dummy;
    int *err = out_error ? out_error : &dummy;
    *err = AU_OK;
    if (!text && length) { *err = AU_ERR_INVALID_ARGUMENT; return NULL; }
    // Playlists are text: NUL bytes near the start mean something else (an audio file, say)
    if (text && memchr(text, '\0', FFMIN(length, 4096))) { *err = AU_ERR_FILE; return NULL; }

    // UTF-8 as it is (without a BOM); anything else is read as Latin-1
    char *raw = av_strndup(text ? text : "", length);
    if (!raw) { *err = AU_ERR_NO_MEMORY; return NULL; }
    const char *start = raw;
    if (strlen(start) >= 3 && !memcmp(start, "\xEF\xBB\xBF", 3)) start += 3;
    char *utf8 = text_to_utf8(start);
    av_free(raw);
    if (!utf8) { *err = AU_ERR_NO_MEMORY; return NULL; }
    const char *end = utf8 + strlen(utf8);

    // PLS when the first line says so
    const char *p = utf8, *s = NULL, *e = NULL;
    bool pls = false;
    while (next_line(&p, end, &s, &e)) {
        if (s == e) continue;
        pls = starts_with_nocase(s, e, "[playlist]");
        break;
    }

    Draft d = { 0 };
    bool hls = false;
    const bool ok = pls ? parse_pls(utf8, end, base, &d) : parse_m3u(utf8, end, base, &d, &hls);
    av_free(utf8);
    if (ok && hls) {
        // An HLS playlist is a stream: the playlist itself is what to play
        draft_free(&d);
        d = (Draft){ 0 };
        if (!base || !base[0]) { *err = AU_ERR_UNSUPPORTED; return NULL; }
        draft_add(&d, av_strdup(base), "", -1);
    }
    const AuPlaylistFile *pl = !d.oom ? freeze(&d) : NULL;
    draft_free(&d);
    if (!pl) *err = AU_ERR_NO_MEMORY;
    return pl;
}

const AuPlaylistFile *au_playlist_read(const char *path_or_url, int *out_error) {
    int dummy;
    int *err = out_error ? out_error : &dummy;
    if (!path_or_url || !path_or_url[0]) { *err = AU_ERR_INVALID_ARGUMENT; return NULL; }

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
        if (text.len > MAX_PLAYLIST_BYTES) { failed = true; break; }   // not a playlist
    }
    if (n < 0 && n != AVERROR_EOF) failed = true;
    avio_closep(&pb);
    if (failed || !av_bprint_is_complete(&text)) {
        *err = failed ? AU_ERR_FILE : AU_ERR_NO_MEMORY;
        av_bprint_finalize(&text, NULL);
        return NULL;
    }
    const AuPlaylistFile *pl = au_playlist_parse(text.str, text.len, path_or_url, err);
    av_bprint_finalize(&text, NULL);
    return pl;
}
