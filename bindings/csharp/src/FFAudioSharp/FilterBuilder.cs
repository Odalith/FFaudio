using System.Text;
using FFAudioSharp.Interop;
using static FFAudioSharp.Interop.Util;

namespace FFAudioSharp;

// Settings of the filter helpers. Each starts at the defaults of the C library (au_*_defaults), so there is one place
// that knows them; change what you need.

/// <summary>A 10-band graphic equalizer.</summary>
public sealed class Equalizer
{
    public const int BandCount = 10;
    /// <summary>-30..+30 dB per band; 0 = band not used.</summary>
    public double[] GainDb { get; } = new double[BandCount];
    /// <summary>Centre frequencies.</summary>
    public double[] FrequencyHz { get; } = new double[BandCount];
    /// <summary>Band widths in Hz.</summary>
    public double[] WidthHz { get; } = new double[BandCount];
    public double PreampDb { get; set; }

    public unsafe Equalizer()
    {
        var d = Lib.au_equalizer_defaults();
        for (var i = 0; i < BandCount; i++) { GainDb[i] = d.gain_db[i]; FrequencyHz[i] = d.frequency_hz[i]; WidthHz[i] = d.width_hz[i]; }
        PreampDb = d.preamp_db;
    }

    internal unsafe AuEqualizer ToNative()
    {
        AuEqualizer n = default;
        for (var i = 0; i < BandCount; i++) { n.gain_db[i] = GainDb[i]; n.frequency_hz[i] = FrequencyHz[i]; n.width_hz[i] = WidthHz[i]; }
        n.preamp_db = PreampDb;
        return n;
    }
}

/// <summary>One band of a parametric equalizer.</summary>
/// <param name="FrequencyHz">1..96000, and below half the output sample rate.</param>
/// <param name="Q">0.025..40 (0.7071 = Butterworth for pass filters and shelves).</param>
/// <param name="GainDb">-30..+30 for peaks and shelves.</param>
public sealed record PeqBand(PeqType Type, double FrequencyHz, double Q, double GainDb);

/// <summary>A parametric equalizer of up to 32 bands, e.g. an AutoEq profile.</summary>
public sealed class ParametricEq
{
    public const int MaxBands = 32;
    /// <summary>-30..+30 dB; AutoEq profiles give one that leaves room for the boosts.</summary>
    public double PreampDb { get; set; }
    public List<PeqBand> Bands { get; } = [];

    /// <summary>Reads an AutoEq / Equalizer APO text ("Preamp: -6 dB", "Filter 1: ON PK Fc 100 Hz Gain 3 dB Q 1.0"...).</summary>
    public static unsafe ParametricEq Parse(string text)
    {
        var bytes = Encoding.UTF8.GetBytes(text + "\0");
        AuParametricEq n;
        var error = stackalloc byte[256];
        int r;
        fixed (byte* t = bytes) r = Lib.au_parametric_eq_parse(t, &n, error, 256);
        if (r < 0) throw new FFAudioException((ResultCode)r, Str(error));
        var eq = new ParametricEq { PreampDb = n.preamp_db };
        for (var i = 0; i < n.band_count; i++)
        {
            var b = n.bands[i];
            eq.Bands.Add(new PeqBand((PeqType)b.type, b.frequency_hz, b.q, b.gain_db));
        }
        return eq;
    }

    internal AuParametricEq ToNative()
    {
        if (Bands.Count > MaxBands) throw new ArgumentException($"at most {MaxBands} bands");
        AuParametricEq n = default;
        n.preamp_db = PreampDb;
        n.band_count = Bands.Count;
        for (var i = 0; i < Bands.Count; i++)
            n.bands[i] = new AuPeqBand { type = (int)Bands[i].Type, frequency_hz = Bands[i].FrequencyHz, q = Bands[i].Q, gain_db = Bands[i].GainDb };
        return n;
    }
}

/// <summary>Balance, width and channel swap.</summary>
public sealed class Stereo
{
    public double Balance { get; set; }
    public double Width { get; set; }
    /// <summary>Swap left and right.</summary>
    public bool Swap { get; set; }

    public Stereo()
    {
        var d = Lib.au_stereo_defaults();
        Balance = d.balance; Width = d.width; Swap = B(d.swap);
    }

    internal AuStereo ToNative() => new() { balance = Balance, width = Width, swap = B(Swap) };
}

/// <summary>A limiter: peaks above the ceiling are turned down smoothly.</summary>
public sealed class Limiter
{
    /// <summary>-24..0 dB.</summary>
    public double CeilingDb { get; set; }
    /// <summary>0.1..80 ms (also the delay the limiter adds).</summary>
    public double AttackMs { get; set; }
    /// <summary>1..8000 ms.</summary>
    public double ReleaseMs { get; set; }

