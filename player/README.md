# ffaudio player

A music player for the terminal, built on ffaudio's Python bindings with [Textual](https://textual.textualize.io/).
It is a complete program, and also an example of ffaudio in use. Every feature below is a few calls into the library.

```
 ffaudio  player                                     ♪ System default · 48 kHz · 2 ch   ReplayGain track · -14 LUFS
╭ Files ──────────────╮╭ Queue · 12 tracks · 52:10 ─────────────────────────────────────────────────────────────╮
│ ▾ Music             ││    #  Title                     Artist            Album                 Gain    Time  │
│   ▸ Albums          ││ ►  1  Majesty                   Apashe            Renaissance           -7.9    3:35  │
│   ▸ Live            ││    2  Lacrimosa                 Apashe            Renaissance           -8.4    4:02  │
│     mix.m3u8        ││ ✗  3  missing.flac                                                                    │
╰─────────────────────╯╰──────────────────────────────────────────────────────────────────────── ↻ all ─────────╯
╭ Now playing ──────────────────────────────────────────────────────────────────────────────────────────────────╮
│ ▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀  Majesty                                        L ███████████████▌········  -6.1      │
│ ▀▀▀  cover art   ▀▀▀  Apashe — Renaissance (2020)                    R ██████████████▌·········  -6.4      │
│ ▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀  FLAC · 44.1 kHz/16-bit · stereo · 912 kbps     -48   -36   -24   -12   0              │
│ ▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀     1:12 ━━━━━━━━━━━━━●──────────────── 3:35      ▃▅▇█▆▅▄▃▃▂▂▂▁▁▁                     │
│ ▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀  ► Playing · vol 100% · crossfade 3 s · EQ bass · limiter                                │
╰───────────────────────────────────────────────────────────────────────────────────────────────────────────────╯
 space Play/Pause  n Next  p Prev  e Effects  o Settings  u Open  ? Help  q Quit                         ^p palette
```

## Running it

```sh
pip install -r player/requirements.txt    # Textual; Pillow for cover art (optional)
python3 player/run.py ~/Music              # or files, playlists, cue sheets, URLs; no arguments: the saved queue
```

It needs Python 3.9 or newer. In a release archive, `run.py` finds the bindings in `python/` and the library next to
them. In the source tree it finds `bindings/python`, and the library is given with `--library build/libffaudio.so.4`
or `FFAUDIO_LIBRARY`. `--fresh` starts with an empty queue, and `--settings FILE` uses another settings file.

## What it does

- **Queue:** files, folders, M3U/M3U8/PLS playlists, cue sheets, audio files with an embedded cue sheet or chapters
  (audiobooks), and HTTP(S) streams. A cue sheet's tracks join gaplessly, to the sample. You can reorder the queue,
  remove tracks, filter it (`/`), sort it, shuffle it, repeat all or one, and save it as a playlist. Tags,
  durations and gains are read in the background. A file that can't be played is marked ✗ with the reason (from
  `AU_EVENT_TRACK_ENDED`), and playback goes on past it.
- **Transitions:** gapless by default. Crossfades can be 0–12 s with a choice of curve, and skipping, stopping and
  pausing fade quickly. The next track is queued in ffaudio when it asks (`AU_EVENT_PREPARE_NEXT`), so every
  transition is ffaudio's own.
- **Loudness:** ReplayGain by track or album, with a target level: −18 LUFS (the ReplayGain reference), or −14 (the
  default, the level of Spotify and YouTube), −11, −16 or −23. There is a gain for untagged files, clipping prevention,
  and background measuring into ffaudio's ReplayGain cache (`ctrl+l` measures the queue, by folder as albums).
- **Effects, live:**
  - a 10-band graphic EQ with presets, where moving a band is a live filter command, so nothing is rebuilt;
  - AutoEq/EqualizerAPO headphone correction;
  - bass and treble, balance, width, mono, and swapping left and right;
  - crossfeed or bs2b;
  - a gentle or a night-mode compressor;
  - a limiter, on automatically while anything boosts;
  - declick and declip, before resampling.

  The effects screen shows the filter string they make.
- **Output:** the device (or the system default, which is followed when it changes), channels and sample rate,
  downmix (full level or normalized), and stereo-to-surround upmix. Volume goes to 200 %, and speed from 0.25× to
  4×, keeping the pitch or tape-style.
- **Meters:** peak and RMS per channel with peak hold, and a spectrum, from what is heard (`au_get_levels()`,
  `au_spectrum_read()`).
- **Now playing:** cover art (embedded, or `cover.jpg`/`folder.jpg`), tags, the format, the gain applied, and a seek
  bar you can click. Streams show their ICY title and buffering.
- The queue, the position in it, the settings and the effects are kept between sessions, in
  `~/.config/ffaudio-player/settings.json` (`%APPDATA%` on Windows).

Press `?` for every key. `ctrl+p` searches every command.

## How it is built

| File | What it is |
|---|---|
| `engine.py` | the player without its UI: the queue, playback, transitions, repeat and shuffle, settings, background probing and measuring. It runs on one thread; a pump thread waits for ffaudio's events and hands each one over |
| `library.py` | what a path stands for (folders, playlists, cue sheets, chapters), and saving the queue as M3U |
| `effects.py` | the effects as ffaudio's filter strings (source and global), with live EQ commands |
| `settings.py` | settings and the saved session, as JSON |
| `ui/` | the Textual app: the main screen, the effects, settings, help and log screens, and the widgets (slider, seek bar, meters, spectrum, cover art) |

The tests (`tests/test_player.py`, CTest's `player`) play real files through ffaudio without the UI, and drive the UI
with Textual's pilot.
