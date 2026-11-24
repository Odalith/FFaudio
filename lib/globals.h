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
 *  - worker thread  : one per track: opens it, reads it (local files), decodes and filters into its SampleRing
 *                     (float32 at the output rate/layout)
 *  - reader thread  : network tracks only: reads ahead into audio_queue, so a stalled connection does not stop the
 *                     worker from decoding what is queued
 *  - SDL callback   : mixer, takes from the rings without locks, never blocks, allocates or frees; fills the meter tap
 * Mixer slots (Mixer.current/next/outgoing) are only changed while holding the SDL audio device lock.
 */

#ifndef FFAUDIO_GLOBALS_H
#define FFAUDIO_GLOBALS_H

#define SecondsToMicroseconds(x) ((int64_t)((x) * 1000000.0))
#define AV_TIME_BASE_DOUBLE 1000000.0
#define MIXER_EVENT_CAPACITY 64

#include <stdbool.h>
#include <string.h>
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
#include "atomics.h"
#include "sample_ring.h"
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

// Where the decoder gets its packets: 1 with a packet (an empty one = the end, drain), -1 when aborted
typedef int (*DecoderGetPacket)(void *ctx, AVPacket *pkt, int *serial);

typedef struct Decoder {
    AVPacket *pkt;
    PacketQueue *queue;                 // its serial and abort state (local tracks put no packets in it)
    DecoderGetPacket get_packet;
    void *get_ctx;
    AVCodecContext *avctx;
    int32_t pkt_serial;
    int32_t finished;
    int32_t packet_pending;
    int64_t start_pts;
    AVRational start_pts_tb;
    int64_t next_pts;
    AVRational next_pts_tb;
} Decoder;

//+-----------------------------------------------------+
//| Tracks                                              |
//+-----------------------------------------------------+


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

// A fade curve sampled at FADE_CURVE_STEPS + 1 points (t = 0..1, linear in between), made on the control thread when
// the fade is configured: the mixer never computes a curve, nor calls an application's custom one
#define FADE_CURVE_STEPS 256
typedef struct FadeCurve {
    float g[FADE_CURVE_STEPS + 1];
} FadeCurve;

// TrackState.gain_mb holds the ReplayGain gain in 0.01 dB and whether a change to it ramps slowly (over 2 s instead of
// 10 ms: a value that arrived after the track started). One atomic for both, so the flag can not outlive its value.
static inline int gain_pack(const int centi_db, const bool slow) { return centi_db * 2 + (slow ? 1 : 0); }
static inline int gain_centi_db(const int packed) { return (packed - (packed & 1)) / 2; }
static inline bool gain_is_slow(const int packed) { return (packed & 1) != 0; }

// Worker only: what the trimmer keeps of the decoded frames of one serial (sample-exact ranges and seeks)
typedef struct TrimState {
    int32_t serial;                     // the serial these values are for, -1 = none yet
    bool active;                        // anything to cut or count in this serial
    bool seek_floor;                    // floor is a seek target: back to the range start once it is reached
    bool end_armed;                     // a frame before the range end was seen: reaching the end ends (or loops)
    int64_t floor;                      // first sample to keep, INT64_MIN = no limit
    int64_t next;                       // the sample expected next, AV_NOPTS_VALUE = unknown
    int64_t base;                       // where a frame without a timestamp starts after a seek
    // Resampled output: the file-rate cut keeps `margin` samples more on each side (its start on a sample that is
    // also one of the output rate's, every `align` samples), so the resampler is settled at both ends of a range and
    // two ranges of a file meet on the same output samples; the exact cut is then made at the output rate
    int64_t margin, align;
} TrimState;

