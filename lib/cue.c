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
 * Cue sheets and chapters (au_cue_read(), au_cue_parse()): the tracks inside one file, as ranges of it. Three
 * sources, in this order for an audio file: an embedded cue sheet text (the CUESHEET tag), a FLAC CUESHEET metadata
 * block (read here: FFmpeg turns it into chapters starting at INDEX 00, but a track starts at INDEX 01), and the
 * file's chapters. Like AuPlaylistFile, the result is one immutable block.
 */

#include "ffaudio_playlist.h"

#include <ctype.h>
#include <math.h>
#include <string.h>
#include "libavformat/avformat.h"
#include "libavutil/avstring.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "network.h"
#include "probe.h"
#include "numbers.h"
#include "text_list.h"

#define MAX_CUE_BYTES (1024 * 1024)
#define MAX_CUE_TRACKS 999
#define MAX_FLAC_CUESHEET_BYTES (1024 * 1024)

//+-----------------------------------------------------+
//| Draft and result                                    |
//+-----------------------------------------------------+

typedef struct DraftTrack {
    int number;
    char *path, *title, *performer, *songwriter, *isrc;   // owned; NULL = ""
    double start, length;                                 // length < 0: worked out from the next track
    bool has_start;
    AuReplayGain rg;
} DraftTrack;

typedef struct CueDraft {
    DraftTrack *t;
    int n, cap;
    char *title, *performer, *genre, *date;
    float album_gain, album_peak;
    bool oom;
} CueDraft;

static void draft_init(CueDraft *d) {
    memset(d, 0, sizeof(*d));
    d->album_gain = d->album_peak = NAN;
}

static DraftTrack *draft_track(CueDraft *d, const int number, char *path) {
    if (d->n >= MAX_CUE_TRACKS || d->oom) { av_free(path); return NULL; }
    if (d->n == d->cap) {
        const int cap = d->cap ? d->cap * 2 : 16;
        DraftTrack *t = av_realloc_array(d->t, (size_t)cap, sizeof(*t));
        if (!t) { d->oom = true; av_free(path); return NULL; }
        d->t = t;
        d->cap = cap;
    }
    DraftTrack *t = &d->t[d->n++];
    memset(t, 0, sizeof(*t));
    t->number = number;
    t->path = path;
    t->length = -1;
    t->rg = AU_REPLAYGAIN_NONE;
    return t;
}

static void draft_free(CueDraft *d) {
    for (int i = 0; i < d->n; i++) {
        av_free(d->t[i].path);
        av_free(d->t[i].title);
        av_free(d->t[i].performer);
        av_free(d->t[i].songwriter);
        av_free(d->t[i].isrc);
    }
    av_free(d->t);
    av_free(d->title);
    av_free(d->performer);
    av_free(d->genre);
    av_free(d->date);
}

static void set_str(CueDraft *d, char **field, char *value) {
    if (!value) { d->oom = true; return; }
    av_free(*field);
    *field = value;
}

// Tracks without a start are dropped; lengths run to the next track of the same file; album values for all
static void draft_finish(CueDraft *d) {
    int k = 0;
    for (int i = 0; i < d->n; i++) {
        if (d->t[i].has_start) d->t[k++] = d->t[i];
        else {
            DraftTrack *t = &d->t[i];
            av_free(t->path); av_free(t->title); av_free(t->performer); av_free(t->songwriter); av_free(t->isrc);
        }
    }
    d->n = k;
    for (int i = 0; i < d->n; i++) {
        DraftTrack *t = &d->t[i];
        if (t->length < 0) {
            t->length = 0;
            if (i + 1 < d->n && t->path && d->t[i + 1].path && !strcmp(t->path, d->t[i + 1].path) &&
                d->t[i + 1].start > t->start)
                t->length = d->t[i + 1].start - t->start;
        }
        t->rg.album_gain_db = d->album_gain;
        t->rg.album_peak = d->album_peak;
    }
}

