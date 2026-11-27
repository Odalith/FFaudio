using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using FFAudioSharp.Interop;
using static FFAudioSharp.Interop.Util;

namespace FFAudioSharp;

/// <summary>
/// The player: one per process, like the C library's. Call <see cref="Init"/> first, then play. Every call returns at
/// once (changes are queued and made in order on ffaudio's control thread); <see cref="Event"/>s report what follows.
/// All members may be called from any thread, and from the event handler (except <see cref="Shutdown"/>).
/// Errors throw <see cref="FFAudioException"/>.
/// </summary>
public static unsafe partial class FFAudio
{
    private static GCHandle _handler;
    private static volatile bool _initialized;

    /// <summary>The library's version, "major.minor.patch".</summary>
    public static string Version => Str(Lib.au_get_version());

    //+-----------------------------------------------------+
    //| Lifetime and devices                                |
    //+-----------------------------------------------------+

    /// <summary>
    /// Starts the player. <paramref name="appName"/> is shown by the system's sound settings. Without
    /// <paramref name="onEvent"/>, events are queued for <see cref="PollEvent"/> / <see cref="WaitEvent"/>; with it,
    /// they are passed to it on ffaudio's control thread (keep it short, and handle your own exceptions: they are
    /// swallowed). No output device is opened until the first track plays, or <see cref="OpenDevice"/>.
    /// </summary>
    public static void Init(string? appName = null, Action<Event>? onEvent = null)
    {
        GCHandle handle = default;
        void* fn = null, user = null;
        if (onEvent != null)
        {
            handle = GCHandle.Alloc(onEvent);
            user = (void*)GCHandle.ToIntPtr(handle);
            fn = (delegate* unmanaged[Cdecl]<AuEvent*, void*, void>)&OnEvent;
        }
        var r = Lib.au_init(appName, fn, user);
        if (r < 0)
        {
            if (handle.IsAllocated) handle.Free();
            throw new FFAudioException((ResultCode)r, "init");
        }
        _handler = handle;
        _initialized = true;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void OnEvent(AuEvent* e, void* user)
    {
        try
        {
            ((Action<Event>?)GCHandle.FromIntPtr((nint)user).Target)?.Invoke(Event.From(e));
        }
        catch
        {
            // an exception must not unwind into native code
        }
    }

    /// <summary>Opens the output device now, instead of when the first track plays. <see cref="DeviceEvent"/> reports the result.</summary>
    public static void OpenDevice() => Check(Lib.au_open_device(), "open device");

    /// <summary>
    /// Stops playback and frees everything; <see cref="Init"/> may be called again afterwards. From the event handler it
    /// throws <see cref="ResultCode.WrongThread"/>.
    /// </summary>
    public static void Shutdown()
    {
        Check(Lib.au_shutdown(), "shutdown");
        _initialized = false;
        if (_handler.IsAllocated) _handler.Free();
        _handler = default;
    }

    /// <summary>Chooses the output device by a name from <see cref="ListDevices"/>; null = the system default (followed when it changes).</summary>
    public static void SetDevice(string? name) => Check(Lib.au_set_device(name), "device");

    /// <summary>
    /// Asks for an output format: channels 1..8 (2 mixes everything down to stereo) and/or a rate of 8000..384000 Hz;
    /// 0 = what the device prefers. An open device is reopened and playback continues.
    /// </summary>
    public static void SetOutputFormat(int channels = 0, int sampleRate = 0) => Check(Lib.au_set_output_format(channels, sampleRate), "output format");

    /// <summary>The names of the output devices.</summary>
    public static IReadOnlyList<string> ListDevices()
    {
        int n;
        var devices = Lib.au_list_devices(&n);
        if (devices == null) return [];
        try
        {
            var list = new string[n];
            for (var i = 0; i < n; i++) list[i] = Str(devices[i]);
            return list;
        }
        finally
        {
            Lib.au_free_devices(devices, n);
        }
    }

    /// <summary>The device in use ("System default" when it can not be told); "" when none is open.</summary>
    public static string DeviceName
    {
        get
        {
            foreach (var size in new[] { 1024, 16384 })
            {
                var bytes = new byte[size];
                fixed (byte* b = bytes)
                {
                    var r = Lib.au_get_device_name(b, (nuint)size);
                    if (r >= 0) return Str(b);
                }
            }
            return "";
        }
    }

    //+-----------------------------------------------------+
    //| Playback                                            |
    //+-----------------------------------------------------+

    /// <summary>Plays a file or URL, replacing the current track (a queued track stays queued). Returns the track's handle.</summary>
    public static int Play(string path, PlayOptions? options = null) => Start(path, options, queue: false);

    /// <summary>
    /// Queues a file to play after the current one (gapless, or crossfaded per <see cref="Crossfade.OnTrackEnd"/>),
    /// replacing a queued one. Starts right away when nothing plays. Returns the handle.
    /// </summary>
    public static int Queue(string path, PlayOptions? options = null) => Start(path, options, queue: true);

    private static int Start(string path, PlayOptions? options, bool queue)
    {
        nint o = 0;
        try
        {
            o = options?.Build() ?? 0;
            return Check(queue ? Lib.au_queue(path, o) : Lib.au_play(path, o), path);
        }
        finally
        {
            if (o != 0) Lib.au_play_options_free(o);   // the call took a copy
        }
    }

    /// <summary>Ends the current track now and starts the queued one; without one, stops.</summary>
    public static void Skip() => Check(Lib.au_skip(), "skip");
    /// <summary>Removes the queued track.</summary>
    public static void ClearQueue() => Check(Lib.au_clear_queue(), "clear queue");
    /// <summary>Stops everything (fading out per <see cref="Crossfade.OnStop"/>) and clears the queue.</summary>
    public static void Stop() => Check(Lib.au_stop(), "stop");
    /// <summary>Pauses (fading out per <see cref="Crossfade.OnPause"/>) or resumes.</summary>
    public static void Pause(bool paused = true) => Check(Lib.au_pause(paused), "pause");
    /// <summary>Seeks the current track to this many seconds from its start (its range's start), exact to the sample.</summary>
    public static void Seek(double seconds) => Check(Lib.au_seek(seconds), "seek");
    /// <summary>Seeks the current track to this fraction (0..1) of its duration.</summary>
    public static void SeekFraction(double fraction) => Check(Lib.au_seek_fraction(fraction), "seek");

    /// <summary>When and how fades and crossfades happen. null disables them (gapless and hard cuts).</summary>
    public static void SetCrossfade(Crossfade? crossfade)
    {
        if (crossfade == null) { Check(Lib.au_set_crossfade(null), "crossfade"); return; }
        var n = crossfade.ToNative();
        Check(Lib.au_set_crossfade(&n), "crossfade");
    }

    /// <summary>Playback speed 0.25..4. <paramref name="keepPitch"/>: change the tempo only; false: tape-style.</summary>
    public static void SetSpeed(double speed, bool keepPitch = true) => Check(Lib.au_set_speed(speed, keepPitch), "speed");
    /// <summary>Linear gain 0..2 for everything played: 1 = 100 %, 2 = +6 dB (loud parts can clip above 1).</summary>
    public static void SetVolume(float gain) => Check(Lib.au_set_volume(gain), "volume");
    public static void SetMuted(bool muted) => Check(Lib.au_set_muted(muted), "muted");
    /// <summary>-1 = infinite, 0 = none. Applies to the current, the queued and future tracks.</summary>
    public static void SetLoopCount(int count) => Check(Lib.au_set_loop_count(count), "loop count");
    /// <summary>How long before the end of the current track <see cref="PrepareNextEvent"/> comes, in seconds (default 10).</summary>
    public static void SetPrepareNext(double seconds) => Check(Lib.au_set_prepare_next(seconds), "prepare next");

    /// <summary>A snapshot of the player; never waits.</summary>
    public static Status GetStatus()
    {
        AuStatus s;
        Lib.au_get_status(&s);
        return new Status(s.handle, s.position, s.duration, B(s.buffering), B(s.paused), B(s.muted), s.volume, s.speed,
                          B(s.keep_pitch), s.loop_count, s.sample_rate, s.channels);
    }

    //+-----------------------------------------------------+
    //| Network                                             |
    //+-----------------------------------------------------+

    /// <summary>Settings for every network stream (player, <see cref="Probe"/>, loudness); null restores the defaults.</summary>
    public static void SetNetwork(NetworkSettings? settings)
    {
        if (settings == null) { Check(Lib.au_set_network(null), "network"); return; }
        var agent = settings.UserAgent == null ? 0 : Marshal.StringToCoTaskMemUTF8(settings.UserAgent);
        try
        {
            var n = new AuNetworkSettings
            {
                user_agent = (byte*)agent, timeout_seconds = settings.TimeoutSeconds, buffer_seconds = settings.BufferSeconds,
                max_buffer_seconds = settings.MaxBufferSeconds, reconnect_attempts = settings.ReconnectAttempts,
            };
            Check(Lib.au_set_network(&n), "network");
        }
        finally
        {
            Marshal.FreeCoTaskMem(agent);
        }
    }

    /// <summary>true for http(s):// and other URLs with a scheme other than file://.</summary>
    public static bool IsNetworkUrl(string path) => Lib.au_is_network_url(path);

    //+-----------------------------------------------------+
    //| Filters, downmix and upmix                          |
    //+-----------------------------------------------------+

    /// <summary>
    /// FFmpeg filters for every track, after the track's own (null or "" removes them). A string that does not parse throws
    /// <see cref="ResultCode.InvalidArgument"/> and changes nothing. <see cref="FilterBuilder"/> builds common ones.
    /// </summary>
    public static void SetFilters(string? filters) => Check(Lib.au_set_filters(filters), "filters");
    /// <summary>Replaces the filters of one track (handle 0 = the current track).</summary>
    public static void SetTrackFilters(int handle, string? filters) => Check(Lib.au_set_track_filters(handle, filters), "track filters");
    /// <summary>Filters before the conversion to the output format, at the file's own rate and layout (e.g. declip).</summary>
    public static void SetSourceFilters(string? filters) => Check(Lib.au_set_source_filters(filters), "source filters");
    /// <summary>Sends a runtime command to a filter, e.g. ("equalizer@eq_3", "g", "-6"). handle 0 = every live track.</summary>
    public static void SendFilterCommand(string target, string command, string arg, int handle = 0) =>
        Check(Lib.au_send_filter_command(handle, target, command, arg), "filter command");
    /// <summary>Whether the string parses at the current output format.</summary>
    public static bool ValidateFilters(string? filters) => Lib.au_validate_filters(filters) == 0;
    /// <summary>Whether this FFmpeg build has the filter.</summary>
    public static bool FilterAvailable(string name) => Lib.au_filter_available(name);

    /// <summary>How files with more channels than the output are mixed down. <paramref name="customFilters"/> only for Custom.</summary>
    public static void SetDownmix(Downmix mode, string? customFilters = null) => Check(Lib.au_set_downmix((int)mode, customFilters), "downmix");
    /// <summary>How files with fewer channels than a surround output are spread out.</summary>
    public static void SetUpmix(Upmix mode, string? customFilters = null) => Check(Lib.au_set_upmix((int)mode, customFilters), "upmix");

    //+-----------------------------------------------------+
    //| ReplayGain                                          |
    //+-----------------------------------------------------+

    /// <summary>Applies ReplayGain / R128 tags automatically. null = off (the default).</summary>
    public static void SetReplayGain(ReplayGainSettings? settings)
    {
        if (settings == null) { Check(Lib.au_set_replaygain(null), "replaygain"); return; }
        var n = new AuReplayGainSettings
        {
            mode = (int)settings.Mode, preamp_db = settings.PreampDb, fallback_gain_db = settings.FallbackGainDb,
            prevent_clipping = B(settings.PreventClipping), measure_missing = B(settings.MeasureMissing),
            measure_wait_seconds = settings.MeasureWaitSeconds,
        };
        Check(Lib.au_set_replaygain(&n), "replaygain");
    }

    /// <summary>The gain in dB ffaudio applies to a track (handle 0 = the current track); null when unknown, ended or not open yet.</summary>
    public static double? TrackGainDb(int handle = 0)
    {
        double db;
        var r = Lib.au_track_gain_db(handle, &db);
        if (r == (int)ResultCode.NothingToDo) return null;
        Check(r, "track gain");
        return db;
    }

    //+-----------------------------------------------------+
    //| Metadata                                            |
    //+-----------------------------------------------------+

    /// <summary>Reads the tags and format of a file or URL without playing it. Works without <see cref="Init"/>, from any thread; blocks while the file is opened.</summary>
    public static MediaInfo Probe(string path)
    {
        int error;
        var p = Lib.au_probe(path, &error);
        if (p == null) throw new FFAudioException((ResultCode)error, path);
        try { return MediaInfo.From(p); }
        finally { Lib.au_media_info_release(p); }
    }

    /// <summary>The player's own information about a queued or playing track (handle 0 = the current track); null when unknown, ended or not open yet.</summary>
    public static MediaInfo? TrackInfo(int handle = 0)
    {
        var p = Lib.au_track_info(handle);
        if (p == null) return null;
        try { return MediaInfo.From(p); }
        finally { Lib.au_media_info_release(p); }
    }

    /// <summary>Reads an embedded picture; null when the file has none of that type. Works without <see cref="Init"/>.</summary>
    public static Picture? GetPicture(string path, PictureType type = PictureType.Any)
    {
        int error;
        var p = Lib.au_media_picture(path, (int)type, &error);
        if (p == null)
        {
            if (error == (int)ResultCode.NothingToDo) return null;
            throw new FFAudioException((ResultCode)error, path);
        }
        try { return Picture.From(p); }
        finally { Lib.au_picture_release(p); }
    }

    //+-----------------------------------------------------+
    //| Events                                              |
    //+-----------------------------------------------------+

    /// <summary>The oldest queued event, or null. Only without a handler passed to <see cref="Init"/>.</summary>
    public static Event? PollEvent()
    {
        AuEvent e;
        return Lib.au_poll_event(&e) ? Event.From(&e) : null;
    }

    /// <summary>
    /// Like <see cref="PollEvent"/>, waiting up to <paramref name="timeout"/> (null = no limit) for one. null on timeout, and
    /// once <see cref="Shutdown"/> has been called.
    /// </summary>
    public static Event? WaitEvent(TimeSpan? timeout = null)
    {
        AuEvent e;
        return Lib.au_wait_event(&e, timeout?.TotalSeconds ?? -1) ? Event.From(&e) : null;
    }

    //+-----------------------------------------------------+
    //| Level meters                                        |
    //+-----------------------------------------------------+

    /// <summary>
    /// Peak and RMS per channel of the last <paramref name="windowSeconds"/> (0 &lt; w &lt;= 0.5) heard: the music after
    /// ReplayGain and fades, before the volume. Zeros when nothing plays or while paused.
    /// </summary>
    public static Levels GetLevels(double windowSeconds = 0.05)
    {
        AuLevels l;
        Check(Lib.au_get_levels(windowSeconds, &l), "levels");
        var n = Math.Clamp(l.channels, 0, 8);
        var peak = new float[n];
        var rms = new float[n];
        for (var i = 0; i < n; i++) { peak[i] = l.peak[i]; rms[i] = l.rms[i]; }
        return new Levels(l.channels, l.sample_rate, l.window_seconds, peak, rms);
    }

    /// <summary>What a result code means, in English.</summary>
    public static string ResultString(ResultCode code) => Str(Lib.au_result_string((int)code));
}
