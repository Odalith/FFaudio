using System.Runtime.InteropServices;
using System.Runtime.CompilerServices;
using FFAudioSharp.Interop;
using static FFAudioSharp.Interop.Util;

namespace FFAudioSharp;

/// <summary>
/// Loudness values of a file. Gains are in dB relative to the ReplayGain 2.0 reference (-18 LUFS); NaN marks a missing
/// value. Peaks are linear (1.0 = full scale).
/// </summary>
public readonly record struct ReplayGain(float TrackGainDb, float TrackPeak, float AlbumGainDb, float AlbumPeak, bool FromR128 = false)
{
    /// <summary>No values at all.</summary>
    public static ReplayGain None => new(float.NaN, float.NaN, float.NaN, float.NaN);

    public bool HasTrackGain => !float.IsNaN(TrackGainDb);
    public bool HasAlbumGain => !float.IsNaN(AlbumGainDb);

    internal AuReplayGain ToNative() => new()
    {
        track_gain_db = TrackGainDb, track_peak = TrackPeak, album_gain_db = AlbumGainDb, album_peak = AlbumPeak,
        from_r128 = B(FromR128),
    };

    internal static ReplayGain From(in AuReplayGain n) =>
        new(n.track_gain_db, n.track_peak, n.album_gain_db, n.album_peak, B(n.from_r128));
}

/// <summary>How a fade happens. <see cref="Seconds"/> &lt;= 0 means no fade (a cut, or gapless at a natural track end).</summary>
public sealed record Fade
{
    public double Seconds { get; init; }
    public FadeCurve Curve { get; init; } = FadeCurve.EqualPower;

    /// <summary>
    /// For <see cref="FadeCurve.Custom"/>: t goes 0 → 1 over the fade; return the gain (0..1) of the track fading in (the
    /// one fading out uses curve(1 - t)). ffaudio samples it on its control thread at 257 points when the fade is set
    /// up, so it must not throw or block. The delegate is kept alive for the rest of the process.
    /// </summary>
    public Func<float, float>? CustomCurve { get; init; }

    internal unsafe AuFade ToNative()
    {
        var custom = Curve == FadeCurve.Custom && CustomCurve != null;
        return new AuFade
        {
            seconds = Seconds, curve = (int)Curve,
            custom = custom ? CurveRegistry.Function : null,
            custom_user = custom ? CurveRegistry.UserOf(CustomCurve!) : null,
        };
    }
}

/// <summary>Keeps custom fade curves alive and calls them from native code.</summary>
internal static unsafe class CurveRegistry
{
    private static readonly Dictionary<Func<float, float>, GCHandle> Handles = new();

    public static void* Function => (delegate* unmanaged[Cdecl]<float, void*, float>)&Call;

