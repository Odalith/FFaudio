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
#include "probe.h"
#include "network.h"
#include "gain_cache.h"
#include "output.h"
#include <math.h>
#include "decoder_utils.h"
#include "filtergraph.h"
#include "packet_queue_utils.h"
#include "libavutil/opt.h"

// Network tracks read ahead by time (max_buffer_seconds); this caps the bytes for very high bit rates
#define NET_MAX_QUEUE_SIZE (32 * 1024 * 1024)
// Packets without durations (some live streams): this many bytes count as a full buffer
#define NET_BUFFER_FALLBACK_BYTES (256 * 1024)
// A backwards jump of the frame timestamps larger than this, without a seek, means the track looped
#define LOOP_DETECT_SECONDS 0.25
// Most audio (in file seconds) the speed stage can hold between its input and output
#define SPEED_MAX_LAG_SECONDS 0.2

static int track_thread(void *arg);
static int net_reader(void *arg);
static void decode_loop(TrackState *t);
static void wake_producer(TrackState *t);

//+-----------------------------------------------------+
//| Helpers                                             |
//+-----------------------------------------------------+

static int decode_interrupt_cb(void *ctx) {
    TrackState *t = ctx;
    return SDL_AtomicGet(&t->abort_request);
}

// Seconds of audio in the packet queue (0 when the packets have no durations)
static double queued_seconds(const TrackState *t) {
    PacketQueue *q = (PacketQueue *)&t->audio_queue;
    SDL_LockMutex(q->mutex);
    const int64_t d = q->duration;
    SDL_UnlockMutex(q->mutex);
    return t->audio_st ? d * av_q2d(t->audio_st->time_base) : 0;
}

static int queue_byte_size(PacketQueue *q);

static void free_stream_meta(StreamMeta *m);

// Queue "now playing" information to be reported when playback reaches `position` (read thread)
static void push_stream_meta(TrackState *t, const double position, const char *title, const char *artist) {
    // The same title again (e.g. the stream's tags re-sent) is no change
    char *key = av_asprintf("%s\n%s", title ? title : "", artist ? artist : "");
    if (!key) return;
    if (t->last_meta && !strcmp(key, t->last_meta)) {
        av_free(key);
        return;
    }
    av_free(t->last_meta);
    t->last_meta = key;
    StreamMeta *m = av_mallocz(sizeof(StreamMeta));
    if (!m) return;
    m->position = position;
    m->title = text_to_utf8(title ? title : "");
    m->artist = artist && artist[0] ? text_to_utf8(artist) : NULL;
    SDL_LockMutex(t->seek_mutex);
    int n = 0;
    StreamMeta **tail = &t->stream_meta;
    while (*tail) { tail = &(*tail)->next; n++; }
    *tail = m;
    if (n >= 512) { // far more than 30 s of read-ahead can hold: this track is not being played
        StreamMeta *old = t->stream_meta;
        t->stream_meta = old->next;
        old->next = NULL;
        free_stream_meta(old);
    }
    SDL_UnlockMutex(t->seek_mutex);
}

// "StreamTitle='Artist - Title';StreamUrl='...';" -> title (and artist when there is " - ")
static void push_icy_title(TrackState *t, const double position, const char *packet) {
    const char *p = strstr(packet, "StreamTitle='");
    if (!p) return;
    p += 13;
    const char *end = strstr(p, "';");
    if (!end) end = p + strlen(p);
    char *text = av_strndup(p, (size_t)(end - p));
    if (!text) return;
    char *sep = strstr(text, " - ");
    if (sep) {
        *sep = '\0';
        push_stream_meta(t, position, sep + 3, text);
    }
    else {
        push_stream_meta(t, position, text, NULL);
    }
    av_free(text);
}

// After every audio packet: new ICY titles (checked a few times a second) and tags that changed inside the stream
// Containers that may start their timestamps again (FFmpeg's AVFMT_TS_DISCONT: Ogg, MPEG-TS, HLS) do so at every part
// of a chained Ogg stream, which is how Ogg radio changes songs. A jump back with no seek or loop continues where the
// previous packet ended instead, so the position does not go back to 0 with every song.
static void continue_timestamps(TrackState *t, const AVFormatContext *ic, AVPacket *pkt) {
    if (!(ic->iformat->flags & AVFMT_TS_DISCONT)) return;
    const AVStream *st = ic->streams[pkt->stream_index];
    const int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
    if (ts == AV_NOPTS_VALUE) return;
    const int64_t tolerance = av_rescale_q(AV_TIME_BASE / 2, AV_TIME_BASE_Q, st->time_base);
    if (t->ts_next != AV_NOPTS_VALUE && ts + t->ts_shift < t->ts_next - tolerance)
        t->ts_shift = t->ts_next - ts;
    if (pkt->pts != AV_NOPTS_VALUE) pkt->pts += t->ts_shift;
    if (pkt->dts != AV_NOPTS_VALUE) pkt->dts += t->ts_shift;
    t->ts_next = ts + t->ts_shift + (pkt->duration > 0 ? pkt->duration : 0);
}

static void reset_timestamps(TrackState *t) {
    t->ts_shift = 0;
    t->ts_next = AV_NOPTS_VALUE;
}

static void check_stream_metadata(TrackState *t, AVFormatContext *ic, const AVPacket *pkt) {
    AVStream *st = ic->streams[pkt->stream_index];
    // Where this packet starts in the file time read so far; then count it
    const double pos = t->read_clock;
    const int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
    double d = pkt->duration > 0 ? pkt->duration * av_q2d(st->time_base) : 0;
    if (d <= 0 && ts != AV_NOPTS_VALUE && t->last_pkt_ts != AV_NOPTS_VALUE && ts > t->last_pkt_ts)
        d = (ts - t->last_pkt_ts) * av_q2d(st->time_base);   // no duration: the gap to the previous packet
    if (d > 0 && d < 10) t->read_clock += d;
    t->last_pkt_ts = ts;

    // Every quarter second of file time (the position matters, not the reading time: the reader runs far ahead); the
    // option is a copy, allocated on every call
    if (t->icy && t->read_clock >= t->icy_next_check) {
        t->icy_next_check = t->read_clock + 0.25;
        uint8_t *icy = NULL;
        if (av_opt_get(ic->pb, "icy_metadata_packet", AV_OPT_SEARCH_CHILDREN, &icy) >= 0 && icy && icy[0] &&
            (!t->last_icy || strcmp((char *)icy, t->last_icy) != 0)) {
            av_free(t->last_icy);
            t->last_icy = av_strdup((char *)icy);
            push_icy_title(t, pos, (char *)icy);
        }
        av_free(icy);
    }
    if ((st->event_flags & AVSTREAM_EVENT_FLAG_METADATA_UPDATED) || (ic->event_flags & AVFMT_EVENT_FLAG_METADATA_UPDATED)) {
        st->event_flags &= ~AVSTREAM_EVENT_FLAG_METADATA_UPDATED;
        ic->event_flags &= ~AVFMT_EVENT_FLAG_METADATA_UPDATED;
        // FFmpeg also flags ICY changes; their titles come from above, the tags here are the file's own (ID3)
        if (t->icy) return;
        const AVDictionaryEntry *title = av_dict_get(st->metadata, "title", NULL, 0);
        if (!title) title = av_dict_get(ic->metadata, "title", NULL, 0);
        const AVDictionaryEntry *artist = av_dict_get(st->metadata, "artist", NULL, 0);
        if (!artist) artist = av_dict_get(ic->metadata, "artist", NULL, 0);
        if (title || artist) push_stream_meta(t, pos, title ? title->value : "", artist ? artist->value : NULL);
    }
}

