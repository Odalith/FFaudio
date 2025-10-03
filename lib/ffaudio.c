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

#include "ffaudio.h"
#include <signal.h>
#include "globals.h"
#include "cmdutils.h"
#include "packet_queue_utils.h"
#include "clock_utils.h"
#include "frame_queue_utils.h"
#include "decoder_utils.h"
#include "filtergraph.h"

const char program_name[] = "ffaudio";
const int program_birth_year = 2025;

/* Minimum SDL audio buffer size, in samples. */
#define SDL_AUDIO_MIN_BUFFER_SIZE 512
/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30

/* polls for possible required screen refresh at least this often, should be less than 1/fps */
#define REFRESH_RATE 0.01
#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_FRAMES 25

static AudioPlayer *audio_player = NULL;

static void remove_avoptions_n(AVDictionary **a, const AVDictionary *b)
{
    const AVDictionaryEntry *t = NULL;

    while ((t = av_dict_iterate(b, t))) {
        av_dict_set(a, t->key, NULL, AV_DICT_MATCH_CASE);
    }
}

static int check_avoptions_n(const AVDictionary *m)
{
    const AVDictionaryEntry *t = av_dict_iterate(m, NULL);
    if (t) {
        av_log(NULL, AV_LOG_FATAL, "Option %s not found.\n", t->key);
        return AVERROR_OPTION_NOT_FOUND;
    }

    return 0;
}

static void track_state_clear(TrackState *is)
{
    /* XXX: use a special url_shutdown call to abort parse cleanly */
    if (!is->abort_request) {
        is->abort_request = true;
    }

    SDL_WaitThread(is->read_tid, NULL);


    if (is->audio_stream >= 0 && is->audio_stream < is->ic->nb_streams) {
        const AVCodecParameters *codec_parameters = is->ic->streams[is->audio_stream]->codecpar;

        if (codec_parameters->codec_type) {
            decoder_abort(&is->audio_decoder, &is->sampq);
            decoder_destroy(&is->audio_decoder);
            swr_free(&is->swr_ctx);

            //audio_buf is filled with either audio_buf1 or AVFrame-data[0]
        }

        is->ic->streams[is->audio_stream]->discard = AVDISCARD_ALL;

        if (codec_parameters->codec_type == AVMEDIA_TYPE_AUDIO) {
            is->audio_st = NULL;
            is->audio_stream = -1;
        }
    }

    free(is->forced_audio_codec_name);
    is->forced_audio_codec_name = NULL;

    avformat_close_input(&is->ic);

    frame_queue_destroy(&is->sampq);//This frees audio_buf
    is->audio_buf0 = NULL;
    is->audio_buf0_size = 0;

    av_freep(&is->audio_buf1);
    is->audio_buf1_size = 0;

    packet_queue_destroy(&is->audio_queue);

    av_channel_layout_uninit(&is->audio_filter_src.ch_layout);
    av_channel_layout_uninit(&is->channel_layout);

    /* These are free'd at the end of audio_thread()
    avfilter_free(is->in_audio_filter);
    avfilter_free(is->out_audio_filter);
    avfilter_graph_free(&is->agraph);*/

    free((void*)is->filename);
    SDL_DestroyCond(is->continue_read_thread);

    av_free(is);

    is = NULL;

}

static void cleanup_for_next_track(TrackState *is) {
    if (is) {
        track_state_clear(is);
        is = NULL;
    }

    //All of these are NULL
    av_dict_free(&audio_player->swr_opts_n);
    av_dict_free(&audio_player->format_opts_n);
    av_dict_free(&audio_player->codec_opts_n);
    SDL_PauseAudioDevice(audio_player->device_id, 1);
}

static void abort_track() {
    if (audio_player->current_track) {
        audio_player->is_eof_from_skip = true;
        audio_player->current_track->abort_request = true;

        SDL_LockMutex(audio_player->abort_mutex);
        while (audio_player->current_track) {
            SDL_CondWait(audio_player->abort_cond, audio_player->abort_mutex);
        }
        SDL_UnlockMutex(audio_player->abort_mutex);
    }
}

static void wait_for_audio_reconfigure() {
    SDL_LockMutex(audio_player->reconfigure_mutex);
    while (audio_player->reconfigure_audio_device) {
        SDL_CondWait(audio_player->reconfigure_cond, audio_player->reconfigure_mutex);
    }
    SDL_UnlockMutex(audio_player->reconfigure_mutex);
}

static void close_audio_device() {
    if (!audio_player) return;

    if (audio_player->device_id != 0) {
        SDL_CloseAudioDevice(audio_player->device_id);
        audio_player->device_id = 0;
    }

    audio_player->is_audio_device_initialized = false;
}


/* seek in the stream */
static void stream_seek(TrackState *is, int64_t pos, int64_t rel, int by_bytes)
{
    if (!is->seek_req) {
        is->seek_pos = pos;
        is->seek_rel = rel;
        is->seek_flags &= ~AVSEEK_FLAG_BYTE;
        if (by_bytes)
            is->seek_flags |= AVSEEK_FLAG_BYTE;
        is->seek_req = 1;
        SDL_CondSignal(is->continue_read_thread);
    }
}

static int decode_interrupt_cb(void *ctx)
{
    TrackState *is = ctx;
    return is->abort_request;
}

