using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using FFAudioSharp.Interop;
using static FFAudioSharp.Interop.Util;

namespace FFAudioSharp;

/// <summary>The loudness of a file or an album (EBU R128 / ITU-R BS.1770). Works without <see cref="FFAudio.Init"/>; files are only read.</summary>
public sealed record Loudness(bool Silent, double IntegratedLufs, double LoudnessRangeLu, double TruePeak, double SamplePeak,
                              double Duration, double ReplayGainGainDb, double R128GainDb, int R128GainQ78)
{
    internal static Loudness From(in AuLoudness n) =>
        new(B(n.silent), n.integrated_lufs, n.loudness_range_lu, n.true_peak, n.sample_peak, n.duration,
            n.replaygain_gain_db, n.r128_gain_db, n.r128_gain_q78);

    internal AuLoudness ToNative() => new()
    {
        silent = B(Silent), integrated_lufs = IntegratedLufs, loudness_range_lu = LoudnessRangeLu, true_peak = TruePeak,
        sample_peak = SamplePeak, duration = Duration, replaygain_gain_db = ReplayGainGainDb, r128_gain_db = R128GainDb,
        r128_gain_q78 = R128GainQ78,
    };

    /// <summary>
    /// Measures one file; blocks for the scan (typically well under a second per song). <paramref name="progress"/> gets
    /// 0..1 (or &lt; 0 when the duration is unknown); return false to cancel (<see cref="ResultCode.Cancelled"/>).
    /// </summary>
    public static unsafe Loudness Analyze(string path, bool skipTruePeak = false, Func<double, bool>? progress = null)
    {
        using var scope = new ProgressScope(skipTruePeak, progress);
        var options = scope.Options;
        AuLoudness l;
        Check(Lib.au_loudness_analyze(path, &options, &l), path);
        return From(l);
    }

    /// <summary>ReplayGain values for <see cref="PlayOptions.ReplayGain"/>: the track's, and the album's when given. Silent results give no gain.</summary>
    public static unsafe ReplayGain ToReplayGain(Loudness track, Loudness? album = null)
    {
        var t = track.ToNative();
        if (album == null) return ReplayGain.From(Lib.au_loudness_to_replaygain(&t, null));
        var a = album.ToNative();
        return ReplayGain.From(Lib.au_loudness_to_replaygain(&t, &a));
    }
}

/// <summary>A measurement that can hold several files merged: an album is its tracks added together, exactly.</summary>
public sealed unsafe class LoudnessMeter : IDisposable
{
    private nint _meter;

    private LoudnessMeter(nint meter) => _meter = meter;
    ~LoudnessMeter() => Dispose(false);

    /// <summary>Measures one file into a new meter. See <see cref="Loudness.Analyze"/> for <paramref name="progress"/>.</summary>
    public static LoudnessMeter Measure(string path, bool skipTruePeak = false, Func<double, bool>? progress = null)
    {
        using var scope = new ProgressScope(skipTruePeak, progress);
        var options = scope.Options;
        nint meter;
        Check(Lib.au_loudness_measure(path, &options, &meter), path);
        return new LoudnessMeter(meter);
    }

    private nint Handle => _meter != 0 ? _meter : throw new ObjectDisposedException(nameof(LoudnessMeter));

    /// <summary>Adds what <paramref name="other"/> measured; it is unchanged.</summary>
    public void Merge(LoudnessMeter other) => Check(Lib.au_loudness_merge(Handle, other.Handle), "merge");

    /// <summary>How many files the meter holds (1 after <see cref="Measure"/>, more after merges).</summary>
    public int FileCount => Lib.au_loudness_file_count(Handle);

    /// <summary>The values of everything measured into the meter. Peaks are the largest of its files.</summary>
    public Loudness Result
    {
        get
        {
            AuLoudness l;
            Check(Lib.au_loudness_result(Handle, &l), "result");
            return Loudness.From(l);
        }
    }

    public void Dispose()
    {
        Dispose(true);
        GC.SuppressFinalize(this);
    }

    private void Dispose(bool disposing)
    {
        var m = Interlocked.Exchange(ref _meter, 0);
        if (m != 0) Lib.au_loudness_meter_free(m);
    }
}

/// <summary>AuLoudnessOptions with a managed progress function behind it, for the length of one call.</summary>
internal unsafe struct ProgressScope : IDisposable
{
    private GCHandle _handle;
    public AuLoudnessOptions Options;

    public ProgressScope(bool skipTruePeak, Func<double, bool>? progress)
    {
        Options = new AuLoudnessOptions { skip_true_peak = B(skipTruePeak) };
        if (progress == null) return;
        _handle = GCHandle.Alloc(progress);
        Options.progress = (delegate* unmanaged[Cdecl]<double, void*, byte>)&Call;
        Options.progress_user = (void*)GCHandle.ToIntPtr(_handle);
    }

