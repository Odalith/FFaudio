using System.Collections.Concurrent;

namespace FFAudioSharp.Tests;

/// <summary>What works without a player: filters, parsing, probing, loudness, the ReplayGain cache.</summary>
public class StandaloneTests
{
    [Fact]
    public void Version_and_results()
    {
        Assert.StartsWith("4.", FFAudio.Version);
        Assert.Equal("not saved to the cache file", FFAudio.ResultString(ResultCode.NotSaved));
        var e = Assert.Throws<FFAudioException>(() => FFAudio.SetVolume(0.5f));   // no player
        Assert.Equal(ResultCode.NotInitialized, e.Code);
    }

    [Fact]
    public void FilterBuilder_builds_and_explains()
    {
        var eq = new Equalizer();
        eq.GainDb[0] = 3;
        using var fb = new FilterBuilder().AddEqualizer(eq, "eq").AddLimiter();
        Assert.Contains("equalizer@eq_0", fb.Filters);
        Assert.Contains("alimiter", fb.Filters);

        var e = Assert.Throws<FFAudioException>(() => fb.AddStereo(new Stereo { Width = 9 }));
        Assert.Equal(ResultCode.InvalidArgument, e.Code);
        Assert.Contains("width", e.Message);

        Assert.True(FFAudio.ValidateFilters(fb.Filters));
        Assert.False(FFAudio.ValidateFilters("no_such_filter"));
        using var flat = new FilterBuilder().AddEqualizer();   // null settings = the defaults: flat
        Assert.Equal("", flat.Filters);
        Assert.Equal("", fb.Clear().ToString());
    }

    [Fact]
    public void Settings_start_at_the_libraries_defaults()
    {
        Assert.Equal(10, new Equalizer().FrequencyHz.Length);
        Assert.True(new Equalizer().FrequencyHz[9] > new Equalizer().FrequencyHz[0]);
        Assert.Equal(-1, new Limiter().CeilingDb);
        Assert.InRange(Compressor.Night().Ratio, 1, 20);
    }

    [Fact]
    public void ParametricEq_reads_AutoEq_text()
    {
        var peq = ParametricEq.Parse("Preamp: -6.4 dB\nFilter 1: ON LSC Fc 105 Hz Gain 5.6 dB Q 0.70\n");
        Assert.Equal(-6.4, peq.PreampDb, 3);
        Assert.Equal(PeqType.LowShelf, Assert.Single(peq.Bands).Type);
        Assert.Throws<FFAudioException>(() => ParametricEq.Parse("Filter 1: ON XX Fc 1 Hz\n"));

        using var fb = new FilterBuilder().AddParametricEq(peq, "headphones");
        Assert.Contains("headphones", fb.Filters);
    }

    [Fact]
    public void Playlist_text_is_parsed_with_its_base()
    {
        var entries = PlaylistFile.Parse("#EXTM3U\n#EXTINF:215,Apashe - Majesty\nsongs/a.flac\n", "/music/lists/mix.m3u8");
        var e = Assert.Single(entries);
        Assert.Equal("/music/lists/songs/a.flac", e.Path);
        Assert.Equal("Apashe - Majesty", e.Title);
        Assert.Equal(215, e.Duration);
    }

    [Fact]
    public void Cue_text_is_parsed()
    {
        var sheet = CueSheet.Parse("PERFORMER \"Band\"\nTITLE \"Album\"\nFILE \"a.flac\" WAVE\n" +
                                   "  TRACK 01 AUDIO\n    TITLE \"One\"\n    INDEX 01 00:00:00\n" +
                                   "  TRACK 02 AUDIO\n    TITLE \"Two\"\n    INDEX 01 01:00:00\n", "/music/album.cue");
        Assert.Equal("Album", sheet.Title);
        Assert.Equal(2, sheet.Tracks.Count);
        Assert.Equal("/music/a.flac", sheet.Tracks[1].Path);
        Assert.Equal(60, sheet.Tracks[1].StartSeconds, 3);
        Assert.Equal(60, sheet.Tracks[0].LengthSeconds, 3);   // up to the next track
        Assert.Equal(0, sheet.Tracks[1].LengthSeconds);       // to the end of the file
        Assert.Equal(60, sheet.Tracks[1].ToPlayOptions().StartSeconds, 3);
    }

    [SkippableFact]
    public void Probe_reads_tags_and_says_why_it_failed()
    {
        var m = FFAudio.Probe(Fixtures.Path("rg_tagged.flac"));
        Assert.Equal("flac", m.Codec);
        Assert.Equal("Test Song", m.Title);
        Assert.Equal("Test Song", m.Tag("TITLE"));
        Assert.Equal(-6.0, m.ReplayGain.TrackGainDb, 3);
        Assert.True(m.ReplayGain.HasTrackGain);
        Assert.Equal(ResultCode.File, Assert.Throws<FFAudioException>(() => FFAudio.Probe(Fixtures.Path("does_not_exist.flac"))).Code);
    }

