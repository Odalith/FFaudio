/*
* Copyright (c) 2003 Fabrice Bellard, 2025 Odalith
 *
 * This file was part of FFmpeg, particularly FFplay.
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
 * One TrackState per file. The read thread owns the demuxer and starts the audio (decoder) thread;
 * when the read thread exits it has already stopped the decoder and closed the input, so the control
 * thread only has to join it and free the struct.
 */

#include "track.h"
#include "metadata.h"
#include <math.h>
#include "decoder_utils.h"
#include "filtergraph.h"
#include "frame_queue_utils.h"
#include "packet_queue_utils.h"

#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_FRAMES 25
// A backwards jump of the frame timestamps larger than this, without a seek, means the track looped
#define LOOP_DETECT_SECONDS 0.25

static int audio_thread(void *arg);
static int read_thread(void *arg);

//+-----------------------------------------------------+
//| Helpers                                             |
//+-----------------------------------------------------+

static int decode_interrupt_cb(void *ctx) {
    TrackState *t = ctx;
    return SDL_AtomicGet(&t->abort_request);
}

static int stream_has_enough_packets(const AVStream *st, const int stream_id, PacketQueue *queue) {
    SDL_LockMutex(queue->mutex);
    const int nb_packets = queue->nb_packets;
    const int64_t duration = queue->duration;
    SDL_UnlockMutex(queue->mutex);
    return stream_id < 0 ||
           packet_queue_aborted(queue) ||
           (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
           (nb_packets > MIN_FRAMES && (!duration || av_q2d(st->time_base) * duration > 1.0));
}

static int queue_byte_size(PacketQueue *q) {
    SDL_LockMutex(q->mutex);
    const int size = q->size;
    SDL_UnlockMutex(q->mutex);
    return size;
}

static int is_realtime(const AVFormatContext *s) {
    if (!strcmp(s->iformat->name, "rtp") || !strcmp(s->iformat->name, "rtsp") || !strcmp(s->iformat->name, "sdp"))
        return 1;
    if (s->pb && (!strncmp(s->url, "rtp:", 4) || !strncmp(s->url, "udp:", 4)))
        return 1;
    return 0;
}

static void wait_ms(TrackState *t, int ms) {
    SDL_LockMutex(t->wait_mutex);
    if (!SDL_AtomicGet(&t->abort_request))
        SDL_CondWaitTimeout(t->continue_read_thread, t->wait_mutex, ms);
    SDL_UnlockMutex(t->wait_mutex);
}

double track_start_position(const TrackState *t) {
    return t->start_time != AV_NOPTS_VALUE ? t->start_time / AV_TIME_BASE_DOUBLE : 0.0;
}

double track_end_position(TrackState *t) {
    SDL_LockMutex(t->seek_mutex);
    const int64_t end = t->end_pos_us;
    SDL_UnlockMutex(t->seek_mutex);
    return end == AV_NOPTS_VALUE ? -1 : end / AV_TIME_BASE_DOUBLE;
}

double track_duration(TrackState *t) {
    const double end = track_end_position(t);
    if (end < 0) return -1;
    return FFMAX(0.0, end - track_start_position(t));
}

//+-----------------------------------------------------+
//| Lifetime                                            |
//+-----------------------------------------------------+

TrackState *track_create(AudioPlayer *ap, const int32_t handle, const char *filename, const PlayAudioConfig *config) {
    TrackState *t = av_mallocz(sizeof(TrackState));
    if (!t) return NULL;

    t->ap = ap;
    t->handle = handle;
    t->audio_stream = -1;
    t->start_time = AV_NOPTS_VALUE;
    t->play_duration = AV_NOPTS_VALUE;
    t->end_pos_us = AV_NOPTS_VALUE;
    t->duration_us = AV_NOPTS_VALUE;
    t->pending_seek_percent = -1;
    t->seek_by_bytes = -1;
    t->last_frame_pts = NAN;
    SDL_AtomicSet(&t->eos_serial, -1);
    SDL_AtomicSet(&t->loops_remaining, SDL_AtomicGet(&ap->loop_count));

    if (!(t->filename = av_strdup(filename))) goto fail;
    t->iformat = av_find_input_format(filename);

    if (config) {
        if (config->av_filtergraph && config->av_filtergraph[0] && !(t->filtergraph_str = av_strdup(config->av_filtergraph)))
            goto fail;
        if (config->play_duration > 0)
            t->play_duration = SecondsToMicroseconds(config->play_duration);
        if (config->skip_seconds > 0)
            t->start_time = SecondsToMicroseconds(config->skip_seconds);
        if (config->fade_in) {
            t->fade_in_override = *config->fade_in;
            t->has_fade_in_override = true;
        }
        if (config->replaygain) {
            t->replaygain_override = *config->replaygain;
            t->has_replaygain_override = true;
        }
    }

    if (packet_queue_init(&t->audio_queue) < 0) goto fail;
    if (frame_queue_init(&t->sampq, &t->audio_queue, SAMPLE_QUEUE_SIZE) < 0) goto fail;
    if (!(t->continue_read_thread = SDL_CreateCond())) goto fail;
    if (!(t->wait_mutex = SDL_CreateMutex())) goto fail;
    if (!(t->seek_mutex = SDL_CreateMutex())) goto fail;

    char name[32];
    snprintf(name, sizeof(name), "ffa_read_%d", handle);
    if (!(t->read_tid = SDL_CreateThread(read_thread, name, t))) {
        ff_log(AU_LOG_FATAL, "SDL_CreateThread(): %s", SDL_GetError());
        goto fail;
    }
    return t;

fail:
    track_destroy(t);
    return NULL;
}

void track_destroy(TrackState *t) {
    if (!t) return;

    if (t->read_tid) {
        SDL_AtomicSet(&t->abort_request, 1);
        packet_queue_abort(&t->audio_queue);
        if (t->sampq.mutex) frame_queue_signal(&t->sampq);
        if (t->wait_mutex && t->continue_read_thread) {
            SDL_LockMutex(t->wait_mutex);
            SDL_CondSignal(t->continue_read_thread);
            SDL_UnlockMutex(t->wait_mutex);
        }
        // The read thread stops the decoder thread and closes the input before it returns
        SDL_WaitThread(t->read_tid, NULL);
        t->read_tid = NULL;
    }

    if (t->sampq.mutex) frame_queue_destroy(&t->sampq);
    if (t->audio_queue.mutex) packet_queue_destroy(&t->audio_queue);
    av_channel_layout_uninit(&t->audio_filter_src.ch_layout);
    av_dict_free(&t->format_opts_n);
    SDL_DestroyCond(t->continue_read_thread);
    SDL_DestroyMutex(t->wait_mutex);
    SDL_DestroyMutex(t->seek_mutex);
    au_media_info_free(t->info);
    av_free(t->filename);
    av_free(t->filtergraph_str);
    while (t->filter_cmds) {
        FilterCmd *c = t->filter_cmds;
        t->filter_cmds = c->next;
        av_free(c->target);
        av_free(c->cmd);
        av_free(c->arg);
        av_free(c);
    }
    av_free(t);
}

void track_request_seek(TrackState *t, int64_t pos_us) {
    if (pos_us < 0) pos_us = 0;
    SDL_LockMutex(t->seek_mutex);
    t->seek_pos = pos_us;
    t->seek_req = true;
    t->pending_seek_percent = -1;
    SDL_UnlockMutex(t->seek_mutex);

    SDL_LockMutex(t->wait_mutex);
    SDL_CondSignal(t->continue_read_thread);
    SDL_UnlockMutex(t->wait_mutex);
}

void track_request_seek_percent(TrackState *t, const double percent) {
    // Resolved by the read thread, which knows the duration once the file is open
    SDL_LockMutex(t->seek_mutex);
    t->pending_seek_percent = percent;
    SDL_UnlockMutex(t->seek_mutex);
}

void track_update_gain(TrackState *t) {
    AudioPlayer *ap = t->ap;
    SDL_LockMutex(ap->format_mutex);
    const AuReplayGainConfig cfg = ap->replaygain;
    SDL_UnlockMutex(ap->format_mutex);

    SDL_LockMutex(t->seek_mutex);
    const bool known = t->info || t->has_replaygain_override;
    AuReplayGainInfo rg;
    if (t->has_replaygain_override) rg = t->replaygain_override;
    else rg = au_media_replaygain(t->info);
    SDL_UnlockMutex(t->seek_mutex);

    if (!known) return; // not open yet: the read thread calls this again once the tags are known
    const double db = replaygain_gain_db(&cfg, &rg);
    SDL_AtomicSet(&t->gain_mb, (int)lrint(db * 100.0));
}

AuMediaInfo *track_copy_info(TrackState *t) {
    SDL_LockMutex(t->seek_mutex);
    AuMediaInfo *copy = media_info_copy(t->info);
    SDL_UnlockMutex(t->seek_mutex);
    return copy;
}

int track_set_filters(TrackState *t, const char *filters) {
    char *copy = NULL;
    if (filters && filters[0] && !(copy = av_strdup(filters)))
        return AVERROR(ENOMEM);
    SDL_LockMutex(t->seek_mutex);
    av_free(t->filtergraph_str);
    t->filtergraph_str = copy;
    SDL_UnlockMutex(t->seek_mutex);
    SDL_AtomicSet(&t->user_filters_changed, 1);
    return 0;
}

int track_queue_filter_command(TrackState *t, const char *target, const char *cmd, const char *arg) {
    FilterCmd *c = av_mallocz(sizeof(FilterCmd));
    if (!c) return AVERROR(ENOMEM);
    c->target = av_strdup(target);
    c->cmd = av_strdup(cmd);
    c->arg = arg ? av_strdup(arg) : NULL;
    if (!c->target || !c->cmd || (arg && !c->arg)) {
        av_free(c->target); av_free(c->cmd); av_free(c->arg); av_free(c);
        return AVERROR(ENOMEM);
    }
    SDL_LockMutex(t->seek_mutex);
    FilterCmd **tail = &t->filter_cmds;
    while (*tail) tail = &(*tail)->next;
    *tail = c;
    SDL_UnlockMutex(t->seek_mutex);
    return 0;
}

//+-----------------------------------------------------+
//| Read thread                                         |
//+-----------------------------------------------------+

static int stream_component_open(TrackState *t, const int stream_index) {
    AVFormatContext *ic = t->ic;
    AVDictionary *opts = NULL;
    int ret;

    if (stream_index < 0 || stream_index >= (int)ic->nb_streams)
        return AVERROR(EINVAL);

    AVCodecContext *avctx = avcodec_alloc_context3(NULL);
    if (!avctx)
        return AVERROR(ENOMEM);

    if ((ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar)) < 0)
        goto fail;
    avctx->pkt_timebase = ic->streams[stream_index]->time_base;

    if (avctx->codec_type != AVMEDIA_TYPE_AUDIO) {
        ff_log(AU_LOG_ERROR, "Unsupported codec type %d!", avctx->codec_type);
        ret = AVERROR(EINVAL);
        goto fail;
    }

    const AVCodec *codec = avcodec_find_decoder(avctx->codec_id);
    if (!codec) {
        ff_log(AU_LOG_WARNING, "No decoder could be found for codec %s", avcodec_get_name(avctx->codec_id));
        ret = AVERROR(EINVAL);
        goto fail;
    }
    avctx->codec_id = codec->id;

    av_dict_set(&opts, "threads", "auto", 0);
    av_dict_set(&opts, "flags", "+copy_opaque", AV_DICT_MULTIKEY);

    if ((ret = avcodec_open2(avctx, codec, &opts)) < 0)
        goto fail;

    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;

    t->audio_filter_src.freq = avctx->sample_rate;
    if ((ret = av_channel_layout_copy(&t->audio_filter_src.ch_layout, &avctx->ch_layout)) < 0)
        goto fail;
    t->audio_filter_src.fmt = avctx->sample_fmt;

    t->audio_stream = stream_index;
    t->audio_st = ic->streams[stream_index];

    if ((ret = decoder_init(&t->audio_decoder, avctx, &t->audio_queue, t->continue_read_thread)) < 0)
        goto fail;
    avctx = NULL; // owned by the decoder now

    if (ic->iformat->flags & AVFMT_NOTIMESTAMPS) {
        t->audio_decoder.start_pts = t->audio_st->start_time;
        t->audio_decoder.start_pts_tb = t->audio_st->time_base;
    }

    char name[32];
    snprintf(name, sizeof(name), "ffa_decode_%d", t->handle);
    if ((ret = decoder_start(&t->audio_decoder, audio_thread, name, t)) < 0) {
        decoder_destroy(&t->audio_decoder);
        t->audio_stream = -1;
        t->audio_st = NULL;
        goto out;
    }
    goto out;

fail:
    avcodec_free_context(&avctx);
out:
    av_dict_free(&opts);
    return ret;
}

