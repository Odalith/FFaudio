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

#include <math.h>
#include <ctype.h>
#include "metadata.h"
#include "probe.h"
#include "numbers.h"
#include "network.h"
#include "libavutil/avstring.h"

// R128 gains are relative to -23 LUFS, ReplayGain 2.0 to -18 LUFS
#define R128_TO_REPLAYGAIN_DB 5.0

//+-----------------------------------------------------+
//| Parsing helpers (locale independent)                |
//+-----------------------------------------------------+

// A decimal number such as "-6.20 dB", "-6,20 dB" (some taggers write the locale's comma), "+3", "0.988"
static bool parse_decimal(const char *s, double *out) {
    if (!s) return false;
    while (*s && isspace((unsigned char)*s)) s++;
    return ff_parse_number(s, NULL, out, true);
}

// Parse "3", "3/12" into number and total (0 when missing)
static void parse_pair(const char *s, int *number, int *total) {
    double n = 0, t = 0;
    if (s && parse_decimal(s, &n)) {
        const char *slash = strchr(s, '/');
        if (slash) parse_decimal(slash + 1, &t);
    }
    if (number) *number = n > 0 ? (int)n : 0;
    if (total) *total = t > 0 ? (int)t : 0;
}

static const char *dict_get(const AVDictionary *d, const char *key) {
    const AVDictionaryEntry *e = av_dict_get(d, key, NULL, 0); // case-insensitive
    return e ? e->value : NULL;
}

//+-----------------------------------------------------+
//| ReplayGain                                          |
//+-----------------------------------------------------+

AuReplayGain replaygain_from_tags(const AVDictionary *tags) {
    AuReplayGain rg = AU_REPLAYGAIN_NONE;
    double v;

    if (parse_decimal(dict_get(tags, "REPLAYGAIN_TRACK_GAIN"), &v) && fabs(v) < 100) rg.track_gain_db = (float)v;
    if (parse_decimal(dict_get(tags, "REPLAYGAIN_ALBUM_GAIN"), &v) && fabs(v) < 100) rg.album_gain_db = (float)v;
    if (parse_decimal(dict_get(tags, "REPLAYGAIN_TRACK_PEAK"), &v) && v > 0 && v < 100) rg.track_peak = (float)v;
    if (parse_decimal(dict_get(tags, "REPLAYGAIN_ALBUM_PEAK"), &v) && v > 0 && v < 100) rg.album_peak = (float)v;

    // R128 tags (standard in Opus, RFC 7845; accepted in any format): Q7.8 fixed point dB relative to -23 LUFS.
    // Only used where ReplayGain tags are missing; both measure BS.1770 loudness, so the references differ by 5 dB.
    if (isnan(rg.track_gain_db) && parse_decimal(dict_get(tags, "R128_TRACK_GAIN"), &v)) {
        rg.track_gain_db = (float)(v / 256.0 + R128_TO_REPLAYGAIN_DB);
        rg.from_r128 = true;
    }
    if (isnan(rg.album_gain_db) && parse_decimal(dict_get(tags, "R128_ALBUM_GAIN"), &v)) {
        rg.album_gain_db = (float)(v / 256.0 + R128_TO_REPLAYGAIN_DB);
        rg.from_r128 = true;
    }
    return rg;
}

double replaygain_gain_db(const AuReplayGainConfig *cfg, const AuReplayGain *rg) {
    if (!cfg || cfg->mode == AU_REPLAYGAIN_OFF) return 0.0;

    const bool album = cfg->mode == AU_REPLAYGAIN_ALBUM;
    // Preferred gain first, the other one as a fallback; the peak that belongs to the chosen gain first
    double gain = NAN, peak = NAN;
    if (album && !isnan(rg->album_gain_db))  gain = rg->album_gain_db;
    else if (!isnan(rg->track_gain_db))      gain = rg->track_gain_db;
    else if (!isnan(rg->album_gain_db))      gain = rg->album_gain_db;

    if (album && !isnan(rg->album_peak))     peak = rg->album_peak;
    else if (!isnan(rg->track_peak))         peak = rg->track_peak;
    else if (!isnan(rg->album_peak))         peak = rg->album_peak;

    double db = !isnan(gain) ? gain + cfg->preamp_db : cfg->fallback_gain_db;
    if (cfg->prevent_clipping && !isnan(peak) && peak > 0) {
        const double max_db = -20.0 * log10(peak);
        if (db > max_db) db = max_db;
    }
    return db;
}