typedef struct TrackState {
    struct AudioPlayer *ap;
    int32_t handle;
    char *filename;
    char *filtergraph_str;              // per-track user filters, may be NULL
    double range_start_s;               // AuPlayConfig.start_seconds as given (0 = from the start)
    double range_len_s;                 // AuPlayConfig.length_seconds as given (0 = to the end)
    AuFade fade_in_override;            // only used when has_fade_in_override
    FadeCurve fade_in_curve;            // its curve, sampled when the track is created
    bool has_fade_in_override;
    AuReplayGain replaygain_override;   // only used when has_replaygain_override
    bool has_replaygain_override;
    const AuMediaInfo *info;            // a reference; replaced (never changed) under seek_mutex

    // ReplayGain: target gain (any thread, see gain_pack()), ramped in the mixer
    SDL_atomic_t gain_mb;
    float gain_cur;                     // SDL callback only
    bool gain_started;                  // SDL callback only
    float gain_seen;                    // SDL callback only: target the current ramp heads for
    float gain_step;                    // SDL callback only: per-sample change of the current ramp
    int32_t gain_packed;                // SDL callback only: gain_mb the target below was computed for
    float gain_target;                  // SDL callback only: linear gain of gain_packed (powf only on a change)

    // ---- threads ----
    // The worker opens the input and decodes; for a local file it also reads it. A network stream also gets a
    // reader thread that reads ahead into audio_queue (a stalled connection must not stop decoding what is queued).
    SDL_Thread *worker_tid;
    SDL_Thread *reader_tid;
    SDL_atomic_t abort_request;
    SDL_atomic_t seek_pending;          // 1 = a seek request waits (set under seek_mutex, cleared when it is done)
    SDL_cond *continue_read_thread;     // wakes whoever reads (worker or reader): seeks and aborts
    SDL_mutex *wait_mutex;
    int32_t read_errors;                // worker: read errors in a row (local files)
    SDL_atomic_t worker_exited;         // 1 = the worker has returned (joining it does not wait)

    // ---- seek requests (control thread -> read thread), guarded by seek_mutex ----
    SDL_mutex *seek_mutex;              // also guards filtergraph_str, filter_cmds and the seek handoff below
    FilterCmd *filter_cmds;             // pending avfilter_graph_send_command() calls
    bool seek_req;
    double seek_pos;                    // seconds into the track (the range)
    double pending_seek_percent;        // < 0 = none; resolved by the read thread once the duration is known
    // The seek that started serial seek_done_serial (-1 = none): the sample it aims for and where the demuxer was
    // asked to go (the trimmer cuts to the first, and counts from the second when frames have no timestamps)
    int32_t seek_done_serial;
    int64_t seek_done_sample, seek_done_base;

    // ---- demux / decode (read + audio threads) ----
    AVFormatContext *ic;
    int32_t audio_stream;
    AVStream *audio_st;
    PacketQueue audio_queue;
    Decoder audio_decoder;
    SampleRing ring;                    // decoded audio for the mixer (audio thread -> SDL callback)
    SDL_mutex *ring_mutex;              // the audio thread waits for room on ring_cond; the mixer never signals it,
    SDL_cond *ring_cond;                // seeks and aborts do
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
    int32_t convert_channels;           // and its channel count
    char *http_headers;                 // PlayAudioConfig.http_headers (owned), network streams only
    // Network tracks (set at creation): the read thread fills buffer_target_s before the mixer takes anything,
    // again after the mixer ran dry while playing, and reads ahead up to max_buffer_s
    bool network;
    double buffer_target_s, max_buffer_s;
    bool live;                          // read thread: no duration (internet radio, live HLS)
    bool chained_ogg;                   // read thread: a chained Ogg file, which FFmpeg can not seek past its first part
    double pts_offset;                  // read thread, before the decoder starts: the container's start time (s),
                                        // taken off frame timestamps so positions count from 0
    // The play range, exact to the sample at the file's rate. Set by the worker before `opened` (fixed after it):
    bool has_range;                     // a start or a length, on a file that is not live
    int32_t range_rate;                 // the decoder's sample rate the samples count in
    int64_t range_start_sample;         // first sample, counted from the container's start
    int64_t range_end_sample;           // first sample past the range, INT64_MAX = to the end of the file
    int64_t pts_offset_samples;         // pts_offset in range_rate samples
    int64_t ts_tol;                     // timestamp jitter that is counted through: the stream time base, + 1 sample
    double origin_s;                    // range_start_sample / range_rate: taken off positions (they count from 0)
    double preroll_s;                   // decoded before a seek target or the range start and dropped (lossy codecs)
    double duration_s;                  // what plays: the range, or the file; < 0 = unknown
    bool range_end_hit;                 // worker: the trimmer reached the range end (local files end or loop there)
    TrimState trim;                     // worker only
    SDL_atomic_t buffering;             // 1 = waiting for data: the mixer takes nothing, the position holds
    SDL_atomic_t buffered_ms;           // read thread: audio queued, for on_buffering
    SDL_atomic_t read_ended;            // 1 = the reader is at the end (demux_eof, for the worker to see)
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
    int64_t ts_shift, ts_next;          // read thread: continue_timestamps() (stream time base)
    double play_clock;                  // SDL callback: file time played
    int clock_serial;                   // SDL callback: the serial play_clock counts for
    bool icy;                           // read thread: the server sends ICY metadata (icy-metaint)
    double icy_next_check;              // read thread: read_clock of the next look at the ICY title
    bool demux_eof;                     // read thread only

    // ---- status published by the worker threads ----
    int32_t opened;                     // atomics.h: 1 once the stream is open and the range is set (and fixed)
    int32_t eos_serial;                 // atomics.h: serial whose last frame is in the ring, -1 = none
    SDL_atomic_t loops_remaining;       // -1 infinite, 0 none
    SDL_atomic_t loops_started;         // read thread: loops it has seeked back for (the mixer reports each once)
    SDL_atomic_t filter_reconfigure;    // 1 = rebuild the convert graph before the next frame (device changed)
    SDL_atomic_t user_filters_changed;  // 1 = rebuild the user graph before the next frame
    SDL_atomic_t had_error;             // 1 = the track ends as an error, even if some audio played

    // ---- playback cursor, SDL callback only ----
    double position;                    // seconds, position of the next sample to be played
    double last_frame_pts;              // used to detect loop restarts
    double play_speed;                  // speed of the audio being played (file seconds per output second)
    bool started;                       // produced at least one sample
    bool announced;                     // MIX_EV_TRACK_STARTED has been sent
    int32_t loops_heard;                // loops reported so far (compared with loops_started)
    int32_t last_frame_serial;          // serial of last_frame_pts

    // ---- under the device lock ----
    TrackEndReason fade_reason;         // why it is fading out (while it is the mixer's outgoing track)

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
    FadeCurve curve;
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
    FadeCurve end_curve;
    double min_track_seconds;
    FadeCurve linear_curve;         // for the short ramps of pause and resume without a configured fade

    // Scratch buffers, interleaved float, allocated with the device
    float *mix_buf;
    float *voice_buf;
    int32_t buf_frames;

    // Callback -> control thread events
    MixerEvent events[MIXER_EVENT_CAPACITY];
    int32_t n_events;
    bool events_overflowed;
    bool wake_control;              // an event was pushed this period: wake the control thread

    uint32_t dither_state;
} Mixer;