// Station details from the ICY response headers ("icy-name: ...") into the track's tags
static bool add_icy_headers(AVFormatContext *ic, AuMediaInfo *info) {
    uint8_t *headers = NULL;
    bool icy_on = false;
    if (!ic->pb || av_opt_get(ic->pb, "icy_metadata_headers", AV_OPT_SEARCH_CHILDREN, &headers) < 0 || !headers)
        return false;
    for (char *line = (char *)headers; *line;) {
        char *end = strchr(line, '\n');
        if (end) *end = '\0';
        char *colon = strchr(line, ':');
        // FFmpeg keeps icy-metaint for itself; any other icy- header means the server speaks ICY
        if (colon && !strncmp(line, "icy-", 4)) {
            icy_on = true;
            *colon = '\0';
            const char *v = colon + 1;
            while (*v == ' ') v++;
            char *value = text_to_utf8(v);
            size_t n = value ? strlen(value) : 0;
            while (n && (value[n - 1] == '\r' || value[n - 1] == ' ')) value[--n] = '\0';
            if (value && value[0]) av_dict_set(&info->tags, line, value, 0);
            av_free(value);
        }
        if (!end) break;
        line = end + 1;
    }
    av_free(headers);
    return icy_on;
}

// Network tracks: end the wait for data once enough is queued (or nothing more will come). The reader calls it for
// every packet; the worker after writing audio, since the reader sleeps while its queue is full or at the end.
static void update_buffering(TrackState *t) {
    if (!t->network) return;
    const double s = queued_seconds(t);
    SDL_AtomicSet(&t->buffered_ms, (int)(s * 1000));
    if (!SDL_AtomicGet(&t->buffering)) return;
    if (SDL_AtomicGet(&t->read_ended) || SDL_AtomicGet(&t->had_error) || s >= t->buffer_target_s ||
        (s == 0 && queue_byte_size(&t->audio_queue) >= NET_BUFFER_FALLBACK_BYTES))
        SDL_AtomicSet(&t->buffering, 0);
}

static int queue_byte_size(PacketQueue *q) {
    SDL_LockMutex(q->mutex);
    const int size = q->size;
    SDL_UnlockMutex(q->mutex);
    return size;
}

static void wait_ms(TrackState *t, int ms) {
    SDL_LockMutex(t->wait_mutex);
    if (!SDL_AtomicGet(&t->abort_request))
        SDL_CondWaitTimeout(t->continue_read_thread, t->wait_mutex, ms);
    SDL_UnlockMutex(t->wait_mutex);
}

static void wake_read_thread(TrackState *t) {
    SDL_LockMutex(t->wait_mutex);
    SDL_CondSignal(t->continue_read_thread);
    SDL_UnlockMutex(t->wait_mutex);
}

// Whoever reads (worker or reader) waits for a seek request or an abort, or `ms` at most. The check is made under
// wait_mutex, which the requests signal under, so none is missed.
static void wait_for_request(TrackState *t, const int ms) {
    SDL_LockMutex(t->wait_mutex);
    if (!SDL_AtomicGet(&t->abort_request) && !SDL_AtomicGet(&t->seek_pending))
        SDL_CondWaitTimeout(t->continue_read_thread, t->wait_mutex, (Uint32)ms);
    SDL_UnlockMutex(t->wait_mutex);
}

double track_start_position(const TrackState *t) {
    return t->start_time != AV_NOPTS_VALUE ? t->start_time / AV_TIME_BASE_DOUBLE : 0.0;
}

// No lock: end_pos_us is written once, before `opened` is set, and read only after `opened` has been seen (the audio
// callback calls this every period)
double track_end_position(TrackState *t) {
    if (!ffa_load_acquire(&t->opened)) return -1;
    const int64_t end = t->end_pos_us;
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
    t->pending_seek_percent = -1;
    t->last_frame_pts = NAN;
    t->play_speed = 1.0;
    ffa_store_relaxed(&t->eos_serial, -1);
    SDL_AtomicSet(&t->loops_remaining, SDL_AtomicGet(&ap->loop_count));

    if (!(t->filename = av_strdup(filename))) goto fail;
    t->buffering_reported = -1;
    t->clock_serial = -1;
    t->last_pkt_ts = AV_NOPTS_VALUE;
    reset_timestamps(t);
    if ((t->network = au_is_network_url(filename))) {
        const AuNetworkConfig nc = network_config(NULL, 0);
        t->buffer_target_s = nc.buffer_seconds;
        t->max_buffer_s = nc.max_buffer_seconds;
        SDL_AtomicSet(&t->buffering, 1);   // nothing plays until the first buffer_seconds are in
    }

    if (config) {
        if (config->av_filtergraph && config->av_filtergraph[0] && !(t->filtergraph_str = av_strdup(config->av_filtergraph)))
            goto fail;
        if (config->http_headers && config->http_headers[0] && !(t->http_headers = av_strdup(config->http_headers)))
            goto fail;
        if (config->play_duration > 0)
            t->play_duration = SecondsToMicroseconds(config->play_duration);
        if (config->skip_seconds > 0)
            t->start_time = SecondsToMicroseconds(config->skip_seconds);
        if (config->fade_in) {
            t->fade_in_override = *config->fade_in;
            fade_curve_build(&t->fade_in_curve, config->fade_in); // control thread: a custom curve is called here
            t->has_fade_in_override = true;
        }
        if (config->replaygain) {
            t->replaygain_override = *config->replaygain;
            t->has_replaygain_override = true;
        }
    }

    if (packet_queue_init(&t->audio_queue) < 0) goto fail;
    if (!(t->ring_mutex = SDL_CreateMutex())) goto fail;
    if (!(t->ring_cond = SDL_CreateCond())) goto fail;
    if (!(t->continue_read_thread = SDL_CreateCond())) goto fail;
    if (!(t->wait_mutex = SDL_CreateMutex())) goto fail;
    if (!(t->seek_mutex = SDL_CreateMutex())) goto fail;

    char name[32];
    snprintf(name, sizeof(name), "ffa_track_%d", handle);
    if (!(t->worker_tid = SDL_CreateThread(track_thread, name, t))) {
        ff_log(AU_LOG_FATAL, "SDL_CreateThread(): %s", SDL_GetError());
        goto fail;
    }
    return t;

fail:
    track_destroy(t);
    return NULL;
}

static void free_stream_meta(StreamMeta *m) {
    while (m) {
        StreamMeta *next = m->next;
        av_free(m->title);
        av_free(m->artist);
        av_free(m);
        m = next;
    }
}

