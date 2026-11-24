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
#include "text_list.h"
#include "globals.h"
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

// Add an entry as written in the playlist. A playlist from a URL may only list URLs: local paths in it (absolute ones,
// file://) are left out, as a stream's playlist may not reach local files either.
static void add_entry(Draft *d, const char *entry, const char *base, const char *title, const double duration) {
    char *path = ff_list_resolve(entry, base);
    if (!path) { d->oom = true; return; }
    if (!ff_list_allows(base, path)) {
        av_free(path);
        return;
    }
    draft_add(d, path, title, duration);
}

//+-----------------------------------------------------+
//| Formats                                             |
//+-----------------------------------------------------+

// #EXTINF:<seconds>[ attributes],<title>
static void parse_extinf(const char *s, const char *e, double *duration, char **title) {
    s += 8;
    double d;
    const char *after;
    char *num = ff_text_dup(s, e);
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
            *title = ff_text_dup(c, e);
            return;
        }
    }
}

static bool parse_m3u(const char *text, const char *end, const char *base, Draft *d, bool *hls) {
    const char *p = text, *s, *e;
    char *title = NULL;
    double duration = -1;
    while (ff_text_next_line(&p, end, &s, &e)) {
        if (s == e) continue;
        if (*s == '#') {
            if (ff_text_starts_nocase(s, e, "#EXTINF:")) parse_extinf(s, e, &duration, &title);
            else if (ff_text_starts_nocase(s, e, "#EXT-X-")) *hls = true;
            continue;
        }
        char *entry = ff_text_dup(s, e);
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
    while (ff_text_next_line(&p, end, &s, &e)) {
        const char *eq = memchr(s, '=', (size_t)(e - s));
        if (!eq) continue;
        const char *key_end = eq;
        while (key_end > s && isspace((unsigned char)key_end[-1])) key_end--;
        const char *value = eq + 1;
        while (value < e && isspace((unsigned char)*value)) value++;
        int which;   // 0 file, 1 title, 2 length
        const char *digits;
        if (ff_text_starts_nocase(s, key_end, "File")) { which = 0; digits = s + 4; }
        else if (ff_text_starts_nocase(s, key_end, "Title")) { which = 1; digits = s + 5; }
        else if (ff_text_starts_nocase(s, key_end, "Length")) { which = 2; digits = s + 6; }
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
        if (which == 0) { av_free(it->file); it->file = ff_text_dup(value, e); }
        else if (which == 1) { av_free(it->title); it->title = ff_text_dup(value, e); }
        else {
            char *num = ff_text_dup(value, e);
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
    char *utf8 = ff_list_text(text, length, err);
    if (!utf8) return NULL;
    const char *end = utf8 + strlen(utf8);
    if (ff_text_is_cue(utf8, end)) {
        // Its lines are not paths: a cue sheet lists ranges of files (au_cue_read())
        av_free(utf8);
        ff_log(AU_LOG_WARNING, "au_playlist: %s is a cue sheet: read it with au_cue_read()", base ? base : "the text");
        *err = AU_ERR_UNSUPPORTED;
        return NULL;
    }

    // PLS when the first line says so
    const char *p = utf8, *s = NULL, *e = NULL;
    bool pls = false;
    while (ff_text_next_line(&p, end, &s, &e)) {
        if (s == e) continue;
        pls = ff_text_starts_nocase(s, e, "[playlist]");
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

    size_t length = 0;
    char *text = ff_list_read(path_or_url, MAX_PLAYLIST_BYTES, &length, err);
    if (!text) return NULL;
    const AuPlaylistFile *pl = au_playlist_parse(text, length, path_or_url, err);
    av_free(text);
    return pl;
}