    public static void* UserOf(Func<float, float> curve)
    {
        lock (Handles)
        {
            if (!Handles.TryGetValue(curve, out var h)) Handles[curve] = h = GCHandle.Alloc(curve);
            return (void*)GCHandle.ToIntPtr(h);
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static float Call(float t, void* user)
    {
        try { return ((Func<float, float>)GCHandle.FromIntPtr((nint)user).Target!)(t); }
        catch { return t; }   // never throw into native code
    }
}

/// <summary>When fades happen. A null member means "no fade".</summary>
public sealed class Crossfade
{
    /// <summary>The current track nears its end and a track is queued.</summary>
    public Fade? OnTrackEnd { get; set; }
    /// <summary><see cref="FFAudio.Skip"/>.</summary>
    public Fade? OnSkip { get; set; }
    /// <summary><see cref="FFAudio.Play"/> while another track is playing.</summary>
    public Fade? OnPlay { get; set; }
    /// <summary><see cref="FFAudio.Stop"/>: fade out, then stop.</summary>
    public Fade? OnStop { get; set; }
    /// <summary>Pausing fades out, resuming fades back in.</summary>
    public Fade? OnPause { get; set; }
    /// <summary>Tracks shorter than this are not crossfaded at their end; they play gapless. 0 = twice OnTrackEnd's seconds.</summary>
    public double MinTrackSeconds { get; set; }

    internal AuCrossfade ToNative() => new()
    {
        on_track_end = (OnTrackEnd ?? new Fade()).ToNative(), on_skip = (OnSkip ?? new Fade()).ToNative(),
        on_play = (OnPlay ?? new Fade()).ToNative(), on_stop = (OnStop ?? new Fade()).ToNative(),
        on_pause = (OnPause ?? new Fade()).ToNative(), min_track_seconds = MinTrackSeconds,
    };
}

/// <summary>Applying ReplayGain / R128 tags: <see cref="FFAudio.SetReplayGain"/>.</summary>
public sealed class ReplayGainSettings
{
    /// <summary>Off by default, as in C and the Python binding: set Track or Album.</summary>
    public ReplayGainMode Mode { get; set; } = ReplayGainMode.Off;
    /// <summary>Added to tagged gains, -15..+15 dB.</summary>
    public double PreampDb { get; set; }
    /// <summary>Used for files without values, -30..+15 dB.</summary>
    public double FallbackGainDb { get; set; }
    /// <summary>Lower the gain so the tagged peak does not exceed full scale.</summary>
    public bool PreventClipping { get; set; }
    /// <summary>Measure a starting track that has no value with the background queue (writes to the ReplayGain cache).</summary>
    public bool MeasureMissing { get; set; }
    /// <summary>How long such a track waits for its measurement. 0 = 0.5 s, &lt; 0 = do not wait. At most 10.</summary>
    public double MeasureWaitSeconds { get; set; }
}

/// <summary>Settings for every network stream: <see cref="FFAudio.SetNetwork"/>. Zero means the default.</summary>
public sealed class NetworkSettings
{
    /// <summary>null = "ffaudio/&lt;version&gt;".</summary>
    public string? UserAgent { get; set; }
    /// <summary>Connecting and every read; 0 = 10 s.</summary>
    public double TimeoutSeconds { get; set; }
    /// <summary>Live streams: audio buffered before starting and after a stall; 0 = 2 s.</summary>
    public double BufferSeconds { get; set; }
    /// <summary>Most audio read ahead from the network; 0 = 10 s.</summary>
    public double MaxBufferSeconds { get; set; }
    /// <summary>After a dropped connection; 0 = 5, -1 = never.</summary>
    public int ReconnectAttempts { get; set; }
}

/// <summary>The options of one track, for <see cref="FFAudio.Play"/> and <see cref="FFAudio.Queue"/>. Reusable.</summary>
public sealed class PlayOptions
{
    /// <summary>Start this far into the file (0 = from the start). Exact to the sample.</summary>
    public double StartSeconds { get; set; }
    /// <summary>Play this long, then end the track (0 = to the end). Positions, seeks and events count from the range's start.</summary>
    public double LengthSeconds { get; set; }
    /// <summary>This track's own FFmpeg filter graph, after the conversion to the output format. null = none.</summary>
    public string? Filters { get; set; }
    /// <summary>The fade into this track, instead of the <see cref="Crossfade"/> setting.</summary>
    public Fade? FadeIn { get; set; }
    /// <summary>Values from your own library; null = the ReplayGain cache, then the file's tags.</summary>
    public ReplayGain? ReplayGain { get; set; }
    /// <summary>Network streams: extra request headers, one "Name: value" per line.</summary>
    public string? HttpHeaders { get; set; }

    /// <summary>A native AuPlayOptions with these values; free it with au_play_options_free.</summary>
    internal unsafe nint Build()
    {
        var o = Lib.au_play_options_create();
        if (o == 0) throw new FFAudioException(ResultCode.NoMemory);
        try
        {
            if (StartSeconds != 0 || LengthSeconds != 0) Check(Lib.au_play_options_set_range(o, StartSeconds, LengthSeconds), "range");
            if (Filters != null) Check(Lib.au_play_options_set_filters(o, Filters), "filters");
            if (FadeIn != null) { var f = FadeIn.ToNative(); Check(Lib.au_play_options_set_fade_in(o, &f), "fade in"); }
            if (ReplayGain is { } rg) { var n = rg.ToNative(); Check(Lib.au_play_options_set_replaygain(o, &n), "replaygain"); }
            if (HttpHeaders != null) Check(Lib.au_play_options_set_http_headers(o, HttpHeaders), "http headers");
            return o;
        }
        catch
        {
            Lib.au_play_options_free(o);
            throw;
        }
    }
}

/// <summary>A snapshot of the player.</summary>
/// <param name="Handle">The audible track, 0 when nothing is playing.</param>
/// <param name="Position">Seconds into it (into its range), as heard; -1 when nothing is playing.</param>
/// <param name="Duration">Seconds, -1 when unknown or nothing is playing.</param>
/// <param name="Volume">0..2, 1 = 100 %.</param>
/// <param name="SampleRate">The output format every track is converted to; 0 before a device is open.</param>
public readonly record struct Status(int Handle, double Position, double Duration, bool Buffering, bool Paused, bool Muted,
                                     float Volume, double Speed, bool KeepPitch, int LoopCount, int SampleRate, int Channels)
{
    public bool Playing => Handle != 0 && !Paused;
}

/// <summary>Everything about a file, copied out of the library.</summary>
public sealed record MediaInfo
{
    public string Path { get; init; } = "";
    public string Container { get; init; } = "";
    public string Codec { get; init; } = "";
    public string ChannelLayout { get; init; } = "";
    /// <summary>Seconds, -1 when unknown.</summary>
    public double Duration { get; init; }
    /// <summary>Bits per second, 0 when unknown.</summary>
    public long BitRate { get; init; }
    public int SampleRate { get; init; }
    public int Channels { get; init; }
    /// <summary>0 for lossy codecs or unknown.</summary>
    public int BitsPerSample { get; init; }
    /// <summary>A live stream: no duration and no seeking.</summary>
    public bool Live { get; init; }
    public bool Seekable { get; init; }
    public int TrackNumber { get; init; }
    public int TrackTotal { get; init; }
    public int DiscNumber { get; init; }
    public int DiscTotal { get; init; }
    public ReplayGain ReplayGain { get; init; }
    /// <summary>Every tag, in file order.</summary>
    public IReadOnlyList<KeyValuePair<string, string>> Tags { get; init; } = [];
    /// <summary>Embedded pictures; <see cref="FFAudio.GetPicture"/> reads one.</summary>
    public int PictureCount { get; init; }

    // The common tags are found by name in every format (ALBUMARTIST, album_artist, ...); null when absent
    public string? Title { get; init; }
    public string? Artist { get; init; }
    public string? Album { get; init; }
    public string? AlbumArtist { get; init; }
    public string? Date { get; init; }
    public string? Genre { get; init; }
    public string? Composer { get; init; }
    public string? Comment { get; init; }

    /// <summary>A tag by its exact name (case-insensitive); null when absent.</summary>
    public string? Tag(string key)
    {
        foreach (var t in Tags)
            if (string.Equals(t.Key, key, StringComparison.OrdinalIgnoreCase)) return t.Value;
        return null;
    }

    internal static unsafe MediaInfo From(AuMediaInfo* p)
    {
        var tags = new KeyValuePair<string, string>[p->tag_count];
        for (var i = 0; i < tags.Length; i++) tags[i] = new(Str(p->tags[i].key), Str(p->tags[i].value));
        string? Named(string key) => StrOrNull(Lib.au_media_tag(p, key));
        return new MediaInfo
        {
            Path = Str(p->path), Container = Str(p->container), Codec = Str(p->codec), ChannelLayout = Str(p->channel_layout),
            Duration = p->duration, BitRate = p->bit_rate, SampleRate = p->sample_rate, Channels = p->channels,
            BitsPerSample = p->bits_per_sample, Live = B(p->live), Seekable = B(p->seekable),
            TrackNumber = p->track_number, TrackTotal = p->track_total, DiscNumber = p->disc_number, DiscTotal = p->disc_total,
            ReplayGain = ReplayGain.From(p->replaygain), Tags = tags, PictureCount = p->picture_count,
            Title = Named("title"), Artist = Named("artist"), Album = Named("album"), AlbumArtist = Named("album_artist"),
            Date = Named("date"), Genre = Named("genre"), Composer = Named("composer"), Comment = Named("comment"),
        };
    }
}

/// <summary>An embedded picture (cover art), copied out of the library.</summary>
public sealed record Picture(byte[] Data, string Mime, PictureType Type, int Width, int Height, string Description)
{
    internal static unsafe Picture From(AuPicture* p) =>
        new(new ReadOnlySpan<byte>(p->data, checked((int)p->size)).ToArray(), Str(p->mime), (PictureType)p->type,
            p->width, p->height, Str(p->description));
}

/// <summary>Peak and RMS of the last moments heard, per channel (linear, 1.0 = full scale; before the volume).</summary>
public sealed record Levels(int Channels, int SampleRate, double WindowSeconds, float[] Peak, float[] Rms);
