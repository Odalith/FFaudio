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
 * Internal data structures shared by the library's modules. Not part of the public API.
 *
 * Threading model (see ARCHITECTURE in readme.md):
 *  - control thread : owns every TrackState (creation, slot changes, destruction) and runs user callbacks
 *  - read thread    : one per track, demuxes packets into audio_queue
 *  - audio thread   : one per track, decodes + filters into sampq (always float32 at the output rate/layout)
 *  - SDL callback   : mixer, pulls from sampq without blocking, never allocates or frees
 * Mixer slots (Mixer.current/next/outgoing) are only changed while holding the SDL audio device lock.
 */

#ifndef FFAUDIO_GLOBALS_H
#define FFAUDIO_GLOBALS_H

#define SAMPLE_QUEUE_SIZE 32
#define FRAME_QUEUE_SIZE SAMPLE_QUEUE_SIZE
#define SecondsToMicroseconds(x) ((int64_t)((x) * 1000000.0))
#define AV_TIME_BASE_DOUBLE 1000000.0
#define MIXER_EVENT_CAPACITY 64

#include <stdbool.h>
#include <stdint.h>

#include "libavutil/avstring.h"
#include "libavutil/channel_layout.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"
#include "libavutil/dict.h"
#include "libavutil/fifo.h"
#include "libavutil/samplefmt.h"
#include "libavutil/time.h"
#include "libavutil/bprint.h"
#include "libavutil/avassert.h"
#include "libavutil/opt.h"

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libswresample/swresample.h"
#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"

#include "SDL2/SDL.h"
#include "SDL2/SDL_thread.h"
#include "SDL2/SDL_mutex.h"

#include "ffaudio.h"

//+-----------------------------------------------------+
//| Queues (from FFplay)                                |
//+-----------------------------------------------------+

typedef struct MyAVPacketList {
    AVPacket *pkt;
    int32_t serial;
} MyAVPacketList;