static int seek_file(TrackState *t, int64_t pos_us) {
    AVFormatContext *ic = t->ic;
    int64_t target = pos_us;
    if (ic->start_time != AV_NOPTS_VALUE)
        target += ic->start_time;
    return avformat_seek_file(ic, -1, INT64_MIN, target, INT64_MAX, 0);
}

// Demuxer reached the end of what should be played. Returns true when the track keeps going (looping).
static bool handle_demux_end(TrackState *t, AVPacket *pkt) {
    int loops = SDL_AtomicGet(&t->loops_remaining);
    if (loops != 0 && !SDL_AtomicGet(&t->had_error)) {
        if (loops > 0)
            SDL_AtomicAdd(&t->loops_remaining, -1);
        // Seek back without flushing the queues so the loop is gapless; the mixer detects the restart
        if (seek_file(t, t->start_time != AV_NOPTS_VALUE ? t->start_time : 0) >= 0)
            return true;
        ff_log(AU_LOG_WARNING, "%s: could not seek back to loop", t->filename);
    }

    if (!t->demux_eof) {
        packet_queue_put_nullpacket(&t->audio_queue, pkt, t->audio_stream);
        t->demux_eof = true;
        ff_post_track_msg(MSG_TRACK_DEMUX_EOF, t->handle, 0);
    }
    return false;
}