static int stream_has_enough_packets(const AVStream *st, const int stream_id, const PacketQueue *queue) {
    return stream_id < 0 ||
           queue->abort_request ||
           (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
           queue->nb_packets > MIN_FRAMES && (!queue->duration || av_q2d(st->time_base) * queue->duration > 1.0);
}

static int is_realtime(const AVFormatContext *s)
{
    if(   !strcmp(s->iformat->name, "rtp")
       || !strcmp(s->iformat->name, "rtsp")
       || !strcmp(s->iformat->name, "sdp")
    )
        return 1;

    if(s->pb && (   !strncmp(s->url, "rtp:", 4)
                 || !strncmp(s->url, "udp:", 4)
                )
    )
        return 1;
    return 0;
}

static int compare_audio_fmts(const enum AVSampleFormat fmt1, const int64_t channel_count1,
                   const enum AVSampleFormat fmt2, const int64_t channel_count2)
{
    /* If channel count == 1, planar and non-planar formats are the same */
    if (channel_count1 == 1 && channel_count2 == 1)
        return av_get_packed_sample_fmt(fmt1) != av_get_packed_sample_fmt(fmt2);
    else
        return channel_count1 != channel_count2 || fmt1 != fmt2;
}

static int audio_thread(void *arg)
{
    TrackState *is = arg;
    AVFrame *frame = av_frame_alloc();
    Frame *af;
    int last_serial = -1;
    int reconfigure;
    int got_frame = 0;
    AVRational tb;
    int ret = 0;

    if (!frame)
        return AVERROR(ENOMEM);

    do {
        if ((got_frame = decoder_decode_frame(&is->audio_decoder, frame)) < 0)
            goto the_end;

        if (got_frame) {
                tb = (AVRational){1, frame->sample_rate};

                reconfigure =
                    compare_audio_fmts(is->audio_filter_src.fmt, is->audio_filter_src.ch_layout.nb_channels, frame->format, frame->ch_layout.nb_channels)
                    || is->audio_filter_src.freq != frame->sample_rate
                    || is->audio_decoder.pkt_serial != last_serial;
                    av_channel_layout_compare(&is->audio_filter_src.ch_layout, &frame->ch_layout)
                    || is->audio_filter_src.freq != frame->sample_rate
                    || is->audio_decoder.pkt_serial != last_serial;

                if (reconfigure) {
                    char buf1[1024], buf2[1024];
                    av_channel_layout_describe(&is->audio_filter_src.ch_layout, buf1, sizeof(buf1));
                    av_channel_layout_describe(&frame->ch_layout, buf2, sizeof(buf2));
                    av_log(NULL, AV_LOG_DEBUG,
                           "Audio frame changed from rate:%d ch:%d fmt:%s layout:%s serial:%d to rate:%d ch:%d fmt:%s layout:%s serial:%d\n",
                           is->audio_filter_src.freq, is->audio_filter_src.ch_layout.nb_channels, av_get_sample_fmt_name(is->audio_filter_src.fmt), buf1, last_serial,
                           frame->sample_rate, frame->ch_layout.nb_channels, av_get_sample_fmt_name(frame->format), buf2, is->audio_decoder.pkt_serial);

                    is->audio_filter_src.fmt            = frame->format;
                    ret = av_channel_layout_copy(&is->audio_filter_src.ch_layout, &frame->ch_layout);
                    if (ret < 0)
                        goto the_end;
                    is->audio_filter_src.freq           = frame->sample_rate;
                    last_serial                         = is->audio_decoder.pkt_serial;

                    if ((ret = configure_audio_filters(audio_player, is, audio_player->audio_filters, true)) < 0)
                        goto the_end;
                }

            if ((ret = av_buffersrc_add_frame(is->in_audio_filter, frame)) < 0)
                goto the_end;

            while ((ret = av_buffersink_get_frame_flags(is->out_audio_filter, frame, 0)) >= 0) {
                FrameData *fd = frame->opaque_ref ? (FrameData*)frame->opaque_ref->data : NULL;
                tb = av_buffersink_get_time_base(is->out_audio_filter);
                if (!((af = frame_queue_peek_writable(&is->sampq))))
                    goto the_end;

                af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
                af->pos = fd ? fd->pkt_pos : -1;
                af->serial = is->audio_decoder.pkt_serial;
                af->duration = av_q2d((AVRational){frame->nb_samples, frame->sample_rate});

                av_frame_move_ref(af->frame, frame);
                frame_queue_push(&is->sampq);

                if (is->audio_queue.serial != is->audio_decoder.pkt_serial)
                    break;
            }
            if (ret == AVERROR_EOF)
                is->audio_decoder.finished = is->audio_decoder.pkt_serial;
        }
    } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
 the_end:
    avfilter_free(is->in_audio_filter);
    avfilter_free(is->out_audio_filter);
    avfilter_graph_free(&is->agraph);
    is->in_audio_filter = NULL;
    is->out_audio_filter = NULL;
    av_frame_free(&frame);
    return ret;
}

/* open a given stream. Return 0 if OK */
static int stream_component_open(TrackState *is, const int stream_index)
{
    AVFormatContext *ic = is->ic;
    AVCodecContext *avctx;
    const AVCodec *codec;
    const char *forced_codec_name = is->forced_audio_codec_name;
    AVDictionary *opts = NULL;
    int ret = 0;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return -1;

    avctx = avcodec_alloc_context3(NULL);
    if (!avctx)
        return AVERROR(ENOMEM);

    ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
    if (ret < 0)
        goto fail;
    avctx->pkt_timebase = ic->streams[stream_index]->time_base;

    codec = avcodec_find_decoder(avctx->codec_id);

    if (avctx->codec_type != AVMEDIA_TYPE_AUDIO) {
        av_log(NULL, AV_LOG_ERROR, "Unsupported codec type %d!\n", avctx->codec_type);
        goto fail;
    }

    is->last_audio_stream = stream_index;

    if (forced_codec_name)
        codec = avcodec_find_decoder_by_name(forced_codec_name);
    if (!codec) {
        if (forced_codec_name) av_log(NULL, AV_LOG_WARNING,
                                      "No codec could be found with name '%s'\n", forced_codec_name);
        else                   av_log(NULL, AV_LOG_WARNING,
                                      "No decoder could be found for codec %s\n", avcodec_get_name(avctx->codec_id));
        ret = AVERROR(EINVAL);
        goto fail;
    }

    avctx->codec_id = codec->id;
    avctx->lowres = codec->max_lowres;

    if (audio_player->fast)
        avctx->flags2 |= AV_CODEC_FLAG2_FAST;


    if (!av_dict_get(opts, "threads", NULL, 0))
        av_dict_set(&opts, "threads", "auto", 0);

    av_dict_set(&opts, "flags", "+copy_opaque", AV_DICT_MULTIKEY);


    if ((ret = avcodec_open2(avctx, codec, &opts)) < 0) {
        goto fail;
    }
    ret = check_avoptions_n(opts);
    if (ret < 0)
        goto fail;

    is->eof = 0;
    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;


    is->audio_filter_src.freq = avctx->sample_rate;
    ret = av_channel_layout_copy(&is->audio_filter_src.ch_layout, &avctx->ch_layout);
    if (ret < 0) goto fail;
    is->audio_filter_src.fmt = avctx->sample_fmt;

    is->audio_hw_buf_size = ret;
    //is->audio_target = *audio_player->audio_target; 34HSD
    is->audio_buf0_size  = 0;
    is->audio_buf_index = 0;


    is->audio_stream = stream_index;
    is->audio_st = ic->streams[stream_index];

    if ((ret = decoder_init(&is->audio_decoder, avctx, &is->audio_queue, is->continue_read_thread)) < 0) goto fail;

    if (is->ic->iformat->flags & AVFMT_NOTIMESTAMPS) {
        is->audio_decoder.start_pts = is->audio_st->start_time;
        is->audio_decoder.start_pts_tb = is->audio_st->time_base;
    }

    if ((ret = decoder_start(&is->audio_decoder, audio_thread, "audio_decoder", is)) < 0) goto out;

    SDL_PauseAudioDevice(audio_player->device_id, 0);

    goto out;

fail:
    avcodec_free_context(&avctx);
out:
    av_dict_free(&opts);

    return ret;
}


/* this thread gets the stream from the disk or the network */
static int read_thread(void *arg)
{
    TrackState *is = arg;
    AVFormatContext *ic = NULL;
    int err, i, ret;
    int st_index[AVMEDIA_TYPE_NB];
    AVPacket *pkt = NULL;
    int64_t stream_start_time;
    int pkt_in_play_range = 0;
    SDL_mutex *wait_mutex = SDL_CreateMutex();
    int scan_all_pmts_set = 0;
    int64_t pkt_ts;

    if (!wait_mutex) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    memset(st_index, -1, sizeof(st_index));
    is->eof = 0;

    pkt = av_packet_alloc();
    if (!pkt) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate packet.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ic = avformat_alloc_context();
    if (!ic) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ic->interrupt_callback.callback = decode_interrupt_cb;
    ic->interrupt_callback.opaque = is;


    //Adds scan_all_pmts = 1 then removes it
    if (!av_dict_get(audio_player->format_opts_n, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE)) {
        av_dict_set(&audio_player->format_opts_n, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
        scan_all_pmts_set = 1;//This is set
    }

    err = avformat_open_input(&ic, is->filename, is->iformat, &audio_player->format_opts_n);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not open input stream.\n");
        ret = -1;
        goto fail;
    }

    if (scan_all_pmts_set)
        av_dict_set(&audio_player->format_opts_n, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);

    remove_avoptions_n(&audio_player->format_opts_n, audio_player->codec_opts_n);

    ret = check_avoptions_n(audio_player->format_opts_n);
    if (ret < 0)
        goto fail;

    is->ic = ic;

    if (audio_player->genpts)
        ic->flags |= AVFMT_FLAG_GENPTS;

    if (audio_player->find_stream_info) {
        AVDictionary **opts;
        const unsigned int orig_nb_streams = ic->nb_streams;

        err = setup_find_stream_info_opts_n(ic, audio_player->codec_opts_n, &opts);
        if (err < 0) {
            av_log(NULL, AV_LOG_ERROR,
                   "Error setting up avformat_find_stream_info() options\n");
            ret = err;
            goto fail;
        }

        err = avformat_find_stream_info(ic, opts);

        for (i = 0; i < orig_nb_streams; i++)
            av_dict_free(&opts[i]);
        av_freep(&opts);

        if (err < 0) {
            av_log(NULL, AV_LOG_WARNING,
                   "%s: could not find codec parameters\n", is->filename);
            ret = -1;
            goto fail;
        }
    }

    if (ic->pb)
        ic->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use avio_feof() to test for the end

    if (audio_player->seek_by_bytes < 0)
        audio_player->seek_by_bytes = !(ic->iformat->flags & AVFMT_NO_BYTE_SEEK) &&
                        !!(ic->iformat->flags & AVFMT_TS_DISCONT) &&
                        strcmp("ogg", ic->iformat->name) != 0;

    //is->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;


    /* if seeking requested, we execute it */
    if (audio_player->start_time != AV_NOPTS_VALUE) {
        int64_t timestamp = audio_player->start_time;
        /* add the stream start time */
        if (ic->start_time != AV_NOPTS_VALUE)
            timestamp += ic->start_time;
        ret = avformat_seek_file(ic, -1, INT64_MIN, timestamp, INT64_MAX, 0);
        if (ret < 0) {
            av_log(NULL, AV_LOG_WARNING, "%s: could not seek to position %0.3f\n",
                    is->filename, (double)timestamp / AV_TIME_BASE);
        }
        else if (audio_player->reset_start_time) {
            audio_player->start_time = AV_NOPTS_VALUE;
        }
    }

    is->realtime = is_realtime(ic);

    for (i = 0; i < ic->nb_streams; i++) {
        AVStream *st = ic->streams[i];
        enum AVMediaType type = st->codecpar->codec_type;
        st->discard = AVDISCARD_ALL;
        if (type >= 0 && audio_player->wanted_stream_spec[type] && st_index[type] == -1)
            if (avformat_match_stream_specifier(ic, st, audio_player->wanted_stream_spec[type]) > 0)
                st_index[type] = i;
    }
    for (i = 0; i < AVMEDIA_TYPE_NB; i++) {
        if (audio_player->wanted_stream_spec[i] && st_index[i] == -1) {
            av_log(NULL, AV_LOG_ERROR, "Stream specifier %s does not match any %s stream\n", audio_player->wanted_stream_spec[i], av_get_media_type_string(i));
            st_index[i] = INT_MAX;
        }
    }

        st_index[AVMEDIA_TYPE_AUDIO] =
            av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO,
                                st_index[AVMEDIA_TYPE_AUDIO],
                                st_index[AVMEDIA_TYPE_VIDEO],
                                NULL, 0);

    /* open the streams */
    if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
        stream_component_open(is, st_index[AVMEDIA_TYPE_AUDIO]);
    }

    ret = -1;


    if (is->audio_stream < 0) {
        av_log(NULL, AV_LOG_FATAL, "Failed to open file '%s' or configure filtergraph\n",
               is->filename);
        ret = -1;
        goto fail;
    }

    if (audio_player->infinite_buffer < 0 && is->realtime)
        audio_player->infinite_buffer = 1;

    // Once per file setup done, loop
    for (;;) {
        if (is->abort_request)
            break;
        if (is->paused != is->last_paused) {
            is->last_paused = is->paused;
            if (is->paused)
                is->read_pause_return = av_read_pause(ic);
            else
                av_read_play(ic);
        }
#if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
        if (is->paused &&
                (!strcmp(ic->iformat->name, "rtsp") ||
                 (ic->pb && !strncmp(input_filename, "mmsh:", 5)))) {
            /* wait 10 ms to avoid trying to get another packet */
            /* XXX: horrible */
            SDL_Delay(10);
            continue;
        }
#endif
        if (is->seek_req) {
            int64_t seek_target = is->seek_pos;
            int64_t seek_min    = is->seek_rel > 0 ? seek_target - is->seek_rel + 2: INT64_MIN;
            int64_t seek_max    = is->seek_rel < 0 ? seek_target - is->seek_rel - 2: INT64_MAX;
// FIXME the +-2 is due to rounding being not done in the correct direction in generation
//      of the seek_pos/seek_rel variables

            ret = avformat_seek_file(is->ic, -1, seek_min, seek_target, seek_max, is->seek_flags);

            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR,
                       "%s: error while seeking\n", is->ic->url);
            } else {
                if (is->audio_stream >= 0)
                    packet_queue_flush(&is->audio_queue);
                if (is->seek_flags & AVSEEK_FLAG_BYTE) {
                   set_clock(&is->extclk, NAN, 0);
                } else {
                   set_clock(&is->extclk, seek_target / (double)AV_TIME_BASE, 0);
                }

                if (audio_player->notify_of_restart_callback) {
                    audio_player->notify_of_restart_callback();
                }
            }
            is->seek_req = 0;
            is->eof = 0;
            /*if (is->paused)
                step_to_next_frame(is);*/
        }

        /* if the queue are full, no need to read more */
        if (audio_player->infinite_buffer < 1 &&
              (is->audio_queue.size > MAX_QUEUE_SIZE
            || (stream_has_enough_packets(is->audio_st, is->audio_stream, &is->audio_queue)))) {
            /* wait 10 ms */
            SDL_LockMutex(wait_mutex);
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
            continue;
        }
        if (!is->paused &&
            (!is->audio_st || (is->audio_decoder.finished == is->audio_queue.serial && frame_queue_nb_remaining(&is->sampq) == 0))) {
            if (audio_player->loop != 0) {

                if (audio_player->loop >= 1) {
                    audio_player->loop--;
                }
                stream_seek(is, audio_player->start_time != AV_NOPTS_VALUE ? audio_player->start_time : 0, 0, 0);
            }
            else {
                ret = AVERROR_EOF;
                goto fail;
            }
        }
        ret = av_read_frame(ic, pkt);
        if (ret < 0) {
            if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !is->eof) {
                if (is->audio_stream >= 0)
                    packet_queue_put_nullpacket(&is->audio_queue, pkt, is->audio_stream);
                is->eof = 1;
                // This is first when ffmpeg is done reading packets, may be useful to prepare the next song
            }
            if (ic->pb && ic->pb->error) {
                goto fail;
            }
            SDL_LockMutex(wait_mutex);
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
            continue;
        } else {
            is->eof = 0;
        }
        /* check if packet is in play range specified by user, then queue, otherwise discard */
        stream_start_time = ic->streams[pkt->stream_index]->start_time;
        pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
        pkt_in_play_range =
            audio_player->play_duration == AV_NOPTS_VALUE
        || (pkt_ts - (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0))
            * av_q2d(ic->streams[pkt->stream_index]->time_base)
            - (double)(audio_player->start_time != AV_NOPTS_VALUE ? audio_player->start_time : 0) / 1000000
            <= ((double)audio_player->play_duration / 1000000);

        if (pkt->stream_index == is->audio_stream && pkt_in_play_range) {
            packet_queue_put(&is->audio_queue, pkt);
        } else {
            av_packet_unref(pkt);
        }
    }

    ret = 0;
 fail:
    if (ic && !is->ic) {
        avformat_close_input(&ic);
    }

    av_packet_free(&pkt);
    /*if (ret != 0) {
        SDL_Event event;
        SDL_memset(&event, 0, sizeof(event));

        event.type = audio_player->eof_event;
        event.user.data1 = is;
        SDL_PushEvent(&event);
    }*/
    SDL_DestroyMutex(wait_mutex);


    SDL_Event event;
    SDL_memset(&event, 0, sizeof(event));

    event.type = audio_player->eof_event;
    event.user.data1 = is;
    SDL_PushEvent(&event);


    return 0;
}