//+-----------------------------------------------------+
//| AuMediaInfo                                         |
//+-----------------------------------------------------+

// Strict UTF-8: no overlong forms, no surrogates (U+D800..DFFF), nothing above U+10FFFF
static bool valid_utf8(const unsigned char *s) {
    while (*s) {
        const unsigned char c = *s;
        if (c < 0x80) { s++; continue; }
        int n;
        unsigned char lo = 0x80, hi = 0xBF;          // allowed range of the second byte
        if (c >= 0xC2 && c <= 0xDF) n = 1;
        else if (c == 0xE0) { n = 2; lo = 0xA0; }
        else if (c == 0xED) { n = 2; hi = 0x9F; }
        else if (c >= 0xE1 && c <= 0xEF) n = 2;
        else if (c == 0xF0) { n = 3; lo = 0x90; }
        else if (c == 0xF4) { n = 3; hi = 0x8F; }
        else if (c >= 0xF1 && c <= 0xF3) n = 3;
        else return false;
        if (s[1] < lo || s[1] > hi) return false;
        for (int i = 2; i <= n; i++)
            if ((s[i] & 0xC0) != 0x80) return false;
        s += n + 1;
    }
    return true;
}

// Windows-1252's characters for 0x80..0x9F, where Latin-1 has control codes; the five it leaves undefined stay those
// controls (as WHATWG reads it). 0xA0..0xFF are the same in both.
static const uint16_t cp1252_high[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D,
    0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A,
    0x0153, 0x009D, 0x017E, 0x0178,
};

char *text_to_utf8(const char *s) {
    if (!s) return NULL;
    if (valid_utf8((const unsigned char *)s)) return av_strdup(s);
    char *out = av_malloc(strlen(s) * 3 + 1), *o = out;
    if (!out) return NULL;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        const unsigned cp = *p >= 0x80 && *p <= 0x9F ? cp1252_high[*p - 0x80] : *p;
        if (cp < 0x80) *o++ = (char)cp;
        else if (cp < 0x800) {
            *o++ = (char)(0xC0 | (cp >> 6));
            *o++ = (char)(0x80 | (cp & 0x3F));
        }
        else {
            *o++ = (char)(0xE0 | (cp >> 12));
            *o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
            *o++ = (char)(0x80 | (cp & 0x3F));
        }
    }
    *o = '\0';
    return out;
}

// Tag values that are not UTF-8 (WAV INFO and AIFF chunks, broken files) read as Windows-1252, like other text
static void tags_to_utf8(AVDictionary **tags) {
    const AVDictionaryEntry *e = NULL;
    bool all_valid = true;
    while (all_valid && (e = av_dict_get(*tags, "", e, AV_DICT_IGNORE_SUFFIX)))
        all_valid = valid_utf8((const unsigned char *)e->value);
    if (all_valid) return;
    AVDictionary *fixed = NULL;
    e = NULL;
    while ((e = av_dict_get(*tags, "", e, AV_DICT_IGNORE_SUFFIX))) {
        char *v = text_to_utf8(e->value);
        if (!v || av_dict_set(&fixed, e->key, v, AV_DICT_DONT_STRDUP_VAL | AV_DICT_MULTIKEY) < 0) {
            av_dict_free(&fixed);
            return; // out of memory: keep them as they are
        }
    }
    av_dict_free(tags);
    *tags = fixed;
}

void media_draft_free(MediaDraft *d) {
    av_freep(&d->path);
    av_freep(&d->container);
    av_freep(&d->codec);
    av_freep(&d->channel_layout);
    av_dict_free(&d->tags);
}