static void read_thread_fail(TrackState *t) {
    SDL_AtomicSet(&t->had_error, 1);
    SDL_AtomicSet(&t->failed, 1);
    // Nothing more will ever come out of this track
    SDL_AtomicSet(&t->eos_serial, packet_queue_serial(&t->audio_queue));
    ff_post_track_msg(MSG_TRACK_FAILED, t->handle, 0);
}

static int read_thread(void *arg) {
    TrackState *t = arg;
    AudioPlayer *ap = t->ap;
    AVFormatContext *ic = NULL;
    AVPacket *pkt = NULL;
    int ret;
    bool last_paused = false;

    if (!(pkt = av_packet_alloc())) {
        ff_log(AU_LOG_FATAL, "Could not allocate packet.");
        goto fail;
    }
    if (!(ic = avformat_alloc_context())) {
        ff_log(AU_LOG_FATAL, "Could not allocate context.");
        goto fail;
    }
    ic->interrupt_callback.callback = decode_interrupt_cb;
    ic->interrupt_callback.opaque = t;

    av_dict_set(&t->format_opts_n, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
    ret = avformat_open_input(&ic, t->filename, t->iformat, &t->format_opts_n);
    if (ret < 0) {
        // avformat_open_input frees ic on failure
        ic = NULL;
        if (!SDL_AtomicGet(&t->abort_request))
            ff_log(AU_LOG_ERROR, "Could not open input '%s': %s", t->filename, av_err2str(ret));
        goto fail;
    }
    av_dict_set(&t->format_opts_n, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);
    t->ic = ic;

    if (ap->find_stream_info) {
        ret = avformat_find_stream_info(ic, NULL);
        if (ret < 0) {
            if (!SDL_AtomicGet(&t->abort_request))
                ff_log(AU_LOG_WARNING, "%s: could not find codec parameters", t->filename);
            goto fail;
        }
    }

    if (ic->pb)
        ic->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use avio_feof() to test for the end

    t->seek_by_bytes = !(ic->iformat->flags & AVFMT_NO_BYTE_SEEK) && !!(ic->iformat->flags & AVFMT_TS_DISCONT) &&
                       strcmp("ogg", ic->iformat->name) != 0;
    t->realtime = is_realtime(ic);
    t->infinite_buffer = t->realtime;

    if (t->start_time != AV_NOPTS_VALUE && seek_file(t, t->start_time) < 0)
        ff_log(AU_LOG_WARNING, "%s: could not seek to position %0.3f", t->filename, t->start_time / AV_TIME_BASE_DOUBLE);

    for (unsigned int i = 0; i < ic->nb_streams; i++)
        ic->streams[i]->discard = AVDISCARD_ALL;

    const int st_index = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);

    // Tags and the ReplayGain gain are ready before the decoder starts, so the first sample is already
    // at the right level
    if (st_index >= 0) {
        AuMediaInfo *info = media_info_from_context(ic, st_index, t->filename);
        SDL_LockMutex(t->seek_mutex);
        t->info = info;
        SDL_UnlockMutex(t->seek_mutex);
        track_update_gain(t);
    }

    if (st_index < 0 || stream_component_open(t, st_index) < 0 || t->audio_stream < 0) {
        if (!SDL_AtomicGet(&t->abort_request))
            ff_log(AU_LOG_ERROR, "No playable audio stream in '%s'", t->filename);
        goto fail;
    }

    // Publish the duration and the end position (read by other threads under seek_mutex)
    int64_t end_pos = ic->duration;
    if (t->play_duration != AV_NOPTS_VALUE) {
        const int64_t end = (t->start_time != AV_NOPTS_VALUE ? t->start_time : 0) + t->play_duration;
        end_pos = end_pos == AV_NOPTS_VALUE ? end : FFMIN(end_pos, end);
    }
    SDL_LockMutex(t->seek_mutex);
    t->duration_us = ic->duration;
    t->end_pos_us = end_pos;
    SDL_UnlockMutex(t->seek_mutex);
    SDL_AtomicSet(&t->opened, 1);
    ff_post_track_msg(MSG_TRACK_OPENED, t->handle, 0);

    for (;;) {
        if (SDL_AtomicGet(&t->abort_request))
            break;

        const bool paused = SDL_AtomicGet(&ap->paused) != 0;
        if (paused != last_paused) {
            last_paused = paused;
            if (paused) av_read_pause(ic);
            else av_read_play(ic);
        }

        // Seek requests from the control thread
        SDL_LockMutex(t->seek_mutex);
        if (t->pending_seek_percent >= 0 && t->end_pos_us != AV_NOPTS_VALUE) {
            const double start = track_start_position(t);
            const double len = FFMAX(0.0, t->end_pos_us / AV_TIME_BASE_DOUBLE - start);
            t->seek_pos = SecondsToMicroseconds(start + len * t->pending_seek_percent / 100.0);
            t->seek_req = true;
            t->pending_seek_percent = -1;
        }
        const bool seek_req = t->seek_req;
        const int64_t seek_pos = t->seek_pos;
        t->seek_req = false;
        SDL_UnlockMutex(t->seek_mutex);

        if (seek_req) {
            if (seek_file(t, seek_pos) < 0) {
                ff_log(AU_LOG_ERROR, "%s: error while seeking", t->filename);
            }
            else {
                // New serial: the decoder, the filter graph and the mixer drop everything older
                packet_queue_flush(&t->audio_queue);
                t->demux_eof = false;
                ff_post_track_msg(MSG_TRACK_SEEKED, t->handle, seek_pos / AV_TIME_BASE_DOUBLE);
            }
        }

        /* if the queue is full, no need to read more */
        if (!t->infinite_buffer &&
            (queue_byte_size(&t->audio_queue) > MAX_QUEUE_SIZE ||
             stream_has_enough_packets(t->audio_st, t->audio_stream, &t->audio_queue))) {
            wait_ms(t, 10);
            continue;
        }

        ret = av_read_frame(ic, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF || avio_feof(ic->pb)) {
                if (handle_demux_end(t, pkt))
                    continue;
            }
            else if (ret != AVERROR(EAGAIN) && !SDL_AtomicGet(&t->abort_request)) {
                ff_log(AU_LOG_ERROR, "%s: read error: %s", t->filename, av_err2str(ret));
            }
            if (ic->pb && ic->pb->error && !t->demux_eof) {
                // I/O error: let what is decoded play out, then end the track as an error
                SDL_AtomicSet(&t->had_error, 1);
                handle_demux_end(t, pkt);
            }
            wait_ms(t, 10);
            continue;
        }
        t->demux_eof = false;

        if (pkt->stream_index != t->audio_stream) {
            av_packet_unref(pkt);
            continue;
        }

        // Stop at the end of the requested play range
        if (t->play_duration != AV_NOPTS_VALUE) {
            const AVStream *st = ic->streams[pkt->stream_index];
            const int64_t stream_start = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
            const int64_t pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
            const double pos = (pkt_ts - stream_start) * av_q2d(st->time_base);
            if (pkt_ts != AV_NOPTS_VALUE && pos > t->end_pos_us / AV_TIME_BASE_DOUBLE) {
                av_packet_unref(pkt);
                handle_demux_end(t, pkt);
                if (t->demux_eof) wait_ms(t, 10);
                continue;
            }
        }

        packet_queue_put(&t->audio_queue, pkt);
    }
    goto end;