static size_t str_size(const char *s) { return (s ? strlen(s) : 0) + 1; }

static const char *put_str(char **arena, const char *s) {
    const size_t n = str_size(s);
    char *out = *arena;
    memcpy(out, s ? s : "", n);
    *arena += n;
    return out;
}

// One block: the sheet, the pointer array, the tracks, then the strings
static const AuCueSheet *freeze(const CueDraft *d) {
    size_t strings = str_size(d->title) + str_size(d->performer) + str_size(d->genre) + str_size(d->date);
    for (int i = 0; i < d->n; i++) {
        const DraftTrack *t = &d->t[i];
        strings += str_size(t->path) + str_size(t->title) + str_size(t->performer ? t->performer : d->performer) +
                   str_size(t->songwriter) + str_size(t->isrc);
    }
    const size_t head = FFALIGN(sizeof(AuCueSheet), 16);
    const size_t ptrs = FFALIGN((size_t)d->n * sizeof(AuCueTrack *), 16);
    const size_t tracks = FFALIGN((size_t)d->n * sizeof(AuCueTrack), 16);
    uint8_t *block = av_mallocz(head + ptrs + tracks + strings);
    if (!block) return NULL;
    AuCueSheet *sheet = (AuCueSheet *)block;
    const AuCueTrack **ptr = (const AuCueTrack **)(block + head);
    AuCueTrack *out = (AuCueTrack *)(block + head + ptrs);
    char *arena = (char *)(block + head + ptrs + tracks);
    sheet->count = d->n;
    sheet->tracks = ptr;
    sheet->title = put_str(&arena, d->title);
    sheet->performer = put_str(&arena, d->performer);
    sheet->genre = put_str(&arena, d->genre);
    sheet->date = put_str(&arena, d->date);
    for (int i = 0; i < d->n; i++) {
        const DraftTrack *t = &d->t[i];
        AuCueTrack *o = &out[i];
        o->number = t->number;
        o->start_seconds = t->start;
        o->length_seconds = t->length;
        o->replaygain = t->rg;
        o->path = put_str(&arena, t->path);
        o->title = put_str(&arena, t->title);
        o->performer = put_str(&arena, t->performer ? t->performer : d->performer);
        o->songwriter = put_str(&arena, t->songwriter);
        o->isrc = put_str(&arena, t->isrc);
        ptr[i] = o;
    }
    return sheet;
}

void au_cue_release(const AuCueSheet *sheet) {
    av_free((void *)sheet);
}

//+-----------------------------------------------------+
//| Cue sheet text                                      |
//+-----------------------------------------------------+

static const char *skip_space(const char *s, const char *e) {
    while (s < e && isspace((unsigned char)*s)) s++;
    return s;
}

// A word, or a "quoted value" (no escapes in cue sheets); *s moves past it. NULL = out of memory.
static char *take_value(const char **s, const char *e) {
    const char *p = skip_space(*s, e);
    const char *v = p, *ve;
    if (p < e && *p == '"') {
        v = ++p;
        while (p < e && *p != '"') p++;
        ve = p;
        if (p < e) p++;
    } else {
        while (p < e && !isspace((unsigned char)*p)) p++;
        ve = p;
    }
    *s = p;
    return ff_text_dup(v, ve);
}

// The rest of the line as a value: quoted, or as written (TITLE Some Words)
static char *take_rest(const char *s, const char *e) {
    s = skip_space(s, e);
    if (s < e && *s == '"') return take_value(&s, e);
    return ff_text_dup(s, e);
}

static bool keyword(const char **s, const char *e, const char *word) {
    const size_t n = strlen(word);
    if ((size_t)(e - *s) < n || av_strncasecmp(*s, word, n)) return false;
    if (*s + n < e && !isspace((unsigned char)(*s)[n])) return false;
    *s += n;
    return true;
}

