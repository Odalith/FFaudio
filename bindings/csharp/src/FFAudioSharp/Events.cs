using System.Runtime.InteropServices;
using FFAudioSharp.Interop;
using static FFAudioSharp.Interop.Util;

namespace FFAudioSharp;

/// <summary>
/// Everything the player reports. Poll for them with <see cref="FFAudio.PollEvent"/> / <see cref="FFAudio.WaitEvent"/>
/// on a thread of your choice, or pass a handler to <see cref="FFAudio.Init"/> (it runs on ffaudio's control thread).
/// Match on the type: <c>case TrackEndedEvent { Error: not ResultCode.Ok } e:</c>.
/// </summary>
public abstract record Event(int Handle)
{
    internal static unsafe Event From(AuEvent* e)
    {
        var h = e->handle;
        switch (e->type)
        {
            case 0:
                return new LogEvent((LogLevel)e->data.log.level, Str(e->data.log.message), e->data.log.request);
            case 1:
                return new DeviceEvent(B(e->data.device.ok), B(e->data.device.fell_back), StrOrNull(e->data.device.name),
                                       e->data.device.sample_rate, e->data.device.channels);
            case 2:
                return new DevicesChangedEvent();
            case 3:
                return new TrackOpenedEvent(h, e->data.opened.duration, B(e->data.opened.live), B(e->data.opened.seekable));
            case 4:
                return new TrackStartedEvent(h);
            case 5:
                return new TrackEndedEvent(h, (EndReason)e->data.ended.reason, (ResultCode)e->data.ended.error,
                                           StrOrNull(e->data.ended.message));
            case 6:
                return new RestartedEvent(h, e->data.restarted.position, B(e->data.restarted.from_loop),
                                          e->data.restarted.loops_remaining);
            case 7:
                return new DurationChangedEvent(h, e->data.duration.seconds);
            case 8:
                return new PrepareNextEvent(h);
            case 9:
                return new BufferingEvent(h, B(e->data.buffering.active), e->data.buffering.buffered_seconds);
            case 10:
                return new StreamMetadataEvent(h, Str(e->data.metadata.title), StrOrNull(e->data.metadata.artist));
            case 11:
                return new OverflowEvent(e->data.overflow.dropped);
            default:
                return new UnknownEvent(e->type, h);   // an event of a later 4.x release
        }
    }
}

/// <summary>A message from the library, or from FFmpeg ("FFmpeg &lt;part&gt;: ...").</summary>
public sealed record LogEvent(LogLevel Level, string Message, long Request) : Event(0);

/// <summary>The output device was opened (<paramref name="Ok"/>), or could not be.</summary>
/// <param name="Name">The device in use; when !Ok, the one that was asked for (null = the system default).</param>
/// <param name="FellBack">The chosen device could not be opened and the system default was opened instead.</param>
public sealed record DeviceEvent(bool Ok, bool FellBack, string? Name, int SampleRate, int Channels) : Event(0);

/// <summary>An output device was added or removed: <see cref="FFAudio.ListDevices"/> has the new list.</summary>
public sealed record DevicesChangedEvent() : Event(0);

/// <summary>The track is open; <see cref="FFAudio.TrackInfo"/> has its details now.</summary>
/// <param name="Duration">Seconds, -1 = unknown.</param>
/// <param name="Live">A stream with no end (no duration, no seeking).</param>
public sealed record TrackOpenedEvent(int Handle, double Duration, bool Live, bool Seekable) : Event(Handle);

/// <summary>The track became the audible one (start, gapless or crossfaded transition, skip).</summary>
public sealed record TrackStartedEvent(int Handle) : Event(Handle);

/// <summary>The track left the player; exactly once for every handle.</summary>
/// <param name="Error">Why, when <paramref name="Reason"/> is Error; Ok otherwise.</param>
/// <param name="Message">The text that was logged for the error.</param>
public sealed record TrackEndedEvent(int Handle, EndReason Reason, ResultCode Error, string? Message) : Event(Handle);

/// <summary>The current track restarted, after a seek or a loop. Position is in seconds, from the track's range start.</summary>
public sealed record RestartedEvent(int Handle, double Position, bool FromLoop, int LoopsRemaining) : Event(Handle);

/// <summary>The duration of the track became known or changed.</summary>
public sealed record DurationChangedEvent(int Handle, double Seconds) : Event(Handle);

/// <summary>The current track nears its end and nothing is queued: a good moment for <see cref="FFAudio.Queue"/>.</summary>
public sealed record PrepareNextEvent(int Handle) : Event(Handle);

/// <summary>A network track waits for data (<paramref name="Active"/>), or plays again.</summary>
public sealed record BufferingEvent(int Handle, bool Active, double BufferedSeconds) : Event(Handle);

/// <summary>The stream's "now playing" changed, at the moment it is heard.</summary>
public sealed record StreamMetadataEvent(int Handle, string Title, string? Artist) : Event(Handle);

/// <summary>The event queue was full: this many events were dropped (log events first).</summary>
public sealed record OverflowEvent(int Dropped) : Event(0);

/// <summary>An event type this version of the binding does not know (added by a later 4.x library).</summary>
public sealed record UnknownEvent(int Type, int Handle) : Event(Handle);
