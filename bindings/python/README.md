# ffaudio for Python

Bindings for ffaudio 4 with `ctypes`: nothing to compile, any Python 3.9+. They cover all five public headers.

```python
import ffaudio

ffaudio.init("My Player")
ffaudio.set_replaygain(ffaudio.ReplayGainSettings(mode=ffaudio.ReplayGainMode.TRACK, preamp_db=4))
ffaudio.play("/music/song.flac", ffaudio.PlayOptions(start_seconds=30))

while True:
    match ffaudio.wait_event(1.0):
        case ffaudio.TrackStarted(handle=h):
            print("playing", ffaudio.track_info(h).title)
        case ffaudio.PrepareNext():
            ffaudio.queue(next_song())                       # gapless, or crossfaded
        case ffaudio.TrackEnded(error=err, message=msg) if err:
            print("could not play:", msg)
```

## Finding the library

In this order:
1. `FFAUDIO_LIBRARY`, the path to `libffaudio.so.4` / `libffaudio.dll`.
2. Next to the package, as in a release archive: `python/ffaudio` with the library in `lib/` (or `bin/`).
3. The system's library path, for an installed ffaudio.

The bindings check that the library is version 4.

## Differences from the C API

The names are the C names without `au_`. `ffaudio.h` and the other headers say what each one does.

- **Errors** raise `ffaudio.Error`. Its `.code` is the `AU_RESULT`, and `str()` says what went wrong, including a
  filter builder's reason.
- **Value structs** are dataclasses: `Fade`, `Crossfade`, `ReplayGain`, `Equalizer`, `Limiter`, and so on. Their
  defaults are read from the library. **Results** are copied into Python objects (`MediaInfo`, `Status`, `CueSheet`,
  `Levels`), so nothing needs releasing.
- **Per-track options** are a `PlayOptions` dataclass, turned into an `AuPlayOptions` for each `play()`/`queue()`.
- **Events** are polled: `wait_event()` or `poll_event()`, on a thread of your choice. Each event is its own class
  (`TrackEnded`, `PrepareNext`, `Device`, ...), ready for `match`.
  No Python runs on ffaudio's threads, except callbacks you pass yourself (`GainScanSettings.on_cached`, a custom fade
  curve, a loudness progress function). Keep those short.
- **`FilterBuilder`** chains: `FilterBuilder().add_equalizer(eq, name="eq").add_limiter().string`.
- **Strings** are `str` in and out (UTF-8).

`ffaudio._native` is the C API one to one (ctypes structs and prototypes), for anything the Pythonic layer leaves out.

## Tests

```sh
FFAUDIO_LIBRARY=build/libffaudio.so.4 FFAUDIO_LAYOUT=build/ffaudio_layout FFAUDIO_FIXTURES=build/fixtures \
SDL_AUDIODRIVER=dummy python3 -m unittest discover -s bindings/python/tests
```
CTest runs them as `python_bindings`. The layout test compares every struct's size and every field's offset with what
the C compiler says (`tests/layout.c`). Run `tests/make_layout.py` after changing a struct in `_native.py`.

The player in `player/` is a complete program built on these bindings.