static bool take_int(const char **s, const char *e, long *out) {
    const char *p = skip_space(*s, e);
    if (p >= e || !isdigit((unsigned char)*p)) return false;
    long v = 0;
    while (p < e && isdigit((unsigned char)*p) && v < 100000000) v = v * 10 + (*p++ - '0');
    *out = v;
    *s = p;
    return true;
}

// mm:ss:ff (75 frames a second; minutes may go past 99)
static bool take_msf(const char **s, const char *e, double *seconds) {
    long m, sec, f;
    const char *p = *s;
    if (!take_int(&p, e, &m) || p >= e || *p++ != ':' || !take_int(&p, e, &sec) || p >= e || *p++ != ':' ||
        !take_int(&p, e, &f) || sec >= 60 || f >= 75)
        return false;
    *seconds = (double)(m * 60 + sec) + f / 75.0;
    *s = p;
    return true;
}

// A gain or peak value of a REM REPLAYGAIN_ line ("-7.89 dB", "0.988")
static float take_float(const char *s, const char *e) {
    char buf[64];
    s = skip_space(s, e);
    const size_t n = FFMIN((size_t)(e - s), sizeof(buf) - 1);
    memcpy(buf, s, n);
    buf[n] = '\0';
    double v;
    const char *end;
    return ff_parse_number(buf, &end, &v, true) && isfinite(v) ? (float)v : NAN;
}

// An audio file a sheet names but that is not there: the same name with another common audio extension
static char *find_audio(char *path) {
    static const char *const exts[] = { ".flac", ".wv", ".ape", ".wav", ".m4a", ".ogg", ".opus", ".mp3", ".tta",
                                        ".aiff", ".aif", ".wma", ".mka", NULL };
    if (!path || au_is_network_url(path) || avio_check(path, AVIO_FLAG_READ) >= 0) return path;
    const char *slash = strrchr(path, '/'), *bslash = strrchr(path, '\\');
    const char *dir_end = slash > bslash ? slash : bslash;
    const char *dot = strrchr(path, '.');
    const size_t stem = dot && (!dir_end || dot > dir_end) ? (size_t)(dot - path) : strlen(path);
    for (int i = 0; exts[i]; i++) {
        char *alt = av_asprintf("%.*s%s", (int)stem, path, exts[i]);
        if (alt && avio_check(alt, AVIO_FLAG_READ) >= 0) {
            av_free(path);
            return alt;
        }
        av_free(alt);
    }
    return path;
}