// Whether a track is in one of the mixer's slots (current, queued or fading out). Device lock held, or no device.
static inline bool mixer_has_track(const Mixer *m, const struct TrackState *t) {
    return t && (m->current == t || m->next == t || m->outgoing == t);
}

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
    CMD_SEEK,
    CMD_SEEK_PERCENT,
    CMD_SET_LOOP,
    CMD_CONFIGURE_DEVICE,
    CMD_SET_CROSSFADE,
    CMD_SET_GLOBAL_FILTERS,
    CMD_SET_DOWNMIX,
    CMD_SET_UPMIX,
    CMD_SET_SOURCE_FILTERS,
    CMD_SET_PREPARE_NEXT,
    CMD_SET_OUTPUT_FORMAT,
    CMD_ENABLE_TAP,
    CMD_SET_REPLAYGAIN,
    CMD_GAIN_CACHE_CHANGED,
    CMD_UPDATE_TRACK_FILTERS,
    CMD_FILTER_COMMAND,
    CMD_SHUTDOWN,
    MSG_LOG,
    MSG_TRACK_OPENED,
    MSG_TRACK_FAILED,
    MSG_TRACK_DEMUX_EOF,
    MSG_TRACK_SEEKED,
    MSG_TRACK_EXITED,       // the worker has ended: the track can be reaped
} MsgType;