static TrackState *stream_open(const char *filename)
{
    TrackState *is = av_mallocz(sizeof(TrackState));
    if (!is)
        return NULL;
    is->last_audio_stream = is->audio_stream = -1;
    is->filename = av_strdup(filename);
    if (!is->filename)
        goto fail;
    is->iformat = av_find_input_format(filename);
    is->forced_audio_codec_name = NULL;

    if (frame_queue_init(&is->sampq, &is->audio_queue, SAMPLE_QUEUE_SIZE, 1) < 0)
        goto fail;

    if (packet_queue_init(&is->audio_queue) < 0)
        goto fail;

    if (!((is->continue_read_thread = SDL_CreateCond()))) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        goto fail;
    }

    init_clock(&is->audclk, &is->audio_queue.serial);
    init_clock(&is->extclk, &is->extclk.serial);
    is->audio_clock_serial = -1;

    audio_player->startup_volume = av_clip(audio_player->startup_volume, 0, 100);
    audio_player->sdl_volume = av_clip(SDL_MIX_MAXVOLUME * audio_player->startup_volume / 100, 0, SDL_MIX_MAXVOLUME);
    is->audio_volume = audio_player->sdl_volume;
    is->muted = 0;
    is->av_sync_type = AV_SYNC_AUDIO_MASTER;
    is->read_tid = SDL_CreateThread(read_thread, "read_thread", is);

    if (!is->read_tid) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateThread(): %s\n", SDL_GetError());
