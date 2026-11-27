using System.Text;
using FFAudioSharp.Interop;
using static FFAudioSharp.Interop.Util;

namespace FFAudioSharp;

/// <summary>One entry of a playlist file.</summary>
/// <param name="Path">A URL or a local path, ready for <see cref="FFAudio.Play"/>: relative entries are resolved against the playlist's folder.</param>
/// <param name="Title">From #EXTINF or TitleN; "" when there is none.</param>
/// <param name="Duration">Seconds from #EXTINF or LengthN; -1 when unknown.</param>
public sealed record PlaylistEntry(string Path, string Title, double Duration);

/// <summary>Reads .m3u, .m3u8 and .pls playlists. Works without <see cref="FFAudio.Init"/>.</summary>
public static unsafe class PlaylistFile
{
    /// <summary>
    /// Reads a playlist file or URL (the format comes from its content). An HLS playlist is a stream, not a list: it gives one
    /// entry, itself. Cue sheets are refused with <see cref="ResultCode.Unsupported"/>: use <see cref="CueSheet"/>.
    /// A playlist with no entries is an empty list, not an error.
    /// </summary>
    public static IReadOnlyList<PlaylistEntry> Read(string pathOrUrl)
    {
        int error;
        var p = Lib.au_playlist_read(pathOrUrl, &error);
        if (p == null) throw new FFAudioException((ResultCode)error, pathOrUrl);
        return Take(p);
    }

    /// <summary>The same for playlist text in memory. <paramref name="baseLocation"/> is where it came from, for relative entries.</summary>
    public static IReadOnlyList<PlaylistEntry> Parse(string text, string? baseLocation = null)
    {
        var bytes = Encoding.UTF8.GetBytes(text);
        var baseBytes = baseLocation == null ? null : Encoding.UTF8.GetBytes(baseLocation + "\0");
        int error;
        AuPlaylistFile* p;
        fixed (byte* t = bytes)
        fixed (byte* b = baseBytes)
            p = Lib.au_playlist_parse(t, (nuint)bytes.Length, b, &error);
        if (p == null) throw new FFAudioException((ResultCode)error, "playlist");
        return Take(p);
    }

    private static List<PlaylistEntry> Take(AuPlaylistFile* p)
    {
        try
        {
            var list = new List<PlaylistEntry>(p->count);
            for (var i = 0; i < p->count; i++)
            {
                var e = p->entries[i];
                list.Add(new PlaylistEntry(Str(e->path), Str(e->title), e->duration));
            }
            return list;
        }
        finally
        {
            Lib.au_playlist_file_release(p);
        }
    }
}

/// <summary>One track of a cue sheet, or one chapter: a range of an audio file.</summary>
/// <param name="Number">TRACK nn of the sheet, or the chapter's number (from 1).</param>
/// <param name="Path">The audio file it is in.</param>
/// <param name="StartSeconds">Where it starts in that file.</param>
/// <param name="LengthSeconds">Up to the next track of the same file; 0 = to the end of the file.</param>
/// <param name="ReplayGain">From REM REPLAYGAIN_* lines (NaN where missing).</param>
public sealed record CueTrack(int Number, string Path, double StartSeconds, double LengthSeconds, string Title,
                              string Performer, string Songwriter, string Isrc, ReplayGain ReplayGain)
{
    /// <summary>The options that play this track: its range, and its ReplayGain when the sheet has one.</summary>
    public PlayOptions ToPlayOptions() => new()
    {
        StartSeconds = StartSeconds,
        LengthSeconds = LengthSeconds,
        ReplayGain = ReplayGain.HasTrackGain || ReplayGain.HasAlbumGain ? ReplayGain : null,
    };
}

/// <summary>
/// Several tracks in one file: a .cue sheet, a cue sheet embedded in a file (FLAC, WavPack, APE), or a file's chapters
/// (M4B/M4A audiobooks, Matroska, MP3). Consecutive tracks of one file join exactly: play them with
/// <see cref="CueTrack.ToPlayOptions"/>. Works without <see cref="FFAudio.Init"/>.
/// <code>
/// var cue = CueSheet.Read("/music/album.cue");
/// FFAudio.Play(cue.Tracks[0].Path, cue.Tracks[0].ToPlayOptions());
/// // the next ones with FFAudio.Queue on PrepareNextEvent
/// </code>
/// </summary>
public sealed record CueSheet(string Title, string Performer, string Genre, string Date, IReadOnlyList<CueTrack> Tracks)
{
    /// <summary>
    /// Reads the tracks of a .cue file, or of an audio file: its embedded sheet, otherwise its chapters.
    /// <see cref="ResultCode.NothingToDo"/>: an audio file with neither.
    /// </summary>
    public static unsafe CueSheet Read(string pathOrUrl)
    {
        int error;
        var p = Lib.au_cue_read(pathOrUrl, &error);
        if (p == null) throw new FFAudioException((ResultCode)error, pathOrUrl);
        return Take(p);
    }

    /// <summary>The same for cue sheet text in memory; <paramref name="baseLocation"/> is where it came from, for the FILE names.</summary>
    public static unsafe CueSheet Parse(string text, string? baseLocation = null)
    {
        var bytes = Encoding.UTF8.GetBytes(text);
        var baseBytes = baseLocation == null ? null : Encoding.UTF8.GetBytes(baseLocation + "\0");
        int error;
        AuCueSheet* p;
        fixed (byte* t = bytes)
        fixed (byte* b = baseBytes)
            p = Lib.au_cue_parse(t, (nuint)bytes.Length, b, &error);
        if (p == null) throw new FFAudioException((ResultCode)error, "cue sheet");
        return Take(p);
    }

    private static unsafe CueSheet Take(AuCueSheet* p)
    {
        try
        {
            var tracks = new CueTrack[p->count];
            for (var i = 0; i < tracks.Length; i++)
            {
                var t = p->tracks[i];
                tracks[i] = new CueTrack(t->number, Str(t->path), t->start_seconds, t->length_seconds, Str(t->title),
                                         Str(t->performer), Str(t->songwriter), Str(t->isrc), ReplayGain.From(t->replaygain));
            }
            return new CueSheet(Str(p->title), Str(p->performer), Str(p->genre), Str(p->date), tracks);
        }
        finally
        {
            Lib.au_cue_release(p);
        }
    }
}