fail:
    read_thread_fail(t);
end:
    if (t->audio_stream >= 0) {
        decoder_abort(&t->audio_decoder, &t->sampq); // joins the audio thread
        decoder_destroy(&t->audio_decoder);
        t->audio_stream = -1;
        t->audio_st = NULL;
    }
    if (t->ic) {
        avformat_close_input(&t->ic);
    }
    else if (ic) {
        avformat_close_input(&ic);
    }
    av_packet_free(&pkt);
    return 0;
}

//+-----------------------------------------------------+
//| Audio (decoder) thread                              |
//+-----------------------------------------------------+

// Decoded audio kept ready for the mixer, after all filters. Small enough that filter changes are
// heard quickly, large enough to ride out scheduling hiccups.
#define DECODED_BUFFER_SECONDS 0.5

static int queue_frame(TrackState *t, AVFrame *frame, const AVRational tb, const int serial) {
    Frame *af = frame_queue_peek_writable(&t->sampq);
    if (!af) {
        av_frame_unref(frame);
        return AVERROR_EXIT;
    }
    af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
    af->pos = -1;
    af->serial = serial;
    af->duration = av_q2d((AVRational){frame->nb_samples, frame->sample_rate});
    av_frame_move_ref(af->frame, frame);
    frame_queue_push(&t->sampq);
    return 0;
}

