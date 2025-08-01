/*
* Copyright (c) 2003 Fabrice Bellard, 2025 Odalith
 *
 * This file was part of FFmpeg, particularly cmdutils.
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
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "cmdutils.h"


enum StreamList {
    STREAM_LIST_ALL,
    STREAM_LIST_STREAM_ID,
    STREAM_LIST_PROGRAM,
    STREAM_LIST_GROUP_ID,
    STREAM_LIST_GROUP_IDX,
};

typedef struct StreamSpecifier {
    // trailing stream index - pick idx-th stream that matches
    // all the other constraints; -1 when not present
    int                  idx;

    // which stream list to consider
    enum StreamList      stream_list;

    // STREAM_LIST_STREAM_ID: stream ID
    // STREAM_LIST_GROUP_IDX: group index
    // STREAM_LIST_GROUP_ID:  group ID
    // STREAM_LIST_PROGRAM:   program ID
    int64_t              list_id;

    // when not AVMEDIA_TYPE_UNKNOWN, consider only streams of this type
    enum AVMediaType     media_type;
    uint8_t              no_apic;

    uint8_t              usable_only;

    int                  disposition;

    char                *meta_key;
    char                *meta_val;

    char                *remainder;
} StreamSpecifier;

void init_dynload(void)
{
#if HAVE_SETDLLDIRECTORY && defined(_WIN32)
    /* Calling SetDllDirectory with the empty string (but not NULL) removes the
     * current working directory from the DLL search path as a security pre-caution. */
    SetDllDirectory("");
#endif
}

static int cmdutils_isalnum(char c)
{
    return (c >= '0' && c <= '9') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z');
}

static void stream_specifier_uninit(StreamSpecifier *ss)
{
    av_freep(&ss->meta_key);
    av_freep(&ss->meta_val);
    av_freep(&ss->remainder);

    memset(ss, 0, sizeof(*ss));
}

