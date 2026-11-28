# ffaudio for .NET

Bindings for [ffaudio](https://github.com/Odalith/FFaudio) 4: gapless and crossfaded music playback on FFmpeg and SDL2,
with ReplayGain, filters, cue sheets, network streams, level meters and a spectrum. `net8.0` and newer; AOT and trimming
safe (the native calls are source-generated).

```csharp
using FFAudioSharp;

FFAudio.Init("My Player");
FFAudio.SetReplayGain(new ReplayGainSettings { Mode = ReplayGainMode.Track, PreampDb = 4 });
FFAudio.Play("/music/song.flac", new PlayOptions { StartSeconds = 30 });

await foreach (var e in FFAudio.ReadEventsAsync(cancellationToken))
{
    switch (e)
    {
        case TrackStartedEvent started:
            Console.WriteLine("playing " + FFAudio.TrackInfo(started.Handle)?.Title);
            break;
        case PrepareNextEvent:
            FFAudio.Queue(NextSong());               // gapless, or crossfaded
            break;
        case TrackEndedEvent { Error: not ResultCode.Ok } failed:
            Console.WriteLine("could not play: " + failed.Message);
            break;
    }
}
```

## Getting the library

The bindings call `libffaudio`, which is not part of the .NET assembly. They look for it, in this order:

1. `FFAUDIO_LIBRARY`: the path of a `libffaudio.so.4` / `libffaudio.dll` / `libffaudio.4.dylib` of your own.
2. `runtimes/<rid>/native/` next to your application (where a NuGet package puts it), then the application's folder.
3. The system's library search path (an installed ffaudio).

A library of another major version is skipped, with a message that says why. The
[release archives](https://github.com/Odalith/FFaudio/releases) contain `libffaudio` with every library it needs (FFmpeg,
soxr; SDL2 as well on Windows). On Linux, SDL2 (2.24 or newer) comes from the system, e.g. `libsdl2-2.0-0`.

> **NuGet:** a package that bundles the libraries is planned with the macOS builds. The `Odalith.FFAudioSharp` 1.0.x
> packages on nuget.org are the old, separate bindings for ffaudio 2.x and do not work with this API; see
> [Coming from FFAudioSharp 1.x](#coming-from-ffaudiosharp-1x).

Until then, reference the project (`src/FFAudioSharp/FFAudioSharp.csproj`) or build it with `dotnet build -c Release`, and
point `FFAUDIO_LIBRARY` at the library of a release archive.

## What is in it

| | |
|---|---|
| `FFAudio` | The player (one per process): `Init`, `Play`, `Queue`, `Skip`, `Pause`, `Seek`, `SetVolume`, `SetCrossfade`, `SetFilters`, `SetReplayGain`, `GetStatus`, `Probe`, `GetPicture`, `GetLevels`, events. |
| `PlayOptions` | Per-track options, reusable: a sample-exact range, filters, a fade in, ReplayGain values, HTTP headers. |
| `Event` records | `TrackStartedEvent`, `TrackEndedEvent`, `PrepareNextEvent`, `DeviceEvent`, `BufferingEvent`, ... for `switch`. |
| `FilterBuilder` | Equalizer (graphic and parametric/AutoEq), tone, stereo, limiter, compressor, declick, declip, crossfeed, any FFmpeg filter. |
| `Loudness`, `LoudnessMeter` | EBU R128 / ReplayGain measurement of files and albums. |
| `GainCache`, `GainScan` | The shared per-user ReplayGain cache, and background measurement. |
| `PlaylistFile`, `CueSheet` | `.m3u`/`.m3u8`/`.pls`; `.cue` sheets, embedded cue sheets and chapters as tracks that join to the sample. |
| `Spectrum` | FFT bins and log-spaced bands for visualizers. |

[ffaudio.h](../../lib/ffaudio.h) and the other headers describe what each call does; the C# names are the C names in
PascalCase without `au_`.

## How it differs from the C API

- **Errors throw** `FFAudioException`; its `Code` is the `AU_RESULT` (`ResultCode`) and its message says what went wrong,
  including a filter builder's reason.
- **Events are polled or pushed, your choice.** `ReadEventsAsync()`, `WaitEvent()` and `PollEvent()` run on your threads.
  A handler passed to `Init()` runs on ffaudio's control thread: keep it short; exceptions in it are swallowed.
  Strings in events are copied, so they are yours to keep. Every call may be made from the handler, except `Shutdown()`.
- **Settings start at the library's defaults.** `new Compressor()`, `new Limiter()`, `new Equalizer()` hold what
  `au_*_defaults()` return; change what you need. `null` passed for settings means the defaults.
- **Results are copied** into objects (`MediaInfo`, `CueSheet`, `Picture`, ...): nothing needs releasing. Native objects
  that live longer (`FilterBuilder`, `LoudnessMeter`, `Spectrum`) are `IDisposable`.
- **Callbacks** (`Fade.CustomCurve`, `GainScanSettings.OnCached`, loudness progress, `GainCache.ForEach`) are managed
  delegates. Native code calls some from its own threads, so they must not throw (exceptions are swallowed) or block.
  A custom fade curve is kept alive for the rest of the process.
- **Seconds are `double`**, as in C: positions inside a range are exact to the sample, which `TimeSpan` ticks are not.
- Only **64-bit** platforms are supported.

## Tests

```sh
FFAUDIO_LIBRARY=build/libffaudio.so.4  FFAUDIO_LAYOUT=build/ffaudio_layout  FFAUDIO_FIXTURES=build/fixtures \
SDL_AUDIODRIVER=dummy  dotnet test bindings/csharp/tests/FFAudioSharp.Tests -f net8.0
```

CTest runs them as `csharp_bindings` when it finds a .NET SDK. The layout test compares every struct's size and every
field's offset with what the C compiler says (`bindings/python/tests/layout.c`, built as `ffaudio_layout`), and every
exported function in `abi/ffaudio.symbols` against the declarations, so a change to the C API that the bindings missed
fails the build. When a public struct changes, update `src/FFAudioSharp/Interop/Types.cs` (the field names are the C
names) and the Python binding's `_native.py`, then run `bindings/python/tests/make_layout.py`.

Layers: `Interop/` is the C API one to one (internal); everything else is the .NET API built on it.

## Examples

`examples/SimpleExample` plays a file and shows the events; `examples/PlaylistExample` plays files, playlists and cue
sheets one after the other with a level meter, queueing the next track when the player asks for it.

```sh
FFAUDIO_LIBRARY=/path/to/libffaudio.so.4 dotnet run --project examples/PlaylistExample -- --crossfade 3 album.cue
```

## Coming from FFAudioSharp 1.x

FFAudioSharp 1.x was a separate project for ffaudio 2.x. Its API is gone with ffaudio 3 and 4, which also changed the
native library, so there is no compatible upgrade: this is the rewrite for the current API, with the same namespace
(`FFAudioSharp`) and class name (`FFAudio`).

| 1.x | now |
|---|---|
| `FFAudio.Initialize(new InitializeConfig { ... callbacks })` | `FFAudio.Init("app", onEvent)`, or poll events; volume, loop count etc. are setters |
| `ConfigureAudioDevice(cfg)`, `GetAudioDevices()` | `SetDevice(name)`, `SetOutputFormat(channels, rate)`, `OpenDevice()`, `ListDevices()`; no call is required before playing |
| `PlayAudio(file, PlayAudioConfig)` | `Play(path, PlayOptions)` returns a handle; `Queue(path, options)` for gapless/crossfaded next tracks |
| `OnEof`, `OnRestart`, `OnPrepareNext`, `OnLog` callbacks | `TrackEndedEvent`, `RestartedEvent`, `PrepareNextEvent`, `LogEvent` |
| `PlayAudioConfig.SkipSeconds` / `PlayDuration` | `PlayOptions.StartSeconds` / `LengthSeconds`: sample-exact, and positions count from the range's start |
| `LoudnormSettings`, `CrossfeedSetting`, `AvFiltergraphOverride` | `FilterBuilder`, `PlayOptions.Filters`, `FFAudio.SetFilters`; ReplayGain via `SetReplayGain`, `Loudness`, `GainCache` |
| `FilterGraph`, `EqualizerConfig` (managed string builders) | `FilterBuilder`, `Equalizer` (the library builds the strings) |
| `SetVolume(int 0..100)`, `GetVolume()` | `SetVolume(float 0..2)` (1 = 100 %), `GetStatus().Volume` |
| `MuteAudio`, `PauseAudio`, `StopAudio` | `SetMuted`, `Pause`, `Stop` |
| `SeekPercent(p)`, `SeekTime(ms)` | `SeekFraction(0..1)`, `Seek(seconds)` |
| `GetPlayTime()`, `GetDuration()` | `GetStatus().Position` / `.Duration` |
| `GetLoopCount()` | `GetStatus().LoopCount` |

The [4.0 migration guide](../../docs/migrating-3-to-4.md) covers the C API changes behind these.