// Feed one converted frame (NULL = flush) through the user graph, or straight into the queue.
// Returns AVERROR_EOF when the user graph ended by itself, AVERROR_EXIT when aborted.
static int feed_user(TrackState *t, AVFrame *frame, AVFrame *tmp, const AVRational tb, const int serial) {
    if (!t->user.graph)
        return frame ? queue_frame(t, frame, tb, serial) : 0;

    if (av_buffersrc_add_frame(t->user.src, frame) < 0) {
        if (frame) av_frame_unref(frame);
        return 0;
    }
    int ret;
    while ((ret = av_buffersink_get_frame_flags(t->user.sink, tmp, 0)) >= 0) {
        if ((ret = queue_frame(t, tmp, av_buffersink_get_time_base(t->user.sink), serial)) < 0)
            return ret;
        if (packet_queue_serial(&t->audio_queue) != serial)
            return 0;
    }
    return ret == AVERROR_EOF ? AVERROR_EOF : 0;
}

// Pass everything the convert graph has produced on to the user graph
static int drain_convert(TrackState *t, AVFrame *conv, AVFrame *tmp, const int serial) {
    int ret;
    while ((ret = av_buffersink_get_frame_flags(t->convert.sink, conv, 0)) >= 0) {
        const AVRational tb = av_buffersink_get_time_base(t->convert.sink);
        if ((ret = feed_user(t, conv, tmp, tb, serial)) < 0)
            return ret;
        if (packet_queue_serial(&t->audio_queue) != serial)
            return 0;
    }
    return ret == AVERROR_EOF ? AVERROR_EOF : 0;
}

