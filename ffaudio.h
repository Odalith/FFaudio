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
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef FFAUDIO_H
#define FFAUDIO_H
#include <stdatomic.h>

#ifdef _WIN32
    #define DLL_EXPORT __declspec(dllexport)
#else
    #define DLL_EXPORT __attribute__((visibility("default")))
#endif

#define VIDEO_PICTURE_QUEUE_SIZE 3
#define SUBPICTURE_QUEUE_SIZE 16
#define SAMPLE_QUEUE_SIZE 9
#define FRAME_QUEUE_SIZE FFMAX(SAMPLE_QUEUE_SIZE, FFMAX(VIDEO_PICTURE_QUEUE_SIZE, SUBPICTURE_QUEUE_SIZE))

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
    SDL_Thread *read_tid;
    int abort_request;
    int paused;
    int last_paused;
    int seek_req;
    int seek_flags;
    int64_t seek_pos;
    int64_t seek_rel;
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
    char *audio_filters;               // default NULL
    const char* wanted_stream_spec[AVMEDIA_TYPE_NB]; // default [NULL]
    int seek_by_bytes;                 // default -1
    int64_t start_time;                // AV_NOPTS_VALUE//Todo should be part of TrackState
    int64_t duration;                  // AV_NOPTS_VALUE//Todo should be part of TrackState
    int loop;                          // default 0
    int infinite_buffer;               // default -1
    int find_stream_info;              // default 1


    TrackState *current_track;         // default NULL
    AVDictionary *format_opts_n;       // default NULL
    AVDictionary *codec_opts_n;        // default NULL
    AVDictionary *swr_opts_n;          // default NULL

    SDL_Thread *event_thread;          // default NULL
    SDL_atomic_t event_thread_running; // default false
    Uint32 eof_event;                  // default SDL_RegisterEvents

    SDL_mutex *abort_mutex;            // default SDL_CreateMutex
    SDL_cond *abort_cond;              // default SDL_CreateCond

    int64_t request_count;             // default 0
    bool is_init_done;                 // default false
    bool is_audio_device_initialized;  // default false
    bool is_eof_from_skip;             // default false

    SDL_AudioDeviceID device_id;       // default 0
    SDL_AudioSpec given_spec;          // default NULL
    SDL_AudioFormat given_format;      // AUDIO_S16SYS
    AudioParams *audio_target;         // default malloc

    // Likely to be removed
    int fast;                          // default 0
    int genpts;                        // default 0
} AudioPlayer;



typedef void (*NotifyOfError)(const char* message, int request);
typedef void (*NotifyOfEndOfFile)(bool is_eof_from_skip);
typedef void (*NotifyOfRestart)();

// Static variables to hold the callback functions
static NotifyOfError notify_of_error_callback = NULL;
static NotifyOfEndOfFile notify_of_eof_callback = NULL;
static NotifyOfRestart notify_of_restart_callback = NULL;


#ifdef __cplusplus
extern "C" {
#endif

enum sample_rates {LOW = 44100, MEDIUM = 48000, HIGH = 96000, ULTRA = 192000};

DLL_EXPORT void shutdown();

DLL_EXPORT int initialize(const char* app_name, const int initial_volume, const int loop_count, const NotifyOfError callback, const NotifyOfEndOfFile callback2, const NotifyOfRestart callback3);

DLL_EXPORT int configure_audio_device(const char* audio_device, int audio_device_index, bool use_default);

DLL_EXPORT void play_audio(const char *filename, const char * loudnorm_settings, const char * crossfeed_setting);

DLL_EXPORT void stop();

DLL_EXPORT void pause_audio(const bool value);

DLL_EXPORT void seek(const double percentPos);

DLL_EXPORT void set_volume(const int volume);

DLL_EXPORT void mute(const bool value);

DLL_EXPORT void set_loop_count(const int loop_count);

DLL_EXPORT void wait_loop();

DLL_EXPORT int get_audio_devices(int *out_total, char ***out_devices);

#ifdef __cplusplus
} // extern "C"
#endif


#endif //FFAUDIO_H
