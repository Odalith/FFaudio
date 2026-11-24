/*
* Copyright (c) 2025 Odalith
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

#ifndef FFAUDIO_METADATA_H
#define FFAUDIO_METADATA_H

#include "globals.h"

// An AuMediaInfo while it is being put together (from an opened input, plus e.g. a stream's ICY headers); frozen
// into the immutable, reference-counted AuMediaInfo with media_info_freeze()
typedef struct MediaDraft {
    char *path;
    char *container;            // demuxer name, e.g. "flac", "mp3", "mov,mp4,m4a,3gp,3g2,mj2"
    char *codec;                // decoder name, e.g. "flac", "mp3", "aac", "opus"
    char *channel_layout;       // e.g. "stereo", "5.1(side)"
    double duration;            // seconds, -1 unknown
    int64_t bit_rate;           // bits per second, 0 unknown
    int sample_rate;
    int channels;
    int bits_per_sample;        // 0 for lossy codecs / unknown
    bool live;                  // no duration: a live stream
    bool seekable;
    AVDictionary *tags;         // container tags, then stream tags for keys the container lacks
    int picture_count;          // attached pictures (cover art)
} MediaDraft;

// UTF-8 copy of text that may be Latin-1 (old Icecast servers): valid UTF-8 is kept, anything else is read as Latin-1
char *text_to_utf8(const char *s);

// Describe the audio stream `stream_index` of an opened (and probed) input. < 0 on failure (nothing to free).
int media_draft_from_context(MediaDraft *d, const AVFormatContext *ic, int stream_index, const char *path);
void media_draft_free(MediaDraft *d);

// The immutable block (one allocation, a reference held by the caller); NULL when out of memory
const AuMediaInfo *media_info_freeze(const MediaDraft *d);
// A new block like `info` with another title and artist ("now playing" of a stream; NULL artist removes it)
const AuMediaInfo *media_info_with_title(const AuMediaInfo *info, const char *title, const char *artist);
// Another reference (NULL is fine); au_media_info_release() drops one
const AuMediaInfo *media_info_retain(const AuMediaInfo *info);

// Parse ReplayGain 2.0 / R128 values from a tag dictionary
AuReplayGain replaygain_from_tags(const AVDictionary *tags);

// Gain in dB that the ReplayGain settings give a track with these values (0 when off)
double replaygain_gain_db(const AuReplayGainConfig *config, const AuReplayGain *rg);

#endif //FFAUDIO_METADATA_H
