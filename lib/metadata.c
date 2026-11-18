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

// R128 gains are relative to -23 LUFS, ReplayGain 2.0 to -18 LUFS
#define R128_TO_REPLAYGAIN_DB 5.0

//+-----------------------------------------------------+
//| Parsing helpers (locale independent)                |
//+-----------------------------------------------------+

// Parse a decimal number such as "-6.20 dB", "+3", "0.988". Returns false if there is no number.
static bool parse_decimal(const char *s, double *out) {
    if (!s) return false;
    while (*s && isspace((unsigned char)*s)) s++;
    double sign = 1.0;
    if (*s == '+' || *s == '-') {
        if (*s == '-') sign = -1.0;
        s++;
    }
    double v = 0.0;
    bool digits = false;
    while (*s >= '0' && *s <= '9') { v = v * 10.0 + (*s - '0'); s++; digits = true; }
    if (*s == '.' || *s == ',') {
        s++;
        double scale = 0.1;
        while (*s >= '0' && *s <= '9') { v += (*s - '0') * scale; scale *= 0.1; s++; digits = true; }
    }
    if (!digits || !isfinite(v)) return false;
    *out = sign * v;
    return true;
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

AuReplayGainInfo replaygain_from_tags(const AVDictionary *tags) {
    AuReplayGainInfo rg;
    memset(&rg, 0, sizeof(rg));
    double v;

    if (parse_decimal(dict_get(tags, "REPLAYGAIN_TRACK_GAIN"), &v) && fabs(v) < 100) {
        rg.has_track_gain = true;
        rg.track_gain_db = v;
    }
    if (parse_decimal(dict_get(tags, "REPLAYGAIN_ALBUM_GAIN"), &v) && fabs(v) < 100) {
        rg.has_album_gain = true;
        rg.album_gain_db = v;
    }
    if (parse_decimal(dict_get(tags, "REPLAYGAIN_TRACK_PEAK"), &v) && v > 0 && v < 100) {
        rg.has_track_peak = true;
        rg.track_peak = v;
    }
    if (parse_decimal(dict_get(tags, "REPLAYGAIN_ALBUM_PEAK"), &v) && v > 0 && v < 100) {
        rg.has_album_peak = true;
        rg.album_peak = v;
    }

    // Opus: Q7.8 fixed point dB relative to -23 LUFS. Only used where ReplayGain tags are missing.
    if (!rg.has_track_gain && parse_decimal(dict_get(tags, "R128_TRACK_GAIN"), &v)) {
        rg.has_track_gain = true;
        rg.track_gain_db = v / 256.0 + R128_TO_REPLAYGAIN_DB;
        rg.from_r128 = true;
    }
    if (!rg.has_album_gain && parse_decimal(dict_get(tags, "R128_ALBUM_GAIN"), &v)) {
        rg.has_album_gain = true;
        rg.album_gain_db = v / 256.0 + R128_TO_REPLAYGAIN_DB;
        rg.from_r128 = true;
    }
    return rg;
}

double replaygain_gain_db(const AuReplayGainConfig *cfg, const AuReplayGainInfo *rg) {
    if (!cfg || cfg->mode == AU_REPLAYGAIN_OFF) return 0.0;

    const bool album = cfg->mode == AU_REPLAYGAIN_ALBUM;
    bool has_gain = false, has_peak = false;
    double gain = 0, peak = 0;

    // Preferred gain first, the other one as a fallback; the peak that belongs to the chosen gain first
    if (album && rg->has_album_gain) { gain = rg->album_gain_db; has_gain = true; }
    else if (rg->has_track_gain)     { gain = rg->track_gain_db; has_gain = true; }
    else if (rg->has_album_gain)     { gain = rg->album_gain_db; has_gain = true; }

    if (album && rg->has_album_peak) { peak = rg->album_peak; has_peak = true; }
    else if (rg->has_track_peak)     { peak = rg->track_peak; has_peak = true; }
    else if (rg->has_album_peak)     { peak = rg->album_peak; has_peak = true; }

    double db = has_gain ? gain + cfg->preamp_db : cfg->fallback_gain_db;
    if (cfg->prevent_clipping && has_peak && peak > 0) {
        const double max_db = -20.0 * log10(peak);
        if (db > max_db) db = max_db;
    }
    return db;
}

//+-----------------------------------------------------+
//| AuMediaInfo                                         |
//+-----------------------------------------------------+

AuMediaInfo *media_info_from_context(const AVFormatContext *ic, const int stream_index, const char *filename) {
    AuMediaInfo *info = av_mallocz(sizeof(AuMediaInfo));
    if (!info) return NULL;

    info->filename = av_strdup(filename ? filename : "");
    info->container = av_strdup(ic->iformat && ic->iformat->name ? ic->iformat->name : "");
    info->duration = ic->duration != AV_NOPTS_VALUE ? ic->duration / AV_TIME_BASE_DOUBLE : -1;
    info->bit_rate = ic->bit_rate > 0 ? ic->bit_rate : 0;

    // Container tags first, stream tags (Ogg, Opus, some MP4) only where the container has none
    av_dict_copy(&info->tags, ic->metadata, 0);

    if (stream_index >= 0 && stream_index < (int)ic->nb_streams) {
        const AVStream *st = ic->streams[stream_index];
        const AVCodecParameters *par = st->codecpar;
        av_dict_copy(&info->tags, st->metadata, AV_DICT_DONT_OVERWRITE);

        info->codec = av_strdup(avcodec_get_name(par->codec_id));
        info->sample_rate = par->sample_rate;
        info->channels = par->ch_layout.nb_channels;
        info->bits_per_sample = par->bits_per_raw_sample > 0 ? par->bits_per_raw_sample
                                                             : av_get_bits_per_sample(par->codec_id);
        if (par->bit_rate > 0 && !info->bit_rate) info->bit_rate = par->bit_rate;
        if (info->duration < 0 && st->duration != AV_NOPTS_VALUE)
            info->duration = st->duration * av_q2d(st->time_base);

        char layout[128];
        if (av_channel_layout_describe(&par->ch_layout, layout, sizeof(layout)) >= 0)
            info->channel_layout = av_strdup(layout);
    }
    if (!info->codec) info->codec = av_strdup("");
    if (!info->channel_layout) info->channel_layout = av_strdup("");

    info->replaygain = replaygain_from_tags(info->tags);

    if (!info->filename || !info->container || !info->codec || !info->channel_layout) {
        au_media_info_free(info);
        return NULL;
    }
    return info;
}

AuMediaInfo *media_info_copy(const AuMediaInfo *src) {
    if (!src) return NULL;
    AuMediaInfo *info = av_mallocz(sizeof(AuMediaInfo));
    if (!info) return NULL;
    *info = *src;
    info->tags = NULL;
    info->filename = av_strdup(src->filename);
    info->container = av_strdup(src->container);
    info->codec = av_strdup(src->codec);
    info->channel_layout = av_strdup(src->channel_layout);
    if (av_dict_copy(&info->tags, src->tags, 0) < 0 ||
        !info->filename || !info->container || !info->codec || !info->channel_layout) {
        au_media_info_free(info);
        return NULL;
    }
    return info;
}

//+-----------------------------------------------------+
//| Public API                                          |
//+-----------------------------------------------------+

AuMediaInfo *au_probe(const char *filename, int *out_error) {
    AVFormatContext *ic = NULL;
    AuMediaInfo *info = NULL;
    int ret;

    if (!filename || !filename[0]) {
        if (out_error) *out_error = AU_ERR_INVALID_ARGUMENT;
        return NULL;
    }
    if ((ret = avformat_open_input(&ic, filename, NULL, NULL)) < 0) goto end;
    if ((ret = avformat_find_stream_info(ic, NULL)) < 0) goto end;

    const int st = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (st < 0) { ret = st; goto end; }

    info = media_info_from_context(ic, st, filename);
    ret = info ? 0 : AVERROR(ENOMEM);

end:
    avformat_close_input(&ic);
    if (out_error) *out_error = ret < 0 ? (ret == AVERROR(ENOMEM) ? AU_ERR_NO_MEMORY : AU_ERR_INVALID_ARGUMENT) : AU_OK;
    return info;
}

void au_media_info_free(AuMediaInfo *info) {
    if (!info) return;
    av_free(info->filename);
    av_free(info->container);
    av_free(info->codec);
    av_free(info->channel_layout);
    av_dict_free(&info->tags);
    av_free(info);
}

const char *au_media_filename(const AuMediaInfo *info) { return info ? info->filename : NULL; }
const char *au_media_container(const AuMediaInfo *info) { return info ? info->container : NULL; }
const char *au_media_codec(const AuMediaInfo *info) { return info ? info->codec : NULL; }
const char *au_media_channel_layout(const AuMediaInfo *info) { return info ? info->channel_layout : NULL; }
double au_media_duration(const AuMediaInfo *info) { return info ? info->duration : -1; }
int64_t au_media_bit_rate(const AuMediaInfo *info) { return info ? info->bit_rate : 0; }
int au_media_sample_rate(const AuMediaInfo *info) { return info ? info->sample_rate : 0; }
int au_media_channels(const AuMediaInfo *info) { return info ? info->channels : 0; }
int au_media_bits_per_sample(const AuMediaInfo *info) { return info ? info->bits_per_sample : 0; }

const char *au_media_tag(const AuMediaInfo *info, const char *key) {
    if (!info || !key || !key[0]) return NULL;
    const char *v = dict_get(info->tags, key);
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
            if ((v = dict_get(info->tags, aliases[i][j]))) return v;
    }
    return NULL;
}

int au_media_tag_count(const AuMediaInfo *info) {
    return info ? av_dict_count(info->tags) : 0;
}

bool au_media_tag_at(const AuMediaInfo *info, const int index, const char **key, const char **value) {
    if (!info || index < 0) return false;
    const AVDictionaryEntry *e = NULL;
    for (int i = 0; (e = av_dict_iterate(info->tags, e)); i++) {
        if (i == index) {
            if (key) *key = e->key;
            if (value) *value = e->value;
            return true;
        }
    }
    return false;
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

void au_media_track_number(const AuMediaInfo *info, int *number, int *total) {
    static const char *const totals[] = { "tracktotal", "totaltracks", NULL };
    number_and_total(info, "track", totals, number, total);
}

void au_media_disc_number(const AuMediaInfo *info, int *number, int *total) {
    static const char *const totals[] = { "disctotal", "totaldiscs", NULL };
    number_and_total(info, "disc", totals, number, total);
}

AuReplayGainInfo au_media_replaygain(const AuMediaInfo *info) {
    if (info) return info->replaygain;
    AuReplayGainInfo none;
    memset(&none, 0, sizeof(none));
    return none;
}
