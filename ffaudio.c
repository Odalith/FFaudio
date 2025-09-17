/*
* Copyright (c) 2003 Fabrice Bellard, 2025 Odalith
 *
 * This file was part of FFmpeg, particularly ffplay.
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
 * License along with ffaudio; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "ffaudio.h"

#include <math.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>

#include "libavutil/avstring.h"        // Internal: String utilities
#include "libavutil/channel_layout.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"             // Internal: Memory management utilities
#include "libavutil/dict.h"
#include "libavutil/fifo.h"            // Internal: FIFO buffer implementation
#include "libavutil/samplefmt.h"
#include "libavutil/time.h"            // Internal: Time utilities
#include "libavutil/bprint.h"          // Internal: Binary print utilities
#include "libavformat/avformat.h"
//#include "libavdevice/avdevice.h"
#include <time.h>
#include <bits/time.h>

#include "libswscale/swscale.h"
#include "libavutil/opt.h"
#include "libswresample/swresample.h"
#include "libavcodec/avcodec.h"

#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

#include "libavutil/avassert.h"


const char program_name[] = "ffaudio";
const int program_birth_year = 2025;

#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_FRAMES 25

/* Minimum SDL audio buffer size, in samples. */
#define SDL_AUDIO_MIN_BUFFER_SIZE 512
/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30
/* no AV correction is done if too big error */
#define AV_NOSYNC_THRESHOLD 10.0
/* polls for possible required screen refresh at least this often, should be less than 1/fps */
#define REFRESH_RATE 0.01

#define FF_QUIT_EVENT    (SDL_USEREVENT + 2)

/* options specified by the user */
static const char* wanted_stream_spec[AVMEDIA_TYPE_NB] = {0};
static int seek_by_bytes = -1;
static int startup_volume = 100;
static int sdl_volume = 0;
static int64_t start_time = AV_NOPTS_VALUE;//Todo should be part of TrackState
static int64_t duration = AV_NOPTS_VALUE;//Todo should be part of TrackState
static int loop = 0;//Todo should be part of TrackState
static int infinite_buffer = -1;
static const char *audio_codec_name;
static char *audio_filters = NULL;
static int find_stream_info = 1;
static int filter_nbthreads = 0;
static int64_t audio_callback_time;

static const char *current_file = NULL;

//These will most likely be removed
static int fast = 0;
static int genpts = 0;

static AVDictionary *swr_opts_n;
static AVDictionary *format_opts_n, *codec_opts_n;
static int64_t request_count;
static bool is_init_done = false;

static bool is_audio_device_initialized = false;
static SDL_AudioDeviceID device_id;
static SDL_AudioSpec given_spec;
static struct AudioParams audio_target;
static timer_t audio_device_close_timer;

static int cmp_audio_fmts(enum AVSampleFormat fmt1, int64_t channel_count1,
                   enum AVSampleFormat fmt2, int64_t channel_count2)
{
    /* If channel count == 1, planar and non-planar formats are the same */
    if (channel_count1 == 1 && channel_count2 == 1)
        return av_get_packed_sample_fmt(fmt1) != av_get_packed_sample_fmt(fmt2);
    else
        return channel_count1 != channel_count2 || fmt1 != fmt2;
}

static void remove_avoptions_n(AVDictionary **a, AVDictionary *b)
{
    const AVDictionaryEntry *t = NULL;

    while ((t = av_dict_iterate(b, t))) {
        av_dict_set(a, t->key, NULL, AV_DICT_MATCH_CASE);
    }
}

static int check_avoptions_n(AVDictionary *m)
{
    const AVDictionaryEntry *t = av_dict_iterate(m, NULL);
    if (t) {
        av_log(NULL, AV_LOG_FATAL, "Option %s not found.\n", t->key);
        return AVERROR_OPTION_NOT_FOUND;
    }

    return 0;
}

static int packet_queue_put_private(PacketQueue *q, AVPacket *pkt)
{
    MyAVPacketList pkt1;
    int ret;

    if (q->abort_request)
       return -1;


    pkt1.pkt = pkt;
    pkt1.serial = q->serial;

    ret = av_fifo_write(q->pkt_list, &pkt1, 1);
    if (ret < 0)
        return ret;
    q->nb_packets++;
    q->size += pkt1.pkt->size + sizeof(pkt1);
    q->duration += pkt1.pkt->duration;
    /* XXX: should duplicate packet data in DV case */
    SDL_CondSignal(q->cond);
    return 0;
}

static int packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    AVPacket *pkt1;
    int ret;

    pkt1 = av_packet_alloc();
    if (!pkt1) {
        av_packet_unref(pkt);
        return -1;
    }
    av_packet_move_ref(pkt1, pkt);

    SDL_LockMutex(q->mutex);
    ret = packet_queue_put_private(q, pkt1);
    SDL_UnlockMutex(q->mutex);

    if (ret < 0)
        av_packet_free(&pkt1);

    return ret;
}

static int packet_queue_put_nullpacket(PacketQueue *q, AVPacket *pkt, int stream_index)
{
    pkt->stream_index = stream_index;
    return packet_queue_put(q, pkt);
}

/* packet queue handling */
static int packet_queue_init(PacketQueue *q)
{
    memset(q, 0, sizeof(PacketQueue));
    q->pkt_list = av_fifo_alloc2(1, sizeof(MyAVPacketList), AV_FIFO_FLAG_AUTO_GROW);
    if (!q->pkt_list)
        return AVERROR(ENOMEM);
    q->mutex = SDL_CreateMutex();
    if (!q->mutex) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    q->cond = SDL_CreateCond();
    if (!q->cond) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    q->abort_request = 1;
    return 0;
}

static void packet_queue_flush(PacketQueue *q)
{
    MyAVPacketList pkt1;

    SDL_LockMutex(q->mutex);
    while (av_fifo_read(q->pkt_list, &pkt1, 1) >= 0)
        av_packet_free(&pkt1.pkt);
    q->nb_packets = 0;
    q->size = 0;
    q->duration = 0;
    q->serial++;
    SDL_UnlockMutex(q->mutex);
}

static void packet_queue_destroy(PacketQueue *q)
{
    packet_queue_flush(q);
    av_fifo_freep2(&q->pkt_list);
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->cond);
}

