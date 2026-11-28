using FFAudioSharp.Interop;
using static FFAudioSharp.Interop.Util;

namespace FFAudioSharp;

/// <summary>
/// A spectrum analyzer over the last <see cref="FftSize"/> samples heard (a Hann-windowed FFT), for visualizers. Like the
/// level meters it shows the music after ReplayGain and fades, before the volume. Each instance is used by one thread
/// at a time; any number may exist.
/// </summary>
public sealed unsafe class Spectrum : IDisposable
{
    private nint _spectrum;

    /// <param name="fftSize">A power of two from 256 to 16384.</param>
    public Spectrum(int fftSize = 2048)
    {
        _spectrum = Lib.au_spectrum_create(fftSize);
        if (_spectrum == 0) throw new ArgumentException("fftSize must be a power of two from 256 to 16384", nameof(fftSize));
        FftSize = fftSize;
    }

    ~Spectrum() => Free();

    public int FftSize { get; }

    /// <summary>The number of values <see cref="Read(int)"/> returns: FftSize / 2 + 1.</summary>
    public int BinCount => FftSize / 2 + 1;

    /// <summary>The output sample rate at the last read (bin k is at k * SampleRate / FftSize Hz); 0 before one.</summary>
    public int SampleRate { get; private set; }

    /// <summary>
    /// Linear magnitudes of each bin, scaled so a full-scale sine reads 1.0. <paramref name="channel"/> -1 = all channels (their
    /// power averaged, so nothing cancels out), else one (0 = front left). Zeros when nothing plays or while paused.
    /// </summary>
    public float[] Read(int channel = -1)
    {
        var magnitudes = new float[BinCount];
        Read(magnitudes, channel);
        return magnitudes;
    }

    /// <summary>The same into your own array or span, which needs <see cref="BinCount"/> or more floats.</summary>
    public void Read(Span<float> magnitudes, int channel = -1)
    {
        int rate;
        int r;
        fixed (float* m = magnitudes) r = Lib.au_spectrum_read(Handle, channel, m, magnitudes.Length, &rate);
        Check(r, "spectrum");
        SampleRate = rate;
    }

    /// <summary>The bins grouped into log-spaced bands, each the largest magnitude in it: for bar visualizers.</summary>
    public float[] ReadBands(int bands = 32, double minHz = 30, double maxHz = 16000, int channel = -1)
    {
        var magnitudes = Read(channel);
        return GroupBands(magnitudes, FftSize, SampleRate, minHz, maxHz, bands);
    }

    /// <summary>Groups magnitudes from <see cref="Read(int)"/> into log-spaced bands (maxHz is capped at sampleRate / 2).</summary>
    public static float[] GroupBands(ReadOnlySpan<float> magnitudes, int fftSize, int sampleRate, double minHz, double maxHz, int bands)
    {
        if (bands <= 0) throw new ArgumentOutOfRangeException(nameof(bands));
        var result = new float[bands];
        fixed (float* m = magnitudes)
        fixed (float* b = result)
            Check(Lib.au_spectrum_bands(m, fftSize, sampleRate, minHz, maxHz, b, bands), "spectrum bands");
        return result;
    }

    private nint Handle => _spectrum != 0 ? _spectrum : throw new ObjectDisposedException(nameof(Spectrum));

    private void Free()
    {
        var s = Interlocked.Exchange(ref _spectrum, 0);
        if (s != 0) Lib.au_spectrum_free(s);
    }

    public void Dispose()
    {
        Free();
        GC.SuppressFinalize(this);
    }
}
