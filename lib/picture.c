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
 * Embedded pictures (au_media_picture()). FFmpeg turns ID3v2 APIC frames, FLAC PICTURE blocks, MP4 covr atoms, APE
 * cover tags and Ogg METADATA_BLOCK_PICTURE comments into "attached picture" streams while it reads the header, with
 * the picture type as an ID3-style description in their "comment" tag. Nothing is decoded: the image comes back as it
 * is stored, its size read from the PNG or JPEG header when the container did not give it.
 */

#include "ffaudio.h"

#include <string.h>
#include "libavformat/avformat.h"
#include "libavutil/avstring.h"
#include "libavutil/intreadwrite.h"
#include "network.h"
#include "probe.h"

// FFmpeg's names for the ID3v2 / FLAC picture types (libavformat/id3v2.c, ff_id3v2_picture_types), in type order
static const char *const picture_types[21] = {
    "Other", "32x32 pixels 'file icon'", "Other file icon", "Cover (front)", "Cover (back)", "Leaflet page",
    "Media (e.g. label side of CD)", "Lead artist/lead performer/soloist", "Artist/performer", "Conductor",
    "Band/Orchestra", "Composer", "Lyricist/text writer", "Recording Location", "During recording",
    "During performance", "Movie/video screen capture", "A bright coloured fish", "Illustration",
    "Band/artist logotype", "Publisher/Studio logotype",
};

static int type_of(const AVStream *st) {
    const AVDictionaryEntry *e = av_dict_get(st->metadata, "comment", NULL, 0);
    if (!e) return AU_PICTURE_FRONT_COVER; // MP4 covers and others without a type: the cover
    for (int i = 0; i < 21; i++)
        if (!av_strcasecmp(e->value, picture_types[i])) return i;
    return AU_PICTURE_OTHER;
}

static const char *mime_of(const enum AVCodecID id) {
    switch (id) {
        case AV_CODEC_ID_MJPEG:  return "image/jpeg";
        case AV_CODEC_ID_PNG:    return "image/png";
        case AV_CODEC_ID_GIF:    return "image/gif";
        case AV_CODEC_ID_BMP:    return "image/bmp";
        case AV_CODEC_ID_WEBP:   return "image/webp";
        case AV_CODEC_ID_TIFF:   return "image/tiff";
        default:                 return "application/octet-stream";
    }
}

// Width and height from a PNG (IHDR) or baseline/progressive JPEG (SOFn) header; false when it is neither
static bool image_size(const uint8_t *p, const size_t n, int *w, int *h) {
    if (n >= 24 && !memcmp(p, "\x89PNG\r\n\x1a\n", 8) && !memcmp(p + 12, "IHDR", 4)) {
        *w = (int)AV_RB32(p + 16);
        *h = (int)AV_RB32(p + 20);
        return true;
    }
    if (n >= 4 && p[0] == 0xFF && p[1] == 0xD8) {
        size_t i = 2;
        while (i + 9 < n) {
            if (p[i] != 0xFF) return false;
            const uint8_t marker = p[i + 1];
            if (marker == 0xFF) { i++; continue; }      // fill byte
            if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) { i += 2; continue; }
            const size_t len = AV_RB16(p + i + 2);
            if (len < 2) return false;
            // SOF0..SOF15 carry the size, except DHT (C4), JPG (C8) and DAC (CC)
            if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
                *h = AV_RB16(p + i + 5);
                *w = AV_RB16(p + i + 7);
                return true;
            }
            if (marker == 0xDA || marker == 0xD9) return false; // image data or the end: no size marker
            i += 2 + len;
        }
    }
    return false;
}

void au_picture_release(const AuPicture *picture) {
    av_free((void *)picture);
}

const AuPicture *au_media_picture(const char *path, const int type, int *out_error) {
    int dummy;
    int *err = out_error ? out_error : &dummy;
    if (!path || !path[0] || type < AU_PICTURE_ANY || type > 20) { *err = AU_ERR_INVALID_ARGUMENT; return NULL; }

    // Only the header is read (no stream info, nothing decoded); the same protocol rules and network settings as
    // the player's inputs
    AVFormatContext *ic = NULL;
    if (ff_open_input(path, NULL, NULL, &ic) < 0) { *err = AU_ERR_FILE; return NULL; }

    const AVStream *best = NULL;
    for (unsigned int i = 0; i < ic->nb_streams; i++) {
        const AVStream *st = ic->streams[i];
        if (!(st->disposition & AV_DISPOSITION_ATTACHED_PIC) || st->attached_pic.size <= 0) continue;
        const int t = type_of(st);
        if (type == AU_PICTURE_ANY) {
            if (!best) best = st;
            if (t == AU_PICTURE_FRONT_COVER) { best = st; break; }
        }
        else if (t == type) { best = st; break; }
    }
    if (!best) {
        avformat_close_input(&ic);
        *err = AU_ERR_NOTHING_TO_DO;
        return NULL;
    }

    const AVDictionaryEntry *title = av_dict_get(best->metadata, "title", NULL, 0);
    const char *description = title ? title->value : "";
    const char *mime = mime_of(best->codecpar->codec_id);
    const size_t size = (size_t)best->attached_pic.size;
    // One block: the struct, the image, then the strings
    const size_t strings = strlen(mime) + 1 + strlen(description) + 1;
    AuPicture *pic = av_malloc(sizeof(AuPicture) + size + strings);
    if (!pic) {
        avformat_close_input(&ic);
        *err = AU_ERR_NO_MEMORY;
        return NULL;
    }
    uint8_t *data = (uint8_t *)(pic + 1);
    memcpy(data, best->attached_pic.data, size);
    char *s = (char *)data + size;
    memcpy(s, mime, strlen(mime) + 1);
    pic->mime = s;
    s += strlen(mime) + 1;
    memcpy(s, description, strlen(description) + 1);
    pic->description = s;
    pic->data = data;
    pic->size = size;
    pic->type = type_of(best);
    pic->width = best->codecpar->width;
    pic->height = best->codecpar->height;
    if (pic->width <= 0 || pic->height <= 0) {
        int w = 0, h = 0;
        if (image_size(data, size, &w, &h) && w > 0 && h > 0) { pic->width = w; pic->height = h; }
        else pic->width = pic->height = 0;
    }
    avformat_close_input(&ic);
    *err = AU_OK;
    return pic;
}