int media_draft_from_context(MediaDraft *d, const AVFormatContext *ic, const int stream_index, const char *path,
                             const bool chained_ogg) {
    memset(d, 0, sizeof(*d));
    d->path = av_strdup(path ? path : "");
    d->container = av_strdup(ic->iformat && ic->iformat->name ? ic->iformat->name : "");
    d->duration = ic->duration != AV_NOPTS_VALUE ? ic->duration / AV_TIME_BASE_DOUBLE : -1;
    d->bit_rate = ic->bit_rate > 0 ? ic->bit_rate : 0;

    // Container tags first, stream tags (Ogg, Opus, some MP4) only where the container has none
    av_dict_copy(&d->tags, ic->metadata, 0);

    if (stream_index >= 0 && stream_index < (int)ic->nb_streams) {
        const AVStream *st = ic->streams[stream_index];
        const AVCodecParameters *par = st->codecpar;
        av_dict_copy(&d->tags, st->metadata, AV_DICT_DONT_OVERWRITE);

        d->codec = av_strdup(avcodec_get_name(par->codec_id));
        d->sample_rate = par->sample_rate;
        d->channels = par->ch_layout.nb_channels;
        d->bits_per_sample = par->bits_per_raw_sample > 0 ? par->bits_per_raw_sample
                                                          : av_get_bits_per_sample(par->codec_id);
        if (par->bit_rate > 0 && !d->bit_rate) d->bit_rate = par->bit_rate;
        if (d->duration < 0 && st->duration != AV_NOPTS_VALUE)
            d->duration = st->duration * av_q2d(st->time_base);

        char layout[128];
        if (av_channel_layout_describe(&par->ch_layout, layout, sizeof(layout)) >= 0)
            d->channel_layout = av_strdup(layout);
    }
    tags_to_utf8(&d->tags);   // AuMediaInfo promises UTF-8
    // After the stream's duration has filled in a missing one. Live = a network stream without an end, as the player
    // treats it; a local file of unknown length still ends. HLS seeks through its playlist, everything else needs a
    // seekable byte stream.
    d->live = d->duration <= 0 && au_is_network_url(d->path);
    d->seekable = !d->live && (!ic->pb || (ic->pb->seekable & AVIO_SEEKABLE_NORMAL) ||
                               (ic->iformat && ic->iformat->name && !strcmp(ic->iformat->name, "hls")));
    if (chained_ogg) d->seekable = false; // only its first part
    for (unsigned int i = 0; i < ic->nb_streams; i++)
        if (ic->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC) d->picture_count++;
    if (!d->codec) d->codec = av_strdup("");
    if (!d->channel_layout) d->channel_layout = av_strdup("");

    if (!d->path || !d->container || !d->codec || !d->channel_layout) {
        media_draft_free(d);
        return AVERROR(ENOMEM);
    }
    return 0;
}

// The block behind an AuMediaInfo: the reference count, the struct, the tag array, then every string
typedef struct MediaBlock {
    SDL_atomic_t refs;
    AuMediaInfo info;
} MediaBlock;

static MediaBlock *block_of(const AuMediaInfo *info) {
    return (MediaBlock *)((char *)info - offsetof(MediaBlock, info));
}

static char *put_string(char **arena, const char *s) {
    const size_t n = strlen(s) + 1;
    char *at = *arena;
    memcpy(at, s, n);
    *arena += n;
    return at;
}

static void number_and_total(const AuMediaInfo *info, const char *key, const char *const totals[],
                             int *number, int *total);

const AuMediaInfo *media_info_freeze(const MediaDraft *d) {
    const int n_tags = av_dict_count(d->tags);
    size_t chars = strlen(d->path) + strlen(d->container) + strlen(d->codec) + strlen(d->channel_layout) + 4;
    const AVDictionaryEntry *e = NULL;
    while ((e = av_dict_iterate(d->tags, e))) chars += strlen(e->key) + strlen(e->value) + 2;

    const size_t head = sizeof(MediaBlock) + (size_t)n_tags * sizeof(AuTag);
    MediaBlock *b = av_malloc(head + chars);
    if (!b) return NULL;
    SDL_AtomicSet(&b->refs, 1);
    AuMediaInfo *info = &b->info;
    AuTag *tags = (AuTag *)(b + 1);
    char *arena = (char *)b + head;

    info->path = put_string(&arena, d->path);
    info->container = put_string(&arena, d->container);
    info->codec = put_string(&arena, d->codec);
    info->channel_layout = put_string(&arena, d->channel_layout);
    info->duration = d->duration;
    info->bit_rate = d->bit_rate;
    info->sample_rate = d->sample_rate;
    info->channels = d->channels;
    info->bits_per_sample = d->bits_per_sample;
    info->live = d->live;
    info->seekable = d->seekable;
    int i = 0;
    for (e = NULL; (e = av_dict_iterate(d->tags, e)); i++) {
        tags[i].key = put_string(&arena, e->key);
        tags[i].value = put_string(&arena, e->value);
    }
    info->tag_count = n_tags;
    info->tags = tags;
    info->picture_count = d->picture_count;
    info->replaygain = replaygain_from_tags(d->tags);

    static const char *const track_totals[] = { "tracktotal", "totaltracks", NULL };
    static const char *const disc_totals[] = { "disctotal", "totaldiscs", NULL };
    number_and_total(info, "track", track_totals, &info->track_number, &info->track_total);
    number_and_total(info, "disc", disc_totals, &info->disc_number, &info->disc_total);
    return info;
}