typedef struct Msg {
    MsgType type;
    int32_t handle;
    char *str;                      // owned
    char *str2;                     // owned
    char *str3;                     // owned
    AuPlayConfig play_config;       // filters points at str2, http_headers at str3
    bool has_play_config;
    AuFade fade_in_override;
    bool has_fade_in_override;
    AuCrossfade crossfade;
    AuReplayGainConfig replaygain;
    AuReplayGain replaygain_override;
    bool has_replaygain_override;
    int64_t i64;
    double d;
    bool b;
    int32_t level;
    int32_t pause_state;            // AudioPlayer.pause_state when the command was posted
    int64_t request;
    SDL_sem *done;                  // CMD_SHUTDOWN: posted when it is done
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
    double period;                  // seconds of audio per callback (SDL's buffer), 0 = no callback yet
    bool buffering;                 // the track waits for network data
    int32_t tap_end;                // MeterTap.published at callback_time
    int32_t tap_gen;                // MeterTap.gen it counts for
} StatusSnapshot;

// What the mixer plays, for level meters and the spectrum (au_get_levels(), au_spectrum_read()): the mix after
// ReplayGain and fades, with the pause fade, before master volume and mute; zeros while paused. A history the audio
// callback overwrites as it goes, never waiting for a reader; readers copy what they need and check it was not
// overwritten meanwhile. buf, frames, channels, rate and gen change only on the control thread with tap_mutex and the
// device lock held: the callback reads them under the device lock, readers under tap_mutex.
typedef struct MeterTap {
    int32_t *buf;                   // float bits, interleaved, frames * channels (atomics.h stores: TSan sees them)
    uint32_t frames;                // a power of two
    int32_t channels, rate;         // the output format it was made for (the callback writes nothing otherwise)
    int32_t gen;                    // bumped with every new buffer
    int32_t written;                // atomics.h: frames counted as written, before their data (wraps)
    int32_t published;              // atomics.h: frames whose data is in (wraps)
} MeterTap;

// A float kept in an SDL_atomic_t (its bits)
static inline void atomic_set_float(SDL_atomic_t *a, const float v) {
    int32_t i;
    memcpy(&i, &v, sizeof(i));
    SDL_AtomicSet(a, i);
}

static inline float atomic_get_float(SDL_atomic_t *a) {
    const int32_t i = SDL_AtomicGet(a);
    float v;
    memcpy(&v, &i, sizeof(v));
    return v;
}

// What any thread may read about a track without waiting for the control thread (au_track_info(),
// au_track_gain_db()), kept in AudioPlayer.pubs under pub_mutex. The mutex is never held while taking another lock.
typedef struct TrackPub {
    int32_t handle;
    bool gain_known;                // the gain below is the one in use
    double gain_db;
    const AuMediaInfo *info;        // a reference, NULL until the track is open
} TrackPub;

