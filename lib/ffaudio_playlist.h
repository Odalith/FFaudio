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
 * Playlist files: .m3u, .m3u8 and .pls, local or from a URL (an internet radio station's .pls, say). They are only
 * read: what to play, and when, stays with the application.
 *
 *   int err;
 *   const AuPlaylistFile *pl = au_playlist_read("/music/road trip.m3u8", &err);
 *   if (pl && pl->count > 0) au_play(pl->entries[0].path, NULL);   // the rest with au_queue() on AU_EVENT_PREPARE_NEXT
 *   ...
 *   au_playlist_file_release(pl);
 *
 * Works without au_init() and from any thread. URLs are read with the network settings of au_set_network() and the
 * same protocol restrictions as streams.
 */

#ifndef FFAUDIO_PLAYLIST_H
#define FFAUDIO_PLAYLIST_H

#include <stddef.h>
#include "ffaudio.h"

#ifdef __cplusplus
extern "C" {
#endif

    typedef struct AuPlaylistEntry {
        // A URL, or a local path: relative entries are resolved against the playlist's folder, file:// URIs become
        // paths. Ready for au_play() / au_queue().
        const char *path;
        const char *title;           // from #EXTINF or TitleN; "" when the playlist has none
        double duration;             // seconds from #EXTINF or LengthN; -1 when unknown
    } AuPlaylistEntry;

    // One immutable block: every pointer in it stays valid until au_playlist_file_release()
    typedef struct AuPlaylistFile {
        int count;
        const AuPlaylistEntry *entries;
    } AuPlaylistFile;

    // Read a playlist file or URL. The format comes from the content ("[playlist]" = PLS, otherwise M3U); text that is
    // not UTF-8 is read as Latin-1. An HLS playlist (.m3u8 with #EXT-X- lines) is a stream, not a list of tracks: it
    // gives one entry, itself. A playlist from a URL may only list URLs: local paths in it are left out, as streams may
    // not reach local files either. NULL on error: *out_error (may be NULL) is AU_ERR_FILE (can not be read, or not text),
    // AU_ERR_INVALID_ARGUMENT or AU_ERR_NO_MEMORY. A playlist with no entries is not an error (count 0).
    FFAUDIO_API const AuPlaylistFile *au_playlist_read(const char *path_or_url, int *out_error);

    // The same for playlist text already in memory. `base` is where it came from (its path or URL), for relative
    // entries; NULL keeps them as written (and makes an HLS playlist AU_ERR_UNSUPPORTED, as it has no address).
    FFAUDIO_API const AuPlaylistFile *au_playlist_parse(const char *text, size_t length, const char *base,
                                                        int *out_error);

    // Done with it (NULL is fine)
    FFAUDIO_API void au_playlist_file_release(const AuPlaylistFile *playlist);

#ifdef __cplusplus
} // extern "C"
#endif

#endif //FFAUDIO_PLAYLIST_H