// Parse cue sheet text. embedded_path: the sheet is inside that audio file, so every track is in it (FILE lines only
// end the first file's tracks). Otherwise FILE names are resolved against base.
static bool parse_cue_text(const char *text, const char *end, const char *base, const char *embedded_path,
                           CueDraft *d) {
    const char *p = text, *s, *e;
    char *file = NULL;          // the current FILE, resolved
    int files = 0;
    DraftTrack *t = NULL;       // the current track (NULL: none yet, or one that is skipped)
    bool in_track = false;      // after the first TRACK line
    while (ff_text_next_line(&p, end, &s, &e) && !d->oom) {
        if (keyword(&s, e, "FILE")) {
            files++;
            av_freep(&file);
            t = NULL;
            if (embedded_path) {
                if (files > 1) break;   // a sheet for several files can not all be in this one
                file = av_strdup(embedded_path);
            } else {
                char *name = take_value(&s, e);
                char *path = name ? ff_list_resolve(name, base) : NULL;
                av_free(name);
                if (!path) { d->oom = true; break; }
                if (!ff_list_allows(base, path)) av_free(path);   // a sheet from a URL lists URLs only: skipped
                else file = find_audio(path);
            }
            if (embedded_path && !file) { d->oom = true; break; }
            continue;
        }
        if (keyword(&s, e, "TRACK")) {
            in_track = true;
            long number = 0;
            take_int(&s, e, &number);
            s = skip_space(s, e);
            t = NULL;
            if (file && keyword(&s, e, "AUDIO")) {
                char *path = av_strdup(file);
                if (!path) { d->oom = true; break; }
                t = draft_track(d, (int)number, path);
            }
            continue;
        }
        if (keyword(&s, e, "INDEX")) {
            long idx = -1;
            double at;
            if (t && take_int(&s, e, &idx) && take_msf(&s, e, &at) && idx == 1) {
                t->start = at;
                t->has_start = true;
            }
            continue;
        }
        char **sheet_field = NULL, **track_field = NULL;
        if (keyword(&s, e, "TITLE")) { sheet_field = &d->title; track_field = t ? &t->title : NULL; }
        else if (keyword(&s, e, "PERFORMER")) { sheet_field = &d->performer; track_field = t ? &t->performer : NULL; }
        else if (keyword(&s, e, "SONGWRITER")) track_field = t ? &t->songwriter : NULL;
        else if (keyword(&s, e, "ISRC")) track_field = t ? &t->isrc : NULL;
        else if (keyword(&s, e, "REM")) {
            s = skip_space(s, e);
            if (keyword(&s, e, "GENRE") && !in_track) set_str(d, &d->genre, take_rest(s, e));
            else if (keyword(&s, e, "DATE") && !in_track) set_str(d, &d->date, take_rest(s, e));
            else if (keyword(&s, e, "REPLAYGAIN_ALBUM_GAIN")) d->album_gain = take_float(s, e);
            else if (keyword(&s, e, "REPLAYGAIN_ALBUM_PEAK")) d->album_peak = take_float(s, e);
            else if (t && keyword(&s, e, "REPLAYGAIN_TRACK_GAIN")) t->rg.track_gain_db = take_float(s, e);
            else if (t && keyword(&s, e, "REPLAYGAIN_TRACK_PEAK")) t->rg.track_peak = take_float(s, e);
            continue;
        }
        else continue;   // CATALOG, FLAGS, PREGAP, POSTGAP, CDTEXTFILE, ...
        if (in_track) {
            if (track_field) set_str(d, track_field, take_rest(s, e));
        }
        else if (sheet_field) set_str(d, sheet_field, take_rest(s, e));
    }
    av_free(file);
    return !d->oom;
}

// Does this text look like a cue sheet (a FILE line, then a TRACK line)?
bool ff_text_is_cue(const char *text, const char *end) {
    const char *p = text, *s, *e;
    bool file = false;
    while (ff_text_next_line(&p, end, &s, &e)) {
        if (keyword(&s, e, "FILE")) file = true;
        else if (file && keyword(&s, e, "TRACK")) return true;
    }
    return false;
}

static const AuCueSheet *parse_with(const char *text, const size_t length, const char *base,
                                    const char *embedded_path, int *err) {
    char *utf8 = ff_list_text(text, length, err);
    if (!utf8) return NULL;
    CueDraft d;
    draft_init(&d);
    const char *end = utf8 + strlen(utf8);
    if (!ff_text_is_cue(utf8, end) && !embedded_path) {
        av_free(utf8);
        draft_free(&d);
        *err = AU_ERR_FILE;   // not a cue sheet
        return NULL;
    }
    const bool ok = parse_cue_text(utf8, end, base, embedded_path, &d);
    av_free(utf8);
    if (ok) draft_finish(&d);
    const AuCueSheet *sheet = ok ? freeze(&d) : NULL;
    draft_free(&d);
    *err = sheet ? AU_OK : AU_ERR_NO_MEMORY;
    return sheet;
}

const AuCueSheet *au_cue_parse(const char *text, const size_t length, const char *base, int *out_error) {
    int dummy;
    int *err = out_error ? out_error : &dummy;
    *err = AU_OK;
    return parse_with(text, length, base, NULL, err);
}

//+-----------------------------------------------------+
//| Audio files: FLAC CUESHEET blocks and chapters      |
//+-----------------------------------------------------+