    [SkippableFact]
    public void Picture_is_copied_out_or_null()
    {
        var pic = FFAudio.GetPicture(Fixtures.Path("art.flac"));
        Assert.NotNull(pic);
        Assert.True(pic.Data.Length > 100);
        Assert.StartsWith("image/", pic.Mime);
        Assert.Null(FFAudio.GetPicture(Fixtures.Path("s44_stereo.flac")));
    }

    [SkippableFact]
    public void Cue_sheet_from_a_file()
    {
        var sheet = CueSheet.Read(Fixtures.Optional("cue_tag.flac"));
        Assert.True(sheet.Tracks.Count > 1);
        Assert.True(sheet.Tracks[1].StartSeconds > 0);
        Assert.Equal(sheet.Tracks[1].StartSeconds, sheet.Tracks[1].ToPlayOptions().StartSeconds);
        Assert.Equal(ResultCode.NothingToDo, Assert.Throws<FFAudioException>(() => CueSheet.Read(Fixtures.Path("s44_stereo.flac"))).Code);
    }

    [SkippableFact]
    public void Loudness_is_measured_and_albums_merge()
    {
        var l = Loudness.Analyze(Fixtures.Path("s44_stereo.flac"));
        Assert.False(l.Silent);
        Assert.True(l.IntegratedLufs < 0);
        var rg = Loudness.ToReplayGain(l);
        Assert.Equal((float)l.ReplayGainGainDb, rg.TrackGainDb, 4);
        Assert.False(rg.HasAlbumGain);

        using var a = LoudnessMeter.Measure(Fixtures.Path("s44_stereo.flac"));
        using var b = LoudnessMeter.Measure(Fixtures.Path("s48_stereo.ogg"));
        a.Merge(b);
        Assert.Equal(2, a.FileCount);
        Assert.True(Loudness.ToReplayGain(l, a.Result).HasAlbumGain);
    }

    [SkippableFact]
    public void Loudness_progress_can_cancel()
    {
        var calls = 0;
        var e = Assert.Throws<FFAudioException>(() => Loudness.Analyze(Fixtures.Path("long_6s.flac"), progress: _ => ++calls < 2));
        Assert.Equal(ResultCode.Cancelled, e.Code);
        Assert.Equal(2, calls);
    }

    [Fact]
    public void GainCache_keeps_values_in_memory()
    {
        GainCache.SetFile("");
        Assert.Equal("", GainCache.File);
        Assert.Equal(0, GainCache.Count);
        var rg = new ReplayGain(-7.5f, 0.9f, float.NaN, float.NaN);
        GainCache.Set("/m/a.flac", rg);
        GainCache.Set("/m/b.flac", rg with { TrackGainDb = -3 });

        var got = GainCache.Get("/m/a.flac");
        Assert.Equal(-7.5f, got!.Value.TrackGainDb);
        Assert.False(got.Value.HasAlbumGain);
        Assert.True(GainCache.Contains("/m/b.flac"));
        Assert.Null(GainCache.Get("/m/none.flac"));

        var seen = new List<string>();
        Assert.Equal(2, GainCache.ForEach((path, _) => { seen.Add(path); return true; }));
        Assert.Equal(["/m/a.flac", "/m/b.flac"], seen.Order());
        Assert.Equal(1, GainCache.ForEach((_, _) => false));   // stops at the first

        Assert.True(GainCache.Remove("/m/a.flac"));
        Assert.False(GainCache.Remove("/m/a.flac"));
        GainCache.Clear();
        Assert.Equal(0, GainCache.Count);
    }

    [SkippableFact]
    public void GainScan_measures_in_the_background()
    {
        GainCache.SetFile("");
        var path = Fixtures.Path("s44_stereo.flac");
        var done = new ConcurrentQueue<(string Path, ResultCode Result, ReplayGain? Rg)>();
        GainScan.Configure(new GainScanSettings { Threads = 1, OnCached = (p, r, rg) => done.Enqueue((p, r, rg)) });
        try
        {
            Assert.Equal(1, GainScan.Request([path]));
            SpinWait.SpinUntil(() => !done.IsEmpty, TimeSpan.FromSeconds(20));
            Assert.True(done.TryDequeue(out var result));
            Assert.Equal(path, result.Path);
            Assert.Equal(ResultCode.Ok, result.Result);
            Assert.True(result.Rg!.Value.HasTrackGain);
            Assert.True(GainCache.Contains(path));
            Assert.Equal(0, GainScan.Request([path]));   // cached: skipped
        }
        finally
        {
            GainScan.Stop();
            GainScan.Configure(null);
            GainCache.Clear();
        }
    }
}
