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

#ifndef FFAUDIO_GLOBALS_H
#define FFAUDIO_GLOBALS_H

#define VIDEO_PICTURE_QUEUE_SIZE 3
#define SUBPICTURE_QUEUE_SIZE 16
#define SAMPLE_QUEUE_SIZE 9
#define FRAME_QUEUE_SIZE FFMAX(SAMPLE_QUEUE_SIZE, FFMAX(VIDEO_PICTURE_QUEUE_SIZE, SUBPICTURE_QUEUE_SIZE))
#define EQ_BAND_COUNT 10
#define SecondsToMicroseconds(x) ((x) * 1000000)
#define AV_TIME_BASE_DOUBLE 1000000.0

#include <stdbool.h>
#include <stddef.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/fifo.h>
#include <SDL2/SDL_mutex.h>
#include <SDL2/SDL_thread.h>
#include <math.h>
#include <limits.h>
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
#include "delagates.h"

typedef struct MyAVPacketList {
    AVPacket *pkt;
    int serial;
} MyAVPacketList;

typedef struct PacketQueue {
    AVFifo *pkt_list;
    int nb_packets;
    int size;
    int64_t duration;
    int abort_request;
    int serial;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;

typedef struct AudioParams {
    int freq;
    AVChannelLayout ch_layout;
    enum AVSampleFormat fmt;
    int frame_size;
    int bytes_per_sec;
} AudioParams;

typedef struct Clock {
    double pts;           /* clock base */
    double pts_drift;     /* clock base minus time at which we updated the clock */
    double last_updated;
    double speed;
    int serial;           /* clock is based on a packet with this serial */
    int paused;
    int *queue_serial;    /* pointer to the current packet queue serial, used for obsolete clock detection */
} Clock;

typedef struct FrameData {
    int64_t pkt_pos;
} FrameData;

/* Common struct for handling all types of decoded data and allocated render buffers. */
typedef struct Frame {
    AVFrame *frame;
    int serial;
    double pts;           /* presentation timestamp for the frame */
    double duration;      /* estimated duration of the frame */
    int64_t pos;          /* byte position of the frame in the input file */
    int format;
} Frame;

typedef struct FrameQueue {
    Frame queue[FRAME_QUEUE_SIZE];
    int rindex;
    int windex;
    int size;
    int max_size;
    int keep_last;
    int rindex_shown;
    SDL_mutex *mutex;
    SDL_cond *cond;
    PacketQueue *pktq;
} FrameQueue;

enum {
    AV_SYNC_AUDIO_MASTER,
};

typedef struct Decoder {
    AVPacket *pkt;
    PacketQueue *queue;
    AVCodecContext *avctx;
    int pkt_serial;
    int finished;
    int packet_pending;
    SDL_cond *empty_queue_cond;
    int64_t start_pts;
    AVRational start_pts_tb;
    int64_t next_pts;
    AVRational next_pts_tb;
    SDL_Thread *decoder_tid;
} Decoder;

typedef struct TrackState {
    int32_t handle;
    SDL_Thread *read_tid;
    int abort_request;
    int paused;
    int last_paused;
    int seek_req;
    int seek_flags;
    int64_t seek_pos;
    int64_t seek_rel;
    int64_t start_time;                // AV_NOPTS_VALUE, how much to seek before playing
    int64_t play_duration;             // AV_NOPTS_VALUE, how much to play
    int read_pause_return;
    AVFormatContext *ic;
    int realtime;

    Clock audclk;
    Clock extclk;

    FrameQueue sampq;

    Decoder audio_decoder;
    char *forced_audio_codec_name;     // default NULL

    int audio_stream;

    int av_sync_type;
    double audio_clock;
    int audio_clock_serial;


    AVStream *audio_st;
    PacketQueue audio_queue;
    int audio_hw_buf_size;
    uint8_t *audio_buf0;
    uint8_t *audio_buf1;
    unsigned int audio_buf0_size; /* in bytes */
    unsigned int audio_buf1_size;
    int audio_buf_index; /* in bytes */
    int audio_write_buf_size;
    int audio_volume;
    int muted;

    struct SwrContext *swr_ctx;
    //struct AudioParams audio_target; /*This is set to the global 'audio_target' not sure if this is needed*/
    struct AudioParams audio_filter_src; /*This is the parameters of the current file */

    AVChannelLayout channel_layout;
    AVDictionary *format_opts_n;       // default NULL
    AVDictionary *codec_opts_n;        // default NULL
    AVDictionary *swr_opts_n;          // default NULL

    AVFilterContext *in_audio_filter;   // the first filter in the audio chain
    AVFilterContext *out_audio_filter;  // the last filter in the audio chain
    AVFilterGraph *agraph;              // audio filter graph

    //AVTXContext *rdft; ~553KD
    //av_tx_fn rdft_fn; ~553KD
    //int rdft_bits; ~553KD
    //float *real_data; ~553KD
    //AVComplexFloat *rdft_data; ~553KD

    int eof;
    const char *filename;
    const AVInputFormat *iformat;

    int last_audio_stream;

    SDL_cond *continue_read_thread;
} TrackState;


typedef struct AudioPlayer {
    int startup_volume;                // default 100
    int sdl_volume;                    // default 0
    int filter_nbthreads;              // default 0
    int64_t audio_callback_time;       // default 0
    char *track_filters;               // default NULL
    char *anequalizer_filter;          // default NULL
    int16_t anequalizer_values[EQ_BAND_COUNT]; //default [0, ...]
    const char* wanted_stream_spec[AVMEDIA_TYPE_NB]; // default [NULL]
    int seek_by_bytes;                 // default -1
    int loop;                          // default 0
    int infinite_buffer;               // default -1
    int find_stream_info;              // default 1

    int32_t handle_count;              // default 0
    int32_t errored_handle;            // default -1, used when stream_open() fails
    TrackState *current_track;         // default NULL

    SDL_Thread *event_thread;          // default NULL
    SDL_atomic_t event_thread_running; // default false

    Uint32 eof_event;                  // default SDL_RegisterEvents
    Uint32 log_event;                  // default SDL_RegisterEvents
    Uint32 restart_event;              // default SDL_RegisterEvents
    Uint32 duration_update_event;      // default SDL_RegisterEvents
    Uint32 prepare_next_event;         // default SDL_RegisterEvents

    SDL_mutex *abort_mutex;            // default SDL_CreateMutex
    SDL_cond *abort_cond;              // default SDL_CreateCond

    SDL_mutex *reconfigure_mutex;       // default SDL_CreateMutex
    SDL_cond *reconfigure_cond;         // default SDL_CreateCond

    int64_t request_count;             // default 0
    bool is_init_done;                 // default false
    bool is_audio_device_initialized;  // default false
    bool is_eof_from_skip;             // default false
    bool is_eof_from_error;            // default false
    bool reconfigure_audio_device;     // default false
    int64_t audio_reconfigure_time;    // AV_NOPTS_VALUE, where to seek after reconfigure is complete


    const char *audio_device_name;     // default NULL
    int audio_device_index;            // default -1
    SDL_AudioDeviceID device_id;       // default 0
    SDL_AudioSpec given_spec;          // default NULL
    SDL_AudioFormat given_format;      // AUDIO_S16SYS
    AudioParams *audio_target;         // default malloc

    //Callbacks
    NotifyOfLog notify_of_log_callback;                  // default NULL
    NotifyOfEndOfFile notify_of_eof_callback;            // default NULL
    NotifyOfRestart notify_of_restart_callback;          // default NULL
    NotifyOfDurationUpdate notify_of_duration_update_callback;    // default NULL
    NotifyOfPrepareNext notify_of_prepare_next_callback; // default NULL

    // Likely to be removed
    int fast;                          // default 0
    int genpts;                        // default 0
} AudioPlayer;

#endif //FFAUDIO_GLOBALS_H