static int stream_specifier_parse(StreamSpecifier *ss, const char *spec,
                           int allow_remainder, void *logctx)
{
    char *endptr;
    int ret;

    memset(ss, 0, sizeof(*ss));

    ss->idx         = -1;
    ss->media_type  = AVMEDIA_TYPE_UNKNOWN;
    ss->stream_list = STREAM_LIST_ALL;

    av_log(logctx, AV_LOG_TRACE, "Parsing stream specifier: %s\n", spec);

    while (*spec) {
        if (*spec <= '9' && *spec >= '0') { /* opt:index */
            ss->idx = strtol(spec, &endptr, 0);

            av_assert0(endptr > spec);
            spec = endptr;

            av_log(logctx, AV_LOG_TRACE,
                   "Parsed index: %d; remainder: %s\n", ss->idx, spec);

            // this terminates the specifier
            break;
        } else if ((*spec == 'v' || *spec == 'a' || *spec == 's' ||
                    *spec == 'd' || *spec == 't' || *spec == 'V') &&
                   !cmdutils_isalnum(*(spec + 1))) { /* opt:[vasdtV] */
            if (ss->media_type != AVMEDIA_TYPE_UNKNOWN) {
                av_log(logctx, AV_LOG_ERROR, "Stream type specified multiple times\n");
                ret = AVERROR(EINVAL);
                goto fail;
            }

            switch (*spec++) {
            case 'v': ss->media_type = AVMEDIA_TYPE_VIDEO;      break;
            case 'a': ss->media_type = AVMEDIA_TYPE_AUDIO;      break;
            case 's': ss->media_type = AVMEDIA_TYPE_SUBTITLE;   break;
            case 'd': ss->media_type = AVMEDIA_TYPE_DATA;       break;
            case 't': ss->media_type = AVMEDIA_TYPE_ATTACHMENT; break;
            case 'V': ss->media_type = AVMEDIA_TYPE_VIDEO;
                      ss->no_apic    = 1;                       break;
            default:  av_assert0(0);
            }

            av_log(logctx, AV_LOG_TRACE, "Parsed media type: %s; remainder: %s\n",
                   av_get_media_type_string(ss->media_type), spec);
        } else if (*spec == 'g' && *(spec + 1) == ':') {
            if (ss->stream_list != STREAM_LIST_ALL)
                goto multiple_stream_lists;

            spec += 2;
            if (*spec == '#' || (*spec == 'i' && *(spec + 1) == ':')) {
                ss->stream_list = STREAM_LIST_GROUP_ID;

                spec += 1 + (*spec == 'i');
            } else
                ss->stream_list = STREAM_LIST_GROUP_IDX;

            ss->list_id = strtol(spec, &endptr, 0);
            if (spec == endptr) {
                av_log(logctx, AV_LOG_ERROR, "Expected stream group idx/ID, got: %s\n", spec);
                ret = AVERROR(EINVAL);
                goto fail;
            }
            spec = endptr;

            av_log(logctx, AV_LOG_TRACE, "Parsed stream group %s: %"PRId64"; remainder: %s\n",
                   ss->stream_list == STREAM_LIST_GROUP_ID ? "ID" : "index", ss->list_id, spec);
        } else if (*spec == 'p' && *(spec + 1) == ':') {
            if (ss->stream_list != STREAM_LIST_ALL)
                goto multiple_stream_lists;

            ss->stream_list = STREAM_LIST_PROGRAM;

            spec += 2;
            ss->list_id = strtol(spec, &endptr, 0);
            if (spec == endptr) {
                av_log(logctx, AV_LOG_ERROR, "Expected program ID, got: %s\n", spec);
                ret = AVERROR(EINVAL);
                goto fail;
            }
            spec = endptr;

            av_log(logctx, AV_LOG_TRACE,
                   "Parsed program ID: %"PRId64"; remainder: %s\n", ss->list_id, spec);
        } else if (!strncmp(spec, "disp:", 5)) {
            const AVClass *st_class = av_stream_get_class();
            const AVOption       *o = av_opt_find(&st_class, "disposition", NULL, 0, AV_OPT_SEARCH_FAKE_OBJ);
            char *disp = NULL;
            size_t len;

            av_assert0(o);

            if (ss->disposition) {
                av_log(logctx, AV_LOG_ERROR, "Multiple disposition specifiers\n");
                ret = AVERROR(EINVAL);
                goto fail;
            }

            spec += 5;

            for (len = 0; cmdutils_isalnum(spec[len]) ||
                          spec[len] == '_' || spec[len] == '+'; len++)
                continue;

            disp = av_strndup(spec, len);
            if (!disp) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }

            ret = av_opt_eval_flags(&st_class, o, disp, &ss->disposition);
            av_freep(&disp);
            if (ret < 0) {
                av_log(logctx, AV_LOG_ERROR, "Invalid disposition specifier\n");
                goto fail;
            }

            spec += len;

            av_log(logctx, AV_LOG_TRACE,
                   "Parsed disposition: 0x%x; remainder: %s\n", ss->disposition, spec);
        } else if (*spec == '#' ||
                   (*spec == 'i' && *(spec + 1) == ':')) {
            if (ss->stream_list != STREAM_LIST_ALL)
                goto multiple_stream_lists;

            ss->stream_list = STREAM_LIST_STREAM_ID;

            spec += 1 + (*spec == 'i');
            ss->list_id = strtol(spec, &endptr, 0);
            if (spec == endptr) {
                av_log(logctx, AV_LOG_ERROR, "Expected stream ID, got: %s\n", spec);
                ret = AVERROR(EINVAL);
                goto fail;
            }
            spec = endptr;

            av_log(logctx, AV_LOG_TRACE,
                   "Parsed stream ID: %"PRId64"; remainder: %s\n", ss->list_id, spec);

            // this terminates the specifier
            break;
        } else if (*spec == 'm' && *(spec + 1) == ':') {
            av_assert0(!ss->meta_key && !ss->meta_val);

            spec += 2;
            ss->meta_key = av_get_token(&spec, ":");
            if (!ss->meta_key) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }
            if (*spec == ':') {
                spec++;
                ss->meta_val = av_get_token(&spec, ":");
                if (!ss->meta_val) {
                    ret = AVERROR(ENOMEM);
                    goto fail;
                }
            }

            av_log(logctx, AV_LOG_TRACE,
                   "Parsed metadata: %s:%s; remainder: %s", ss->meta_key,
                   ss->meta_val ? ss->meta_val : "<any value>", spec);

            // this terminates the specifier
            break;
        } else if (*spec == 'u' && (*(spec + 1) == '\0' || *(spec + 1) == ':')) {
            ss->usable_only = 1;
            spec++;
            av_log(logctx, AV_LOG_ERROR, "Parsed 'usable only'\n");

            // this terminates the specifier
            break;
        } else
            break;

        if (*spec == ':')
            spec++;
    }

    if (*spec) {
        if (!allow_remainder) {
            av_log(logctx, AV_LOG_ERROR,
                   "Trailing garbage at the end of a stream specifier: %s\n",
                   spec);
            ret = AVERROR(EINVAL);
            goto fail;
        }

        if (*spec == ':')
            spec++;

        ss->remainder = av_strdup(spec);
        if (!ss->remainder) {
            ret = AVERROR(EINVAL);
            goto fail;
        }
    }

    return 0;