fail:
        /*SDL_Event event;
        SDL_memset(&event, 0, sizeof(event));

        event.type = audio_player->eof_event;
        event.user.data1 = is;
        SDL_PushEvent(&event); Todo send event here?*/
        return NULL;
    }
    return is;
}

/**
 * Decode one audio frame and return its uncompressed size.
 *
 * The processed audio frame is decoded, converted if required, and
 * stored in is->audio_buf, with size in bytes given by the return
 * value.
 */
static int audio_decode_frame(TrackState *is)
{
    int resampled_data_size;
    av_unused double audio_clock0;
    Frame *frame;

    if (is->paused)
        return -1;

    do {
#if defined(_WIN32)
        while (frame_queue_nb_remaining(&is->sampq) == 0) {
            if ((av_gettime_relative() - audio_callback_time) > 1000000LL * is->audio_hw_buf_size / audio_target.bytes_per_sec / 2)
                return -1;
            av_usleep (1000);
        }
#endif
        if (!((frame = frame_queue_peek_readable(&is->sampq))))
            return -1;
        frame_queue_next(&is->sampq);
    } while (frame->serial != is->audio_queue.serial);

    const int data_size = av_samples_get_buffer_size(NULL, frame->frame->ch_layout.nb_channels,
                                               frame->frame->nb_samples,
                                               frame->frame->format, 1);

    const int wanted_nb_samples = frame->frame->nb_samples;

    if (frame->frame->format != audio_player->audio_target->fmt
        || av_channel_layout_compare(&frame->frame->ch_layout, &audio_player->audio_target->ch_layout)
        || frame->frame->sample_rate   != audio_player->audio_target->freq
        || (wanted_nb_samples != frame->frame->nb_samples && !is->swr_ctx)) {
        swr_free(&is->swr_ctx);

        const int ret = swr_alloc_set_opts2(&is->swr_ctx,
                                      &audio_player->audio_target->ch_layout, audio_player->audio_target->fmt,
                                      audio_player->audio_target->freq,
                                      &frame->frame->ch_layout, frame->frame->format, frame->frame->sample_rate,
                                      0, NULL);

        if (ret < 0 || swr_init(is->swr_ctx) < 0) {
            av_log(NULL, AV_LOG_ERROR,
                   "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                    frame->frame->sample_rate, av_get_sample_fmt_name(frame->frame->format), frame->frame->ch_layout.nb_channels,
                    audio_player->audio_target->freq, av_get_sample_fmt_name(audio_player->audio_target->fmt), audio_player->audio_target->ch_layout.nb_channels);
            swr_free(&is->swr_ctx);
            return -1;
        }
        if (av_channel_layout_copy(&audio_player->audio_target->ch_layout, &frame->frame->ch_layout) < 0)
            return -1;
        audio_player->audio_target->freq = frame->frame->sample_rate;
        audio_player->audio_target->fmt = frame->frame->format;
    }

    if (is->swr_ctx) {
        const uint8_t **in = (const uint8_t **)frame->frame->extended_data;
        uint8_t **out = &is->audio_buf1;
        const unsigned int out_count = (int64_t)wanted_nb_samples * audio_player->audio_target->freq / frame->frame->sample_rate + 256;
        const int out_size  = av_samples_get_buffer_size(NULL, audio_player->audio_target->ch_layout.nb_channels, out_count, audio_player->audio_target->fmt, 0);
        if (out_size < 0) {
            av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
            return -1;
        }
        if (wanted_nb_samples != frame->frame->nb_samples) {
            if (swr_set_compensation(is->swr_ctx, (wanted_nb_samples - frame->frame->nb_samples) * audio_player->audio_target->freq / frame->frame->sample_rate,
                                        wanted_nb_samples * audio_player->audio_target->freq / frame->frame->sample_rate) < 0) {
                av_log(NULL, AV_LOG_ERROR, "swr_set_compensation() failed\n");
                return -1;
            }
        }
        av_fast_malloc(&is->audio_buf1, &is->audio_buf1_size, out_size);
        if (!is->audio_buf1)
            return AVERROR(ENOMEM);
        const int len2 = swr_convert(is->swr_ctx, out, out_count, in, frame->frame->nb_samples);
        if (len2 < 0) {
            av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
            return -1;
        }
        if (len2 == out_count) {
            av_log(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
            if (swr_init(is->swr_ctx) < 0)
                swr_free(&is->swr_ctx);
        }
        is->audio_buf0 = is->audio_buf1;
        resampled_data_size = len2 * audio_player->audio_target->ch_layout.nb_channels * av_get_bytes_per_sample(audio_player->audio_target->fmt);
    } else {
        is->audio_buf0 = frame->frame->data[0];
        resampled_data_size = data_size;
    }

    audio_clock0 = is->audio_clock;
    /* update the audio clock with the pts */
    if (!isnan(frame->pts))
        is->audio_clock = frame->pts + (double) frame->frame->nb_samples / frame->frame->sample_rate;
    else
        is->audio_clock = NAN;
    is->audio_clock_serial = frame->serial;
#ifdef DEBUG
    {
        static double last_clock;
        printf("audio: delay=%0.3f clock=%0.3f clock0=%0.3f\n",
               is->audio_clock - last_clock,
               is->audio_clock, audio_clock0);
        last_clock = is->audio_clock;
    }
#endif
    return resampled_data_size;
}

/* prepare a new audio buffer */
static void sdl_audio_callback(void *opaque, Uint8 *stream, int len)
{
    //Global is used here because the opaque * gets de-allocated after first track
    TrackState *is = audio_player->current_track;

    audio_player->audio_callback_time = av_gettime_relative();

    while (len > 0) {
        if (is->audio_buf_index >= is->audio_buf0_size) {
           const int audio_size = audio_decode_frame(is);
           if (audio_size < 0) {
                /* if error, just output silence */
               is->audio_buf0 = NULL;
               is->audio_buf0_size = SDL_AUDIO_MIN_BUFFER_SIZE / audio_player->audio_target->frame_size * audio_player->audio_target->frame_size;
           } else {
               is->audio_buf0_size = audio_size;
           }
           is->audio_buf_index = 0;
        }

        unsigned int len1 = is->audio_buf0_size - is->audio_buf_index;
        if (len1 > len)
            len1 = len;
        if (!is->muted && is->audio_buf0 && is->audio_volume == SDL_MIX_MAXVOLUME)
            memcpy(stream, (uint8_t *)is->audio_buf0 + is->audio_buf_index, len1);
        else {
            memset(stream, audio_player->given_spec.silence, len1);//Write silence into the buffer

            if (!is->muted && is->audio_buf0)
                SDL_MixAudioFormat(stream, (uint8_t *)is->audio_buf0 + is->audio_buf_index, audio_player->given_format, len1, is->audio_volume);
        }
        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
    is->audio_write_buf_size = is->audio_buf0_size - is->audio_buf_index;
    /* Let's assume the audio driver that is used by SDL has two periods. */
    if (!isnan(is->audio_clock)) {
        set_clock_at(&is->audclk, is->audio_clock - (double)(2 * is->audio_hw_buf_size + is->audio_write_buf_size) / audio_player->audio_target->bytes_per_sec, is->audio_clock_serial, audio_player->audio_callback_time / 1000000.0);
        sync_clock_to_slave(&is->extclk, &is->audclk);
    }
}

static Uint32 audio_open(const char* audio_device, const int audio_device_index, const bool use_default)
{
    if (audio_player->is_audio_device_initialized) return audio_player->given_spec.size;

    if (!use_default && (!audio_device || audio_device_index < 0)) {
        av_log(NULL, AV_LOG_ERROR, "Audio device cannot be null or index cannot be negative\n");
    }

    SDL_AudioSpec preferred_spec, spec;


    if (use_default) {
        char *default_device = {0};
        if (SDL_GetDefaultAudioInfo(&default_device, &preferred_spec, false) != 0) {//Todo inform user of chosen audio device? So far it only has "System default"
            av_log(NULL, AV_LOG_ERROR, "Failed to get default preferred audio device spec for %s\n", audio_device);
            free(default_device);
            return -1;
        }
        free(default_device);
    }
    else {
        if (SDL_GetAudioDeviceSpec(audio_device_index, false, &preferred_spec) != 0) {
            av_log(NULL, AV_LOG_ERROR, "Failed to get preferred audio device spec\n");
            return -1;
        }
    }


    AVChannelLayout wanted_channel_layout = {0};
    av_channel_layout_default(&wanted_channel_layout, preferred_spec.channels);

    if (preferred_spec.freq <= 0 || preferred_spec.channels <= 0) {
        av_log(NULL, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
        av_channel_layout_uninit(&wanted_channel_layout);
        return -1;
    }

    preferred_spec.format = audio_player->given_format;
    //preferred_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC)); Todo use this?
    preferred_spec.callback = sdl_audio_callback;
    preferred_spec.userdata = NULL;

    if (!((audio_player->device_id = SDL_OpenAudioDevice(use_default ? NULL : audio_device, false, &preferred_spec, &spec, 0)))) {
        av_log(NULL, AV_LOG_ERROR, "SDL_OpenAudioDevice (%d channels, %d Hz): %s\n", preferred_spec.channels, preferred_spec.freq, SDL_GetError());
        av_channel_layout_uninit(&wanted_channel_layout);
        return -1;
    }


    if (spec.format != audio_player->given_format) {
        av_log(NULL, AV_LOG_ERROR,
               "SDL advised audio format %d is not supported!\n", spec.format);
        av_channel_layout_uninit(&wanted_channel_layout);
        return -1;
    }
    if (spec.channels != preferred_spec.channels) {
        av_channel_layout_uninit(&wanted_channel_layout);
        av_channel_layout_default(&wanted_channel_layout, spec.channels);

        if (wanted_channel_layout.order != AV_CHANNEL_ORDER_NATIVE) {
            av_log(NULL, AV_LOG_ERROR,
                   "SDL advised channel count %d is not supported!\n", spec.channels);
            av_channel_layout_uninit(&wanted_channel_layout);
            return -1;
        }
    }

    audio_player->audio_target->fmt = AV_SAMPLE_FMT_S16;//Todo make this have a switch for sdl formats
    audio_player->audio_target->freq = spec.freq;
    if (av_channel_layout_copy(&audio_player->audio_target->ch_layout, &wanted_channel_layout) < 0) {
        av_channel_layout_uninit(&wanted_channel_layout);
        return -1;
    }
    audio_player->audio_target->frame_size = av_samples_get_buffer_size(NULL, audio_player->audio_target->ch_layout.nb_channels, 1, audio_player->audio_target->fmt, 1);
    audio_player->audio_target->bytes_per_sec = av_samples_get_buffer_size(NULL, audio_player->audio_target->ch_layout.nb_channels, audio_player->audio_target->freq, audio_player->audio_target->fmt, 1);
    if (audio_player->audio_target->bytes_per_sec <= 0 || audio_player->audio_target->frame_size <= 0) {
        av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
        av_channel_layout_uninit(&wanted_channel_layout);
        return -1;
    }

    audio_player->given_spec = spec;
    return audio_player->given_spec.size;
}

void wait_loop() {
    double remaining_time = 0.0;
    while (1) {
        if (remaining_time > 0.0)
            av_usleep((int64_t)(remaining_time * 1000000.0));
        remaining_time = REFRESH_RATE;
    }
}

static bool reconfigure_audio_device(const double orig_pos, const char* orig_file) {
    close_audio_device();

    bool custom_device = audio_player->audio_device_index > -1 && audio_player->audio_device_name != NULL;

    if (custom_device && audio_open(audio_player->audio_device_name, audio_player->audio_device_index, false) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed to open custom audio device %s trying default\n", audio_player->audio_device_name);
        custom_device = false;
    }

    if (!custom_device && audio_open(NULL, -1, true) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed to open default audio device\n");
        return false;
    }

    audio_player->start_time = (int64_t)(orig_pos * 1000000);
    audio_player->reset_start_time = true;
    audio_player->current_track = stream_open(orig_file);

    if (!audio_player->current_track) {
        av_log(NULL, AV_LOG_ERROR, "Failed to initialize TrackState after device reconfiguration\n");
        return false;
    }

    audio_player->is_audio_device_initialized = true;
    audio_player->reconfigure_audio_device = false;
    SDL_LockMutex(audio_player->reconfigure_mutex);
    SDL_CondBroadcast(audio_player->reconfigure_cond);
    SDL_UnlockMutex(audio_player->reconfigure_mutex);

    return true;
}

static int event_thread(void* opaque) {
    SDL_AtomicSet(&audio_player->event_thread_running, true);

    SDL_Event e;
    while (SDL_AtomicGet(&audio_player->event_thread_running)) {
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_AUDIODEVICEADDED && !e.adevice.iscapture) {
                //In the future, forward this event to user for updating device options
            }
            else if (e.type == SDL_AUDIODEVICEREMOVED && !e.adevice.iscapture) {
                if (e.adevice.which == audio_player->device_id) {
                    audio_player->reconfigure_audio_device = true;
                    audio_player->current_track->abort_request = true;
                }
                //In the future, forward this event to user for updating device options
            }
            else if (e.type == audio_player->eof_event && audio_player->current_track) {
                const char *filename = NULL;
                double pos = 0;

                if (audio_player->reconfigure_audio_device) {
                    filename = av_strdup(audio_player->current_track->filename);
                    pos = get_clock(&audio_player->current_track->audclk);

                    if (pos == NAN)
                        pos = 0;
                }

                cleanup_for_next_track(audio_player->current_track);
                audio_player->current_track = NULL;

                if (audio_player->reconfigure_audio_device) {
                    if (!reconfigure_audio_device(pos, filename)) {
                        av_log(NULL, AV_LOG_ERROR, "Failed to reconfigure audio device\n");
                    }
                    av_free((void*)filename);
                    continue;
                }

                if (audio_player->notify_of_eof_callback) {
                    audio_player->notify_of_eof_callback(audio_player->is_eof_from_skip);
                }

                if (audio_player->is_eof_from_skip) {
                    audio_player->is_eof_from_skip = false;
                }

                SDL_LockMutex(audio_player->abort_mutex);
                SDL_CondSignal(audio_player->abort_cond);
                SDL_UnlockMutex(audio_player->abort_mutex);
            }
        }

        // Sleep a bit to avoid busy-waiting
        SDL_Delay(1);
    }

    return 0;
}

static void app_state_init(AudioPlayer *s) {
    if (!s) return;

    // Zero everything first
    memset(s, 0, sizeof(*s));

    // Explicit defaults mirroring previous globals
    s->startup_volume = 100;
    s->sdl_volume = 0;
    s->filter_nbthreads = 0;
    s->audio_callback_time = 0;
    s->audio_filters = NULL;


    s->seek_by_bytes = -1;
    s->start_time = AV_NOPTS_VALUE;
    s->play_duration = AV_NOPTS_VALUE;
    s->loop = 0;
    s->infinite_buffer = -1;
    s->find_stream_info = 1;

    s->current_track = NULL;
    s->format_opts_n = NULL;
    s->codec_opts_n = NULL;
    s->swr_opts_n = NULL;


    SDL_AtomicSet(&s->event_thread_running, false);
    s->event_thread = NULL;
    s->eof_event = SDL_RegisterEvents(1);

    s->abort_mutex = SDL_CreateMutex();
    s->abort_cond = SDL_CreateCond();

    s->request_count = 0;
    s->is_eof_from_skip = false;
    s->is_init_done = false;
    s->is_audio_device_initialized = false;
    s->reconfigure_audio_device = false;
    s->reset_start_time = false;


    s->audio_device_index = -1;
    s->audio_device_name  = NULL;
    s->device_id = (SDL_AudioDeviceID)0;
    s->given_format = AUDIO_S16SYS;
    s->audio_target = (AudioParams *)malloc(sizeof(AudioParams));

    s->fast = 0;
    s->genpts = 0;
}

void shutdown() {
    if (!audio_player) return;

    abort_track();

    close_audio_device();

    if (audio_player->audio_target) {
        free(audio_player->audio_target);
    }

    if (audio_player->audio_device_name) {
        av_free((void*)audio_player->audio_device_name);
    }

    audio_player->is_init_done = false;
    SDL_WaitThread(audio_player->event_thread, NULL);
    SDL_DestroyMutex(audio_player->reconfigure_mutex);
    SDL_DestroyCond(audio_player->reconfigure_cond);
    SDL_DestroyMutex(audio_player->abort_mutex);
    SDL_DestroyCond(audio_player->abort_cond);

    SDL_Quit();
    //Todo avformat_network_deinit();
}

int initialize(const InitializeConfig* config)
{
    if (audio_player || !config) return -1;

    audio_player = (AudioPlayer *)malloc(sizeof(AudioPlayer));
    app_state_init(audio_player);

    if (config->on_error) {
        audio_player->notify_of_error_callback = config->on_error;
    }

    if (config->on_eof) {
        audio_player->notify_of_eof_callback = config->on_eof;
    }

    if (config->on_restart) {
        audio_player->notify_of_restart_callback = config->on_restart;
    }


    init_dynload();

    av_log_set_flags(AV_LOG_SKIP_REPEATED);
    av_log_set_level(AV_LOG_INFO);

    /* register all codecs, demux and protocols */
#if CONFIG_AVDEVICE
    avdevice_register_all();
#endif

    audio_player->startup_volume = config->initial_volume;
    audio_player->loop = config->initial_loop_count;

    /* Try to work around an occasional ALSA buffer underflow issue when the
     * period size is NPOT due to ALSA resampling by forcing the buffer size. */
    if (!SDL_getenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE"))
        SDL_setenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE","1", 1);


    if (config->app_name) {
        SDL_SetHint(SDL_HINT_APP_NAME, config->app_name);
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_NAME, config->app_name);
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_APP_NAME, config->app_name);
    }


    //Todo add these as options to config
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_ROLE, "music");
    SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");
    SDL_SetHint(SDL_HINT_AUDIO_RESAMPLING_MODE, "3");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "0");

    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER | SDL_INIT_EVENTS)) {
        av_log(NULL, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
        SDL_Quit();
        return -1;
    }

    audio_player->eof_event = SDL_RegisterEvents(1);
    if (audio_player->eof_event  == (Uint32)-1) {
        SDL_Log("Failed to register custom events");
        SDL_Quit();
        return -1;
    }

    SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
    SDL_EventState(SDL_USEREVENT, SDL_IGNORE);
    SDL_EventState(SDL_DISPLAYEVENT, SDL_IGNORE);

    // Start the event thread
    audio_player->event_thread = SDL_CreateThread(event_thread, "Event_Thread", NULL);
    if (!audio_player->event_thread) {
        SDL_Log("SDL_CreateThread Error: %s", SDL_GetError());
        SDL_Quit();
        return -1;
    }


    audio_player->is_init_done = true;

    return 0;
}

