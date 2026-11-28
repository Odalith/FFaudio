using System.Collections.Concurrent;

namespace FFAudioSharp.Tests;

/// <summary>The player, with SDL's dummy audio driver (SDL_AUDIODRIVER=dummy).</summary>
public class PlaybackTests : PlayerTest
{
    [SkippableFact]
    public void A_track_plays_to_the_end_with_its_events()
    {
        var h = FFAudio.Play(Fixtures.Path("s44_stereo.flac"));
        Assert.True(h > 0);
        var device = WaitFor<DeviceEvent>();
        Assert.True(device.Ok);
        Assert.True(device.SampleRate > 0);
        var opened = WaitFor<TrackOpenedEvent>(e => e.Handle == h);
        Assert.True(opened.Duration > 0);
        var info = FFAudio.TrackInfo(h);
        Assert.NotNull(info);
        Assert.Equal("flac", info.Codec);
        WaitFor<TrackStartedEvent>(e => e.Handle == h);
        Assert.Equal(h, FFAudio.GetStatus().Handle);
        var ended = WaitFor<TrackEndedEvent>(e => e.Handle == h);
        Assert.Equal(EndReason.Finished, ended.Reason);
        Assert.Equal(ResultCode.Ok, ended.Error);
    }

    [SkippableFact]
    public void A_failed_track_says_why()
    {
        var h = FFAudio.Play(Fixtures.Path("does_not_exist.flac"));
        var ended = WaitFor<TrackEndedEvent>(e => e.Handle == h);
        Assert.Equal(EndReason.Error, ended.Reason);
        Assert.Equal(ResultCode.File, ended.Error);
        Assert.Contains("does_not_exist.flac", ended.Message);
    }

    [SkippableFact]
    public void Settings_options_and_meters()
    {
        FFAudio.SetVolume(1.5f);
        FFAudio.SetCrossfade(new Crossfade { OnSkip = new Fade { Seconds = 0.2 } });
        FFAudio.SetReplayGain(new ReplayGainSettings { Mode = ReplayGainMode.Track, PreampDb = 4 });
        Assert.Equal(ResultCode.InvalidArgument, Assert.Throws<FFAudioException>(() => FFAudio.SetVolume(2.5f)).Code);
        var bad = new PlayOptions { Filters = "no_such_filter" };
        Assert.Equal(ResultCode.InvalidArgument, Assert.Throws<FFAudioException>(() => FFAudio.Play(Fixtures.Path("long_6s.flac"), bad)).Code);

        var h = FFAudio.Play(Fixtures.Path("long_6s.flac"), new PlayOptions { StartSeconds = 1, LengthSeconds = 2, Filters = "volume=0.5" });
        var opened = WaitFor<TrackOpenedEvent>(e => e.Handle == h);
        Assert.Equal(2.0, opened.Duration, 3);   // the range's length, not the file's
        WaitFor<TrackStartedEvent>(e => e.Handle == h);

        FFAudio.GetLevels(0.05);   // the first call turns the meters on
        Thread.Sleep(400);
        var status = FFAudio.GetStatus();
        Assert.Equal(1.5f, status.Volume);
        Assert.Equal(h, status.Handle);
        var levels = FFAudio.GetLevels(0.05);
        Assert.Equal(status.Channels, levels.Channels);
        Assert.Equal(levels.Channels, levels.Peak.Length);

        using var spectrum = new Spectrum(1024);
        Assert.Equal(513, spectrum.Read().Length);
        Assert.Equal(spectrum.SampleRate, status.SampleRate);
        Assert.Equal(16, spectrum.ReadBands(16).Length);
        Assert.Throws<ArgumentException>(() => new Spectrum(1000));

        FFAudio.Stop();
        WaitFor<TrackEndedEvent>(e => e.Handle == h);
    }

