using System.Diagnostics;
using Xunit.Abstractions;

// The player is one per process: tests must not run at the same time
[assembly: CollectionBehavior(DisableTestParallelization = true)]

namespace FFAudioSharp.Tests;

internal static class Fixtures
{
    private static readonly string? Dir = Environment.GetEnvironmentVariable("FFAUDIO_FIXTURES");

    /// <summary>The path of a test fixture (made by CMake); skips the test when the fixtures are not there.</summary>
    public static string Path(string name)
    {
        Skip.If(string.IsNullOrEmpty(Dir) || !Directory.Exists(Dir), "FFAUDIO_FIXTURES is not set");
        return System.IO.Path.Combine(Dir!, name);
    }

    /// <summary>Like <see cref="Path"/>, and skips the test when this one file was not made (metaflac may be missing).</summary>
    public static string Optional(string name)
    {
        var p = Path(name);
        Skip.IfNot(File.Exists(p), $"no {name}");
        return p;
    }
}

/// <summary>A started player for the test (memory-only ReplayGain cache), shut down afterwards.</summary>
public abstract class PlayerTest : IDisposable
{
    protected PlayerTest()
    {
        GainCache.SetFile("");
        FFAudio.Init("FFAudioSharp tests");
    }

    public virtual void Dispose()
    {
        try { FFAudio.Shutdown(); }
        catch (FFAudioException) { /* a test that shut it down itself */ }
    }

    /// <summary>The next event of this type (and matching), polled for up to the timeout; fails with what was seen otherwise.</summary>
    protected static T WaitFor<T>(Func<T, bool>? match = null, double seconds = 10) where T : Event
    {
        var seen = new List<string>();
        var clock = Stopwatch.StartNew();
        while (clock.Elapsed.TotalSeconds < seconds)
        {
            var e = FFAudio.WaitEvent(TimeSpan.FromMilliseconds(100));
            if (e == null) continue;
            seen.Add(e.GetType().Name);
            if (e is T t && (match == null || match(t))) return t;
        }
        Assert.Fail($"no {typeof(T).Name} within {seconds} s; saw: {string.Join(", ", seen)}");
        return null!;
    }
}