typedef struct PacketQueue {
    AVFifo *pkt_list;
    int32_t nb_packets;
    int32_t size;
    int64_t duration;
    int32_t abort_request;      // guarded by mutex; mirrored in abort_atomic
    int32_t serial;             // guarded by mutex; mirrored in serial_atomic
    SDL_atomic_t abort_atomic;  // lock-free reads from other threads
    SDL_atomic_t serial_atomic; // lock-free reads from other threads (decoder, callback)
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;

typedef struct AudioParams {
    int32_t freq;
    AVChannelLayout ch_layout;
    enum AVSampleFormat fmt;
    int32_t frame_size;
    int32_t bytes_per_sec;
} AudioParams;

typedef struct FrameData {
    int64_t pkt_pos;
} FrameData;

typedef struct Frame {
    AVFrame *frame;
    int32_t serial;
    double pts;           /* presentation timestamp of the first sample, seconds; NAN if unknown */
    double duration;      /* duration of the frame, seconds */
    int64_t pos;          /* byte position of the frame in the input file */
} Frame;

typedef struct FrameQueue {
    Frame queue[FRAME_QUEUE_SIZE];
    int32_t rindex;   // consumer only
    int32_t windex;   // producer only
    int32_t size;     // guarded by mutex
    int32_t max_size;
    int64_t queued_samples;   // guarded by mutex
    int64_t max_samples;      // guarded by mutex; 0 = only max_size limits the queue
    SDL_mutex *mutex;
    SDL_cond *cond;
    PacketQueue *pktq;
} FrameQueue;

typedef struct Decoder {
    AVPacket *pkt;
    PacketQueue *queue;
    AVCodecContext *avctx;
    int32_t pkt_serial;
    int32_t finished;
    int32_t packet_pending;
    SDL_cond *empty_queue_cond;
    int64_t start_pts;
    AVRational start_pts_tb;
    int64_t next_pts;
    AVRational next_pts_tb;
    SDL_Thread *decoder_tid;
} Decoder;

//+-----------------------------------------------------+
//| Tracks                                              |
//+-----------------------------------------------------+

typedef enum TrackRole {
    ROLE_NONE = 0,      // not audible and not queued (retired, waiting for destruction)
    ROLE_CURRENT,
    ROLE_NEXT,
    ROLE_OUTGOING,      // fading out (crossfade or fade-out on stop)
} TrackRole;

typedef enum TrackEndReason {
    END_NONE = 0,
    END_NATURAL,        // played to the end
    END_SKIPPED,        // replaced, skipped, cleared or stopped by the user
    END_ERROR,          // could not be opened or decoding failed
} TrackEndReason;

struct AudioPlayer;

typedef struct FilterChain {
    AVFilterGraph *graph;
    AVFilterContext *src;
    AVFilterContext *sink;
} FilterChain;

typedef struct FilterCmd {
    char *target;
    char *cmd;
    char *arg;
    struct FilterCmd *next;
} FilterCmd;

typedef struct TrackState {
    struct AudioPlayer *ap;
    int32_t handle;
    char *filename;
    char *filtergraph_str;              // per-track user filters, may be NULL
    int64_t start_time;                 // AV_TIME_BASE units, AV_NOPTS_VALUE = from the start
    int64_t play_duration;              // AV_TIME_BASE units, AV_NOPTS_VALUE = to the end
    AuFade fade_in_override;            // only used when has_fade_in_override
    bool has_fade_in_override;
    AuReplayGainInfo replaygain_override;   // only used when has_replaygain_override
    bool has_replaygain_override;
    AuMediaInfo *info;                  // written by the read thread under seek_mutex before `opened` is set

    // ReplayGain: target gain in millibels (any thread), ramped in the mixer
    SDL_atomic_t gain_mb;
    float gain_cur;                     // SDL callback only
    bool gain_started;                  // SDL callback only
    float gain_seen;                    // SDL callback only: target the current ramp heads for
    float gain_step;                    // SDL callback only: per-sample change of the current ramp
    SDL_atomic_t gain_slow;             // 1 = the next gain change arrived late: ramp over 2 s instead of 10 ms

    // ---- threads ----
    SDL_Thread *read_tid;
    SDL_atomic_t abort_request;
    SDL_cond *continue_read_thread;
    SDL_mutex *wait_mutex;

    // ---- seek requests (control thread -> read thread), guarded by seek_mutex ----
    SDL_mutex *seek_mutex;              // also guards filtergraph_str, filter_cmds, duration_us and end_pos_us
    FilterCmd *filter_cmds;             // pending avfilter_graph_send_command() calls
    bool seek_req;
    int64_t seek_pos;                   // AV_TIME_BASE units
    double pending_seek_percent;        // < 0 = none; resolved by the read thread once the duration is known

    // ---- demux / decode (read + audio threads) ----
    AVFormatContext *ic;
    const AVInputFormat *iformat;
    int32_t audio_stream;
    AVStream *audio_st;
    PacketQueue audio_queue;
    Decoder audio_decoder;
    FrameQueue sampq;
    AudioParams audio_filter_src;       // format of the frames entering the convert graph
    FilterChain convert;                // audio thread only: decoder format -> output format
    FilterChain user;                   // audio thread only: user + global filters, may be empty
    int32_t convert_rate;               // output rate the graphs were built for
    AVDictionary *format_opts_n;
    int32_t realtime;
    int32_t seek_by_bytes;
    bool infinite_buffer;
    bool demux_eof;                     // read thread only

    // ---- status published by the worker threads ----
    SDL_atomic_t opened;                // 1 once the stream is open and duration_us is valid
    SDL_atomic_t failed;                // 1 when the track can not produce (more) audio because of an error
    SDL_atomic_t eos_serial;            // queue serial whose last frame has been pushed into sampq, -1 = none
    SDL_atomic_t loops_remaining;       // -1 infinite, 0 none
    SDL_atomic_t filter_reconfigure;    // 1 = rebuild the convert graph before the next frame (device changed)
    SDL_atomic_t user_filters_changed;  // 1 = rebuild the user graph before the next frame
    SDL_atomic_t had_error;             // 1 = the track ends as an error, even if some audio played
    int64_t duration_us;                // written before `opened` is set, AV_NOPTS_VALUE = unknown
    int64_t end_pos_us;                 // file position where playback ends, written before `opened` is set

    // ---- playback cursor, SDL callback only ----
    Frame *cur;                         // frame being played, NULL = take the next one
    int32_t cur_offset;                 // samples of `cur` already played
    double position;                    // seconds, position of the next sample to be played
    double last_frame_pts;              // used to detect loop restarts
    bool started;                       // produced at least one sample
    bool announced;                     // MIX_EV_TRACK_STARTED has been sent

    // ---- slot bookkeeping, changed under the device lock ----
    TrackRole role;
    TrackEndReason fade_reason;         // why it is fading out (ROLE_OUTGOING)

    // ---- control thread only ----
    TrackEndReason end_reason;
    bool ended;                         // removed from the mixer, waiting for destruction
    bool reported;                      // on_eof has been called
    bool prepare_next_sent;
    bool duration_reported;
    double reported_duration;
} TrackState;

//+-----------------------------------------------------+
//| Mixer (SDL callback state)                          |
//+-----------------------------------------------------+

typedef enum MixerEventType {
    MIX_EV_TRACK_STARTED,       // track produced its first audible sample as current
    MIX_EV_TRACK_ENDED,         // track reached its end and left the mixer
    MIX_EV_TRACK_LOOPED,        // playback position wrapped back to the start of the track
    MIX_EV_FADE_DONE,           // the outgoing track finished fading out and left the mixer
} MixerEventType;

typedef struct MixerEvent {
    MixerEventType type;
    TrackState *track;
    double position;
} MixerEvent;

typedef struct FadeState {
    bool active;
    int64_t pos;                // frames into the fade
    int64_t len;                // total frames
    AuFade fade;
} FadeState;

typedef struct Mixer {
    // Slots. Changed by the callback (gapless handover, end crossfade) and by the control thread,
    // always with the device lock held.
    TrackState *current;
    TrackState *next;
    TrackState *outgoing;           // fading out
    FadeState in_fade;              // applied to current
    FadeState out_fade;             // applied to outgoing

    // Output format of the open device
    int32_t rate;
    int32_t channels;
    SDL_AudioFormat format;
    int32_t bytes_per_sample;
    int32_t period_frames;          // SDL buffer size in frames

    // Gains. vol_gain follows volume * !muted with a short ramp, pause_gain follows the pause fade.
    float vol_gain;
    int32_t vol_ramp_frames;
    float pause_gain;
    float pause_from;
    float pause_to;
    FadeState pause_fade;
    bool frozen;                    // paused and silent: tracks are not consumed

    // Crossfade at a natural track end
    AuFade end_fade;
    double min_track_seconds;

    // Scratch buffers, interleaved float, allocated with the device
    float *mix_buf;
    float *voice_buf;
    int32_t buf_frames;

    // Callback -> control thread events
    MixerEvent events[MIXER_EVENT_CAPACITY];
    int32_t n_events;
    bool events_overflowed;

    uint32_t dither_state;
} Mixer;

//+-----------------------------------------------------+
//| Control-thread messages                             |
//+-----------------------------------------------------+

typedef enum MsgType {
    CMD_PLAY,
    CMD_SET_NEXT,
    CMD_SKIP,
    CMD_CLEAR_NEXT,
    CMD_STOP,
    CMD_PAUSE,
    CMD_SEEK_US,
    CMD_SEEK_PERCENT,
    CMD_SET_LOOP,
    CMD_CONFIGURE_DEVICE,
    CMD_SET_CROSSFADE,
    CMD_SET_GLOBAL_FILTERS,
    CMD_SET_DOWNMIX,
    CMD_SET_REPLAYGAIN,
    CMD_GET_TRACK_INFO,
    CMD_GET_TRACK_GAIN,
    CMD_GAIN_CACHE_CHANGED,
    CMD_UPDATE_TRACK_FILTERS,
    CMD_FILTER_COMMAND,
    CMD_SHUTDOWN,
    MSG_LOG,
    MSG_TRACK_OPENED,
    MSG_TRACK_FAILED,
    MSG_TRACK_DEMUX_EOF,
    MSG_TRACK_SEEKED,
} MsgType;

typedef struct Msg {
    MsgType type;
    int32_t handle;
    char *str;                      // owned
    char *str2;                     // owned
    char *str3;                     // owned
    PlayAudioConfig play_config;    // av_filtergraph points at str2
    bool has_play_config;
    AuFade fade_in_override;
    bool has_fade_in_override;
    CrossfadeConfig crossfade;
    AudioDeviceConfig device;       // audio_device points at str
    bool has_device;
    AuReplayGainConfig replaygain;
    AuReplayGainInfo replaygain_override;
    bool has_replaygain_override;
    void **out_ptr;                 // synchronous queries
    double *out_double;
    int64_t i64;
    double d;
    bool b;
    int32_t level;
    int64_t request;
    // Synchronous commands
    SDL_sem *done;
    int32_t *result;
    struct Msg *next;
} Msg;

//+-----------------------------------------------------+
//| Player                                              |
//+-----------------------------------------------------+

typedef struct StatusSnapshot {
    int32_t handle;                 // current track, 0 = none
    double position;                // seconds at callback_time
    double callback_time;           // seconds, SDL performance counter
    double duration;                // seconds, < 0 unknown
    bool advancing;                 // false while paused / underrun / idle
} StatusSnapshot;

typedef struct AudioPlayer {
    // ---- configuration ----
    SDL_atomic_t volume;                // 0..100
    SDL_atomic_t muted;
    SDL_atomic_t paused;
    SDL_atomic_t loop_count;            // default for new tracks, -1 infinite
    double prepare_next_seconds;
    int32_t output_channels;            // 0 = device preference
    int32_t output_sample_rate;         // 0 = device preference
    CrossfadeConfig crossfade;          // control thread only
    bool use_soxr;
    int32_t filter_nbthreads;
    int32_t find_stream_info;

    // ---- callbacks ----
    void *user_data;
    NotifyOfLog notify_of_log_callback;
    NotifyOfEndOfFile notify_of_eof_callback;
    NotifyOfRestart notify_of_restart_callback;
    NotifyOfDurationUpdate notify_of_duration_update_callback;
    NotifyOfPrepareNext notify_of_prepare_next_callback;
    NotifyOfTrackStarted notify_of_track_started_callback;
    NotifyOfAudioDeviceUpdate notify_of_audio_device_update_callback;

    // ---- control thread ----
    SDL_Thread *control_thread;
    SDL_threadID control_thread_id;
    SDL_atomic_t shutting_down;
    SDL_mutex *msg_mutex;
    SDL_cond *msg_cond;
    Msg *msg_head;
    Msg *msg_tail;
    SDL_atomic_t handle_count;
    SDL_atomic_t request_count;
    TrackState **tracks;                // every live track, control thread only
    int32_t n_tracks;
    int32_t cap_tracks;

    // ---- output device (control thread writes, others read under format_mutex) ----
    SDL_mutex *format_mutex;
    SDL_cond *format_cond;              // signalled when the output format becomes available
    bool output_ready;                  // guarded by format_mutex
    char *global_filters;               // guarded by format_mutex, applied after each track's own filters
    AU_DOWNMIX downmix;                 // guarded by format_mutex
    AuReplayGainConfig replaygain;      // guarded by format_mutex
    char *downmix_filters;              // guarded by format_mutex, AU_DOWNMIX_CUSTOM only
    AudioParams audio_target;           // float32 interleaved at the device rate/layout, guarded by format_mutex
    SDL_AudioDeviceID device_id;
    SDL_AudioSpec obtained_spec;
    bool device_running;
    bool is_audio_device_initialized;
    bool use_default_device;
    int32_t audio_device_index;
    char *audio_device_name;

    // ---- mixer (SDL callback; others under SDL_LockAudioDevice) ----
    Mixer mixer;

    // ---- status for getters ----
    SDL_mutex *status_mutex;            // a mutex (not a spinlock) so ThreadSanitizer can follow it
    StatusSnapshot status;
} AudioPlayer;

// Shared helpers implemented in ffaudio.c
void ff_log(enum AU_LOG_LEVEL level, const char *fmt, ...);
void ff_post_track_msg(MsgType type, int32_t handle, double d);

#endif //FFAUDIO_GLOBALS_H