int configure_audio_device(const AudioDeviceConfig* custom_config) {
    if (!audio_player || !audio_player->is_init_done) return -1;

    wait_for_audio_reconfigure();

    if (audio_player->is_audio_device_initialized) {

        if (custom_config) {

            if (custom_config->audio_device == NULL || custom_config->audio_device_index < 0) {
                return -1;
            }

            if (audio_player->audio_device_index == custom_config->audio_device_index
                || (audio_player->audio_device_name != NULL && strcmp(audio_player->audio_device_name, custom_config->audio_device) == 0)) {
                return -1;
            }

            av_free((void*)audio_player->audio_device_name);
            audio_player->audio_device_index = custom_config->audio_device_index;
            audio_player->audio_device_name = av_strdup(custom_config->audio_device);
        }
        else {
            if (audio_player->audio_device_name == NULL || audio_player->audio_device_index < 0) {
                return -1;
            }

            av_free((void*)audio_player->audio_device_name);
            audio_player->audio_device_index = -1;
            audio_player->audio_device_name = NULL;
        }

        audio_player->reconfigure_audio_device = true;
        audio_player->current_track->abort_request = true;

        return 0;
    }

    if (custom_config) {
        audio_player->audio_device_index = custom_config->audio_device_index;
        audio_player->audio_device_name = av_strdup(custom_config->audio_device);
        if (audio_open(custom_config->audio_device, custom_config->audio_device_index, false) < 0) {
            SDL_Quit();
            return -1;
        }
    }
    else {
        if (audio_open(NULL, -1, true) < 0) {
            SDL_Quit();
            return -1;
        }
    }


    audio_player->is_audio_device_initialized = true;

    return 0;
}