void track_stop(TrackState *t) {
    if (!t || !t->worker_tid) return;
    SDL_AtomicSet(&t->abort_request, 1);
    packet_queue_abort(&t->audio_queue);
    if (t->ring_mutex && t->ring_cond) wake_producer(t);
    if (t->wait_mutex && t->continue_read_thread) wake_read_thread(t);
    // A decoder waiting for an output device (ensure_convert_graph) sees the abort now instead of after 100 ms
    SDL_LockMutex(t->ap->format_mutex);
    SDL_CondBroadcast(t->ap->format_cond);
    SDL_UnlockMutex(t->ap->format_mutex);
}

bool track_stopped(TrackState *t) {
    return !t->worker_tid || SDL_AtomicGet(&t->worker_exited);
}

void track_destroy(TrackState *t) {
    if (!t) return;

    if (t->worker_tid) {
        SDL_AtomicSet(&t->abort_request, 1);
        packet_queue_abort(&t->audio_queue);
        if (t->ring_mutex && t->ring_cond) {
            SDL_LockMutex(t->ring_mutex);
            SDL_CondSignal(t->ring_cond);
            SDL_UnlockMutex(t->ring_mutex);
        }
        if (t->wait_mutex && t->continue_read_thread) wake_read_thread(t);
        // A decoder waiting for an output device (ensure_convert_graph) sees the abort now instead of after 100 ms
        SDL_LockMutex(t->ap->format_mutex);
        SDL_CondBroadcast(t->ap->format_cond);
        SDL_UnlockMutex(t->ap->format_mutex);
        // The worker stops the reader thread (network) and closes the input before it returns
        SDL_WaitThread(t->worker_tid, NULL);
        t->worker_tid = NULL;
    }

    {
        float *old;
        size_t old_floats;
        ring_set_storage(&t->ring, NULL, 1, 0, 0, &old, &old_floats); // nothing reads it any more
        if (old) ring_storage_give(t->ap, old, old_floats);
    }
    SDL_DestroyCond(t->ring_cond);
    SDL_DestroyMutex(t->ring_mutex);
    if (t->audio_queue.mutex) packet_queue_destroy(&t->audio_queue);
    av_channel_layout_uninit(&t->audio_filter_src.ch_layout);
    av_dict_free(&t->format_opts_n);
    free_stream_meta(t->stream_meta);
    av_free(t->last_icy);
    av_free(t->last_meta);
    SDL_DestroyCond(t->continue_read_thread);
    SDL_DestroyMutex(t->wait_mutex);
    SDL_DestroyMutex(t->seek_mutex);
    au_media_info_free(t->info);
    av_free(t->filename);
    av_free(t->filtergraph_str);
    av_free(t->http_headers);
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
    SDL_AtomicSet(&t->seek_pending, 1);
    SDL_UnlockMutex(t->seek_mutex);
    wake_read_thread(t);
    wake_producer(t); // a local track's worker may be waiting for room in the ring: it seeks itself
}

void track_request_seek_percent(TrackState *t, const double percent) {
    // Resolved by the read thread, which knows the duration once the file is open
    SDL_LockMutex(t->seek_mutex);
    t->pending_seek_percent = percent;
    SDL_AtomicSet(&t->seek_pending, 1);
    SDL_UnlockMutex(t->seek_mutex);
    wake_read_thread(t);
    wake_producer(t);
}

void track_update_gain(TrackState *t, const bool late) {
    AudioPlayer *ap = t->ap;
    SDL_LockMutex(ap->format_mutex);
    const AuReplayGainConfig cfg = ap->replaygain;
    SDL_UnlockMutex(ap->format_mutex);

    // Values come from, in order: PlayAudioConfig.replaygain, the ReplayGain cache, the file's tags
    SDL_LockMutex(t->seek_mutex);
    const bool known = t->info || t->has_replaygain_override;
    const bool has_override = t->has_replaygain_override;
    AuReplayGainInfo rg;
    if (has_override) rg = t->replaygain_override;
    else rg = au_media_replaygain(t->info);
    SDL_UnlockMutex(t->seek_mutex);

    if (!known) return; // not open yet: the read thread calls this again once the tags are known
    if (!has_override) {
        AuReplayGainInfo cached;
        if (gain_cache_lookup(t->filename, &cached)) rg = cached;
    }
    const double db = replaygain_gain_db(&cfg, &rg);
    SDL_AtomicSet(&t->gain_mb, gain_pack((int)lrint(db * 100.0), late));
}

#define DEFAULT_MEASURE_WAIT_MS 500