static void packet_queue_abort(PacketQueue *q)
{
    SDL_LockMutex(q->mutex);

    q->abort_request = 1;

    SDL_CondSignal(q->cond);

    SDL_UnlockMutex(q->mutex);
}

static void packet_queue_start(PacketQueue *q)
{
    SDL_LockMutex(q->mutex);
    q->abort_request = 0;
    q->serial++;
    SDL_UnlockMutex(q->mutex);
}

/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial)
{
    MyAVPacketList pkt1;
    int ret;

    SDL_LockMutex(q->mutex);

    for (;;) {
        if (q->abort_request) {
            ret = -1;
            break;
        }

        if (av_fifo_read(q->pkt_list, &pkt1, 1) >= 0) {
            q->nb_packets--;
            q->size -= pkt1.pkt->size + sizeof(pkt1);
            q->duration -= pkt1.pkt->duration;
            av_packet_move_ref(pkt, pkt1.pkt);
            if (serial)
                *serial = pkt1.serial;
            av_packet_free(&pkt1.pkt);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

static int decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, SDL_cond *empty_queue_cond) {
    memset(d, 0, sizeof(Decoder));
    d->pkt = av_packet_alloc();
    if (!d->pkt)
        return AVERROR(ENOMEM);
    d->avctx = avctx;
    d->queue = queue;
    d->empty_queue_cond = empty_queue_cond;
    d->start_pts = AV_NOPTS_VALUE;
    d->pkt_serial = -1;
    return 0;
}

static int decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub) {
    int ret = AVERROR(EAGAIN);

    for (;;) {
        if (d->queue->serial == d->pkt_serial) {
            do {
                if (d->queue->abort_request)
                    return -1;

                if (d->avctx->codec_type == AVMEDIA_TYPE_AUDIO) {
                    ret = avcodec_receive_frame(d->avctx, frame);
                    if (ret >= 0) {
                        AVRational tb = (AVRational){1, frame->sample_rate};
                        if (frame->pts != AV_NOPTS_VALUE)
                            frame->pts = av_rescale_q(frame->pts, d->avctx->pkt_timebase, tb);
                        else if (d->next_pts != AV_NOPTS_VALUE)
                            frame->pts = av_rescale_q(d->next_pts, d->next_pts_tb, tb);
                        if (frame->pts != AV_NOPTS_VALUE) {
                            d->next_pts = frame->pts + frame->nb_samples;
                            d->next_pts_tb = tb;
                        }
                    }
                }

                if (ret == AVERROR_EOF) {
                    d->finished = d->pkt_serial;
                    avcodec_flush_buffers(d->avctx);
                    return 0;
                }
                if (ret >= 0)
                    return 1;
            } while (ret != AVERROR(EAGAIN));
        }

        do {
            if (d->queue->nb_packets == 0)
                SDL_CondSignal(d->empty_queue_cond);
            if (d->packet_pending) {
                d->packet_pending = 0;
            } else {
                int old_serial = d->pkt_serial;
                if (packet_queue_get(d->queue, d->pkt, 1, &d->pkt_serial) < 0)
                    return -1;
                if (old_serial != d->pkt_serial) {
                    avcodec_flush_buffers(d->avctx);
                    d->finished = 0;
                    d->next_pts = d->start_pts;
                    d->next_pts_tb = d->start_pts_tb;
                }
            }
            if (d->queue->serial == d->pkt_serial)
                break;
            av_packet_unref(d->pkt);
        } while (1);

        if (d->avctx->codec_type != AVMEDIA_TYPE_SUBTITLE) {
            if (d->pkt->buf && !d->pkt->opaque_ref) {
                FrameData *fd;

                d->pkt->opaque_ref = av_buffer_allocz(sizeof(*fd));
                if (!d->pkt->opaque_ref)
                    return AVERROR(ENOMEM);
                fd = (FrameData*)d->pkt->opaque_ref->data;
                fd->pkt_pos = d->pkt->pos;
            }

            if (avcodec_send_packet(d->avctx, d->pkt) == AVERROR(EAGAIN)) {
                av_log(d->avctx, AV_LOG_ERROR, "Receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");
                d->packet_pending = 1;
            } else {
                av_packet_unref(d->pkt);
            }
        }
    }
}

static void decoder_destroy(Decoder *d) {
    av_packet_free(&d->pkt);
    avcodec_free_context(&d->avctx);
}

static void frame_queue_unref_item(Frame *vp)
{
    av_frame_unref(vp->frame);
    avsubtitle_free(&vp->sub);
}

static int frame_queue_init(FrameQueue *f, PacketQueue *pktq, int max_size, int keep_last)
{
    int i;
    memset(f, 0, sizeof(FrameQueue));
    if (!(f->mutex = SDL_CreateMutex())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    if (!(f->cond = SDL_CreateCond())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    f->pktq = pktq;
    f->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
    f->keep_last = !!keep_last;
    for (i = 0; i < f->max_size; i++)
        if (!(f->queue[i].frame = av_frame_alloc()))
            return AVERROR(ENOMEM);
    return 0;
}

static void frame_queue_destroy(FrameQueue *f)
{
    int i;
    for (i = 0; i < f->max_size; i++) {
        Frame *vp = &f->queue[i];
        frame_queue_unref_item(vp);
        av_frame_free(&vp->frame);
    }
    SDL_DestroyMutex(f->mutex);
    SDL_DestroyCond(f->cond);
}

static void frame_queue_signal(FrameQueue *f)
{
    SDL_LockMutex(f->mutex);
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

static Frame *frame_queue_peek_writable(FrameQueue *f)
{
    /* wait until we have space to put a new frame */
    SDL_LockMutex(f->mutex);
    while (f->size >= f->max_size &&
           !f->pktq->abort_request) {
        SDL_CondWait(f->cond, f->mutex);
    }
    SDL_UnlockMutex(f->mutex);

    if (f->pktq->abort_request)
        return NULL;

    return &f->queue[f->windex];
}

static Frame *frame_queue_peek_readable(FrameQueue *f)
{
    /* wait until we have a readable a new frame */
    SDL_LockMutex(f->mutex);
    while (f->size - f->rindex_shown <= 0 && !f->pktq->abort_request) {
        SDL_CondWait(f->cond, f->mutex);
    }
    SDL_UnlockMutex(f->mutex);

    if (f->pktq->abort_request)
        return NULL;

    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

static void frame_queue_push(FrameQueue *f)
{
    if (++f->windex == f->max_size)
        f->windex = 0;
    SDL_LockMutex(f->mutex);
    f->size++;
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

static void frame_queue_next(FrameQueue *f)
{
    if (f->keep_last && !f->rindex_shown) {
        f->rindex_shown = 1;
        return;
    }
    frame_queue_unref_item(&f->queue[f->rindex]);
    if (++f->rindex == f->max_size)
        f->rindex = 0;
    SDL_LockMutex(f->mutex);
    f->size--;
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

/* return the number of undisplayed frames in the queue */
static int frame_queue_nb_remaining(FrameQueue *f)
{
    return f->size - f->rindex_shown;
}

/* return last shown position */
static int64_t frame_queue_last_pos(FrameQueue *f)
{
    Frame *fp = &f->queue[f->rindex];
    if (f->rindex_shown && fp->serial == f->pktq->serial)
        return fp->pos;
    else
        return -1;
}

static void decoder_abort(Decoder *d, FrameQueue *fq)
{
    packet_queue_abort(d->queue);
    frame_queue_signal(fq);
    SDL_WaitThread(d->decoder_tid, NULL);
    d->decoder_tid = NULL;
    packet_queue_flush(d->queue);
}

static void stream_component_close(TrackState *is, int stream_index)
{
    const AVFormatContext *ic = is->ic;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return;
    const AVCodecParameters *codec_parameters = ic->streams[stream_index]->codecpar;

    if (codec_parameters->codec_type) {
        decoder_abort(&is->audio_decoder, &is->sampq);
        decoder_destroy(&is->audio_decoder);
        SDL_PauseAudioDevice(device_id, 1);
        swr_free(&is->swr_ctx);
        av_freep(&is->audio_buf1);
        is->audio_buf1_size = 0;
        is->audio_buf = NULL;

        /*if (is->rdft) {
            av_tx_uninit(&is->rdft);
            av_freep(&is->real_data);
            av_freep(&is->rdft_data);
            is->rdft = NULL;
            is->rdft_bits = 0;
        } ~553KD*/
    }

    ic->streams[stream_index]->discard = AVDISCARD_ALL;

    if (codec_parameters->codec_type == AVMEDIA_TYPE_AUDIO) {
        is->audio_st = NULL;
        is->audio_stream = -1;
    }
}

static void stream_close(TrackState *is)
{
    /* XXX: use a special url_shutdown call to abort parse cleanly */
    is->abort_request = 1;
    SDL_WaitThread(is->read_tid, NULL);

    /* close each stream */
    if (is->audio_stream >= 0)
        stream_component_close(is, is->audio_stream);

    avformat_close_input(&is->ic);

    packet_queue_destroy(&is->audio_queue);
    avfilter_graph_free(&is->agraph);
    av_channel_layout_uninit(&is->channel_layout);

    /* free all pictures */
    frame_queue_destroy(&is->sampq);
    SDL_DestroyCond(is->continue_read_thread);
    av_free((void*)is->filename);

    av_free(is);
}

static void clean_video_state(TrackState *is) {
    if (is) {
        stream_close(is);
    }

    av_dict_free(&swr_opts_n);
    av_dict_free(&format_opts_n);
    av_dict_free(&codec_opts_n);
    av_freep(&codec_opts_n);
    av_freep(&audio_codec_name);
    free(current_file);
}

static void do_exit(TrackState *is)
{


    /*for (int i = 0; i < nb_vfilters; i++)
    av_freep(&vfilters_list[i]);
av_freep(&vfilters_list);*/

    clean_video_state(is);
    current_track = NULL;
    //avformat_network_deinit();
    SDL_Quit();
    av_log(NULL, AV_LOG_QUIET, "%s", "");
    exit(0);
}

static void sigterm_handler(int sig)
{
    exit(123);
}

static double get_clock(Clock *c)
{
    if (*c->queue_serial != c->serial)
        return NAN;
    if (c->paused) {
        return c->pts;
    } else {
        double time = av_gettime_relative() / 1000000.0;
        return c->pts_drift + time - (time - c->last_updated) * (1.0 - c->speed);
    }
}

static void set_clock_at(Clock *c, double pts, int serial, double time)
{
    c->pts = pts;
    c->last_updated = time;
    c->pts_drift = c->pts - time;
    c->serial = serial;
}

static void set_clock(Clock *c, double pts, int serial)
{
    double time = av_gettime_relative() / 1000000.0;
    set_clock_at(c, pts, serial, time);
}

static void set_clock_speed(Clock *c, double speed)
{
    set_clock(c, get_clock(c), c->serial);
    c->speed = speed;
}

static void init_clock(Clock *c, int *queue_serial)
{
    c->speed = 1.0;
    c->paused = 0;
    c->queue_serial = queue_serial;
    set_clock(c, NAN, -1);
}

static void sync_clock_to_slave(Clock *c, Clock *slave)
{
    double clock = get_clock(c);
    double slave_clock = get_clock(slave);
    if (!isnan(slave_clock) && (isnan(clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD))
        set_clock(c, slave_clock, slave->serial);
}

static int get_master_sync_type(const TrackState *is) {
    if (is->av_sync_type == AV_SYNC_AUDIO_MASTER) {
        if (is->audio_st)
            return AV_SYNC_AUDIO_MASTER;
        else
            av_log(NULL, AV_LOG_ERROR, "audio_st was not valid!");
            return AV_SYNC_AUDIO_MASTER;
            //return AV_SYNC_EXTERNAL_CLOCK;
    } else {
        av_log(NULL, AV_LOG_DEBUG, "Attempted to use AV_SYNC_EXTERNAL_CLOCK, this is not enabled");
        return AV_SYNC_AUDIO_MASTER;
        //return AV_SYNC_EXTERNAL_CLOCK;
    }
}

/* get the current master clock value */
static double get_master_clock(TrackState *is)
{
    double val;

    switch (get_master_sync_type(is)) {
        case AV_SYNC_AUDIO_MASTER:
            val = get_clock(&is->audclk);
            break;
        default:
            //val = get_clock(&is->extclk);
            break;
    }
    return val;
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


static int configure_filtergraph(AVFilterGraph *graph, const char *filtergraph,
                                 AVFilterContext *source_ctx, AVFilterContext *sink_ctx)
{
    int ret;
    const int nb_filters = graph->nb_filters;


    if (filtergraph) {
        AVFilterInOut *outputs = avfilter_inout_alloc();
        AVFilterInOut *inputs  = avfilter_inout_alloc();
        if (!outputs || !inputs) {
            ret = AVERROR(ENOMEM);
            return ret;
        }

        outputs->name       = av_strdup("in");
        outputs->filter_ctx = source_ctx;
        outputs->pad_idx    = 0;
        outputs->next       = NULL;

        inputs->name        = av_strdup("out");
        inputs->filter_ctx  = sink_ctx;
        inputs->pad_idx     = 0;
        inputs->next        = NULL;

        ret = avfilter_graph_parse_ptr(graph, filtergraph, &inputs, &outputs, NULL);
        avfilter_inout_free(&outputs);
        avfilter_inout_free(&inputs);

        if (ret < 0) {
            return ret;
        }

    }
    else if ((ret = avfilter_link(source_ctx, 0, sink_ctx, 0)) < 0) {
        return ret;
    }

    /* Reorder the filters to ensure that inputs of the custom filters are merged first */
    for (int i = 0; i < graph->nb_filters - nb_filters; i++) {
        FFSWAP(AVFilterContext*, graph->filters[i], graph->filters[i + nb_filters]);
    }

    ret = avfilter_graph_config(graph, NULL);

    return ret;
}

static int configure_audio_filters(TrackState *is, const char *afilters, int force_output_format)
{
    static const enum AVSampleFormat sample_fmts[] = { AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_NONE };
    int sample_rates[2] = { 0, -1 };
    AVFilterContext *audio_source_filter = NULL, *audio_sink_filter = NULL;
    char aresample_swr_opts[512] = "";
    const AVDictionaryEntry *e = NULL;
    AVBPrint bp;
    char asrc_args[256];
    int ret;

    avfilter_graph_free(&is->agraph);
    if (!(is->agraph = avfilter_graph_alloc()))
        return AVERROR(ENOMEM);
    is->agraph->nb_threads = filter_nbthreads;

    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_AUTOMATIC);

    //Todo swr_opts was empty so swr_opts_n is left null but this should probably be implemented at some point
    while ((e = av_dict_iterate(swr_opts_n, e))) {
        av_strlcatf(aresample_swr_opts, sizeof(aresample_swr_opts), "%s=%s:", e->key, e->value);
    }

    if (strlen(aresample_swr_opts)) {
        aresample_swr_opts[strlen(aresample_swr_opts)-1] = '\0';
    }
    av_opt_set(is->agraph, "aresample_swr_opts", aresample_swr_opts, 0);

    av_channel_layout_describe_bprint(&is->audio_filter_src.ch_layout, &bp);

    ret = snprintf(asrc_args, sizeof(asrc_args),
                   "sample_rate=%d:sample_fmt=%s:time_base=%d/%d:channel_layout=%s",
                   is->audio_filter_src.freq, av_get_sample_fmt_name(is->audio_filter_src.fmt),
                   1, is->audio_filter_src.freq, bp.str);

    ret = avfilter_graph_create_filter(&audio_source_filter,
                                       avfilter_get_by_name("abuffer"), "ffaudio_abuffer",
                                       asrc_args, NULL, is->agraph);
    if (ret < 0) goto end;


    ret = avfilter_graph_create_filter(&audio_sink_filter,
                                       avfilter_get_by_name("abuffersink"), "ffaudio_abuffersink",
                                       NULL, NULL, is->agraph);
    if (ret < 0) goto end;

    if ((ret = av_opt_set_int_list(audio_sink_filter, "sample_fmts", sample_fmts,  AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0) goto end;
    if ((ret = av_opt_set_int(audio_sink_filter, "all_channel_counts", 1, AV_OPT_SEARCH_CHILDREN)) < 0) goto end;

    if (force_output_format) {
        av_bprint_clear(&bp);
        av_channel_layout_describe_bprint(&audio_target.ch_layout, &bp);//Todo audio_target is empty here
        sample_rates[0] = audio_target.freq;
        if ((ret = av_opt_set_int(audio_sink_filter, "all_channel_counts", 0, AV_OPT_SEARCH_CHILDREN)) < 0) goto end;
        if ((ret = av_opt_set(audio_sink_filter, "ch_layouts", bp.str, AV_OPT_SEARCH_CHILDREN)) < 0) goto end;
        if ((ret = av_opt_set_int_list(audio_sink_filter, "sample_rates", sample_rates, -1, AV_OPT_SEARCH_CHILDREN)) < 0) goto end;
    }

    if ((ret = configure_filtergraph(is->agraph, afilters, audio_source_filter, audio_sink_filter)) < 0) goto end;

    is->in_audio_filter  = audio_source_filter;
    is->out_audio_filter = audio_sink_filter;

end:
    if (ret < 0) {
        avfilter_graph_free(&is->agraph);
    }
    av_bprint_finalize(&bp, NULL);

    return ret;
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
        if ((got_frame = decoder_decode_frame(&is->audio_decoder, frame, NULL)) < 0)
            goto the_end;

        if (got_frame) {
                tb = (AVRational){1, frame->sample_rate};

                reconfigure =
                    cmp_audio_fmts(is->audio_filter_src.fmt, is->audio_filter_src.ch_layout.nb_channels, frame->format, frame->ch_layout.nb_channels)
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

                    if ((ret = configure_audio_filters(is, audio_filters, true)) < 0)
                        goto the_end;
                }

            if ((ret = av_buffersrc_add_frame(is->in_audio_filter, frame)) < 0)
                goto the_end;

            while ((ret = av_buffersink_get_frame_flags(is->out_audio_filter, frame, 0)) >= 0) {
                FrameData *fd = frame->opaque_ref ? (FrameData*)frame->opaque_ref->data : NULL;
                tb = av_buffersink_get_time_base(is->out_audio_filter);
                if (!(af = frame_queue_peek_writable(&is->sampq)))
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
    avfilter_graph_free(&is->agraph);
    av_frame_free(&frame);
    return ret;
}

static int decoder_start(Decoder *d, int (*fn)(void *), const char *thread_name, void* arg)
{
    packet_queue_start(d->queue);
    d->decoder_tid = SDL_CreateThread(fn, thread_name, arg);
    if (!d->decoder_tid) {
        av_log(NULL, AV_LOG_ERROR, "SDL_CreateThread(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    return 0;
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
    int data_size, resampled_data_size;
    av_unused double audio_clock0;
    int wanted_nb_samples;
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
        if (!(frame = frame_queue_peek_readable(&is->sampq)))
            return -1;
        frame_queue_next(&is->sampq);
    } while (frame->serial != is->audio_queue.serial);

    data_size = av_samples_get_buffer_size(NULL, frame->frame->ch_layout.nb_channels,
                                           frame->frame->nb_samples,
                                           frame->frame->format, 1);

    wanted_nb_samples = frame->frame->nb_samples;

    if (frame->frame->format        != is->audio_target.fmt            ||
        av_channel_layout_compare(&frame->frame->ch_layout, &is->audio_target.ch_layout) ||
        frame->frame->sample_rate   != is->audio_target.freq           ||
        (wanted_nb_samples       != frame->frame->nb_samples && !is->swr_ctx)) {
        int ret;
        swr_free(&is->swr_ctx);
        ret = swr_alloc_set_opts2(&is->swr_ctx,
                            &audio_target.ch_layout, audio_target.fmt, audio_target.freq,
                            &frame->frame->ch_layout, frame->frame->format, frame->frame->sample_rate,
                            0, NULL);
        if (ret < 0 || swr_init(is->swr_ctx) < 0) {
            av_log(NULL, AV_LOG_ERROR,
                   "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                    frame->frame->sample_rate, av_get_sample_fmt_name(frame->frame->format), frame->frame->ch_layout.nb_channels,
                    audio_target.freq, av_get_sample_fmt_name(audio_target.fmt), audio_target.ch_layout.nb_channels);
            swr_free(&is->swr_ctx);
            return -1;
        }
        if (av_channel_layout_copy(&is->audio_target.ch_layout, &frame->frame->ch_layout) < 0)
            return -1;
        is->audio_target.freq = frame->frame->sample_rate;
        is->audio_target.fmt = frame->frame->format;
    }

    if (is->swr_ctx) {
        const uint8_t **in = (const uint8_t **)frame->frame->extended_data;
        uint8_t **out = &is->audio_buf1;
        int out_count = (int64_t)wanted_nb_samples * audio_target.freq / frame->frame->sample_rate + 256;
        int out_size  = av_samples_get_buffer_size(NULL, audio_target.ch_layout.nb_channels, out_count, audio_target.fmt, 0);
        int len2;
        if (out_size < 0) {
            av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
            return -1;
        }
        if (wanted_nb_samples != frame->frame->nb_samples) {
            if (swr_set_compensation(is->swr_ctx, (wanted_nb_samples - frame->frame->nb_samples) * audio_target.freq / frame->frame->sample_rate,
                                        wanted_nb_samples * audio_target.freq / frame->frame->sample_rate) < 0) {
                av_log(NULL, AV_LOG_ERROR, "swr_set_compensation() failed\n");
                return -1;
            }
        }
        av_fast_malloc(&is->audio_buf1, &is->audio_buf1_size, out_size);
        if (!is->audio_buf1)
            return AVERROR(ENOMEM);
        len2 = swr_convert(is->swr_ctx, out, out_count, in, frame->frame->nb_samples);
        if (len2 < 0) {
            av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
            return -1;
        }
        if (len2 == out_count) {
            av_log(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
            if (swr_init(is->swr_ctx) < 0)
                swr_free(&is->swr_ctx);
        }
        is->audio_buf = is->audio_buf1;
        resampled_data_size = len2 * audio_target.ch_layout.nb_channels * av_get_bytes_per_sample(audio_target.fmt);
    } else {
        is->audio_buf = frame->frame->data[0];
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
    TrackState *is = current_track;
    int audio_size, len1;

    audio_callback_time = av_gettime_relative();

    while (len > 0) {
        if (is->audio_buf_index >= is->audio_buf_size) {
           audio_size = audio_decode_frame(is);
           if (audio_size < 0) {
                /* if error, just output silence */
               is->audio_buf = NULL;
               is->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / audio_target.frame_size * audio_target.frame_size;
           } else {
               is->audio_buf_size = audio_size;
           }
           is->audio_buf_index = 0;
        }
        len1 = is->audio_buf_size - is->audio_buf_index;
        if (len1 > len)
            len1 = len;
        if (!is->muted && is->audio_buf && is->audio_volume == SDL_MIX_MAXVOLUME)
            memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
        else {
            memset(stream, 0, len1);
            if (!is->muted && is->audio_buf)
                SDL_MixAudioFormat(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, AUDIO_S16SYS, len1, is->audio_volume);
        }
        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
    is->audio_write_buf_size = is->audio_buf_size - is->audio_buf_index;
    /* Let's assume the audio driver that is used by SDL has two periods. */
    if (!isnan(is->audio_clock)) {
        set_clock_at(&is->audclk, is->audio_clock - (double)(2 * is->audio_hw_buf_size + is->audio_write_buf_size) / audio_target.bytes_per_sec, is->audio_clock_serial, audio_callback_time / 1000000.0);
        sync_clock_to_slave(&is->extclk, &is->audclk);
    }
}

/* open a given stream. Return 0 if OK */
static int stream_component_open(TrackState *is, int stream_index)
{
    AVFormatContext *ic = is->ic;
    AVCodecContext *avctx;
    const AVCodec *codec;
    const char *forced_codec_name = NULL;
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
    forced_codec_name = audio_codec_name;

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

    if (fast)
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

    if ((ret = configure_audio_filters(is, audio_filters, true)) < 0) goto fail;

    const AVFilterContext * sink = is->out_audio_filter;
    /* this is useful only when configure_audio_filters() is called with force_output_format == false
     * We call it with true because we want it to have exactly the same output that the sdl audio device has
     * so sdl does not try to resample or modify the audio*/
    //is->filter_sink_sample_rate = av_buffersink_get_sample_rate(sink);
    ret = av_buffersink_get_ch_layout(sink, &is->channel_layout);
    if (ret < 0) goto fail;

    //prepare audio output Todo make option to (A) create new audio device based on this stream (B) create new audio device for every new stream
    /*av_log(NULL, AV_LOG_INFO, "%d\n", is->sample_rate);
    if ((ret = audio_open(is, &is->channel_layout, is->sample_rate)) < 0)
        goto fail;*/

    is->audio_hw_buf_size = ret;
    is->audio_target = audio_target;
    is->audio_buf_size  = 0;
    is->audio_buf_index = 0;


    is->audio_stream = stream_index;
    is->audio_st = ic->streams[stream_index];

    if ((ret = decoder_init(&is->audio_decoder, avctx, &is->audio_queue, is->continue_read_thread)) < 0) goto fail;

    if (is->ic->iformat->flags & AVFMT_NOTIMESTAMPS) {
        is->audio_decoder.start_pts = is->audio_st->start_time;
        is->audio_decoder.start_pts_tb = is->audio_st->time_base;
    }

    if ((ret = decoder_start(&is->audio_decoder, audio_thread, "audio_decoder", is)) < 0) goto out;

    SDL_PauseAudioDevice(device_id, 0);

    goto out;

fail:
    avcodec_free_context(&avctx);
out:
    av_dict_free(&opts);

    return ret;
}

static int decode_interrupt_cb(void *ctx)
{
    TrackState *is = ctx;
    return is->abort_request;
}

static int stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue) {
    return stream_id < 0 ||
           queue->abort_request ||
           (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
           queue->nb_packets > MIN_FRAMES && (!queue->duration || av_q2d(st->time_base) * queue->duration > 1.0);
}

static int is_realtime(AVFormatContext *s)
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
    const AVDictionaryEntry *t;
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
    if (!av_dict_get(format_opts_n, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE)) {
        av_dict_set(&format_opts_n, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
        scan_all_pmts_set = 1;//This is set
    }
    //av_log(NULL, AV_LOG_INFO, "%s\n", current_file);
    err = avformat_open_input(&ic, current_file, is->iformat, &format_opts_n);
    if (err < 0) {
        //print_error(is->filename, err);
        av_log(NULL, AV_LOG_ERROR, "Could not open input stream.\n");
        ret = -1;
        goto fail;
    }

    if (scan_all_pmts_set)
        av_dict_set(&format_opts_n, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);

    remove_avoptions_n(&format_opts_n, codec_opts_n);

    ret = check_avoptions_n(format_opts_n);
    if (ret < 0)
        goto fail;

    is->ic = ic;

    if (genpts)
        ic->flags |= AVFMT_FLAG_GENPTS;

    if (find_stream_info) {
        AVDictionary **opts;
        int orig_nb_streams = ic->nb_streams;

        err = setup_find_stream_info_opts_n(ic, codec_opts_n, &opts);
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

    if (seek_by_bytes < 0)
        seek_by_bytes = !(ic->iformat->flags & AVFMT_NO_BYTE_SEEK) &&
                        !!(ic->iformat->flags & AVFMT_TS_DISCONT) &&
                        strcmp("ogg", ic->iformat->name);

    //is->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;


    /* if seeking requested, we execute it */
    if (start_time != AV_NOPTS_VALUE) {
        int64_t timestamp;

        timestamp = start_time;
        /* add the stream start time */
        if (ic->start_time != AV_NOPTS_VALUE)
            timestamp += ic->start_time;
        ret = avformat_seek_file(ic, -1, INT64_MIN, timestamp, INT64_MAX, 0);
        if (ret < 0) {
            av_log(NULL, AV_LOG_WARNING, "%s: could not seek to position %0.3f\n",
                    is->filename, (double)timestamp / AV_TIME_BASE);
        }
    }

    is->realtime = is_realtime(ic);

    for (i = 0; i < ic->nb_streams; i++) {
        AVStream *st = ic->streams[i];
        enum AVMediaType type = st->codecpar->codec_type;
        st->discard = AVDISCARD_ALL;
        if (type >= 0 && wanted_stream_spec[type] && st_index[type] == -1)
            if (avformat_match_stream_specifier(ic, st, wanted_stream_spec[type]) > 0)
                st_index[type] = i;
    }
    for (i = 0; i < AVMEDIA_TYPE_NB; i++) {
        if (wanted_stream_spec[i] && st_index[i] == -1) {
            av_log(NULL, AV_LOG_ERROR, "Stream specifier %s does not match any %s stream\n", wanted_stream_spec[i], av_get_media_type_string(i));
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

    if (infinite_buffer < 0 && is->realtime)
        infinite_buffer = 1;

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

                if (notify_of_restart_callback) {
                    notify_of_restart_callback();
                }
            }
            is->seek_req = 0;
            is->eof = 0;
            /*if (is->paused)
                step_to_next_frame(is);*/
        }

        /* if the queue are full, no need to read more */
        if (infinite_buffer<1 &&
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
            if (loop != 0) {

                if (loop >= 1) {
                    loop--;
                }
                stream_seek(is, start_time != AV_NOPTS_VALUE ? start_time : 0, 0, 0);
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
        pkt_in_play_range = duration == AV_NOPTS_VALUE ||
                (pkt_ts - (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
                av_q2d(ic->streams[pkt->stream_index]->time_base) -
                (double)(start_time != AV_NOPTS_VALUE ? start_time : 0) / 1000000
                <= ((double)duration / 1000000);
        if (pkt->stream_index == is->audio_stream && pkt_in_play_range) {
            packet_queue_put(&is->audio_queue, pkt);
        } else {
            av_packet_unref(pkt);
        }
    }

    ret = 0;
 fail:
    if (ic && !is->ic)
        avformat_close_input(&ic);

    av_packet_free(&pkt);
    if (ret != 0) {
        SDL_Event event;

        event.type = FF_QUIT_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);
    }
    SDL_DestroyMutex(wait_mutex);

    if (notify_of_eof_callback) {
        notify_of_eof_callback();
    }
    return 0;
}

static int audio_open(const int wanted_sample_rate, const int wanted_channels)
{
    if (is_audio_device_initialized) return given_spec.size;

    SDL_AudioSpec wanted_spec, spec;
    static const int next_nb_channels[] = {0, 0, 1, 6, 2, 6, 4, 6};
    static const int next_sample_rates[] = {44100, 48000, 96000, 192000};
    static const int next_sample_rates_size = FF_ARRAY_ELEMS(next_sample_rates);
    int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;
    int wanted_nb_channels = wanted_channels;

    AVChannelLayout wanted_channel_layout = {0};
    av_channel_layout_default(&wanted_channel_layout, wanted_nb_channels);

    const char *env = SDL_getenv("SDL_AUDIO_CHANNELS");
    if (env) {
        wanted_nb_channels = atoi(env);
        av_channel_layout_uninit(&wanted_channel_layout);
        av_channel_layout_default(&wanted_channel_layout, wanted_nb_channels);
    }
    if (wanted_channel_layout.order != AV_CHANNEL_ORDER_NATIVE) {
        av_channel_layout_uninit(&wanted_channel_layout);
        av_channel_layout_default(&wanted_channel_layout, wanted_nb_channels);
    }

    wanted_nb_channels = wanted_channel_layout.nb_channels;
    wanted_spec.channels = wanted_nb_channels;
    wanted_spec.freq = wanted_sample_rate;
    if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
        av_log(NULL, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
        av_channel_layout_uninit(&wanted_channel_layout);
        return -1;
    }
    while (next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq)
        next_sample_rate_idx--;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = NULL;

    while (!(device_id = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE | SDL_AUDIO_ALLOW_FORMAT_CHANGE))) {//Todo make adaptive to format and other data
        av_log(NULL, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n",
               wanted_spec.channels, wanted_spec.freq, SDL_GetError());

        wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];

        if (!wanted_spec.channels) {
            wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
            wanted_spec.channels = wanted_nb_channels;
            if (!wanted_spec.freq) {
                av_log(NULL, AV_LOG_ERROR,
                       "No more combinations to try, audio open failed\n");
                av_channel_layout_uninit(&wanted_channel_layout);
                return -1;
            }
        }
        av_channel_layout_default(&wanted_channel_layout, wanted_spec.channels);
    }


    if (spec.format != AUDIO_S16SYS) {
        av_log(NULL, AV_LOG_ERROR,
               "SDL advised audio format %d is not supported!\n", spec.format);
        av_channel_layout_uninit(&wanted_channel_layout);
        return -1;
    }
    if (spec.channels != wanted_spec.channels) {
        av_channel_layout_uninit(&wanted_channel_layout);
        av_channel_layout_default(&wanted_channel_layout, spec.channels);

        if (wanted_channel_layout.order != AV_CHANNEL_ORDER_NATIVE) {
            av_log(NULL, AV_LOG_ERROR,
                   "SDL advised channel count %d is not supported!\n", spec.channels);
            av_channel_layout_uninit(&wanted_channel_layout);
            return -1;
        }
    }

    audio_target.fmt = AV_SAMPLE_FMT_S16;
    audio_target.freq = spec.freq;
    if (av_channel_layout_copy(&audio_target.ch_layout, &wanted_channel_layout) < 0) {
        av_channel_layout_uninit(&wanted_channel_layout);
        return -1;
    }
    audio_target.frame_size = av_samples_get_buffer_size(NULL, audio_target.ch_layout.nb_channels, 1, audio_target.fmt, 1);
    audio_target.bytes_per_sec = av_samples_get_buffer_size(NULL, audio_target.ch_layout.nb_channels, audio_target.freq, audio_target.fmt, 1);
    if (audio_target.bytes_per_sec <= 0 || audio_target.frame_size <= 0) {
        av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
        return -1;
    }

    given_spec = spec;
    is_audio_device_initialized = true;
    return given_spec.size;
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

    current_file = av_strdup(filename);

    if (frame_queue_init(&is->sampq, &is->audio_queue, SAMPLE_QUEUE_SIZE, 1) < 0)
        goto fail;

    if (packet_queue_init(&is->audio_queue) < 0)
        goto fail;

    if (!(is->continue_read_thread = SDL_CreateCond())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        goto fail;
    }

    init_clock(&is->audclk, &is->audio_queue.serial);
    init_clock(&is->extclk, &is->extclk.serial);
    is->audio_clock_serial = -1;

    startup_volume = av_clip(startup_volume, 0, 100);
    sdl_volume = av_clip(SDL_MIX_MAXVOLUME * startup_volume / 100, 0, SDL_MIX_MAXVOLUME);
    is->audio_volume = sdl_volume;
    is->muted = 0;
    is->av_sync_type = AV_SYNC_AUDIO_MASTER;
    is->read_tid     = SDL_CreateThread(read_thread, "read_thread", is);

    if (!is->read_tid) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateThread(): %s\n", SDL_GetError());
fail:
        stream_close(is);
        return NULL;
    }
    return is;
}

void wait_loop() {
    double remaining_time = 0.0;
    while (1) {
        if (remaining_time > 0.0)
            av_usleep((int64_t)(remaining_time * 1000000.0));
        remaining_time = REFRESH_RATE;
    }
}


static void add_to_filter_chain(const char *filter_name)
{
    if (audio_filters && strlen(audio_filters) > 0) {
        // Combine user filters with loudnorm
        audio_filters = av_asprintf("%s,%s", audio_filters, filter_name);
    } else {
        audio_filters = av_strdup(filter_name);
    }
}

static void clear_filter_chain() {
    av_freep(&audio_filters);
}


/*static void timer_callback(union sigval sigval) {
    if (!current_track->paused) {
        return;
    }

    SDL_CloseAudioDevice(audio_dev);
}

int cancel_timer(timer_t tid) {
    const struct itimerspec disarm = {0};
    return timer_settime(tid, 0, &disarm, NULL);
}

int start_timer(timer_t tid) {
    struct itimerspec its = {0};
    its.it_value.tv_sec = 30;              // fire after 30 seconds
    its.it_value.tv_nsec = 0;             // one-shot: it_interval = 0
    timer_settime(tid, 0, &its, NULL);
}


static void re_create_audio_device(TrackState *is) {
    if (audio_dev) {
        return;
    }

    if (audio_open(&is, &is->channel_layout, is->sample_rate) < 0) {
        av_log(NULL, AV_LOG_FATAL, "Failed to re-create audio device");
    }
}*/

/* Called from the main */
void initialize(const char* app_name, const int initial_volume, const int loop_count, const int wanted_sample_rate, const NotifyOfError callback, const NotifyOfEndOfFile callback2, const NotifyOfRestart callback3)
{
    if (is_init_done) return;

    notify_of_error_callback = callback;
    notify_of_eof_callback = callback2;
    notify_of_restart_callback = callback3;

    init_dynload();

    av_log_set_flags(AV_LOG_SKIP_REPEATED);
    av_log_set_level(AV_LOG_INFO);

    /* register all codecs, demux and protocols */
#if CONFIG_AVDEVICE
    avdevice_register_all();
#endif

    signal(SIGINT , sigterm_handler); /* Interrupt (ANSI).    */
    signal(SIGTERM, sigterm_handler); /* Termination (ANSI).  */

    startup_volume = initial_volume;
    loop = loop_count;

    /* Try to work around an occasional ALSA buffer underflow issue when the
     * period size is NPOT due to ALSA resampling by forcing the buffer size. */
    if (!SDL_getenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE"))
        SDL_setenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE","1", 1);

    SDL_SetHint(SDL_HINT_APP_NAME, app_name);
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_NAME, app_name);
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_ROLE, "music");
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_APP_NAME, app_name);
    SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");
    SDL_SetHint(SDL_HINT_AUDIO_RESAMPLING_MODE, "3");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "0");

    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        av_log(NULL, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
        return;
    }

    SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
    SDL_EventState(SDL_USEREVENT, SDL_IGNORE);
    SDL_EventState(SDL_DISPLAYEVENT, SDL_IGNORE);

    /* prepare audio output */
    if (audio_open(wanted_sample_rate, 2) < 0) return;//Todo Hard coded to 2 channels for now


    // Setup timer to close audio device on pause
    /*struct sigevent sev = {0};
    sev.sigev_notify = SIGEV_THREAD;      // run callback in a new thread
    sev.sigev_notify_function = timer_callback;

    if (timer_create(CLOCK_REALTIME, &sev, &audio_device_close_timer) == -1) {
        perror("timer_create");
        exit(1);
    }*/

    is_init_done = true;
}

void shutdown() {
    if (device_id != 0) {
        SDL_CloseAudioDevice(device_id);
        device_id = 0;
    }

    is_audio_device_initialized = false;
    is_init_done = false;
    SDL_Quit();
}

void play_audio(const char *filename, const char * loudnorm_settings, const char * crossfeed_setting) {

    if (current_track) {
        clean_video_state(current_track);
        current_track = NULL;
    }

    clear_filter_chain();
    // Add loudness normalization filter. Ex: I=-16:TP=-1.5:LRA=11:measured_I=-8.9:measured_LRA=5.2:measured_TP=1.1:measured_thresh=-19.1:offset=-0.8
    if (loudnorm_settings) {
        const char *loudnorm_filter = av_asprintf("loudnorm=%s:linear=true", loudnorm_settings);
        add_to_filter_chain(loudnorm_filter);
        av_freep(&loudnorm_filter);
    }

    if (crossfeed_setting) {
        const char *crossfeed_filter = av_asprintf("crossfeed=%s", crossfeed_setting);
        add_to_filter_chain(crossfeed_filter);
        av_freep(&crossfeed_filter);
    }

    current_track = stream_open(filename);

    if (!current_track) {
        av_log(NULL, AV_LOG_FATAL, "Failed to initialize VideoState!\n");
        do_exit(NULL);
    }

    ++request_count;
}

void stop() {
    if (!current_track) return;

    clean_video_state(current_track);
    current_track = NULL;

    ++request_count;
}

void pause(const bool value) {

    if (!current_track || value == current_track->paused) return;


    if (value) {
        SDL_PauseAudioDevice(device_id, 1);
    }
    else if (device_id) {
        SDL_PauseAudioDevice(device_id, 0);
    }


    set_clock(&current_track->extclk, get_clock(&current_track->extclk), current_track->extclk.serial);
    current_track->paused = current_track->audclk.paused = current_track->extclk.paused = !current_track->paused;

    ++request_count;
}

void seek(const double percentPos) {
    if (!current_track || !current_track->ic) return;

    // Get the total duration of the media file
    int64_t duration = current_track->ic->duration;
    if (duration == AV_NOPTS_VALUE) {
        // If duration is unknown, we can't seek by percentage
        return;
    }

    // duration is already in AV_TIME_BASE units (microseconds)
    int64_t target_pos = (int64_t)(percentPos / 100.0 * duration);

    if (target_pos < 0) target_pos = 0;
    if (target_pos > duration) target_pos = duration;

    stream_seek(current_track, target_pos, 0, 0);

    ++request_count;
}

void set_volume(const int volume) {
    if (!current_track || volume > 100 || volume < 0 || volume == startup_volume) return;

    startup_volume = volume;
    sdl_volume = av_clip(SDL_MIX_MAXVOLUME * volume / 100, 0, SDL_MIX_MAXVOLUME);
    current_track->audio_volume = sdl_volume;

    ++request_count;
}

void mute(const bool value) {

    if (!current_track || value == current_track->muted) return;

    current_track->muted = !current_track->muted;

    ++request_count;
}

void set_loop_count(const int loop_count) {
    loop = loop_count;
}



/*packet_queue_init
Starting thread: read_tid
packet queue start
Starting thread: decoder_tid
EOF
Cleaning thread: read_tid
packet_queue_abort
frame_queue_peek_readable: aborts
Cleaning thread: decoder_tid
packet_queue_destroy
packet_queue_init
Starting read thread: read_tid
packet queue start
Starting read thread: decoder_tid*/
