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
} AudioParams;

typedef struct Frame {
    AVFrame *frame;
    int32_t serial;
    double pts;           /* presentation timestamp of the first sample, seconds; NAN if unknown */
    double speed;         /* playback speed it was made with: file seconds per output second */
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

// "Now playing" information read from a stream, waiting until playback reaches `position`
typedef struct StreamMeta {
    double position;            // seconds of file time read since the start or the last seek (read_clock)
    char *title, *artist;       // UTF-8, owned; artist may be NULL
    struct StreamMeta *next;
} StreamMeta;

typedef struct FilterCmd {
    char *target;
    char *cmd;
    char *arg;
    struct FilterCmd *next;
} FilterCmd;

// TrackState.gain_mb holds the ReplayGain gain in 0.01 dB and whether a change to it ramps slowly (over 2 s instead of
// 10 ms: a value that arrived after the track started). One atomic for both, so the flag can not outlive its value.
static inline int gain_pack(const int centi_db, const bool slow) { return centi_db * 2 + (slow ? 1 : 0); }
static inline int gain_centi_db(const int packed) { return (packed - (packed & 1)) / 2; }
static inline bool gain_is_slow(const int packed) { return (packed & 1) != 0; }

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

    // ReplayGain: target gain (any thread, see gain_pack()), ramped in the mixer
    SDL_atomic_t gain_mb;
    float gain_cur;                     // SDL callback only
    bool gain_started;                  // SDL callback only
    float gain_seen;                    // SDL callback only: target the current ramp heads for
    float gain_step;                    // SDL callback only: per-sample change of the current ramp

    // ---- threads ----
    SDL_Thread *read_tid;
    SDL_atomic_t abort_request;
    SDL_cond *continue_read_thread;
    SDL_mutex *wait_mutex;

    // ---- seek requests (control thread -> read thread), guarded by seek_mutex ----
    SDL_mutex *seek_mutex;              // also guards filtergraph_str, filter_cmds and end_pos_us
    FilterCmd *filter_cmds;             // pending avfilter_graph_send_command() calls
    bool seek_req;
    int64_t seek_pos;                   // AV_TIME_BASE units
    double pending_seek_percent;        // < 0 = none; resolved by the read thread once the duration is known

    // ---- demux / decode (read + audio threads) ----
    AVFormatContext *ic;
    int32_t audio_stream;
    AVStream *audio_st;
    PacketQueue audio_queue;
    Decoder audio_decoder;
    FrameQueue sampq;
    AudioParams audio_filter_src;       // format of the frames entering the convert graph
    FilterChain convert;                // audio thread only: decoder format -> output format
    FilterChain user;                   // audio thread only: user + global filters, may be empty
    FilterChain speed_chain;            // audio thread only: tempo / rate change, empty at normal speed
    double speed_cur;                   // audio thread only: speed the speed chain was built for
    bool speed_keep_pitch_cur;          // audio thread only
    int32_t speed_gen_seen;             // audio thread only: last applied AudioPlayer.speed_gen
    double speed_seg_in;                // audio thread only: file position where the current speed segment began
    double speed_in_end;                // audio thread only: file position just past the last frame fed into it
    int64_t speed_seg_out;              // audio thread only: samples output since then
    bool speed_seg_open;                // audio thread only: speed_seg_in is known
    int32_t convert_rate;               // output rate the graphs were built for
    AVDictionary *format_opts_n;
    char *http_headers;                 // PlayAudioConfig.http_headers (owned), network streams only
    // Network tracks (set at creation): the read thread fills buffer_target_s before the mixer takes anything,
    // again after the mixer ran dry while playing, and reads ahead up to max_buffer_s
    bool network;
    double buffer_target_s, max_buffer_s;
    bool live;                          // read thread: no duration (internet radio, live HLS)
    double pts_offset;                  // read thread, before the decoder starts: the container's start time (s),
                                        // taken off frame timestamps so positions count from 0
    SDL_atomic_t buffering;             // 1 = waiting for data: the mixer takes nothing, the position holds
    SDL_atomic_t buffered_ms;           // read thread: audio queued, for on_buffering
    bool stalled;                       // SDL callback only: ran dry while playing; fades back in when data returns
    int resume_fade, resume_fade_len;   // SDL callback only: frames left of that fade-in, and its length
    int buffering_reported;             // control thread: last state sent to on_buffering (-1 = none yet)
    uint32_t buffering_note_ticks;      // control thread: when it was last sent
    StreamMeta *stream_meta;            // guarded by seek_mutex: read thread appends, control thread reports
    char *last_icy;                     // read thread: the last ICY packet seen
    char *last_meta;                    // read thread: "title\nartist" last queued, to skip repeats
    // File time read and played since the start (or the last seek), for "now playing": timestamps can not be
    // used for that, chained Ogg (FFmpeg < 7) and HLS discontinuities start them again from 0
    double read_clock;                  // read thread: durations of the audio packets read
    int64_t last_pkt_ts;                // read thread: for packets without a duration
    double play_clock;                  // SDL callback: file time played
    int clock_serial;                   // SDL callback: the serial play_clock counts for
    bool icy;                           // read thread: the server sends ICY metadata (icy-metaint)
    bool demux_eof;                     // read thread only

    // ---- status published by the worker threads ----
    SDL_atomic_t opened;                // 1 once the stream is open and end_pos_us is valid
    SDL_atomic_t eos_serial;            // queue serial whose last frame has been pushed into sampq, -1 = none
    SDL_atomic_t loops_remaining;       // -1 infinite, 0 none
    SDL_atomic_t loops_started;         // read thread: loops it has seeked back for (the mixer reports each once)
    SDL_atomic_t filter_reconfigure;    // 1 = rebuild the convert graph before the next frame (device changed)
    SDL_atomic_t user_filters_changed;  // 1 = rebuild the user graph before the next frame
    SDL_atomic_t had_error;             // 1 = the track ends as an error, even if some audio played
    int64_t end_pos_us;                 // file position where playback ends, written before `opened` is set

    // ---- playback cursor, SDL callback only ----
    Frame *cur;                         // frame being played, NULL = take the next one
    int32_t cur_offset;                 // samples of `cur` already played
    double position;                    // seconds, position of the next sample to be played
    double last_frame_pts;              // used to detect loop restarts
    double play_speed;                  // speed of the audio being played (file seconds per output second)
    bool started;                       // produced at least one sample
    bool announced;                     // MIX_EV_TRACK_STARTED has been sent
    int32_t loops_heard;                // loops reported so far (compared with loops_started)
    int32_t last_frame_serial;          // serial of last_frame_pts

    // ---- slot bookkeeping, changed under the device lock ----
    TrackRole role;
    TrackEndReason fade_reason;         // why it is fading out (ROLE_OUTGOING)

    // ---- control thread only ----
    bool ended;                         // removed from the mixer and reported, waiting for destruction
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
    PlayAudioConfig play_config;    // av_filtergraph points at str2, http_headers at str3
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
    double speed;                   // playback speed of the audible audio
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
    NotifyOfAudioDevicesChanged notify_of_audio_devices_changed_callback;
    NotifyOfBuffering notify_of_buffering_callback;
    NotifyOfStreamMetadata notify_of_stream_metadata_callback;

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
    double speed;                       // guarded by format_mutex, playback speed (1 = normal)
    bool keep_pitch;                    // guarded by format_mutex
    int32_t speed_gen;                  // guarded by format_mutex, bumped on every change
    char *downmix_filters;              // guarded by format_mutex, AU_DOWNMIX_CUSTOM only
    AudioParams audio_target;           // float32 interleaved at the device rate/layout, guarded by format_mutex
    SDL_AudioDeviceID device_id;
    SDL_AudioSpec obtained_spec;
    bool device_running;
    bool use_default_device;
    int32_t audio_device_index;
    char *audio_device_name;
    char *device_in_use;                // guarded by format_mutex: name of the open device (default resolved)
    char **known_devices;               // control thread: last seen output device list
    int32_t n_known_devices;
    bool devices_checked_once;          // control thread: the first comparison only records the list
    uint32_t devices_checked_at;        // control thread: SDL_GetTicks() of the last comparison
    // Set by an SDL event watch (on whichever thread adds events to SDL's queue), taken by the control thread
    SDL_atomic_t device_events;         // 1 = an output device was added or removed
    SDL_atomic_t open_device;           // device_id, for the watch
    SDL_atomic_t open_device_lost;      // 1 = the open device was removed

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