// Read thread, before the decoder starts: a track without any ReplayGain value gets an urgent measurement
// (AuReplayGainConfig.measure_missing) and waits for it a little, so it can start at the right level
static void wait_for_measurement(TrackState *t, const AuMediaInfo *info) {
    AudioPlayer *ap = t->ap;
    SDL_LockMutex(ap->format_mutex);
    const AuReplayGainConfig cfg = ap->replaygain;
    SDL_UnlockMutex(ap->format_mutex);

    if (cfg.mode == AU_REPLAYGAIN_OFF || !cfg.measure_missing || t->has_replaygain_override) return;
    if (au_is_network_url(t->filename) && !network_config(NULL, 0).measure_streams) return; // a download
    const AuReplayGainInfo tags = au_media_replaygain(info);
    if (tags.has_track_gain || tags.has_album_gain) return;
    if (gain_cache_lookup(t->filename, NULL)) return;

    const unsigned failures = gain_cache_failure_mark();
    if (!gain_scan_request_for_player(t->filename)) return; // could not be measured before
    const int wait_ms = cfg.measure_wait_ms == 0 ? DEFAULT_MEASURE_WAIT_MS : cfg.measure_wait_ms;
    if (wait_ms <= 0) return;
    switch (gain_cache_wait(t->filename, wait_ms, &t->abort_request, failures)) {
        case GAIN_WAIT_TIMEOUT:
            ff_log(AU_LOG_INFO, "'%s' starts before its loudness is measured; the gain follows when it is ready",
                   t->filename);
            break;
        case GAIN_WAIT_FAILED:
            ff_log(AU_LOG_INFO, "'%s' could not be measured; it plays at the fallback gain", t->filename);
            break;
        default:
            break;
    }
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
//| Reading                                             |
//+-----------------------------------------------------+

static int local_get_packet(void *ctx, AVPacket *pkt, int *serial);

static int queue_get_packet(void *ctx, AVPacket *pkt, int *serial) {
    TrackState *t = ctx;
    return packet_queue_get(&t->audio_queue, pkt, 1, serial);
}

// Open the decoder for the stream (no thread: the worker runs it)
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

    const AVCodec *codec = avcodec_find_decoder(avctx->codec_id);
    if (!codec) {
        ff_log(AU_LOG_WARNING, "No decoder could be found for codec %s", avcodec_get_name(avctx->codec_id));
        ret = AVERROR(EINVAL);
        goto fail;
    }
    avctx->codec_id = codec->id;

    // One thread: audio decodes hundreds of times faster than real time, and "auto" gave e.g. the FLAC decoder a
    // frame thread per core (13 threads per track on 12 cores) for nothing
    av_dict_set(&opts, "threads", "1", 0);

    if ((ret = avcodec_open2(avctx, codec, &opts)) < 0)
        goto fail;

    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;

    t->audio_filter_src.freq = avctx->sample_rate;
    if ((ret = av_channel_layout_copy(&t->audio_filter_src.ch_layout, &avctx->ch_layout)) < 0)
        goto fail;
    t->audio_filter_src.fmt = avctx->sample_fmt;

    // Local files are read by the worker between decodes; network streams come through the reader's queue
    if ((ret = decoder_init(&t->audio_decoder, avctx, &t->audio_queue,
                            t->network ? queue_get_packet : local_get_packet, t)) < 0)
        goto fail;
    avctx = NULL; // owned by the decoder now
    t->audio_stream = stream_index;
    t->audio_st = ic->streams[stream_index];

    if (ic->iformat->flags & AVFMT_NOTIMESTAMPS) {
        t->audio_decoder.start_pts = t->audio_st->start_time;
        t->audio_decoder.start_pts_tb = t->audio_st->time_base;
    }
    ret = 0;

fail:
    avcodec_free_context(&avctx);
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

// One more loop to play? Takes it from the count in one step: au_set_loop_count() can store a new count at any time,
// and a separate read and decrement could turn its 0 into -1 (loop forever)
static bool take_loop(TrackState *t) {
    for (;;) {
        const int loops = SDL_AtomicGet(&t->loops_remaining);
        if (loops <= 0) return loops < 0;
        if (SDL_AtomicCAS(&t->loops_remaining, loops, loops - 1)) return true;
    }
}

typedef enum DemuxEnd { DEMUX_LOOPED, DEMUX_ENDED, DEMUX_ENDED_BEFORE } DemuxEnd;

// The demuxer reached the end of what should be played. DEMUX_LOOPED: it seeked back for another loop (no flush, so the
// loop is gapless; the mixer reports the restart when it is heard). DEMUX_ENDED: the end, the first time (the caller
// then drains the decoder with an empty packet).
static DemuxEnd handle_demux_end(TrackState *t) {
    if (!SDL_AtomicGet(&t->had_error) && take_loop(t)) {
        if (seek_file(t, t->start_time != AV_NOPTS_VALUE ? t->start_time : 0) >= 0) {
            SDL_AtomicAdd(&t->loops_started, 1);
            reset_timestamps(t); // the jump back of a loop is meant
            return DEMUX_LOOPED;
        }
        ff_log(AU_LOG_WARNING, "%s: could not seek back to loop", t->filename);
    }
    if (t->demux_eof) return DEMUX_ENDED_BEFORE;
    t->demux_eof = true;
    SDL_AtomicSet(&t->read_ended, 1);
    ff_post_track_msg(MSG_TRACK_DEMUX_EOF, t->handle, 0);
    return DEMUX_ENDED;
}

// A pending seek request, done by whoever reads (the worker for local files, the reader for streams)
static void process_seek_request(TrackState *t) {
    SDL_LockMutex(t->seek_mutex);
    if (t->pending_seek_percent >= 0 && t->end_pos_us != AV_NOPTS_VALUE) {
        const double start = track_start_position(t);
        const double len = FFMAX(0.0, t->end_pos_us / AV_TIME_BASE_DOUBLE - start);
        t->seek_pos = SecondsToMicroseconds(start + len * t->pending_seek_percent / 100.0);
        t->seek_req = true;
        t->pending_seek_percent = -1;
    }
    bool seek_req = t->seek_req;
    const int64_t seek_pos = t->seek_pos;
    t->seek_req = false;
    if (t->pending_seek_percent < 0) SDL_AtomicSet(&t->seek_pending, 0);
    SDL_UnlockMutex(t->seek_mutex);
    if (seek_req && t->live) {
        ff_log(AU_LOG_INFO, "%s: live stream, seeking is ignored", t->filename);
        seek_req = false;
    }
    if (!seek_req) return;

    if (seek_file(t, seek_pos) < 0) {
        ff_log(AU_LOG_ERROR, "%s: error while seeking", t->filename);
        return;
    }
    // New serial: the decoder, the filter graph and the mixer drop everything older
    packet_queue_flush(&t->audio_queue);
    wake_producer(t); // it may be waiting for room with audio from before the seek
    SDL_LockMutex(t->seek_mutex);
    free_stream_meta(t->stream_meta);
    t->stream_meta = NULL;
    SDL_UnlockMutex(t->seek_mutex);
    t->read_clock = 0;   // the mixer starts its play_clock again with the new serial
    t->icy_next_check = 0;
    t->last_pkt_ts = AV_NOPTS_VALUE;
    reset_timestamps(t);
    t->demux_eof = false;
    SDL_AtomicSet(&t->read_ended, 0);
    t->read_errors = 0;
    ff_post_track_msg(MSG_TRACK_SEEKED, t->handle, seek_pos / AV_TIME_BASE_DOUBLE);
}

// After a packet of the audio stream: false when it is past the end of the requested play range
static bool within_play_range(const TrackState *t, const AVPacket *pkt) {
    if (t->play_duration == AV_NOPTS_VALUE) return true;
    const AVStream *st = t->ic->streams[pkt->stream_index];
    const int64_t stream_start = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
    const int64_t pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
    const double pos = (pkt_ts - stream_start) * av_q2d(st->time_base);
    return pkt_ts == AV_NOPTS_VALUE || pos <= t->end_pos_us / AV_TIME_BASE_DOUBLE;
}

// Local files: the decoder's packets, read straight from the file by the worker (no packet queue, no reader thread).
// Seeks are done here, between packets. At the end it returns one empty packet to drain the decoder, then waits for a
// seek or the end of the track instead of reading the end of the file again and again.
static int local_get_packet(void *ctx, AVPacket *pkt, int *serial) {
    TrackState *t = ctx;
    AVFormatContext *ic = t->ic;
    for (;;) {
        if (SDL_AtomicGet(&t->abort_request) || packet_queue_aborted(&t->audio_queue)) return -1;
        if (SDL_AtomicGet(&t->seek_pending)) process_seek_request(t);
        if (t->demux_eof) {
            wait_for_request(t, 1000);
            continue;
        }

        const int ret = av_read_frame(ic, pkt);
        if (ret < 0) {
            bool end = ret == AVERROR_EOF || avio_feof(ic->pb);
            if (!end) {
                if (!SDL_AtomicGet(&t->abort_request))
                    ff_log(AU_LOG_ERROR, "%s: read error: %s", t->filename, av_err2str(ret));
                // An I/O error, or a file that keeps failing to read: play out what is decoded, end as an error
                if ((ic->pb && ic->pb->error) || ++t->read_errors >= 100) {
                    SDL_AtomicSet(&t->had_error, 1);
                    end = true;
                }
            }
            if (!end) {
                wait_ms(t, 10);
                continue;
            }
            if (handle_demux_end(t) == DEMUX_LOOPED) continue;
            av_packet_unref(pkt); // empty: drains the decoder
            pkt->stream_index = t->audio_stream;
            *serial = packet_queue_serial(&t->audio_queue);
            return 1;
        }
        t->read_errors = 0;
        if (pkt->stream_index != t->audio_stream) {
            av_packet_unref(pkt);
            continue;
        }
        continue_timestamps(t, ic, pkt);
        if (!within_play_range(t, pkt)) {
            av_packet_unref(pkt);
            if (handle_demux_end(t) == DEMUX_LOOPED) continue;
            pkt->stream_index = t->audio_stream;
            *serial = packet_queue_serial(&t->audio_queue);
            return 1;
        }
        check_stream_metadata(t, ic, pkt);
        *serial = packet_queue_serial(&t->audio_queue);
        return 1;
    }
}

// Network streams: read ahead into the packet queue, so the worker keeps decoding while the connection stalls
static int net_reader(void *arg) {
    TrackState *t = arg;
    AVFormatContext *ic = t->ic;
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
        ff_log(AU_LOG_FATAL, "Could not allocate packet.");
        return 0;
    }

    for (;;) {
        if (SDL_AtomicGet(&t->abort_request) || packet_queue_aborted(&t->audio_queue))
            break;
        process_seek_request(t);
        update_buffering(t);

        // Enough read ahead: the queue drains at playback speed, so a few checks a second are plenty
        const bool full = queue_byte_size(&t->audio_queue) > NET_MAX_QUEUE_SIZE || queued_seconds(t) >= t->max_buffer_s;
        if (full) {
            wait_for_request(t, 250);
            continue;
        }

        // A stream that has ended stays ended (reading again only logs errors); a seek starts it again
        if (t->demux_eof) {
            wait_for_request(t, 1000);
            continue;
        }

        const int ret = av_read_frame(ic, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF || avio_feof(ic->pb)) {
                const DemuxEnd e = handle_demux_end(t);
                if (e == DEMUX_LOOPED) continue;
                if (e == DEMUX_ENDED) packet_queue_put_nullpacket(&t->audio_queue, pkt, t->audio_stream);
            }
            else if (ret != AVERROR(EAGAIN) && !SDL_AtomicGet(&t->abort_request)) {
                ff_log(AU_LOG_ERROR, "%s: read error: %s", t->filename, av_err2str(ret));
            }
            if (ic->pb && ic->pb->error && !t->demux_eof) {
                // I/O error (after the reconnects): let what is decoded play out, then end the track as an error
                SDL_AtomicSet(&t->had_error, 1);
                if (handle_demux_end(t) == DEMUX_ENDED)
                    packet_queue_put_nullpacket(&t->audio_queue, pkt, t->audio_stream);
            }
            wait_ms(t, 10);
            continue;
        }
        if (t->demux_eof) {
            t->demux_eof = false;
            SDL_AtomicSet(&t->read_ended, 0);
        }

        if (pkt->stream_index != t->audio_stream) {
            av_packet_unref(pkt);
            continue;
        }
        continue_timestamps(t, ic, pkt);
        if (!within_play_range(t, pkt)) {
            av_packet_unref(pkt);
            if (handle_demux_end(t) == DEMUX_ENDED)
                packet_queue_put_nullpacket(&t->audio_queue, pkt, t->audio_stream);
            continue;
        }

        check_stream_metadata(t, ic, pkt);
        packet_queue_put(&t->audio_queue, pkt);
    }
    av_packet_free(&pkt);
    return 0;
}