    public void Dispose()
    {
        if (_handle.IsAllocated) _handle.Free();
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static byte Call(double fraction, void* user)
    {
        try { return B(((Func<double, bool>)GCHandle.FromIntPtr((nint)user).Target!)(fraction)); }
        catch { return 0; }   // an exception cancels the scan; it must not unwind into native code
    }
}

/// <summary>
/// A persistent per-user store of ReplayGain values by path, shared by every program using ffaudio. A track that starts
/// uses its entry instead of the file's tags; changing an entry also updates queued and playing tracks. Media files are
/// never touched. Works without <see cref="FFAudio.Init"/> and survives <see cref="FFAudio.Shutdown"/>; thread-safe.
/// </summary>
public static unsafe class GainCache
{
    /// <summary>Uses another cache file. null = the default location, "" = memory only. Discards the entries in memory.</summary>
    public static void SetFile(string? path) => Check(Lib.au_gain_cache_set_file(path), "cache file");

    /// <summary>The cache file in use, "" when memory only.</summary>
    public static string File
    {
        get
        {
            var bytes = new byte[4096];
            fixed (byte* b = bytes)
            {
                Check(Lib.au_gain_cache_get_file(b, (nuint)bytes.Length), "cache file");
                return Str(b);
            }
        }
    }

    /// <summary>Reads the file again, e.g. to pick up entries another process added.</summary>
    public static void Reload() => Check(Lib.au_gain_cache_reload(), "reload");

    /// <summary>Stores (or replaces) the values for a path. <see cref="ResultCode.NotSaved"/>: kept in memory, but the file could not be written.</summary>
    public static void Set(string path, ReplayGain rg)
    {
        var n = rg.ToNative();
        Check(Lib.au_gain_cache_set(path, &n), path);
    }

    /// <summary>The cached values for a path, or null.</summary>
    public static ReplayGain? Get(string path)
    {
        AuReplayGain n;
        return Lib.au_gain_cache_get(path, &n) ? ReplayGain.From(n) : null;
    }

    public static bool Contains(string path) => Lib.au_gain_cache_get(path, null);

    /// <summary>Removes an entry: true when there was one. <see cref="ResultCode.NotSaved"/>: gone from memory, but the file could not be written.</summary>
    public static bool Remove(string path)
    {
        var r = Lib.au_gain_cache_remove(path);
        if (r == (int)ResultCode.NothingToDo) return false;
        Check(r, path);
        return true;
    }

    /// <summary>Removes every entry, also from the file.</summary>
    public static void Clear() => Check(Lib.au_gain_cache_clear(), "clear");

    public static int Count => Lib.au_gain_cache_count();

    /// <summary>Visits every entry (a snapshot, so the visitor may change the cache). Return false to stop. Returns the number visited.</summary>
    public static int ForEach(Func<string, ReplayGain, bool> visitor)
    {
        var handle = GCHandle.Alloc(visitor);
        try { return Lib.au_gain_cache_foreach((delegate* unmanaged[Cdecl]<byte*, AuReplayGain*, void*, byte>)&Visit, (void*)GCHandle.ToIntPtr(handle)); }
        finally { handle.Free(); }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static byte Visit(byte* path, AuReplayGain* rg, void* user)
    {
        try { return B(((Func<string, ReplayGain, bool>)GCHandle.FromIntPtr((nint)user).Target!)(Str(path), ReplayGain.From(*rg))); }
        catch { return 0; }
    }

    /// <summary>
    /// The entry for a path: returned at once when cached, otherwise the file is measured (it blocks for the scan) and the
    /// result stored. <see cref="ResultCode.NotSaved"/>: measured and cached in memory, but not written. Avoid it in the
    /// event handler: it runs on the player's control thread. <see cref="GainScan"/> measures in the background.
    /// </summary>
    public static ReplayGain Scan(string path, bool skipTruePeak = false, Func<double, bool>? progress = null)
    {
        using var scope = new ProgressScope(skipTruePeak, progress);
        var options = scope.Options;
        AuReplayGain rg;
        Check(Lib.au_gain_cache_scan(path, &options, &rg), path);
        return ReplayGain.From(rg);
    }
}

/// <summary>The background measurement queue's settings: <see cref="GainScan.Configure"/>.</summary>
public sealed class GainScanSettings
{
    /// <summary>Worker threads, 1..16; 0 = the default (2, or 1 on single-core machines).</summary>
    public int Threads { get; set; }
    /// <summary>Faster; the sample peak is stored as the peak.</summary>
    public bool SkipTruePeak { get; set; }
    /// <summary>
    /// A requested file was measured and stored (Ok, with the values; NotSaved: in memory only), could not be measured
    /// (File, ...; null) or was cancelled (Cancelled). Runs on a worker thread: keep it short.
    /// </summary>
    public Action<string, ResultCode, ReplayGain?>? OnCached { get; set; }
    /// <summary>Measure network files too (it downloads the whole file). Default: they are skipped.</summary>
    public bool MeasureStreams { get; set; }
}

/// <summary>
/// ffaudio measures requested files on its own low-priority threads and stores the results in the <see cref="GainCache"/>;
/// queued and playing tracks pick them up. Nothing blocks the caller, and requests for paths that are cached, queued or
/// being measured are skipped, so a playlist window can be re-sent whenever the position changes. Works without
/// <see cref="FFAudio.Init"/>; files are only read.
/// </summary>
public static unsafe class GainScan
{
    // The handles of callbacks that were replaced. A worker may still be running a job that holds the old pointer, so they
    // are only freed by Stop(), which waits for the workers to be gone (a handful of small handles per Configure call).
    private static readonly List<GCHandle> Retired = [];