multiple_stream_lists:
    av_log(logctx, AV_LOG_ERROR,
           "Cannot combine multiple program/group designators in a "
           "single stream specifier");
    ret = AVERROR(EINVAL);

fail:
    stream_specifier_uninit(ss);
    return ret;
}

static unsigned stream_specifier_match(const StreamSpecifier *ss,
                                const AVFormatContext *s, const AVStream *st,
                                void *logctx)
{
    const AVStreamGroup *g = NULL;
    const AVProgram *p = NULL;
    int start_stream = 0, nb_streams;
    int nb_matched = 0;

    switch (ss->stream_list) {
    case STREAM_LIST_STREAM_ID:
        // <n-th> stream with given ID makes no sense and should be impossible to request
        av_assert0(ss->idx < 0);
        // return early if we know for sure the stream does not match
        if (st->id != ss->list_id)
            return 0;
        start_stream = st->index;
        nb_streams   = st->index + 1;
        break;
    case STREAM_LIST_ALL:
        start_stream = ss->idx >= 0 ? 0 : st->index;
        nb_streams   = st->index + 1;
        break;
    case STREAM_LIST_PROGRAM:
        for (unsigned i = 0; i < s->nb_programs; i++) {
            if (s->programs[i]->id == ss->list_id) {
                p          = s->programs[i];
                break;
            }
        }
        if (!p) {
            av_log(logctx, AV_LOG_WARNING, "No program with ID %"PRId64" exists,"
                   " stream specifier can never match\n", ss->list_id);
            return 0;
        }
        nb_streams = p->nb_stream_indexes;
        break;
    case STREAM_LIST_GROUP_ID:
        for (unsigned i = 0; i < s->nb_stream_groups; i++) {
            if (ss->list_id == s->stream_groups[i]->id) {
                g = s->stream_groups[i];
                break;
            }
        }
        // fall-through
    case STREAM_LIST_GROUP_IDX:
        if (ss->stream_list == STREAM_LIST_GROUP_IDX &&
            ss->list_id >= 0 && ss->list_id < s->nb_stream_groups)
            g = s->stream_groups[ss->list_id];

        if (!g) {
            av_log(logctx, AV_LOG_WARNING, "No stream group with group %s %"
                   PRId64" exists, stream specifier can never match\n",
                   ss->stream_list == STREAM_LIST_GROUP_ID ? "ID" : "index",
                   ss->list_id);
            return 0;
        }
        nb_streams = g->nb_streams;
        break;
    default: av_assert0(0);
    }

    for (int i = start_stream; i < nb_streams; i++) {
        const AVStream *candidate = s->streams[g ? g->streams[i]->index :
                                               p ? p->stream_index[i]   : i];

        if (ss->media_type != AVMEDIA_TYPE_UNKNOWN &&
            (ss->media_type != candidate->codecpar->codec_type ||
             (ss->no_apic && (candidate->disposition & AV_DISPOSITION_ATTACHED_PIC))))
            continue;

        if (ss->meta_key) {
            const AVDictionaryEntry *tag = av_dict_get(candidate->metadata,
                                                       ss->meta_key, NULL, 0);

            if (!tag)
                continue;
            if (ss->meta_val && strcmp(tag->value, ss->meta_val))
                continue;
        }

        if (ss->usable_only) {
            const AVCodecParameters *par = candidate->codecpar;

            switch (par->codec_type) {
            case AVMEDIA_TYPE_AUDIO:
                if (!par->sample_rate || !par->ch_layout.nb_channels ||
                    par->format == AV_SAMPLE_FMT_NONE)
                    continue;
                break;
            case AVMEDIA_TYPE_VIDEO:
                if (!par->width || !par->height || par->format == AV_PIX_FMT_NONE)
                    continue;
                break;
            case AVMEDIA_TYPE_UNKNOWN:
                continue;
            }
        }

        if (ss->disposition &&
            (candidate->disposition & ss->disposition) != ss->disposition)
            continue;

        if (st == candidate)
            return ss->idx < 0 || ss->idx == nb_matched;

        nb_matched++;
    }

    return 0;
}

