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

/*
 * The duration of a chained Ogg file (parts one after another, e.g. "cat a.ogg b.ogg", or a recorded radio stream).
 * FFmpeg takes an Ogg file's duration from the granule position of its last page, and every part starts its granules
 * again, so it reports the last part's length. This walks the page headers, adds up each part's length (its last
 * granule, less the Opus pre-skip, over its sample rate) and returns the sum.
 */

#include "probe.h"
#include "ffaudio.h"

#include <string.h>
#include "libavutil/intreadwrite.h"

#define PAGE_HEADER 27
#define MAX_PARTS 4096
#define TAIL_BYTES 65536

static uint32_t page_serial_at(AVIOContext *pb, const int64_t pos) {
    uint8_t h[PAGE_HEADER];
    if (avio_seek(pb, pos, SEEK_SET) < 0 || avio_read(pb, h, PAGE_HEADER) != PAGE_HEADER || memcmp(h, "OggS", 4))
        return 0;
    return AV_RL32(h + 14);
}

// Serial number of the last page (searched in the last 64 KB), 0 when there is none
static uint32_t last_page_serial(AVIOContext *pb, const int64_t size) {
    const int64_t start = size > TAIL_BYTES ? size - TAIL_BYTES : 0;
    const int len = (int)(size - start);
    uint8_t *buf = av_malloc(len);
    if (!buf) return 0;
    uint32_t serial = 0;
    if (avio_seek(pb, start, SEEK_SET) >= 0 && avio_read(pb, buf, len) == len) {
        for (int i = len - PAGE_HEADER; i >= 0; i--) {
            if (!memcmp(buf + i, "OggS", 4) && buf[i + 4] == 0) {
                serial = AV_RL32(buf + i + 14);
                break;
            }
        }
    }
    av_free(buf);
    return serial;
}

// Sample rate the granules of a stream count in, from its first packet; pre-skip for Opus. 0 = not a codec known here.
static int granule_rate(const uint8_t *p, const int n, int64_t *preskip) {
    *preskip = 0;
    if (n >= 16 && p[0] == 1 && !memcmp(p + 1, "vorbis", 6)) return (int)AV_RL32(p + 12);
    if (n >= 12 && !memcmp(p, "OpusHead", 8)) {
        *preskip = AV_RL16(p + 10);
        return 48000; // Opus granules always count 48 kHz samples
    }
    if (n >= 30 && p[0] == 0x7F && !memcmp(p + 1, "FLAC", 4) && !memcmp(p + 9, "fLaC", 4))
        return (p[27] << 12) | (p[28] << 4) | (p[29] >> 4);   // STREAMINFO's 20-bit sample rate
    return 0;
}

// A local, seekable Ogg input whose first and last pages belong to different streams (pb position restored)
static bool chained(AVFormatContext *ic, const char *path, int64_t *size) {
    AVIOContext *pb = ic->pb;
    if (!pb || !ic->iformat || strcmp(ic->iformat->name, "ogg") || au_is_network_url(path)) return false;
    if (!(pb->seekable & AVIO_SEEKABLE_NORMAL)) return false;
    *size = avio_size(pb);
    if (*size <= PAGE_HEADER) return false;
    const int64_t saved = avio_tell(pb);
    const uint32_t first = page_serial_at(pb, 0), last = last_page_serial(pb, *size);
    avio_seek(pb, saved, SEEK_SET);
    return first && last && first != last;
}

bool ff_ogg_is_chained(AVFormatContext *ic, const char *path) {
    int64_t size;
    return chained(ic, path, &size);
}

int64_t ff_ogg_chained_duration(AVFormatContext *ic, const char *path) {
    int64_t size = 0;
    if (!chained(ic, path, &size)) return AV_NOPTS_VALUE;
    AVIOContext *pb = ic->pb;
    const int64_t saved = avio_tell(pb);
    int64_t total = AV_NOPTS_VALUE;

    // Walk every page. A part is one logical stream from its first page (BOS) to the next BOS after data pages; a
    // part with several streams at once (multiplexed, e.g. with a skeleton) is not handled here.
    int64_t sum = 0;
    int parts = 0;
    uint32_t serial = 0;
    int rate = 0;
    int64_t preskip = 0, granule = -1;
    bool in_data = false;
    for (int64_t pos = 0; pos + PAGE_HEADER <= size;) {
        uint8_t h[PAGE_HEADER], seg[255];
        if (avio_seek(pb, pos, SEEK_SET) < 0 || avio_read(pb, h, PAGE_HEADER) != PAGE_HEADER) break;
        if (memcmp(h, "OggS", 4)) goto end; // damaged: keep FFmpeg's value
        const int nseg = h[26];
        if (avio_read(pb, seg, nseg) != nseg) break;
        int body = 0;
        for (int i = 0; i < nseg; i++) body += seg[i];
        const uint32_t page_serial = AV_RL32(h + 14);
        const int64_t page_granule = (int64_t)AV_RL64(h + 6);
        if (h[5] & 0x02) {                       // beginning of a stream
            if (!in_data && parts > 0) goto end; // a second stream in the same part: multiplexed
            if (parts > 0) {
                if (granule < 0 || !rate) goto end;
                sum += av_rescale(granule - preskip > 0 ? granule - preskip : 0, AV_TIME_BASE, rate);
            }
            if (++parts > MAX_PARTS) goto end;
            uint8_t first_packet[64];
            const int n = FFMIN(body, (int)sizeof(first_packet));
            if (avio_read(pb, first_packet, n) != n) goto end;
            rate = granule_rate(first_packet, n, &preskip);
            serial = page_serial;
            granule = -1;
            in_data = false;
        }
        else {
            if (page_serial != serial) goto end; // pages of another stream inside the part: multiplexed
            in_data = true;
        }
        if (page_granule >= 0) granule = page_granule;
        pos += PAGE_HEADER + nseg + body;
    }
    if (parts > 1 && granule >= 0 && rate > 0)
        total = sum + av_rescale(granule - preskip > 0 ? granule - preskip : 0, AV_TIME_BASE, rate);

end:
    avio_seek(pb, saved, SEEK_SET);
    return total;
}