    public Limiter()
    {
        var d = Lib.au_limiter_defaults();
        CeilingDb = d.ceiling_db; AttackMs = d.attack_ms; ReleaseMs = d.release_ms;
    }

    internal AuLimiter ToNative() => new() { ceiling_db = CeilingDb, attack_ms = AttackMs, release_ms = ReleaseMs };
}

/// <summary>A dynamic range compressor.</summary>
public sealed class Compressor
{
    /// <summary>-60..0 dB: compression starts above this level.</summary>
    public double ThresholdDb { get; set; }
    /// <summary>1..20: 4 = 4 dB over the threshold become 1 dB.</summary>
    public double Ratio { get; set; }
    public double AttackMs { get; set; }
    public double ReleaseMs { get; set; }
    /// <summary>0..18 dB: width of the soft transition around the threshold.</summary>
    public double KneeDb { get; set; }
    /// <summary>0..36 dB: gain after compression.</summary>
    public double MakeupDb { get; set; }

    public Compressor() : this(Lib.au_compressor_defaults()) { }
    private Compressor(AuCompressor d)
    {
        ThresholdDb = d.threshold_db; Ratio = d.ratio; AttackMs = d.attack_ms; ReleaseMs = d.release_ms; KneeDb = d.knee_db; MakeupDb = d.makeup_db;
    }

    /// <summary>A preset for quiet listening: loud parts come down, quiet ones up.</summary>
    public static Compressor Night() => new(Lib.au_compressor_night());

    internal AuCompressor ToNative() => new()
    {
        threshold_db = ThresholdDb, ratio = Ratio, attack_ms = AttackMs, release_ms = ReleaseMs, knee_db = KneeDb, makeup_db = MakeupDb,
    };
}

/// <summary>Click repair (FFmpeg's adeclick).</summary>
public sealed class Declick
{
    public double WindowMs { get; set; }
    public double OverlapPercent { get; set; }
    public double Threshold { get; set; }
    /// <summary>0..10: clicks closer than this are repaired as one.</summary>
    public double Burst { get; set; }

    public Declick()
    {
        var d = Lib.au_declick_defaults();
        WindowMs = d.window_ms; OverlapPercent = d.overlap_percent; Threshold = d.threshold; Burst = d.burst;
    }

    internal AuDeclick ToNative() => new() { window_ms = WindowMs, overlap_percent = OverlapPercent, threshold = Threshold, burst = Burst };
}

/// <summary>Clipping repair (FFmpeg's adeclip). Best used with <see cref="FFAudio.SetSourceFilters"/>.</summary>
public sealed class Declip
{
    public double WindowMs { get; set; }
    public double OverlapPercent { get; set; }
    public double Threshold { get; set; }

    public Declip()
    {
        var d = Lib.au_declip_defaults();
        WindowMs = d.window_ms; OverlapPercent = d.overlap_percent; Threshold = d.threshold;
    }

    internal AuDeclip ToNative() => new() { window_ms = WindowMs, overlap_percent = OverlapPercent, threshold = Threshold };
}

/// <summary>Headphone crossfeed (FFmpeg's crossfeed).</summary>
public sealed class Crossfeed
{
    /// <summary>0..1.</summary>
    public double Strength { get; set; }
    /// <summary>Soundstage wideness 0..1.</summary>
    public double Range { get; set; }
    /// <summary>Low shelf slope 0.01..1.</summary>
    public double Slope { get; set; }
    public double LevelIn { get; set; }
    public double LevelOut { get; set; }

    public Crossfeed()
    {
        var d = Lib.au_crossfeed_defaults();
        Strength = d.strength; Range = d.range; Slope = d.slope; LevelIn = d.level_in; LevelOut = d.level_out;
    }

    internal AuCrossfeed ToNative() => new() { strength = Strength, range = Range, slope = Slope, level_in = LevelIn, level_out = LevelOut };
}

/// <summary>
/// Builds FFmpeg filter strings for <see cref="FFAudio.SetFilters"/>, <see cref="PlayOptions.Filters"/> and
/// <see cref="FFAudio.SetTrackFilters"/>. Every helper takes an optional <c>name</c>: a named filter can be changed
/// while playing with <see cref="FFAudio.SendFilterCommand"/>. Passing null for the settings uses the defaults. A helper
/// that fails throws <see cref="FFAudioException"/> (with the library's reason) and leaves the builder unchanged.
/// <code>
/// using var fb = new FilterBuilder().AddEqualizer(eq, "eq").AddLimiter();
/// FFAudio.SetFilters(fb.Filters);
/// </code>
/// </summary>
public sealed unsafe class FilterBuilder : IDisposable
{
    private nint _fb;

