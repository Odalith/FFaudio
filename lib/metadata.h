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

struct AuMediaInfo {
    char *filename;
    char *container;            // demuxer name, e.g. "flac", "mp3", "mov,mp4,m4a,3gp,3g2,mj2"
    char *codec;                // decoder name, e.g. "flac", "mp3", "aac", "opus"
    char *channel_layout;       // e.g. "stereo", "5.1(side)"
    double duration;            // seconds, -1 unknown
    int64_t bit_rate;           // bits per second, 0 unknown
    int sample_rate;
    int channels;
    int bits_per_sample;        // 0 for lossy codecs / unknown
    AVDictionary *tags;         // container tags, then stream tags for keys the container lacks
    AuReplayGainInfo replaygain;
};

// Describe the audio stream `stream_index` of an opened (and probed) input
AuMediaInfo *media_info_from_context(const AVFormatContext *ic, int stream_index, const char *filename);

AuMediaInfo *media_info_copy(const AuMediaInfo *info);

// Parse ReplayGain 2.0 / R128 values from a tag dictionary
AuReplayGainInfo replaygain_from_tags(const AVDictionary *tags);

// Gain in dB that the ReplayGain settings give a track with these values (0 when off)
double replaygain_gain_db(const AuReplayGainConfig *config, const AuReplayGainInfo *rg);

#endif //FFAUDIO_METADATA_H