static void read_thread_fail(TrackState *t) {
    SDL_AtomicSet(&t->had_error, 1);
    // Nothing more will ever come out of this track
    ffa_store_release(&t->eos_serial, packet_queue_serial(&t->audio_queue));
    ff_post_track_msg(MSG_TRACK_FAILED, t->handle, 0);
}

// Open the input and the decoder, read the tags and publish the duration. < 0 = the track can not be played.
static int open_track(TrackState *t) {
    AudioPlayer *ap = t->ap;
    AVFormatContext *ic = avformat_alloc_context();
    int ret;
    if (!ic) {
        ff_log(AU_LOG_FATAL, "Could not allocate context.");
        return AVERROR(ENOMEM);
    }
    ic->interrupt_callback.callback = decode_interrupt_cb;
    ic->interrupt_callback.opaque = t;

    av_dict_set(&t->format_opts_n, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
    network_input_options(&t->format_opts_n, t->filename, t->http_headers);
    ret = avformat_open_input(&ic, t->filename, NULL, &t->format_opts_n); // probed: never chosen by the file's name
    if (ret < 0) {
        // avformat_open_input frees ic on failure
        if (!SDL_AtomicGet(&t->abort_request))
            ff_log(AU_LOG_ERROR, "Could not open input '%s': %s", t->filename, av_err2str(ret));
        return ret;
    }
    av_dict_set(&t->format_opts_n, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);
    t->ic = ic;
    network_after_open(ic, t->filename);

    if (ff_input_lacks_audio(ic)) {
        if (!SDL_AtomicGet(&t->abort_request))
            ff_log(AU_LOG_ERROR, "No playable audio stream in '%s'", t->filename);
        return AVERROR_STREAM_NOT_FOUND;
    }

    if (ap->find_stream_info) {
        ret = avformat_find_stream_info(ic, NULL);
        if (ret < 0) {
            if (!SDL_AtomicGet(&t->abort_request))
                ff_log(AU_LOG_WARNING, "%s: could not find codec parameters", t->filename);
            return ret;
        }
    }

    if (ic->pb)
        ic->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use avio_feof() to test for the end

    t->live = t->network && (ic->duration == AV_NOPTS_VALUE || ic->duration <= 0);
    // Positions count from the start of the file, like seeks and the duration: MPEG-TS (and so HLS) and some other
    // containers start their timestamps later
    t->pts_offset = ic->start_time != AV_NOPTS_VALUE ? ic->start_time / AV_TIME_BASE_DOUBLE : 0.0;

    if (t->start_time != AV_NOPTS_VALUE && seek_file(t, t->start_time) < 0)
        ff_log(AU_LOG_WARNING, "%s: could not seek to position %0.3f", t->filename, t->start_time / AV_TIME_BASE_DOUBLE);

    for (unsigned int i = 0; i < ic->nb_streams; i++)
        ic->streams[i]->discard = AVDISCARD_ALL;

    const int st_index = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);

    // Tags and the ReplayGain gain are ready before the decoder starts, so the first sample is already
    // at the right level
    if (st_index >= 0) {
        AuMediaInfo *info = media_info_from_context(ic, st_index, t->filename);
        if (info && t->network) t->icy = add_icy_headers(ic, info);
        ic->event_flags &= ~AVFMT_EVENT_FLAG_METADATA_UPDATED;
        ic->streams[st_index]->event_flags &= ~AVSTREAM_EVENT_FLAG_METADATA_UPDATED;
        SDL_LockMutex(t->seek_mutex);
        t->info = info;
        SDL_UnlockMutex(t->seek_mutex);
        wait_for_measurement(t, info);
        track_update_gain(t, false);
    }

    if (st_index < 0 || stream_component_open(t, st_index) < 0 || t->audio_stream < 0) {
        if (!SDL_AtomicGet(&t->abort_request))
            ff_log(AU_LOG_ERROR, "No playable audio stream in '%s'", t->filename);
        return AVERROR_STREAM_NOT_FOUND;
    }

    // Publish the duration and the end position (read by other threads under seek_mutex)
    int64_t end_pos = ic->duration;
    if (t->play_duration != AV_NOPTS_VALUE) {
        const int64_t end = (t->start_time != AV_NOPTS_VALUE ? t->start_time : 0) + t->play_duration;
        end_pos = end_pos == AV_NOPTS_VALUE ? end : FFMIN(end_pos, end);
    }
    SDL_LockMutex(t->seek_mutex);
    t->end_pos_us = end_pos;
    SDL_UnlockMutex(t->seek_mutex);
    ffa_store_release(&t->opened, 1); // publishes end_pos_us
    ff_post_track_msg(MSG_TRACK_OPENED, t->handle, 0);
    return 0;
}

