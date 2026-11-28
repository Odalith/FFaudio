// The C structs of the five public headers, field for field. Internal: the public API (FFAudioSharp namespace) is
// built on them. Names are the C names so the layout test (tests/LayoutTests.cs) can compare every size and offset
// with what the C compiler says (bindings/python/tests/layout.c). bool is a byte here, const char * a byte *.

using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

#pragma warning disable CS0649 // fields are written by the native side
#pragma warning disable IDE1006, SA1300

namespace FFAudioSharp.Interop;

//+-----------------------------------------------------+
//| ffaudio.h                                           |
//+-----------------------------------------------------+

internal unsafe struct EvLog { public int level; public byte* message; public long request; }
internal unsafe struct EvDevice { public byte ok, fell_back; public byte* name; public int sample_rate, channels; }
internal struct EvOpened { public double duration; public byte live, seekable; }
internal unsafe struct EvEnded { public int reason; public int error; public byte* message; }
internal struct EvRestarted { public double position; public byte from_loop; public int loops_remaining; }
internal struct EvDuration { public double seconds; }
internal struct EvBuffering { public byte active; public double buffered_seconds; }
internal unsafe struct EvMetadata { public byte* title; public byte* artist; }
internal struct EvOverflow { public int dropped; }

/// <summary>The union of AuEvent: 64 bytes, padded for the major version.</summary>
[StructLayout(LayoutKind.Explicit, Size = 64)]
internal unsafe struct EvData
{
    [FieldOffset(0)] public EvLog log;
    [FieldOffset(0)] public EvDevice device;
    [FieldOffset(0)] public EvOpened opened;
    [FieldOffset(0)] public EvEnded ended;
    [FieldOffset(0)] public EvRestarted restarted;
    [FieldOffset(0)] public EvDuration duration;
    [FieldOffset(0)] public EvBuffering buffering;
    [FieldOffset(0)] public EvMetadata metadata;
    [FieldOffset(0)] public EvOverflow overflow;
    [FieldOffset(0)] public fixed byte padding[64];
}

internal struct AuEvent { public int type; public int handle; public EvData data; }

internal unsafe struct AuFade
{
    public double seconds;
    public int curve;
    public void* custom;        // float (*)(float t, void *custom_user)
    public void* custom_user;
}

internal struct AuCrossfade
{
    public AuFade on_track_end, on_skip, on_play, on_stop, on_pause;
    public double min_track_seconds;
}

internal struct AuReplayGain
{
    public float track_gain_db, track_peak, album_gain_db, album_peak;
    public byte from_r128;
}

internal struct AuReplayGainSettings
{
    public int mode;
    public double preamp_db, fallback_gain_db;
    public byte prevent_clipping, measure_missing;
    public double measure_wait_seconds;
}

internal unsafe struct AuNetworkSettings
{
    public byte* user_agent;
    public double timeout_seconds, buffer_seconds, max_buffer_seconds;
    public int reconnect_attempts;
}

internal unsafe struct AuTag { public byte* key; public byte* value; }

internal unsafe struct AuMediaInfo
{
    public byte* path, container, codec, channel_layout;
    public double duration;
    public long bit_rate;
    public int sample_rate, channels, bits_per_sample;
    public byte live, seekable;
    public int track_number, track_total, disc_number, disc_total;
    public AuReplayGain replaygain;
    public int tag_count;
    public AuTag* tags;
    public int picture_count;
}

internal unsafe struct AuPicture
{
    public byte* data;
    public nuint size;
    public byte* mime;
    public int type;
    public int width, height;
    public byte* description;
}

internal struct AuStatus
{
    public int handle;
    public double position, duration;
    public byte buffering, paused, muted;
    public float volume;
    public double speed;
    public byte keep_pitch;
    public int loop_count;
    public int sample_rate, channels;
}

//+-----------------------------------------------------+
//| ffaudio_filters.h                                   |
//+-----------------------------------------------------+

internal unsafe struct AuEqualizer
{
    public fixed double gain_db[10];
    public fixed double frequency_hz[10];
    public fixed double width_hz[10];
    public double preamp_db;
}

internal struct AuPeqBand { public int type; public double frequency_hz, q, gain_db; }

[InlineArray(32)]
internal struct PeqBands { private AuPeqBand _element; }

internal struct AuParametricEq
{
    public double preamp_db;
    public int band_count;
    public PeqBands bands;
}

internal struct AuStereo { public double balance, width; public byte swap; }
internal struct AuLimiter { public double ceiling_db, attack_ms, release_ms; }
internal struct AuCompressor { public double threshold_db, ratio, attack_ms, release_ms, knee_db, makeup_db; }
internal struct AuDeclick { public double window_ms, overlap_percent, threshold, burst; }
internal struct AuDeclip { public double window_ms, overlap_percent, threshold; }
internal struct AuCrossfeed { public double strength, range, slope, level_in, level_out; }

//+-----------------------------------------------------+
//| ffaudio_loudness.h                                  |
//+-----------------------------------------------------+

internal struct AuLoudness
{
    public byte silent;
    public double integrated_lufs, loudness_range_lu, true_peak, sample_peak, duration, replaygain_gain_db, r128_gain_db;
    public int r128_gain_q78;
}

internal unsafe struct AuLoudnessOptions
{
    public byte skip_true_peak;
    public void* progress;       // bool (*)(double fraction, void *user)
    public void* progress_user;
}

internal unsafe struct AuGainScanSettings
{
    public int threads;
    public byte skip_true_peak;
    public void* on_cached;      // void (*)(const char *path, int result, const AuReplayGain *rg, void *user)
    public void* user;
    public byte measure_streams;
}

//+-----------------------------------------------------+
//| ffaudio_meters.h                                    |
//+-----------------------------------------------------+

internal unsafe struct AuLevels
{
    public int channels, sample_rate;
    public double window_seconds;
    public fixed float peak[8];
    public fixed float rms[8];
}

//+-----------------------------------------------------+
//| ffaudio_playlist.h                                  |
//+-----------------------------------------------------+

internal unsafe struct AuPlaylistEntry { public byte* path; public byte* title; public double duration; }
internal unsafe struct AuPlaylistFile { public int count; public AuPlaylistEntry** entries; }

internal unsafe struct AuCueTrack
{
    public int number;
    public byte* path;
    public double start_seconds, length_seconds;
    public byte* title, performer, songwriter, isrc;
    public AuReplayGain replaygain;
}

internal unsafe struct AuCueSheet
{
    public int count;
    public AuCueTrack** tracks;
    public byte* title, performer, genre, date;
}