void play_audio(const char *filename, const PlayAudioConfig* config) {
    if (!audio_player || !audio_player->is_init_done) return;

    wait_for_audio_reconfigure();

    abort_track();

    clear_filter_chain(audio_player);

    if (config && config->loudnorm_settings) {
        const char *loudnorm_filter = av_asprintf("loudnorm=%s:linear=true", config->loudnorm_settings);
        add_to_filter_chain(audio_player, loudnorm_filter);
        av_freep(&loudnorm_filter);
    }

    if (config && config->crossfeed_setting) {
        const char *crossfeed_filter = av_asprintf("crossfeed=%s", config->crossfeed_setting);
        add_to_filter_chain(audio_player, crossfeed_filter);
        av_freep(&crossfeed_filter);
    }

    audio_player->current_track = stream_open(filename);

    if (!audio_player->current_track) {
        av_log(NULL, AV_LOG_FATAL, "Failed to initialize TrackState!\n");
        //do_exit(NULL);
    }

    ++audio_player->request_count;
}

void stop_audio() {

    wait_for_audio_reconfigure();

    abort_track();

    ++audio_player->request_count;
}

void pause_audio(const bool value) {

    if (!audio_player->current_track || value == audio_player->current_track->paused) return;

    wait_for_audio_reconfigure();

    if (value) {
        SDL_PauseAudioDevice(audio_player->device_id, 1);
    }
    else if (audio_player->device_id) {
        SDL_PauseAudioDevice(audio_player->device_id, 0);
    }


    set_clock(&audio_player->current_track->extclk, get_clock(&audio_player->current_track->extclk), audio_player->current_track->extclk.serial);
    audio_player->current_track->paused = audio_player->current_track->audclk.paused = audio_player->current_track->extclk.paused = !audio_player->current_track->paused;

    ++audio_player->request_count;
}