// The track's thread: open, then decode (and for local files read) until the track is destroyed
static int track_thread(void *arg) {
    TrackState *t = arg;
    if (open_track(t) < 0) {
        read_thread_fail(t);
        goto end;
    }
    packet_queue_start(&t->audio_queue);
    if (t->network) {
        char name[32];
        snprintf(name, sizeof(name), "ffa_read_%d", t->handle);
        if (!(t->reader_tid = SDL_CreateThread(net_reader, name, t))) {
            ff_log(AU_LOG_FATAL, "SDL_CreateThread(): %s", SDL_GetError());
            read_thread_fail(t);
            goto end;
        }
    }
    decode_loop(t);

end:
    if (t->reader_tid) {
        // The decoding ended (destroyed, or failed): the reader stops too
        SDL_AtomicSet(&t->abort_request, 1);
        packet_queue_abort(&t->audio_queue);
        wake_read_thread(t);
        SDL_WaitThread(t->reader_tid, NULL);
        t->reader_tid = NULL;
    }
    if (t->audio_stream >= 0) {
        decoder_destroy(&t->audio_decoder);
        t->audio_stream = -1;
        t->audio_st = NULL;
    }
    if (t->ic) avformat_close_input(&t->ic);
    // Last: from here on the control thread may join this thread without waiting, and free the track
    const int32_t handle = t->handle;
    SDL_AtomicSet(&t->worker_exited, 1);
    ff_post_track_msg(MSG_TRACK_EXITED, handle, 0);
    return 0;
}

//+-----------------------------------------------------+
//| Audio (decoder) thread                              |
//+-----------------------------------------------------+

// Frames per ring marker at most; larger frames are written in pieces
#define RING_MAX_CHUNK_FRAMES 8192

// Decoded audio kept ready for the mixer, after all filters. Small enough that filter changes are
// heard quickly, large enough to ride out scheduling hiccups.
#define DECODED_BUFFER_SECONDS 0.5

// Ring storage is kept between tracks: a few hundred KB allocated and freed with every track would be the kind of
// large block glibc raises its trim thresholds for (see "Memory use" in the readme)
static float *ring_storage_take(AudioPlayer *ap, const size_t floats, size_t *got) {
    SDL_LockMutex(ap->ring_pool_mutex);
    int best = -1;
    for (int i = 0; i < ap->n_ring_pool; i++)
        if (ap->ring_pool_floats[i] >= floats && (best < 0 || ap->ring_pool_floats[i] < ap->ring_pool_floats[best]))
            best = i;
    float *buf = NULL;
    if (best >= 0) {
        buf = ap->ring_pool[best];
        *got = ap->ring_pool_floats[best];
        ap->n_ring_pool--;
        ap->ring_pool[best] = ap->ring_pool[ap->n_ring_pool];
        ap->ring_pool_floats[best] = ap->ring_pool_floats[ap->n_ring_pool];
    }
    SDL_UnlockMutex(ap->ring_pool_mutex);
    if (!buf && (buf = av_malloc(floats * sizeof(float)))) *got = floats;
    return buf;
}

void ring_storage_give(AudioPlayer *ap, float *buf, const size_t floats) {
    SDL_LockMutex(ap->ring_pool_mutex);
    const int n = (int)(sizeof(ap->ring_pool) / sizeof(ap->ring_pool[0]));
    if (ap->n_ring_pool < n) {
        ap->ring_pool[ap->n_ring_pool] = buf;
        ap->ring_pool_floats[ap->n_ring_pool] = floats;
        ap->n_ring_pool++;
        buf = NULL;
    }
    SDL_UnlockMutex(ap->ring_pool_mutex);
    av_free(buf);
}

void ring_storage_free_all(AudioPlayer *ap) {
    for (int i = 0; i < ap->n_ring_pool; i++) av_free(ap->ring_pool[i]);
    ap->n_ring_pool = 0;
}

static void wake_producer(TrackState *t) {
    SDL_LockMutex(t->ring_mutex);
    SDL_CondSignal(t->ring_cond);
    SDL_UnlockMutex(t->ring_mutex);
}

// Storage for DECODED_BUFFER_SECONDS of the output format plus a chunk. Another device that needs more (more channels,
// a higher rate) gets new storage once the mixer has taken or dropped everything in the old one.
static int ensure_ring(TrackState *t) {
    SampleRing *r = &t->ring;
    const int channels = t->convert_channels;
    const uint32_t need = (uint32_t)(t->convert_rate * DECODED_BUFFER_SECONDS) + RING_MAX_CHUNK_FRAMES;
    uint32_t frames = 1;
    while (frames < need) frames <<= 1;
    if (r->base && r->stride == channels && ring_capacity(r) >= frames) return 0;
    while (ring_pending(r)) { // stale for the new format: the mixer drops it within a period
        if (packet_queue_aborted(&t->audio_queue)) return AVERROR_EXIT;
        SDL_LockMutex(t->ring_mutex);
        if (!packet_queue_aborted(&t->audio_queue))
            SDL_CondWaitTimeout(t->ring_cond, t->ring_mutex, 10);
        SDL_UnlockMutex(t->ring_mutex);
    }
    size_t got = 0;
    float *base = ring_storage_take(t->ap, (size_t)frames * (size_t)channels, &got);
    if (!base) return AVERROR(ENOMEM);
    float *old;
    size_t old_floats;
    ring_set_storage(r, base, frames, channels, got, &old, &old_floats);
    if (old) ring_storage_give(t->ap, old, old_floats);
    return 0;
}


// Wait until a chunk of `frames` fits under the fill limit. The mixer never signals (no system calls on the audio
// thread), so this sleeps as long as the mixer needs to play the ring down to half the limit; seeks and aborts wake
// it early. Returns 1 when there is room, 0 when a seek made the chunk stale, -1 when aborted.
static int ring_wait_room(TrackState *t, const int frames, const int serial) {
    const int rate = FFMAX(1, t->convert_rate);
    const uint32_t high = (uint32_t)(rate * DECODED_BUFFER_SECONDS), low = high / 2;
    for (;;) {
        if (packet_queue_aborted(&t->audio_queue)) return -1;
        if (packet_queue_serial(&t->audio_queue) != serial) return 0;
        if (!t->network && SDL_AtomicGet(&t->seek_pending)) return 0; // superseded: this worker seeks next
        bool stale;
        const uint32_t used = ring_used_frames(&t->ring, serial, &stale);
        if (used < high && ring_has_room(&t->ring, (uint32_t)frames)) return 1;
        // Audio from before a seek is dropped by the mixer within a period; a paused or queued track comes back to
        // check four times a second
        const int ms = stale ? 10 : (int)FFMIN(250, FFMAX(5, (int64_t)(used > low ? used - low : 0) * 1000 / rate));
        SDL_LockMutex(t->ring_mutex);
        if (!packet_queue_aborted(&t->audio_queue) && packet_queue_serial(&t->audio_queue) == serial)
            SDL_CondWaitTimeout(t->ring_cond, t->ring_mutex, (Uint32)ms);
        SDL_UnlockMutex(t->ring_mutex);
    }
}