// (Re)configure the convert graph, waiting for an output device if none is open yet. < 0 = give up.
static int ensure_convert_graph(TrackState *t) {
    AudioPlayer *ap = t->ap;
    for (;;) {
        const int ret = configure_convert_graph(ap, t);
        if (ret != AVERROR(EAGAIN)) {
            if (ret >= 0)
                frame_queue_set_max_samples(&t->sampq, (int64_t)(t->convert_rate * DECODED_BUFFER_SECONDS));
            return ret;
        }
        SDL_LockMutex(ap->format_mutex);
        if (!ap->output_ready && !packet_queue_aborted(&t->audio_queue))
            SDL_CondWaitTimeout(ap->format_cond, ap->format_mutex, 100);
        SDL_UnlockMutex(ap->format_mutex);
        if (packet_queue_aborted(&t->audio_queue))
            return AVERROR_EXIT;
    }
}

// Rebuild the user graph from the current track and global filter strings. On failure the previous
// graph is kept, except on the first build (then the error is returned and the track fails).
static int rebuild_user_graph(TrackState *t, const bool first) {
    AudioPlayer *ap = t->ap;
    char *track_filters = NULL, *global_filters = NULL;
    AVChannelLayout layout = {0};

    SDL_LockMutex(t->seek_mutex);
    if (t->filtergraph_str) track_filters = av_strdup(t->filtergraph_str);
    SDL_UnlockMutex(t->seek_mutex);

    SDL_LockMutex(ap->format_mutex);
    if (ap->global_filters) global_filters = av_strdup(ap->global_filters);
    const int rate = ap->audio_target.freq;
    int ret = av_channel_layout_copy(&layout, &ap->audio_target.ch_layout);
    SDL_UnlockMutex(ap->format_mutex);

    FilterChain fresh = {0};
    if (ret >= 0)
        ret = build_user_graph(&fresh, track_filters, global_filters, rate, &layout, ap->filter_nbthreads);

    if (ret < 0) {
        ff_log(AU_LOG_ERROR, "Invalid filters for '%s' (track: \"%s\", global: \"%s\"): %s%s", t->filename,
               track_filters ? track_filters : "", global_filters ? global_filters : "", av_err2str(ret),
               first ? "" : "; keeping the previous filters");
    }
    else {
        free_filter_chain(&t->user);
        t->user = fresh;
    }

    av_free(track_filters);
    av_free(global_filters);
    av_channel_layout_uninit(&layout);
    return first ? ret : 0;
}