// The CUESHEET metadata block of a FLAC file (and the sample rate from STREAMINFO); false when there is none
static bool read_flac_cuesheet(AVIOContext *pb, uint8_t **data, int *size, int *rate) {
    *data = NULL;
    *rate = 0;
    if (avio_seek(pb, 0, SEEK_SET) < 0) return false;
    uint8_t h[10];
    if (avio_read(pb, h, 10) != 10) return false;
    int64_t pos = 0;
    if (!memcmp(h, "ID3", 3)) {   // an ID3v2 tag before the stream
        pos = 10 + (((int64_t)(h[6] & 0x7f) << 21) | ((h[7] & 0x7f) << 14) | ((h[8] & 0x7f) << 7) | (h[9] & 0x7f));
        if (h[5] & 0x10) pos += 10;
    }
    if (avio_seek(pb, pos, SEEK_SET) < 0 || avio_rb32(pb) != MKBETAG('f', 'L', 'a', 'C')) return false;
    for (int blocks = 0; blocks < 1000 && !avio_feof(pb); blocks++) {
        const uint32_t hdr = avio_rb32(pb);
        const int type = (hdr >> 24) & 0x7f, len = (int)(hdr & 0xffffff);
        if (type == 0 && len >= 18) {
            uint8_t si[18];
            if (avio_read(pb, si, 18) != 18) return false;
            *rate = (si[10] << 12) | (si[11] << 4) | (si[12] >> 4);
            avio_skip(pb, len - 18);
        }
        else if (type == 5 && len >= 396 && len <= MAX_FLAC_CUESHEET_BYTES) {
            if (!(*data = av_malloc((size_t)len))) return false;
            if (avio_read(pb, *data, len) != len) { av_freep(data); return false; }
            *size = len;
            return *rate > 0;
        }
        else avio_skip(pb, len);
        if (hdr & 0x80000000u) break;   // the last metadata block
    }
    return false;
}

static bool parse_flac_cuesheet(const uint8_t *b, const int size, const int rate, const char *path, CueDraft *d) {
    const bool is_cd = b[136] & 0x80;
    int ntracks = b[395], off = 396;
    for (int i = 0; i < ntracks && off + 36 <= size; i++) {
        const uint64_t offset = AV_RB64(b + off);
        const int number = b[off + 8];
        char isrc[13];
        memcpy(isrc, b + off + 9, 12);
        isrc[12] = '\0';
        const bool audio = !(b[off + 21] & 0x80);
        const int nindex = b[off + 35];
        off += 36;
        if (off + 12 * nindex > size) break;
        // INDEX 01, else the first index point there is
        int64_t index1 = -1, first = -1;
        for (int k = 0; k < nindex; k++) {
            const uint64_t at = AV_RB64(b + off + 12 * k);
            const int n = b[off + 12 * k + 8];
            if (first < 0) first = (int64_t)at;
            if (n == 1) index1 = (int64_t)at;
        }
        off += 12 * nindex;
        const bool lead_out = number == (is_cd ? 170 : 255);
        if (lead_out || !audio || nindex == 0) continue;
        char *p = av_strdup(path);
        if (!p) return false;
        DraftTrack *t = draft_track(d, number, p);
        if (!t) return !d->oom;
        t->start = (double)(offset + (uint64_t)(index1 >= 0 ? index1 : first)) / rate;
        t->has_start = true;
        if (isrc[0]) set_str(d, &t->isrc, av_strdup(isrc));
    }
    return !d->oom;
}

static void sheet_tags(CueDraft *d, const AVFormatContext *ic) {
    const AVDictionaryEntry *e;
    if ((e = av_dict_get(ic->metadata, "album", NULL, 0)) || (e = av_dict_get(ic->metadata, "title", NULL, 0)))
        set_str(d, &d->title, av_strdup(e->value));
    if ((e = av_dict_get(ic->metadata, "album_artist", NULL, 0)) || (e = av_dict_get(ic->metadata, "artist", NULL, 0)))
        set_str(d, &d->performer, av_strdup(e->value));
    if ((e = av_dict_get(ic->metadata, "genre", NULL, 0))) set_str(d, &d->genre, av_strdup(e->value));
    if ((e = av_dict_get(ic->metadata, "date", NULL, 0))) set_str(d, &d->date, av_strdup(e->value));
}