typedef struct AudioPlayer {
    // ---- configuration ----
    SDL_atomic_t volume;                // linear gain 0..1, a float (atomic_get_float())
    SDL_atomic_t muted;
    SDL_atomic_t paused;                // applied (control thread)
    // For au_get_status(): bit 0 = paused as last set (au_pause(), au_play() unpausing); the other bits count the
    // calls that set it, so the control thread's own unpause (au_queue() starting a track) can not undo a newer call
    SDL_atomic_t pause_state;
    SDL_atomic_t loop_count;            // default for new tracks, -1 infinite
    double prepare_next_seconds;        // control thread (after au_init())
    int32_t output_channels;            // control thread (after au_init()); 0 = device preference
    int32_t output_sample_rate;         // control thread (after au_init()); 0 = device preference
    AuCrossfade crossfade;              // control thread only
    FadeCurve play_curve, skip_curve, stop_curve, pause_curve; // control thread: its curves, sampled when set
    bool use_soxr;
    int32_t filter_nbthreads;

    // ---- published track state (TrackPub) ----
    SDL_mutex *pub_mutex;
    TrackPub *pubs;
    int32_t n_pubs, cap_pubs;

    // ---- events ----
    AuEventFn on_event;                 // NULL = queued for au_poll_event() (lib/events.c)
    void *user_data;

    // ---- control thread ----
    SDL_Thread *control_thread;
    SDL_threadID control_thread_id;     // set by the control thread itself before au_init() returns
    SDL_sem *started_sem;               // au_init() waits on it for control_thread_id
    SDL_atomic_t shutting_down;
    SDL_mutex *msg_mutex;
    SDL_sem *wake_sem;                  // posted for every message and by the mixer when it raises an event
    Msg *msg_head;
    Msg *msg_tail;
    SDL_atomic_t handle_count;
    SDL_atomic_t request_count;
    TrackState **tracks;                // every live track, control thread only
    int32_t n_tracks;
    int32_t cap_tracks;
    TrackState **dying;                 // ended tracks whose worker is stopping, control thread only
    int32_t n_dying;
    int32_t cap_dying;

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
    AU_UPMIX upmix;                     // guarded by format_mutex
    char *upmix_filters;                // guarded by format_mutex, AU_UPMIX_CUSTOM only
    char *source_filters;               // guarded by format_mutex: before the conversion, NULL = none
    AudioParams audio_target;           // float32 interleaved at the device rate/layout, guarded by format_mutex
    SDL_AudioDeviceID device_id;
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

    // ---- ring storage kept between tracks (track.c), guarded by ring_pool_mutex ----
    SDL_mutex *ring_pool_mutex;
    float *ring_pool[4];
    size_t ring_pool_floats[4];
    int32_t n_ring_pool;

    // ---- mixer (SDL callback; others under SDL_LockAudioDevice) ----
    Mixer mixer;

    // ---- status for getters ----
    SDL_mutex *status_mutex;            // a mutex (not a spinlock) so ThreadSanitizer can follow it
    StatusSnapshot status;

    // ---- meters ----
    MeterTap tap;
    SDL_mutex *tap_mutex;               // readers and the control thread; never the callback. Taken before the device lock
    SDL_atomic_t tap_requested;         // 1 once a reader asked for the tap (CMD_ENABLE_TAP is posted once)
    bool tap_enabled;                   // control thread: the tap is kept, and remade for every output format
} AudioPlayer;

// Shared helpers implemented in ffaudio.c
void ff_log(enum AU_LOG_LEVEL level, const char *fmt, ...);
// Any thread: ask the control thread to start the meter tap (once)
void ff_request_tap(struct AudioPlayer *ap);
// The player, NULL without one; like au_get_status(), a getter may not race au_shutdown()
struct AudioPlayer *ff_player(void);
void ff_post_track_msg(MsgType type, int32_t handle, double d);
// Publish a track's info (copied) or gain for au_track_info() / au_track_gain_db(); any thread
void ff_publish_info(struct AudioPlayer *ap, int32_t handle, const AuMediaInfo *info);
void ff_publish_gain(struct AudioPlayer *ap, int32_t handle, double gain_db);

#endif //FFAUDIO_GLOBALS_H
