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
    // not UTF-8 is read as Windows-1252 (Latin-1 with quotes and dashes). An HLS playlist (.m3u8 with #EXT-X- lines) is a stream, not a list of tracks: it
    // gives one entry, itself. A playlist from a URL may only list URLs: local paths in it are left out, as streams may
    // not reach local files either. NULL on error: *out_error (may be NULL) is AU_ERR_FILE (can not be read, or not text),
    // AU_ERR_UNSUPPORTED (a cue sheet: since 3.2, read those with au_cue_read()), AU_ERR_INVALID_ARGUMENT or
    // AU_ERR_NO_MEMORY. A playlist with no entries is not an error (count 0).
    FFAUDIO_API const AuPlaylistFile *au_playlist_read(const char *path_or_url, int *out_error);

    // The same for playlist text already in memory. `base` is where it came from (its path or URL), for relative
    // entries; NULL keeps them as written (and makes an HLS playlist AU_ERR_UNSUPPORTED, as it has no address).
    FFAUDIO_API const AuPlaylistFile *au_playlist_parse(const char *text, size_t length, const char *base,
                                                        int *out_error);

    // Done with it (NULL is fine)
    FFAUDIO_API void au_playlist_file_release(const AuPlaylistFile *playlist);

    //+-----------------------------------------------------+
    //| Cue sheets and chapters                             |
    //+-----------------------------------------------------+
    // Several tracks in one file: a .cue sheet next to an album image, a cue sheet embedded in the file (the CUESHEET
    // tag of FLAC/WavPack/APE, or a FLAC CUESHEET block), or the file's chapters (M4B/M4A audiobooks, Matroska, MP3
    // ID3 CHAP frames). Each track is a range of a file, played with AuPlayConfig.start_seconds / .length_seconds:
    // consecutive tracks of one file join exactly, with nothing missing or doubled.
    //
    //   const AuCueSheet *cue = au_cue_read("/music/album.cue", NULL);   // or the audio file itself
    //   const AuCueTrack *t = cue->tracks[0];
    //   const AuPlayConfig pc = { .start_seconds = t->start_seconds, .length_seconds = t->length_seconds,
    //                             .replaygain = &t->replaygain };
    //   au_play(t->path, &pc);                     // the next ones with au_queue() on AU_EVENT_PREPARE_NEXT
    //   au_cue_release(cue);                       // after the last use of its strings
    // Since 3.2.

    // One track. Reached through a pointer (AuCueSheet.tracks), so it may gain fields at the end.
    typedef struct AuCueTrack {
        int number;                  // TRACK nn of the sheet, or the chapter's number (from 1)
        const char *path;            // the audio file it is in (resolved like playlist entries)
        double start_seconds;        // where it starts in that file: INDEX 01, or the chapter's start
        // How long: up to the next track of the same file (a pregap, INDEX 00, plays at the end of the track before);
        // 0 = to the end of the file. As AuPlayConfig.length_seconds.
        double length_seconds;
        const char *title, *performer, *songwriter, *isrc;   // "" when not given; performer falls back to the sheet's
        AuReplayGain replaygain;     // REM REPLAYGAIN_* lines of the sheet (NaN where missing), for AuPlayConfig
    } AuCueTrack;

    // Library-allocated: may gain fields at the end
    typedef struct AuCueSheet {
        int count;
        const AuCueTrack *const *tracks;
        const char *title, *performer, *genre, *date;          // "" when not given
    } AuCueSheet;

    // Read the tracks of a .cue file (by its name, local or a URL), or of an audio file: its embedded cue sheet if it
    // has one, otherwise its chapters. Only tracks of type AUDIO are listed; PREGAP / POSTGAP (silence to generate)
    // are ignored. A FILE that does not exist is also looked for with the other common audio extensions in the same
    // folder (a sheet written for a .wav, then encoded to .flac). NULL on error: *out_error (may be NULL) is
    // AU_ERR_FILE (can not be read, or not a cue sheet or audio file), AU_ERR_NOTHING_TO_DO (an audio file with
    // neither), AU_ERR_INVALID_ARGUMENT or AU_ERR_NO_MEMORY. Works without au_init() and from any thread.
    FFAUDIO_API const AuCueSheet *au_cue_read(const char *path_or_url, int *out_error);

    // The same for cue sheet text in memory; `base` is where it came from, for the FILE names (NULL keeps them).
    FFAUDIO_API const AuCueSheet *au_cue_parse(const char *text, size_t length, const char *base, int *out_error);

    // Done with it (NULL is fine)
    FFAUDIO_API void au_cue_release(const AuCueSheet *sheet);

#ifdef __cplusplus
} // extern "C"
#endif

#endif //FFAUDIO_PLAYLIST_H