static bool parse_chapters(const AVFormatContext *ic, const char *path, CueDraft *d) {
    const double origin = ic->start_time != AV_NOPTS_VALUE ? ic->start_time / (double)AV_TIME_BASE : 0.0;
    const double file_s = ic->duration > 0 ? ic->duration / (double)AV_TIME_BASE : -1;
    for (unsigned int i = 0; i < ic->nb_chapters; i++) {
        const AVChapter *ch = ic->chapters[i];
        char *p = av_strdup(path);
        if (!p) return false;
        DraftTrack *t = draft_track(d, (int)i + 1, p);
        if (!t) return !d->oom;
        const double start = FFMAX(0.0, ch->start * av_q2d(ch->time_base) - origin);
        const double end = ch->end * av_q2d(ch->time_base) - origin;
        t->start = start;
        t->has_start = true;
        // The last one plays to the end of the file (its end is often rounded)
        const bool last = i + 1 == ic->nb_chapters;
        t->length = last && (file_s < 0 || end >= file_s - 0.002) ? 0 : FFMAX(0.0, end - start);
        const AVDictionaryEntry *e = av_dict_get(ch->metadata, "title", NULL, 0);
        if (e && e->value[0]) set_str(d, &t->title, av_strdup(e->value));
    }
    return !d->oom;
}

static bool is_cue_name(const char *path) {
    const size_t n = strcspn(path, au_is_network_url(path) ? "?#" : "");
    return n >= 4 && !av_strncasecmp(path + n - 4, ".cue", 4);
}

const AuCueSheet *au_cue_read(const char *path_or_url, int *out_error) {
    int dummy;
    int *err = out_error ? out_error : &dummy;
    if (!path_or_url || !path_or_url[0]) { *err = AU_ERR_INVALID_ARGUMENT; return NULL; }

    if (is_cue_name(path_or_url)) {
        size_t length = 0;
        char *text = ff_list_read(path_or_url, MAX_CUE_BYTES, &length, err);
        if (!text) return NULL;
        const AuCueSheet *sheet = parse_with(text, length, path_or_url, NULL, err);
        av_free(text);
        return sheet;
    }

    // An audio file: only its header is read
    AVFormatContext *ic = NULL;
    if (ff_open_input(path_or_url, NULL, NULL, &ic) < 0) { *err = AU_ERR_FILE; return NULL; }

    const AuCueSheet *sheet = NULL;
    const AVDictionaryEntry *tag = av_dict_get(ic->metadata, "cuesheet", NULL, 0);
    if (tag && tag->value[0]) {
        sheet = parse_with(tag->value, strlen(tag->value), NULL, path_or_url, err);
        if (sheet && sheet->count > 0) {
            avformat_close_input(&ic);
            return sheet;
        }
        au_cue_release(sheet);
        sheet = NULL;
    }

    CueDraft d;
    draft_init(&d);
    sheet_tags(&d, ic);
    uint8_t *block = NULL;
    int size = 0, rate = 0;
    bool ok;
    if (!strcmp(ic->iformat->name, "flac") && ic->pb && read_flac_cuesheet(ic->pb, &block, &size, &rate))
        ok = parse_flac_cuesheet(block, size, rate, path_or_url, &d);
    else
        ok = parse_chapters(ic, path_or_url, &d);
    av_free(block);
    avformat_close_input(&ic);
    if (ok) draft_finish(&d);
    if (ok && d.n == 0) {
        draft_free(&d);
        *err = AU_ERR_NOTHING_TO_DO;
        return NULL;
    }
    sheet = ok ? freeze(&d) : NULL;
    draft_free(&d);
    *err = sheet ? AU_OK : AU_ERR_NO_MEMORY;
    return sheet;
}