void seek_percent(const double percentPos) {
    if (!audio_player) return;

    if (!audio_player->current_track || !audio_player->current_track->ic) return;

    // Get the total duration of the media file
    const int64_t duration = audio_player->current_track->ic->duration;
    if (duration == AV_NOPTS_VALUE) {
        // If duration is unknown, we can't seek by percentage
        return;
    }

    // duration is already in AV_TIME_BASE units (microseconds)
    int64_t target_pos = (int64_t)(percentPos / 100.0 * duration);

    if (target_pos < 0) target_pos = 0;
    if (target_pos > duration) target_pos = duration;

    stream_seek(audio_player->current_track, target_pos, 0, 0);

    ++audio_player->request_count;
}

void seek_time(const int64_t milliseconds) {
    if (!audio_player) return;

    if (!audio_player->current_track || !audio_player->current_track->ic) return;

    int64_t target_pos = milliseconds * 1000;

    if (target_pos < 0) target_pos = 0;

    stream_seek(audio_player->current_track, target_pos, 0, 0);
}

void set_audio_volume(const int volume) {
    if (!audio_player->current_track || volume > 100 || volume < 0 || volume == audio_player->startup_volume) return;

    audio_player->startup_volume = volume;
    audio_player->sdl_volume = av_clip(SDL_MIX_MAXVOLUME * volume / 100, 0, SDL_MIX_MAXVOLUME);
    audio_player->current_track->audio_volume = audio_player->sdl_volume;

    ++audio_player->request_count;
}