    private static GCHandle _onCached;

    /// <summary>Sets how the queue works; null restores the defaults. Can be called at any time.</summary>
    public static void Configure(GainScanSettings? settings)
    {
        GCHandle handle = default;
        int result;
        if (settings == null)
        {
            result = Lib.au_set_gain_scan(null);
        }
        else
        {
            var n = new AuGainScanSettings { threads = settings.Threads, skip_true_peak = B(settings.SkipTruePeak), measure_streams = B(settings.MeasureStreams) };
            if (settings.OnCached != null)
            {
                handle = GCHandle.Alloc(settings.OnCached);
                n.on_cached = (delegate* unmanaged[Cdecl]<byte*, int, AuReplayGain*, void*, void>)&Cached;
                n.user = (void*)GCHandle.ToIntPtr(handle);
            }
            result = Lib.au_set_gain_scan(&n);
        }
        if (result < 0)
        {
            if (handle.IsAllocated) handle.Free();
            throw new FFAudioException((ResultCode)result, "gain scan settings");
        }
        lock (Retired)
        {
            if (_onCached.IsAllocated) Retired.Add(_onCached);
            _onCached = handle;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void Cached(byte* path, int result, AuReplayGain* rg, void* user)
    {
        try
        {
            ((Action<string, ResultCode, ReplayGain?>)GCHandle.FromIntPtr((nint)user).Target!)(
                Str(path), (ResultCode)result, rg == null ? null : ReplayGain.From(*rg));
        }
        catch
        {
            // an exception must not unwind into native code
        }
    }

    /// <summary>Measures these files (track values). Returns the number queued: cached, queued and known-bad files are skipped.</summary>
    public static int Request(IReadOnlyCollection<string> paths, ScanPriority priority = ScanPriority.Prefetch) => Submit(paths, priority, album: false);

    /// <summary>Measures these files as one album: each gets its track values and the album's. Returns the number queued.</summary>
    public static int RequestAlbum(IReadOnlyCollection<string> paths, ScanPriority priority = ScanPriority.Prefetch) => Submit(paths, priority, album: true);

    private static int Submit(IReadOnlyCollection<string> paths, ScanPriority priority, bool album)
    {
        if (paths.Count == 0) return 0;
        using var utf8 = new Utf8Array(paths);
        fixed (nint* p = utf8.Pointers)
        {
            var r = album ? Lib.au_gain_scan_request_album((byte**)p, utf8.Count, (int)priority)
                          : Lib.au_gain_scan_request((byte**)p, utf8.Count, (int)priority);
            return Check(r, "gain scan request");
        }
    }

    /// <summary>Drops every queued request and stops the measurement in progress.</summary>
    public static void Cancel() => Lib.au_gain_scan_cancel();

    /// <summary>Paths queued or being measured.</summary>
    public static int Pending => Lib.au_gain_scan_pending();

    /// <summary>
    /// Cancels everything and stops the worker threads, waiting for them; they start again on the next request. Call it
    /// before the process unloads the library. From the <see cref="GainScanSettings.OnCached"/> callback it throws
    /// <see cref="ResultCode.WrongThread"/>.
    /// </summary>
    public static void Stop()
    {
        Check(Lib.au_gain_scan_stop(), "gain scan stop");
        // No worker is left, so no replaced callback can be called any more
        lock (Retired)
        {
            foreach (var h in Retired) h.Free();
            Retired.Clear();
        }
    }
}
