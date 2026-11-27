using System.Runtime.InteropServices;
using FFAudioSharp.Interop;

namespace FFAudioSharp;

/// <summary>What a call returned: <c>Ok</c>, or why it failed (AU_RESULT in ffaudio.h).</summary>
public enum ResultCode
{
    Ok = 0,
    NotInitialized = -1,
    InvalidArgument = -2,
    AlreadyInitialized = -3,
    NoMemory = -4,
    Sdl = -5,
    Device = -6,
    /// <summary>Called from a thread that would wait for itself (<c>Shutdown</c> from the event callback).</summary>
    WrongThread = -7,
    NothingToDo = -8,
    /// <summary>A file could not be read (missing, no permission, an I/O or network error).</summary>
    File = -9,
    Cancelled = -10,
    /// <summary>Not available in this build, or nothing in the file that can be played.</summary>
    Unsupported = -11,
    /// <summary>Done in memory, but the ReplayGain cache file could not be written.</summary>
    NotSaved = -12,
}

/// <summary>A call into ffaudio failed. <see cref="Code"/> says why.</summary>
public sealed class FFAudioException : Exception
{
    public ResultCode Code { get; }

    public FFAudioException(ResultCode code, string? detail = null)
        : base(Describe(code, detail))
    {
        Code = code;
    }

    private static unsafe string Describe(ResultCode code, string? detail)
    {
        var text = Marshal.PtrToStringUTF8((nint)Lib.au_result_string((int)code)) ?? code.ToString();
        return string.IsNullOrEmpty(detail) ? text : $"{detail}: {text}";
    }
}

public enum LogLevel { Info = 0, Warning = 1, Error = 2, Fatal = 3 }

/// <summary>Why a track left the player.</summary>
public enum EndReason
{
    /// <summary>Played to the end.</summary>
    Finished = 0,
    /// <summary>Replaced, skipped, cleared or stopped.</summary>
    Skipped = 1,
    /// <summary>Could not be opened, or decoding failed.</summary>
    Error = 2,
}

public enum FadeCurve
{
    /// <summary>Constant loudness for uncorrelated material. The default.</summary>
    EqualPower = 0,
    Linear = 1,
    SCurve = 2,
    Logarithmic = 3,
    Exponential = 4,
    /// <summary>Uses <see cref="Fade.CustomCurve"/>.</summary>
    Custom = 5,
}

public enum ReplayGainMode { Off = 0, Track = 1, Album = 2 }
public enum Downmix { FullLevel = 0, Normalized = 1, Custom = 2 }
public enum Upmix { None = 0, Surround = 1, Custom = 2 }

/// <summary>Picture types as ID3v2 APIC and FLAC PICTURE number them.</summary>
public enum PictureType
{
    /// <summary>For <c>GetPicture</c>: the front cover, or else the first picture.</summary>
    Any = -1,
    Other = 0,
    FrontCover = 3,
    BackCover = 4,
    Leaflet = 5,
    Media = 6,
    Artist = 8,
    BandLogo = 19,
}

public enum PeqType { Peak = 0, LowShelf = 1, HighShelf = 2, LowPass = 3, HighPass = 4 }
public enum Bs2bProfile { Default = 0, ChuMoy = 1, JanMeier = 2 }
public enum ScanPriority { Prefetch = 0, Urgent = 1 }