// Copy a converted frame into the ring (in pieces of at most RING_MAX_CHUNK_FRAMES); the frame is unreferenced
static int queue_frame(TrackState *t, AVFrame *frame, const AVRational tb, const int serial) {
    const int channels = frame->ch_layout.nb_channels;
    if (frame->format != AV_SAMPLE_FMT_FLT || channels != t->ring.stride || frame->sample_rate != t->convert_rate) {
        av_frame_unref(frame); // made for an output format that is gone (the graphs are being rebuilt)
        return 0;
    }
    const double speed = t->speed_chain.graph ? t->speed_cur : 1.0;
    const double pts = frame->pts == AV_NOPTS_VALUE ? NAN : frame->pts * av_q2d(tb) - t->pts_offset;
    const float *src = (const float *)frame->data[0];
    int ret = 0;
    for (int done = 0; done < frame->nb_samples;) {
        const int n = FFMIN(frame->nb_samples - done, RING_MAX_CHUNK_FRAMES);
        const int room = ring_wait_room(t, n, serial);
        if (room <= 0) {
            ret = room < 0 ? AVERROR_EXIT : 0;
            break;
        }
        const RingMarker info = { .serial = serial, .rate = frame->sample_rate, .channels = channels,
                                  .pts = isnan(pts) ? NAN : pts + (double)done / frame->sample_rate * speed,
                                  .speed = speed };
        ring_write(&t->ring, src + (size_t)done * channels, n, &info);
        done += n;
    }
    av_frame_unref(frame);
    if (t->network && SDL_AtomicGet(&t->buffering)) update_buffering(t);
    return ret;
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

// Pull what the speed graph produced and pass it on. Tempo and rate filters do not keep file timestamps, so the
// file position is counted instead: segment start + output samples * speed.
static int drain_speed(TrackState *t, AVFrame *spd, AVFrame *tmp, const int serial) {
    int ret;
    const int rate = t->convert_rate;
    while ((ret = av_buffersink_get_frame_flags(t->speed_chain.sink, spd, 0)) >= 0) {
        double file_pos = t->speed_seg_in + (double)t->speed_seg_out / rate * t->speed_cur;
        // The estimate drifts a little with every live speed change (the filters hold some audio). The real
        // position can not be ahead of what went in, nor further behind it than the filters can hold: keep it
        // in that window and restart the segment from there, so errors never add up
        const double lo = t->speed_in_end - SPEED_MAX_LAG_SECONDS;
        if (file_pos > t->speed_in_end || file_pos < lo) {
            file_pos = file_pos > t->speed_in_end ? t->speed_in_end : lo;
            t->speed_seg_in = file_pos;
            t->speed_seg_out = 0;
        }
        t->speed_seg_out += spd->nb_samples;
        spd->pts = llrint(file_pos * rate);
        if ((ret = feed_user(t, spd, tmp, (AVRational){1, rate}, serial)) < 0)
            return ret;
        if (packet_queue_serial(&t->audio_queue) != serial)
            return 0;
    }
    return ret == AVERROR_EOF ? AVERROR_EOF : 0;
}

// Feed one converted frame through the speed graph (when the speed is not 1), then on to the user graph
static int feed_speed(TrackState *t, AVFrame *frame, AVFrame *spd, AVFrame *tmp, const AVRational tb, const int serial) {
    if (!t->speed_chain.graph)
        return feed_user(t, frame, tmp, tb, serial);
    const double in_pos = frame->pts != AV_NOPTS_VALUE ? frame->pts * av_q2d(tb) : t->speed_in_end;
    if (!t->speed_seg_open) {
        t->speed_seg_in = in_pos;
        t->speed_seg_out = 0;
        t->speed_seg_open = true;
    }
    t->speed_in_end = in_pos + (double)frame->nb_samples / frame->sample_rate;
    if (av_buffersrc_add_frame(t->speed_chain.src, frame) < 0) {
        av_frame_unref(frame);
        return 0;
    }
    const int ret = drain_speed(t, spd, tmp, serial);
    return ret == AVERROR_EOF ? 0 : ret;
}

// Let the speed graph play out what it still holds (into the user graph), then free it
static int close_speed_graph(TrackState *t, AVFrame *spd, AVFrame *tmp, const int serial) {
    int ret = 0;
    if (t->speed_chain.graph && t->speed_seg_open && av_buffersrc_add_frame(t->speed_chain.src, NULL) >= 0)
        ret = drain_speed(t, spd, tmp, serial);
    free_filter_chain(&t->speed_chain);
    t->speed_seg_open = false;
    return ret == AVERROR_EXIT ? ret : 0;
}

// Apply a changed playback speed: a command to the running atempo filters when possible, otherwise the old speed
// graph plays out and a new one (or none, at normal speed) takes over, so no audio is dropped
static int apply_speed(TrackState *t, AVFrame *spd, AVFrame *tmp, const int serial) {
    AudioPlayer *ap = t->ap;
    AVChannelLayout layout = {0};
    SDL_LockMutex(ap->format_mutex);
    const int32_t gen = ap->speed_gen;
    const double speed = ap->speed > 0 ? ap->speed : 1.0;
    const bool keep_pitch = ap->keep_pitch;
    int ret = gen != t->speed_gen_seen ? av_channel_layout_copy(&layout, &ap->audio_target.ch_layout) : 0;
    SDL_UnlockMutex(ap->format_mutex);
    if (gen == t->speed_gen_seen || ret < 0) {
        av_channel_layout_uninit(&layout);
        return 0;
    }
    t->speed_gen_seen = gen;

    const bool want = fabs(speed - 1.0) > 1e-6;
    if (want && t->speed_chain.graph && keep_pitch && t->speed_keep_pitch_cur) {
        char arg[32];
        snprintf(arg, sizeof(arg), "%.9f", sqrt(speed));
        if (avfilter_graph_send_command(t->speed_chain.graph, SPEED_FILTER_1, "tempo", arg, NULL, 0, 0) >= 0 &&
            avfilter_graph_send_command(t->speed_chain.graph, SPEED_FILTER_2, "tempo", arg, NULL, 0, 0) >= 0) {
            // A new segment starts where the old one got to
            if (t->speed_seg_open) {
                t->speed_seg_in += (double)t->speed_seg_out / t->convert_rate * t->speed_cur;
                t->speed_seg_out = 0;
            }
            t->speed_cur = speed;
            av_channel_layout_uninit(&layout);
            return 0;
        }
    }

    if ((ret = close_speed_graph(t, spd, tmp, serial)) < 0) {
        av_channel_layout_uninit(&layout);
        return ret;
    }
    if (want && (ret = build_speed_graph(&t->speed_chain, t->convert_rate, &layout, speed, keep_pitch,
                                         ap->use_soxr, ap->filter_nbthreads)) < 0)
        ff_log(AU_LOG_ERROR, "Could not set the playback speed to %.2f: %s", speed, av_err2str(ret));
    t->speed_cur = t->speed_chain.graph ? speed : 1.0;
    t->speed_keep_pitch_cur = keep_pitch;
    av_channel_layout_uninit(&layout);
    return 0;
}

// Pass everything the convert graph has produced on to the speed and user graphs
static int drain_convert(TrackState *t, AVFrame *conv, AVFrame *spd, AVFrame *tmp, const int serial) {
    int ret;
    while ((ret = av_buffersink_get_frame_flags(t->convert.sink, conv, 0)) >= 0) {
        const AVRational tb = av_buffersink_get_time_base(t->convert.sink);
        if ((ret = feed_speed(t, conv, spd, tmp, tb, serial)) < 0)
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
            return ret >= 0 ? ensure_ring(t) : ret;
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
    ffa_store_release(&t->eos_serial, packet_queue_serial(&t->audio_queue));
    ff_post_track_msg(MSG_TRACK_FAILED, t->handle, 0);
}

static void decode_loop(TrackState *t) {
    AVFrame *frame = av_frame_alloc();
    AVFrame *conv = av_frame_alloc();
    AVFrame *spd = av_frame_alloc();
    AVFrame *tmp = av_frame_alloc();
    int last_serial = -1;
    int ended_serial = -1;     // serial whose audio has completely been queued
    bool user_built = false;
    int ret = 0;

    if (!frame || !conv || !spd || !tmp)
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
                        drain_convert(t, conv, spd, tmp, serial) == AVERROR_EXIT)
                        break;
                    if (close_speed_graph(t, spd, tmp, serial) == AVERROR_EXIT)
                        break;
                    t->speed_gen_seen = -1; // rebuilt if playback continues after a seek
                    if (feed_user(t, NULL, tmp, (AVRational){1, t->convert_rate}, serial) == AVERROR_EXIT)
                        break;
                }
                ended_serial = serial;
                ffa_store_release(&t->eos_serial, serial); // after the last frames are in the ring
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
            // The speed and user graphs restart with the conversion (seek, device change): stale state is dropped
            free_filter_chain(&t->speed_chain);
            t->speed_seg_open = false;
            t->speed_gen_seen = -1;
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
        if (apply_speed(t, spd, tmp, serial) == AVERROR_EXIT)
            break;

        if (av_buffersrc_add_frame(t->convert.src, frame) < 0) {
            av_frame_unref(frame);
            continue;
        }
        ret = drain_convert(t, conv, spd, tmp, serial);
        if (ret == AVERROR_EXIT)
            break;
        if (ret == AVERROR_EOF) {
            ended_serial = serial;
            ffa_store_release(&t->eos_serial, serial);
        }
    }