int get_audio_volume() {
    if (!audio_player) return -1;

    return audio_player->startup_volume;
}

void mute_audio(const bool value) {
    if (!audio_player) return;

    if (!audio_player->current_track || value == audio_player->current_track->muted) return;

    audio_player->current_track->muted = !audio_player->current_track->muted;

    ++audio_player->request_count;
}

void set_loop_count(const int loop_count) {
    audio_player->loop = loop_count;
}

int get_loop_count() {
    if (!audio_player) return 0;

    return audio_player->loop;
}

double get_audio_play_time() {
    if (!audio_player || !audio_player->current_track) return -1;

    //audio_player->play_duration is for how long a file *should* play how how long it *has* been playing
    return get_clock(&audio_player->current_track->audclk);
}

double get_audio_duration() {
    if (!audio_player) return -1;

    if (!audio_player->current_track || !audio_player->current_track->ic) return -1;

    const int64_t duration = audio_player->current_track->ic->duration;
    if (duration == AV_NOPTS_VALUE) {
        return -1;
    }

    return duration / 1000000.0;
}

int get_audio_devices(int *out_total, char ***out_devices) {
    if (!out_total || !out_devices) {
        return -1;
    }

    const int count = SDL_GetNumAudioDevices(false);
    if (count <= 0) {
        *out_total = 0;
        *out_devices = NULL;
        return (count == 0) ? 0 : -1;
    }

    char **devices = malloc(count * sizeof(char *));
    if (!devices) {
        *out_total = 0;
        *out_devices = NULL;
        return -1;
    }

    int filled = 0;
    for (int i = 0; i < count; i++) {
        const char *name = SDL_GetAudioDeviceName(i, false);
        if (!name) {
            // Clean up on failure
            for (int j = 0; j < filled; j++) {
                free(devices[j]);
            }
            free(devices);
            *out_total = 0;
            *out_devices = NULL;
            return -1;
        }

        devices[i] = strdup(name);

        if (!devices[i]) {
            // Clean up on failure
            for (int j = 0; j < filled; j++) {
                free(devices[j]);
            }
            free(devices);
            *out_total = 0;
            *out_devices = NULL;
            return -1;
        }
        filled++;
    }

    *out_total = count;
    *out_devices = devices;
    return 0;
}