    public FilterBuilder()
    {
        _fb = Lib.au_fb_create();
        if (_fb == 0) throw new FFAudioException(ResultCode.NoMemory);
    }

    ~FilterBuilder() => Free();

    public void Dispose()
    {
        Free();
        GC.SuppressFinalize(this);
    }

    private void Free()
    {
        var fb = Interlocked.Exchange(ref _fb, 0);
        if (fb != 0) Lib.au_fb_free(fb);
    }

    private nint Handle => _fb != 0 ? _fb : throw new ObjectDisposedException(nameof(FilterBuilder));

    private FilterBuilder Done(int result)
    {
        if (result < 0) throw new FFAudioException((ResultCode)result, Str(Lib.au_fb_last_error(Handle)));
        return this;
    }

    /// <summary>The filter string so far.</summary>
    public string Filters => Str(Lib.au_fb_peek(Handle));
    public override string ToString() => _fb == 0 ? "" : Filters;

    public FilterBuilder Clear() { Lib.au_fb_clear(Handle); return this; }

    /// <summary>Appends any FFmpeg filter, e.g. "aecho=0.8:0.9:1000:0.3". No name.</summary>
    public FilterBuilder Add(string filter) => Done(Lib.au_fb_add(Handle, filter));
    public FilterBuilder AddVolumeDb(double gainDb, string? name = null) => Done(Lib.au_fb_add_volume_db(Handle, name, gainDb));

    public FilterBuilder AddEqualizer(Equalizer? eq = null, string? name = null)
    {
        if (eq == null) return Done(Lib.au_fb_add_equalizer(Handle, name, null));
        var n = eq.ToNative();
        return Done(Lib.au_fb_add_equalizer(Handle, name, &n));
    }

    public FilterBuilder AddParametricEq(ParametricEq eq, string? name = null)
    {
        var n = eq.ToNative();
        return Done(Lib.au_fb_add_parametric_eq(Handle, name, &n));
    }

    /// <summary>Bass and treble shelves in dB.</summary>
    public FilterBuilder AddTone(double bassDb, double trebleDb, string? name = null) => Done(Lib.au_fb_add_tone(Handle, name, bassDb, trebleDb));

    public FilterBuilder AddStereo(Stereo? stereo = null, string? name = null)
    {
        if (stereo == null) return Done(Lib.au_fb_add_stereo(Handle, name, null));
        var n = stereo.ToNative();
        return Done(Lib.au_fb_add_stereo(Handle, name, &n));
    }

    public FilterBuilder AddLimiter(Limiter? limiter = null, string? name = null)
    {
        if (limiter == null) return Done(Lib.au_fb_add_limiter(Handle, name, null));
        var n = limiter.ToNative();
        return Done(Lib.au_fb_add_limiter(Handle, name, &n));
    }

    public FilterBuilder AddCompressor(Compressor? compressor = null, string? name = null)
    {
        if (compressor == null) return Done(Lib.au_fb_add_compressor(Handle, name, null));
        var n = compressor.ToNative();
        return Done(Lib.au_fb_add_compressor(Handle, name, &n));
    }

    public FilterBuilder AddDeclick(Declick? declick = null, string? name = null)
    {
        if (declick == null) return Done(Lib.au_fb_add_declick(Handle, name, null));
        var n = declick.ToNative();
        return Done(Lib.au_fb_add_declick(Handle, name, &n));
    }

    public FilterBuilder AddDeclip(Declip? declip = null, string? name = null)
    {
        if (declip == null) return Done(Lib.au_fb_add_declip(Handle, name, null));
        var n = declip.ToNative();
        return Done(Lib.au_fb_add_declip(Handle, name, &n));
    }

    public FilterBuilder AddCrossfeed(Crossfeed? crossfeed = null, string? name = null)
    {
        if (crossfeed == null) return Done(Lib.au_fb_add_crossfeed(Handle, name, null));
        var n = crossfeed.ToNative();
        return Done(Lib.au_fb_add_crossfeed(Handle, name, &n));
    }

    /// <summary>Bauer stereophonic-to-binaural, with one of the standard profiles (needs an FFmpeg built with libbs2b).</summary>
    public FilterBuilder AddBs2b(Bs2bProfile profile = Bs2bProfile.Default, string? name = null) => Done(Lib.au_fb_add_bs2b_profile(Handle, name, (int)profile));
    /// <summary>bs2b with your own cut-off frequency (Hz) and feed level (tenths of a dB).</summary>
    public FilterBuilder AddBs2b(int fcutHz, int feedTenthDb, string? name = null) => Done(Lib.au_fb_add_bs2b(Handle, name, fcutHz, feedTenthDb));
}