static void run_filter_commands(TrackState *t) {
    SDL_LockMutex(t->seek_mutex);
    FilterCmd *list = t->filter_cmds;
    t->filter_cmds = NULL;
    SDL_UnlockMutex(t->seek_mutex);

    while (list) {
        FilterCmd *c = list;
        list = c->next;
        if (t->user.graph) {
            char response[256] = "";
            const int ret = avfilter_graph_send_command(t->user.graph, c->target, c->cmd, c->arg,
                                                        response, sizeof(response), 0);
            if (ret < 0)
                ff_log(AU_LOG_WARNING, "Filter command '%s %s %s' failed: %s", c->target, c->cmd,
                       c->arg ? c->arg : "", av_err2str(ret));
        }
        av_free(c->target);
        av_free(c->cmd);
        av_free(c->arg);
        av_free(c);
    }
}

static void fail_track(TrackState *t) {
    SDL_AtomicSet(&t->had_error, 1);
    SDL_AtomicSet(&t->failed, 1);
    SDL_AtomicSet(&t->eos_serial, packet_queue_serial(&t->audio_queue));
    ff_post_track_msg(MSG_TRACK_FAILED, t->handle, 0);
}

static int audio_thread(void *arg) {
    TrackState *t = arg;
    AVFrame *frame = av_frame_alloc();
    AVFrame *conv = av_frame_alloc();
    AVFrame *tmp = av_frame_alloc();
    int last_serial = -1;
    int ended_serial = -1;     // serial whose audio has completely been queued
    bool user_built = false;
    int ret = 0;

    if (!frame || !conv || !tmp)
        goto end;

    for (;;) {
        const int got_frame = decoder_decode_frame(&t->audio_decoder, frame);
        if (got_frame < 0)
            break;

        const int serial = t->audio_decoder.pkt_serial;

        if (!got_frame) {
            // Decoder drained: flush both graphs so their buffered tails are played, then mark the end
            if (t->audio_decoder.finished == serial && serial == packet_queue_serial(&t->audio_queue)
                && ended_serial != serial) {
                if (t->convert.graph && last_serial == serial) {
                    if (av_buffersrc_add_frame(t->convert.src, NULL) >= 0 &&
                        drain_convert(t, conv, tmp, serial) == AVERROR_EXIT)
                        break;
                    if (feed_user(t, NULL, tmp, (AVRational){1, t->convert_rate}, serial) == AVERROR_EXIT)
                        break;
                }
                ended_serial = serial;
                SDL_AtomicSet(&t->eos_serial, serial);
            }
            continue;
        }

        if (ended_serial == serial) {
            // The user graph already ended (e.g. an atrim filter): drop the rest until the next seek
            av_frame_unref(frame);
            continue;
        }

        const bool rebuild_convert =
            !t->convert.graph
            || SDL_AtomicGet(&t->filter_reconfigure)
            || serial != last_serial
            || t->audio_filter_src.fmt != frame->format
            || av_channel_layout_compare(&t->audio_filter_src.ch_layout, &frame->ch_layout)
            || t->audio_filter_src.freq != frame->sample_rate;

        if (rebuild_convert) {
            SDL_AtomicSet(&t->filter_reconfigure, 0);
            t->audio_filter_src.fmt = frame->format;
            if ((ret = av_channel_layout_copy(&t->audio_filter_src.ch_layout, &frame->ch_layout)) < 0)
                break;
            t->audio_filter_src.freq = frame->sample_rate;
            last_serial = serial;

            if ((ret = ensure_convert_graph(t)) < 0) {
                if (ret != AVERROR_EXIT) {
                    ff_log(AU_LOG_ERROR, "Could not configure the conversion for '%s': %s", t->filename, av_err2str(ret));
                    fail_track(t);
                }
                break;
            }
            // The user graph restarts with the conversion (seek, device change): stale state is dropped
            SDL_AtomicSet(&t->user_filters_changed, 1);
        }

        if (SDL_AtomicGet(&t->user_filters_changed)) {
            SDL_AtomicSet(&t->user_filters_changed, 0);
            if (rebuild_user_graph(t, !user_built) < 0) {
                fail_track(t);
                break;
            }
            user_built = true;
        }

        run_filter_commands(t);

        if (av_buffersrc_add_frame(t->convert.src, frame) < 0) {
            av_frame_unref(frame);
            continue;
        }
        ret = drain_convert(t, conv, tmp, serial);
        if (ret == AVERROR_EXIT)
            break;
        if (ret == AVERROR_EOF) {
            ended_serial = serial;
            SDL_AtomicSet(&t->eos_serial, serial);
        }
    }

end:
    free_filter_chain(&t->convert);
    free_filter_chain(&t->user);
    av_frame_free(&frame);
    av_frame_free(&conv);
    av_frame_free(&tmp);
    return ret;
}