static int check_stream_specifier(AVFormatContext *s, AVStream *st, const char *spec)
{
    StreamSpecifier ss;
    int ret;

    ret = stream_specifier_parse(&ss, spec, 0, NULL);
    if (ret < 0)
        return ret;

    ret = stream_specifier_match(&ss, s, st, NULL);
    stream_specifier_uninit(&ss);
    return ret;
}

int filter_codec_opts_n(const AVDictionary *opts, enum AVCodecID codec_id,
                      AVFormatContext *s, AVStream *st, const AVCodec *codec,
                      AVDictionary **dst, AVDictionary **opts_used)
{
    AVDictionary    *ret = NULL;
    const AVDictionaryEntry *t = NULL;
    int            flags = s->oformat ? AV_OPT_FLAG_ENCODING_PARAM
                                      : AV_OPT_FLAG_DECODING_PARAM;
    char          prefix = 0;
    const AVClass    *cc = avcodec_get_class();

    switch (st->codecpar->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        prefix  = 'v';
        flags  |= AV_OPT_FLAG_VIDEO_PARAM;
        break;
    case AVMEDIA_TYPE_AUDIO:
        prefix  = 'a';
        flags  |= AV_OPT_FLAG_AUDIO_PARAM;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        prefix  = 's';
        flags  |= AV_OPT_FLAG_SUBTITLE_PARAM;
        break;
    }

    while (t = av_dict_iterate(opts, t)) {
        const AVClass *priv_class;
        char *p = strchr(t->key, ':');
        int used = 0;

        /* check stream specification in opt name */
        if (p) {
            int err = check_stream_specifier(s, st, p + 1);
            if (err < 0) {
                av_dict_free(&ret);
                return err;
            } else if (!err)
                continue;

            *p = 0;
        }

        if (av_opt_find(&cc, t->key, NULL, flags, AV_OPT_SEARCH_FAKE_OBJ) ||
            !codec ||
            ((priv_class = codec->priv_class) &&
             av_opt_find(&priv_class, t->key, NULL, flags,
                         AV_OPT_SEARCH_FAKE_OBJ))) {
            av_dict_set(&ret, t->key, t->value, 0);
            used = 1;
        } else if (t->key[0] == prefix &&
                 av_opt_find(&cc, t->key + 1, NULL, flags,
                             AV_OPT_SEARCH_FAKE_OBJ)) {
            av_dict_set(&ret, t->key + 1, t->value, 0);
            used = 1;
        }

        if (p)
            *p = ':';

        if (used && opts_used)
            av_dict_set(opts_used, t->key, "", 0);
    }

    *dst = ret;
    return 0;
}

int setup_find_stream_info_opts_n(AVFormatContext *s,
                                AVDictionary *local_codec_opts,
                                AVDictionary ***dst)
{
    int ret;
    AVDictionary **opts;

    *dst = NULL;

    if (!s->nb_streams)
        return 0;

    opts = av_calloc(s->nb_streams, sizeof(*opts));
    if (!opts)
        return AVERROR(ENOMEM);

    for (int i = 0; i < s->nb_streams; i++) {
        ret = filter_codec_opts_n(local_codec_opts, s->streams[i]->codecpar->codec_id,
                                s, s->streams[i], NULL, &opts[i], NULL);
        if (ret < 0)
            goto fail;
    }
    *dst = opts;
    return 0;
    fail:
        for (int i = 0; i < s->nb_streams; i++)
            av_dict_free(&opts[i]);
    av_freep(&opts);
    return ret;
}