const AuMediaInfo *media_info_retain(const AuMediaInfo *info) {
    if (info) SDL_AtomicIncRef(&block_of(info)->refs);
    return info;
}

const AuMediaInfo *media_info_with_title(const AuMediaInfo *info, const char *title, const char *artist) {
    if (!info) return NULL;
    MediaDraft d = {
        .path = av_strdup(info->path), .container = av_strdup(info->container), .codec = av_strdup(info->codec),
        .channel_layout = av_strdup(info->channel_layout), .duration = info->duration, .bit_rate = info->bit_rate,
        .sample_rate = info->sample_rate, .channels = info->channels, .bits_per_sample = info->bits_per_sample,
        .live = info->live, .seekable = info->seekable, .picture_count = info->picture_count,
    };
    for (int i = 0; i < info->tag_count; i++) av_dict_set(&d.tags, info->tags[i].key, info->tags[i].value, 0);
    av_dict_set(&d.tags, "title", title, 0);
    av_dict_set(&d.tags, "artist", artist, 0);   // NULL removes it
    const AuMediaInfo *out = d.path && d.container && d.codec && d.channel_layout ? media_info_freeze(&d) : NULL;
    media_draft_free(&d);
    return out;
}

//+-----------------------------------------------------+
//| Public API                                          |
//+-----------------------------------------------------+

const AuMediaInfo *au_probe(const char *filename, int *out_error) {
    AVFormatContext *ic = NULL;
    const AuMediaInfo *info = NULL;
    int ret;

    if (!filename || !filename[0]) {
        if (out_error) *out_error = AU_ERR_INVALID_ARGUMENT;
        return NULL;
    }
    int st = -1;
    bool chained = false;
    FfOpenStep failed;
    if ((ret = ff_open_audio_input(filename, NULL, NULL, &ic, &st, &chained, &failed)) < 0) goto end;

    MediaDraft d;
    if ((ret = media_draft_from_context(&d, ic, st, filename, chained)) < 0) goto end;
    info = media_info_freeze(&d);
    media_draft_free(&d);
    ret = info ? 0 : AVERROR(ENOMEM);

end:
    avformat_close_input(&ic);
    if (out_error) *out_error = ret < 0 ? (ret == AVERROR(ENOMEM) ? AU_ERR_NO_MEMORY : AU_ERR_FILE) : AU_OK;
    return info;
}

void au_media_info_release(const AuMediaInfo *info) {
    if (!info) return;
    MediaBlock *b = block_of(info);
    if (SDL_AtomicDecRef(&b->refs)) av_free(b);
}

// Case-insensitive, like AVDictionary
static const char *tag_get(const AuMediaInfo *info, const char *key) {
    for (int i = 0; i < info->tag_count; i++)
        if (!av_strcasecmp(info->tags[i].key, key)) return info->tags[i].value;
    return NULL;
}

const char *au_media_tag(const AuMediaInfo *info, const char *key) {
    if (!info || !key || !key[0]) return NULL;
    const char *v = tag_get(info, key);
    if (v) return v;

    // Spellings that differ between formats
    static const char *const aliases[][5] = {
        { "album_artist", "albumartist", "album artist", "TPE2", NULL },
        { "date", "year", "TDRC", "TYER", NULL },
        { "track", "tracknumber", "TRCK", NULL },
        { "disc", "discnumber", "TPOS", NULL },
        { "comment", "description", NULL },
    };
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
        bool match = false;
        for (int j = 0; aliases[i][j] && !match; j++)
            match = av_strcasecmp(aliases[i][j], key) == 0;
        if (!match) continue;
        for (int j = 0; aliases[i][j]; j++)
            if ((v = tag_get(info, aliases[i][j]))) return v;
    }
    return NULL;
}

static void number_and_total(const AuMediaInfo *info, const char *key, const char *const totals[],
                             int *number, int *total) {
    int n = 0, t = 0;
    parse_pair(au_media_tag(info, key), &n, &t);
    for (int i = 0; !t && totals[i]; i++)
        parse_pair(au_media_tag(info, totals[i]), &t, NULL);
    if (number) *number = n;
    if (total) *total = t;
}