//+-----------------------------------------------------+
//| Mixer side (SDL callback)                           |
//+-----------------------------------------------------+

bool track_has_audio(TrackState *t) {
    if (t->cur) return true;
    const int serial = packet_queue_serial(&t->audio_queue);
    for (;;) {
        Frame *f = frame_queue_peek_nb(&t->sampq);
        if (!f) return false;
        if (f->serial == serial) return true;
        frame_queue_next(&t->sampq); // stale frame from before a seek
    }
}

int track_pull(TrackState *t, float *dst, const int frames, const int rate, const int channels, bool *eos, bool *looped) {
    int written = 0;
    const int serial = packet_queue_serial(&t->audio_queue);
    *eos = false;
    *looped = false;

    if (t->cur && t->cur->serial != serial) {
        frame_queue_next(&t->sampq);
        t->cur = NULL;
    }

    while (written < frames) {
        if (!t->cur) {
            Frame *f = frame_queue_peek_nb(&t->sampq);
            if (!f) {
                if (SDL_AtomicGet(&t->eos_serial) == serial)
                    *eos = true;
                break; // end of track, or the decoder has not caught up yet (underrun)
            }
            if (f->serial != serial
                || f->frame->sample_rate != rate
                || f->frame->ch_layout.nb_channels != channels
                || f->frame->format != AV_SAMPLE_FMT_FLT) {
                frame_queue_next(&t->sampq); // stale: from before a seek or a device change
                continue;
            }
            if (!isnan(f->pts)) {
                if (t->started && !isnan(t->last_frame_pts) && f->pts < t->last_frame_pts - LOOP_DETECT_SECONDS)
                    *looped = true;
                t->last_frame_pts = f->pts;
                t->position = f->pts;
            }
            t->cur = f;
            t->cur_offset = 0;
        }

        const int available = t->cur->frame->nb_samples - t->cur_offset;
        const int n = FFMIN(frames - written, available);
        memcpy(dst + (size_t)written * channels,
               (const float *)t->cur->frame->data[0] + (size_t)t->cur_offset * channels,
               (size_t)n * channels * sizeof(float));
        written += n;
        t->cur_offset += n;
        t->position += (double)n / rate;

        if (t->cur_offset >= t->cur->frame->nb_samples) {
            frame_queue_next(&t->sampq);
            t->cur = NULL;
        }
    }

    if (written > 0)
        t->started = true;
    return written;
}