end:
    free_filter_chain(&t->convert);
    free_filter_chain(&t->speed_chain);
    free_filter_chain(&t->user);
    av_frame_free(&frame);
    av_frame_free(&conv);
    av_frame_free(&spd);
    av_frame_free(&tmp);
}

//+-----------------------------------------------------+
//| Mixer side (SDL callback)                           |
//+-----------------------------------------------------+

StreamMeta *track_take_stream_meta(TrackState *t, const double position) {
    SDL_LockMutex(t->seek_mutex);
    StreamMeta *m = t->stream_meta;
    if (m && m->position <= position + 0.05) {
        t->stream_meta = m->next;
        m->next = NULL;
        if (t->info) {
            av_dict_set(&t->info->tags, "title", m->title, 0);
            av_dict_set(&t->info->tags, "artist", m->artist, 0);   // NULL removes it
        }
    }
    else {
        m = NULL;
    }
    SDL_UnlockMutex(t->seek_mutex);
    return m;
}

void track_free_stream_meta(StreamMeta *m) {
    free_stream_meta(m);
}

// Drop what the mixer can not play: audio decoded before a seek, or for another output format (a device change). Only
// index arithmetic: the mixer calls it for every slot each period, also while paused, so a seek frees the ring at once.
static void drop_stale(TrackState *t, const int serial, const int rate, const int channels) {
    const RingMarker *m;
    while ((m = ring_peek(&t->ring)) && (m->serial != serial || m->rate != rate || m->channels != channels))
        ring_skip(&t->ring);
}

void track_drop_stale(TrackState *t, const int rate, const int channels) {
    drop_stale(t, packet_queue_serial(&t->audio_queue), rate, channels);
}

bool track_has_audio(TrackState *t, const int rate, const int channels) {
    if (SDL_AtomicGet(&t->buffering)) return false; // a network track that is still filling its buffer
    drop_stale(t, packet_queue_serial(&t->audio_queue), rate, channels);
    return ring_peek(&t->ring) != NULL;
}

int track_pull(TrackState *t, float *dst, const int frames, const int rate, const int channels, bool *eos, bool *looped) {
    int written = 0;
    const int serial = packet_queue_serial(&t->audio_queue);
    *eos = false;
    *looped = false;

    if (SDL_AtomicGet(&t->buffering)) {
        if (t->started) t->stalled = true;
        return 0;
    }

    while (written < frames) {
        // The end marker is read before the ring: the decoder sets it after writing its last frames, so an empty
        // ring after a set marker really is the end (read the other way round, those frames could be cut)
        const bool ended = ffa_load_acquire(&t->eos_serial) == serial;
        drop_stale(t, serial, rate, channels);
        const RingMarker *m = ring_peek(&t->ring);
        if (!m) {
            if (ended)
                *eos = true;
            else if (t->network && t->started) {
                // The network fell behind: wait for buffer_seconds of data instead of stuttering
                SDL_AtomicSet(&t->buffering, 1);
                t->stalled = true;
            }
            break; // end of track, or the decoder has not caught up yet (underrun)
        }
        if (ring_offset(&t->ring) == 0) {
            // The first frames of a chunk: its position, speed and (at the start or after a seek) the clock
            if (!isnan(m->pts)) {
                // A jump back without a seek is a loop only if the read thread actually looped; anything else
                // (e.g. a small position correction from the speed stage) is not reported
                if (t->started && !isnan(t->last_frame_pts) && m->serial == t->last_frame_serial &&
                    m->pts < t->last_frame_pts - LOOP_DETECT_SECONDS &&
                    SDL_AtomicGet(&t->loops_started) > t->loops_heard) {
                    t->loops_heard++;
                    *looped = true;
                }
                t->last_frame_pts = m->pts;
                t->last_frame_serial = m->serial;
                t->position = m->pts;
            }
            t->play_speed = m->speed > 0 ? m->speed : 1.0;
            if (m->serial != t->clock_serial) {
                t->clock_serial = m->serial;
                t->play_clock = 0;
            }
        }
        const double speed = m->speed; // the marker may be reused once ring_read() has finished it
        const int n = ring_read(&t->ring, dst + (size_t)written * channels, frames - written);
        written += n;
        t->position += (double)n / rate * speed;
        t->play_clock += (double)n / rate * speed;
    }

    if (written > 0) {
        t->started = true;
        if (t->stalled) { // after a stall: fade in over 10 ms instead of starting with a click
            t->stalled = false;
            t->resume_fade = t->resume_fade_len = FFMAX(1, rate / 100);
        }
    }
    return written;
}