    [SkippableFact]
    public void A_cue_track_plays_as_its_own_range()
    {
        var sheet = CueSheet.Read(Fixtures.Optional("cue_tag.flac"));
        var t = sheet.Tracks[1];
        var h = FFAudio.Play(t.Path, t.ToPlayOptions());
        var opened = WaitFor<TrackOpenedEvent>(e => e.Handle == h);
        if (t.LengthSeconds > 0) Assert.Equal(t.LengthSeconds, opened.Duration, 2);
        FFAudio.Seek(0.5);
        var restarted = WaitFor<RestartedEvent>(e => e.Handle == h);
        Assert.Equal(0.5, restarted.Position, 2);   // time counts from the track's start, not the file's
        Assert.False(restarted.FromLoop);
    }

    [SkippableFact]
    public void Queued_track_follows_on_prepare_next()
    {
        FFAudio.SetPrepareNext(60);   // longer than the track: the event comes at once
        var first = FFAudio.Play(Fixtures.Path("s44_stereo.flac"));
        WaitFor<PrepareNextEvent>(e => e.Handle == first);
        var second = FFAudio.Queue(Fixtures.Path("s48_stereo.ogg"));
        Assert.NotEqual(first, second);
        WaitFor<TrackEndedEvent>(e => e.Handle == first);
        WaitFor<TrackStartedEvent>(e => e.Handle == second);
    }

    [SkippableFact]
    public void A_custom_fade_curve_is_called_by_the_library()
    {
        var calls = 0;
        FFAudio.SetCrossfade(new Crossfade
        {
            OnSkip = new Fade { Seconds = 0.2, Curve = FadeCurve.Custom, CustomCurve = t => { Interlocked.Increment(ref calls); return t; } },
        });
        // The library samples the curve at a few hundred points when the fade is set up, on its control thread
        Assert.True(SpinWait.SpinUntil(() => Volatile.Read(ref calls) >= 200, TimeSpan.FromSeconds(5)), $"curve called {calls} times");
        FFAudio.SetCrossfade(null);
    }

    [Fact]
    public void Devices_can_be_listed_and_the_device_opened()
    {
        Assert.NotNull(FFAudio.ListDevices());
        Assert.Equal("", FFAudio.DeviceName);   // none open yet
        FFAudio.OpenDevice();
        var device = WaitFor<DeviceEvent>();
        Assert.True(device.Ok);
        Assert.NotEqual("", FFAudio.DeviceName);
    }

    [Fact]
    public void Calling_init_twice_is_refused()
    {
        Assert.Equal(ResultCode.AlreadyInitialized, Assert.Throws<FFAudioException>(() => FFAudio.Init("again")).Code);
    }
}

/// <summary>The other ways to get events: a handler on the control thread, and an async stream.</summary>
public class EventDeliveryTests
{
    [SkippableFact]
    public void A_handler_gets_the_events_on_ffaudios_thread()
    {
        GainCache.SetFile("");
        var events = new ConcurrentQueue<Event>();
        var threads = new ConcurrentBag<int>();
        FFAudio.Init("handler test", e => { threads.Add(Environment.CurrentManagedThreadId); events.Enqueue(e); });
        try
        {
            var h = FFAudio.Play(Fixtures.Path("s44_stereo.flac"));
            Assert.True(SpinWait.SpinUntil(() => events.OfType<TrackEndedEvent>().Any(e => e.Handle == h), TimeSpan.FromSeconds(15)),
                        $"saw: {string.Join(", ", events.Select(e => e.GetType().Name))}");
            Assert.DoesNotContain(Environment.CurrentManagedThreadId, threads);
            Assert.Null(FFAudio.PollEvent());   // nothing is queued for polling with a handler
        }
        finally
        {
            FFAudio.Shutdown();
        }
    }

    [SkippableFact]
    public async Task Events_can_be_read_as_an_async_stream()
    {
        GainCache.SetFile("");
        FFAudio.Init("async test");
        try
        {
            var h = FFAudio.Play(Fixtures.Path("s44_stereo.flac"));
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(15));
            TrackEndedEvent? ended = null;
            await foreach (var e in FFAudio.ReadEventsAsync(cts.Token))
            {
                if (e is TrackEndedEvent t && t.Handle == h) { ended = t; break; }
            }
            Assert.NotNull(ended);
            Assert.Equal(EndReason.Finished, ended.Reason);
        }
        finally
        {
            FFAudio.Shutdown();
        }
    }
}
