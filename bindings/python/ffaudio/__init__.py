#
# Copyright (c) 2025 Odalith
#
# ffaudio is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either version 2.1 of the License, or (at your option) any
# later version.
#
"""
Python bindings for ffaudio 4, the music playback library (ctypes; no compiler needed).

    import ffaudio
    ffaudio.init("My Player")
    ffaudio.play("/music/song.flac")
    while True:
        match ffaudio.wait_event(1.0):
            case ffaudio.TrackEnded(error=err, message=msg) if err:
                print("could not play:", msg)
            case ffaudio.PrepareNext():
                ffaudio.queue(next_song())     # gapless, or crossfaded

The names follow the C API without the au_ prefix (ffaudio.h is the reference for what each one does). Differences:

- Errors raise ffaudio.Error (with .code, an AU_RESULT) instead of returning a negative number.
- Structs that hold values are dataclasses (Fade, Crossfade, ReplayGain, Equalizer, ...); results are copied into
  Python objects (MediaInfo, Status, CueSheet, ...), so there is nothing to release.
- Events are read by polling (wait_event() / poll_event(), on a thread of your choice); there is no on_event
  callback, so no Python code runs on ffaudio's threads, except the callbacks you pass yourself
  (GainScanSettings.on_cached, a custom fade curve, a loudness progress function).
- Strings are str in and out (UTF-8).
"""

import ctypes
import enum
import math
import threading
from dataclasses import dataclass, field
from typing import Callable, Iterable, Optional, Sequence

from . import _native as _n
from ._native import lib as _lib

__all__ = []   # filled below with every public name

NAN = float("nan")


def _enc(s: Optional[str]) -> Optional[bytes]:
    return None if s is None else s.encode("utf-8")


def _dec(b: Optional[bytes]) -> Optional[str]:
    return None if b is None else b.decode("utf-8", "replace")


#+-----------------------------------------------------+
#| Results and errors                                  |
#+-----------------------------------------------------+

class Result(enum.IntEnum):
    OK = 0
    ERR_NOT_INITIALIZED = -1
    ERR_INVALID_ARGUMENT = -2
    ERR_ALREADY_INITIALIZED = -3
    ERR_NO_MEMORY = -4
    ERR_SDL = -5
    ERR_DEVICE = -6
    ERR_WRONG_THREAD = -7
    ERR_NOTHING_TO_DO = -8
    ERR_FILE = -9
    ERR_CANCELLED = -10
    ERR_UNSUPPORTED = -11
    ERR_NOT_SAVED = -12


def result_string(result: int) -> str:
    return _dec(_lib.au_result_string(int(result)))


class Error(Exception):
    """A call failed: .code is the AU_RESULT, str() says what it means (and, for filters, what was wrong)."""

    def __init__(self, code: int, detail: str = ""):
        self.code = code
        try:
            self.result = Result(code)
        except ValueError:
            self.result = None
        text = result_string(code)
        super().__init__(f"{text}: {detail}" if detail else text)


def _check(ret: int, detail: str = "") -> int:
    if ret < 0:
        raise Error(ret, detail)
    return ret


def version() -> str:
    return _dec(_lib.au_get_version())


#+-----------------------------------------------------+
#| Enums                                               |
#+-----------------------------------------------------+

class LogLevel(enum.IntEnum):
    INFO = 0
    WARNING = 1
    ERROR = 2
    FATAL = 3


class EventType(enum.IntEnum):
    LOG = 0
    DEVICE = 1
    DEVICES_CHANGED = 2
    TRACK_OPENED = 3
    TRACK_STARTED = 4
    TRACK_ENDED = 5
    RESTARTED = 6
    DURATION = 7
    PREPARE_NEXT = 8
    BUFFERING = 9
    STREAM_METADATA = 10
    OVERFLOW = 11


class End(enum.IntEnum):
    FINISHED = 0
    SKIPPED = 1
    ERROR = 2


class FadeCurve(enum.IntEnum):
    EQUAL_POWER = 0
    LINEAR = 1
    S_CURVE = 2
    LOGARITHMIC = 3
    EXPONENTIAL = 4
    CUSTOM = 5


class ReplayGainMode(enum.IntEnum):
    OFF = 0
    TRACK = 1
    ALBUM = 2


class Downmix(enum.IntEnum):
    FULL_LEVEL = 0
    NORMALIZED = 1
    CUSTOM = 2


class Upmix(enum.IntEnum):
    NONE = 0
    SURROUND = 1
    CUSTOM = 2


class PictureType(enum.IntEnum):
    ANY = -1
    OTHER = 0
    FRONT_COVER = 3
    BACK_COVER = 4
    LEAFLET = 5
    MEDIA = 6
    ARTIST = 8
    BAND_LOGO = 19


class PeqType(enum.IntEnum):
    PEAK = 0
    LOW_SHELF = 1
    HIGH_SHELF = 2
    LOW_PASS = 3
    HIGH_PASS = 4


class Bs2bProfile(enum.IntEnum):
    DEFAULT = 0
    CHU_MOY = 1
    JAN_MEIER = 2


class ScanPriority(enum.IntEnum):
    PREFETCH = 0
    URGENT = 1


#+-----------------------------------------------------+
#| Events                                              |
#+-----------------------------------------------------+

@dataclass(frozen=True)
class Event:
    """Every event; the subclasses carry its data (match on them)."""
    handle: int


@dataclass(frozen=True)
class Log(Event):
    level: LogLevel
    message: str
    request: int


@dataclass(frozen=True)
class Device(Event):
    ok: bool
    fell_back: bool
    name: Optional[str]
    sample_rate: int
    channels: int


@dataclass(frozen=True)
class DevicesChanged(Event):
    pass


@dataclass(frozen=True)
class TrackOpened(Event):
    duration: float
    live: bool
    seekable: bool


@dataclass(frozen=True)
class TrackStarted(Event):
    pass


@dataclass(frozen=True)
class TrackEnded(Event):
    reason: End
    error: int                     # an AU_RESULT for End.ERROR, else 0
    message: Optional[str]         # what was logged for the error


@dataclass(frozen=True)
class Restarted(Event):
    position: float
    from_loop: bool
    loops_remaining: int


@dataclass(frozen=True)
class DurationChanged(Event):
    seconds: float


@dataclass(frozen=True)
class PrepareNext(Event):
    pass


@dataclass(frozen=True)
class Buffering(Event):
    active: bool
    buffered_seconds: float


@dataclass(frozen=True)
class StreamMetadata(Event):
    title: Optional[str]
    artist: Optional[str]


@dataclass(frozen=True)
class Overflow(Event):
    dropped: int


def _event(e: _n.AuEvent) -> Event:
    t, h, d = e.type, e.handle, e.data
    if t == EventType.LOG:
        return Log(h, LogLevel(d.log.level), _dec(d.log.message) or "", d.log.request)
    if t == EventType.DEVICE:
        v = d.device
        return Device(h, v.ok, v.fell_back, _dec(v.name), v.sample_rate, v.channels)
    if t == EventType.DEVICES_CHANGED:
        return DevicesChanged(h)
    if t == EventType.TRACK_OPENED:
        return TrackOpened(h, d.opened.duration, d.opened.live, d.opened.seekable)
    if t == EventType.TRACK_STARTED:
        return TrackStarted(h)
    if t == EventType.TRACK_ENDED:
        return TrackEnded(h, End(d.ended.reason), d.ended.error, _dec(d.ended.message))
    if t == EventType.RESTARTED:
        r = d.restarted
        return Restarted(h, r.position, r.from_loop, r.loops_remaining)
    if t == EventType.DURATION:
        return DurationChanged(h, d.duration.seconds)
    if t == EventType.PREPARE_NEXT:
        return PrepareNext(h)
    if t == EventType.BUFFERING:
        return Buffering(h, d.buffering.active, d.buffering.buffered_seconds)
    if t == EventType.STREAM_METADATA:
        return StreamMetadata(h, _dec(d.metadata.title), _dec(d.metadata.artist))
    return Overflow(h, d.overflow.dropped)


def poll_event() -> Optional[Event]:
    """The oldest queued event, or None."""
    e = _n.AuEvent()
    return _event(e) if _lib.au_poll_event(ctypes.byref(e)) else None


def wait_event(timeout_seconds: float = -1.0) -> Optional[Event]:
    """Waits up to timeout_seconds (< 0 = no limit); None on timeout and once shutdown() has been called."""
    e = _n.AuEvent()
    return _event(e) if _lib.au_wait_event(ctypes.byref(e), timeout_seconds) else None


#+-----------------------------------------------------+
#| Value types                                         |
#+-----------------------------------------------------+

_keep_alive = []   # ctypes callbacks C may still call (custom fade curves, the gain scan's on_cached)


@dataclass
class Fade:
    seconds: float = 0.0                       # <= 0: no fade (a cut, or gapless at a natural end)
    curve: FadeCurve = FadeCurve.EQUAL_POWER
    custom: Optional[Callable[[float], float]] = None   # for FadeCurve.CUSTOM: t (0..1) -> gain of the fade-in

    def _native(self) -> _n.AuFade:
        f = _n.AuFade(self.seconds, int(self.curve))
        if self.custom is not None:
            fn = self.custom
            cb = _n.AuFadeCurveFn(lambda t, _user: float(fn(t)))
            _keep_alive.append(cb)   # sampled on ffaudio's control thread, later
            f.custom = cb
        return f


@dataclass
class Crossfade:
    on_track_end: Fade = field(default_factory=Fade)
    on_skip: Fade = field(default_factory=Fade)
    on_play: Fade = field(default_factory=Fade)
    on_stop: Fade = field(default_factory=Fade)
    on_pause: Fade = field(default_factory=Fade)
    min_track_seconds: float = 0.0

    def _native(self) -> _n.AuCrossfade:
        return _n.AuCrossfade(self.on_track_end._native(), self.on_skip._native(), self.on_play._native(),
                              self.on_stop._native(), self.on_pause._native(), self.min_track_seconds)


@dataclass
class ReplayGain:
    """Gains in dB (re -18 LUFS) and linear peaks; NaN = missing."""
    track_gain_db: float = NAN
    track_peak: float = NAN
    album_gain_db: float = NAN
    album_peak: float = NAN
    from_r128: bool = False

    def _native(self) -> _n.AuReplayGain:
        return _n.AuReplayGain(self.track_gain_db, self.track_peak, self.album_gain_db, self.album_peak,
                               self.from_r128)

    @staticmethod
    def _of(r: _n.AuReplayGain) -> "ReplayGain":
        return ReplayGain(r.track_gain_db, r.track_peak, r.album_gain_db, r.album_peak, r.from_r128)

    @property
    def empty(self) -> bool:
        return math.isnan(self.track_gain_db) and math.isnan(self.album_gain_db)


@dataclass
class ReplayGainSettings:
    mode: ReplayGainMode = ReplayGainMode.OFF
    preamp_db: float = 0.0                 # -15..15; +4 plays at -14 LUFS (as most streaming services)
    fallback_gain_db: float = 0.0          # for files without values, -30..15
    prevent_clipping: bool = False
    measure_missing: bool = False          # measure untagged tracks in the background (writes the gain cache)
    measure_wait_seconds: float = 0.0      # 0 = 0.5 s, < 0 = do not wait

    def _native(self):
        return _n.AuReplayGainSettings(int(self.mode), self.preamp_db, self.fallback_gain_db, self.prevent_clipping,
                                       self.measure_missing, self.measure_wait_seconds)


@dataclass
class NetworkSettings:
    user_agent: Optional[str] = None
    timeout_seconds: float = 0.0
    buffer_seconds: float = 0.0
    max_buffer_seconds: float = 0.0
    reconnect_attempts: int = 0


@dataclass
class PlayOptions:
    """Options for one play()/queue(); None/0 = the default. Checked when the track is played."""
    start_seconds: float = 0.0
    length_seconds: float = 0.0
    filters: Optional[str] = None
    fade_in: Optional[Fade] = None
    replaygain: Optional[ReplayGain] = None
    http_headers: Optional[str] = None

    def _native(self):
        o = _lib.au_play_options_create()
        if not o:
            raise Error(Result.ERR_NO_MEMORY)
        try:
            _check(_lib.au_play_options_set_range(o, self.start_seconds, self.length_seconds), "range")
            if self.filters:
                _check(_lib.au_play_options_set_filters(o, _enc(self.filters)), f"filters {self.filters!r}")
            if self.fade_in is not None:
                f = self.fade_in._native()
                _check(_lib.au_play_options_set_fade_in(o, ctypes.byref(f)), "fade_in")
            if self.replaygain is not None:
                rg = self.replaygain._native()
                _check(_lib.au_play_options_set_replaygain(o, ctypes.byref(rg)))
            if self.http_headers:
                _check(_lib.au_play_options_set_http_headers(o, _enc(self.http_headers)))
        except Error:
            _lib.au_play_options_free(o)
            raise
        return o


@dataclass(frozen=True)
class Status:
    handle: int
    position: float
    duration: float
    buffering: bool
    paused: bool
    muted: bool
    volume: float
    speed: float
    keep_pitch: bool
    loop_count: int
    sample_rate: int
    channels: int


@dataclass(frozen=True)
class MediaInfo:
    path: str
    container: str
    codec: str
    channel_layout: str
    duration: float                   # -1 = unknown
    bit_rate: int
    sample_rate: int
    channels: int
    bits_per_sample: int
    live: bool
    seekable: bool
    track_number: int
    track_total: int
    disc_number: int
    disc_total: int
    replaygain: ReplayGain
    tags: tuple                       # ((key, value), ...) in file order
    picture_count: int

    def tag(self, key: str, default: Optional[str] = None) -> Optional[str]:
        """A tag by its normalized lower-case name ("title", "artist", "album", "album_artist", "date", ...)."""
        k = key.lower()
        for name, value in self.tags:
            if name.lower() == k:
                return value
        return default

    @property
    def title(self) -> Optional[str]:
        return self.tag("title")

    @property
    def artist(self) -> Optional[str]:
        return self.tag("artist")

    @property
    def album(self) -> Optional[str]:
        return self.tag("album")


def _media_info(p) -> MediaInfo:
    m = p.contents
    tags = tuple((_dec(m.tags[i].key), _dec(m.tags[i].value)) for i in range(m.tag_count))
    return MediaInfo(_dec(m.path), _dec(m.container), _dec(m.codec), _dec(m.channel_layout), m.duration, m.bit_rate,
                     m.sample_rate, m.channels, m.bits_per_sample, m.live, m.seekable, m.track_number, m.track_total,
                     m.disc_number, m.disc_total, ReplayGain._of(m.replaygain), tags, m.picture_count)


@dataclass(frozen=True)
class Picture:
    data: bytes
    mime: str
    type: int
    width: int
    height: int
    description: str


#+-----------------------------------------------------+
#| Lifetime, device, playback                          |
#+-----------------------------------------------------+

def init(app_name: Optional[str] = None) -> None:
    """Start the player. Events are queued for poll_event() / wait_event(). The device opens with the first
    play() or queue(), or at open_device()."""
    _check(_lib.au_init(_enc(app_name), _n.AuEventFn(), None))   # a NULL callback: events are queued


def open_device() -> None:
    _check(_lib.au_open_device())


def shutdown() -> None:
    _check(_lib.au_shutdown())


def set_device(name: Optional[str]) -> None:
    """A name from list_devices(); None = the system default (followed when it changes)."""
    _check(_lib.au_set_device(_enc(name)))


def set_output_format(channels: int = 0, sample_rate: int = 0) -> None:
    _check(_lib.au_set_output_format(channels, sample_rate))


def list_devices() -> list:
    n = ctypes.c_int(0)
    p = _lib.au_list_devices(ctypes.byref(n))
    try:
        return [_dec(p[i]) for i in range(n.value)] if p else []
    finally:
        _lib.au_free_devices(p, n.value)


def device_name() -> str:
    buf = ctypes.create_string_buffer(512)
    _check(_lib.au_get_device_name(buf, len(buf)))
    return _dec(buf.value)


def _track_call(fn, path: str, options: Optional[PlayOptions]) -> int:
    o = options._native() if options is not None else None
    try:
        return _check(fn(_enc(path), o), path)
    finally:
        if o:
            _lib.au_play_options_free(o)


def play(path: str, options: Optional[PlayOptions] = None) -> int:
    """Play a file or URL now; returns its handle."""
    return _track_call(_lib.au_play, path, options)


def queue(path: str, options: Optional[PlayOptions] = None) -> int:
    """Play it after the current track (gapless or crossfaded), replacing a queued one; returns its handle."""
    return _track_call(_lib.au_queue, path, options)


def skip() -> None:
    _check(_lib.au_skip())


def clear_queue() -> None:
    _check(_lib.au_clear_queue())


def stop() -> None:
    _check(_lib.au_stop())


def pause(paused: bool = True) -> None:
    _check(_lib.au_pause(paused))


def seek(seconds: float) -> None:
    _check(_lib.au_seek(seconds))


def seek_fraction(fraction: float) -> None:
    _check(_lib.au_seek_fraction(fraction))


def set_crossfade(crossfade: Optional[Crossfade]) -> None:
    c = crossfade._native() if crossfade is not None else None
    _check(_lib.au_set_crossfade(ctypes.byref(c) if c is not None else None))


def set_speed(speed: float, keep_pitch: bool = True) -> None:
    _check(_lib.au_set_speed(speed, keep_pitch))


def set_volume(gain: float) -> None:
    """0..2, 1 = 100 %."""
    _check(_lib.au_set_volume(gain))


def set_muted(muted: bool) -> None:
    _check(_lib.au_set_muted(muted))


def set_loop_count(count: int) -> None:
    """-1 = forever, 0 = none."""
    _check(_lib.au_set_loop_count(count))


def set_prepare_next(seconds: float) -> None:
    _check(_lib.au_set_prepare_next(seconds))


def set_network(settings: Optional[NetworkSettings]) -> None:
    if settings is None:
        _check(_lib.au_set_network(None))
        return
    s = _n.AuNetworkSettings(_enc(settings.user_agent), settings.timeout_seconds, settings.buffer_seconds,
                             settings.max_buffer_seconds, settings.reconnect_attempts)
    _check(_lib.au_set_network(ctypes.byref(s)))


def is_network_url(path: str) -> bool:
    return bool(_lib.au_is_network_url(_enc(path)))


def status() -> Status:
    s = _n.AuStatus()
    _lib.au_get_status(ctypes.byref(s))
    return Status(s.handle, s.position, s.duration, s.buffering, s.paused, s.muted, s.volume, s.speed, s.keep_pitch,
                  s.loop_count, s.sample_rate, s.channels)


#+-----------------------------------------------------+
#| Filters                                             |
#+-----------------------------------------------------+

def set_filters(filters: Optional[str]) -> None:
    _check(_lib.au_set_filters(_enc(filters or None)), filters or "")


def set_track_filters(handle: int, filters: Optional[str]) -> None:
    _check(_lib.au_set_track_filters(handle, _enc(filters or None)), filters or "")


def set_source_filters(filters: Optional[str]) -> None:
    _check(_lib.au_set_source_filters(_enc(filters or None)), filters or "")


def send_filter_command(target: str, command: str, arg: str, handle: int = 0) -> None:
    """A runtime command to a named filter, e.g. ("equalizer@eq_3", "g", "-6"); handle 0 = every live track."""
    _check(_lib.au_send_filter_command(handle, _enc(target), _enc(command), _enc(arg)))


def validate_filters(filters: str) -> bool:
    return _lib.au_validate_filters(_enc(filters)) == 0


def set_downmix(mode: Downmix, custom_filters: Optional[str] = None) -> None:
    _check(_lib.au_set_downmix(int(mode), _enc(custom_filters)))


def set_upmix(mode: Upmix, custom_filters: Optional[str] = None) -> None:
    _check(_lib.au_set_upmix(int(mode), _enc(custom_filters)))


def filter_available(name: str) -> bool:
    return bool(_lib.au_filter_available(_enc(name)))


def _from_native(cls, n):
    return cls(**{name: (list(getattr(n, name)) if isinstance(getattr(n, name), ctypes.Array) else getattr(n, name))
                  for name, _ in n._fields_})


def _to_native(ncls, v):
    n = ncls()
    for name, ctype in ncls._fields_:
        value = getattr(v, name)
        if isinstance(getattr(n, name), ctypes.Array):
            getattr(n, name)[:] = list(value)
        else:
            setattr(n, name, value)
    return n


_EQ = _lib.au_equalizer_defaults()
_STEREO, _LIMITER, _COMP = _lib.au_stereo_defaults(), _lib.au_limiter_defaults(), _lib.au_compressor_defaults()
_DECLICK, _DECLIP, _XFEED = _lib.au_declick_defaults(), _lib.au_declip_defaults(), _lib.au_crossfeed_defaults()
EQ_BANDS = _n.EQ_BANDS


@dataclass
class Equalizer:
    """10 bands at 31, 62, 125, 250, 500 Hz, 1, 2, 4, 8, 16 kHz; every default is the library's."""
    gain_db: list = field(default_factory=lambda: list(_EQ.gain_db))
    frequency_hz: list = field(default_factory=lambda: list(_EQ.frequency_hz))
    width_hz: list = field(default_factory=lambda: list(_EQ.width_hz))
    preamp_db: float = _EQ.preamp_db


def equalizer_defaults() -> Equalizer:
    return _from_native(Equalizer, _lib.au_equalizer_defaults())


@dataclass
class PeqBand:
    type: PeqType = PeqType.PEAK
    frequency_hz: float = 1000.0
    q: float = 0.7071
    gain_db: float = 0.0


@dataclass
class ParametricEq:
    preamp_db: float = 0.0
    bands: list = field(default_factory=list)   # up to 32 PeqBand

    def _native(self):
        n = _n.AuParametricEq()
        n.preamp_db = self.preamp_db
        n.band_count = len(self.bands)
        for i, b in enumerate(self.bands[:_n.PEQ_MAX_BANDS]):
            n.bands[i] = _n.AuPeqBand(int(b.type), b.frequency_hz, b.q, b.gain_db)
        return n


def parametric_eq_parse(text: str) -> ParametricEq:
    """EqualizerAPO / AutoEq "ParametricEQ.txt" text."""
    out = _n.AuParametricEq()
    err = ctypes.create_string_buffer(256)
    _check(_lib.au_parametric_eq_parse(_enc(text), ctypes.byref(out), err, len(err)), _dec(err.value))
    return ParametricEq(out.preamp_db, [PeqBand(PeqType(out.bands[i].type), out.bands[i].frequency_hz,
                                                out.bands[i].q, out.bands[i].gain_db) for i in range(out.band_count)])


@dataclass
class Stereo:
    balance: float = _STEREO.balance    # -1 (left) .. 1 (right)
    width: float = _STEREO.width        # 0 = mono .. 4
    swap: bool = _STEREO.swap


@dataclass
class Limiter:
    ceiling_db: float = _LIMITER.ceiling_db
    attack_ms: float = _LIMITER.attack_ms
    release_ms: float = _LIMITER.release_ms


@dataclass
class Compressor:
    threshold_db: float = _COMP.threshold_db
    ratio: float = _COMP.ratio
    attack_ms: float = _COMP.attack_ms
    release_ms: float = _COMP.release_ms
    knee_db: float = _COMP.knee_db
    makeup_db: float = _COMP.makeup_db


def compressor_night() -> Compressor:
    return _from_native(Compressor, _lib.au_compressor_night())


@dataclass
class Declick:
    window_ms: float = _DECLICK.window_ms
    overlap_percent: float = _DECLICK.overlap_percent
    threshold: float = _DECLICK.threshold
    burst: float = _DECLICK.burst


@dataclass
class Declip:
    window_ms: float = _DECLIP.window_ms
    overlap_percent: float = _DECLIP.overlap_percent
    threshold: float = _DECLIP.threshold


@dataclass
class Crossfeed:
    strength: float = _XFEED.strength
    range: float = _XFEED.range
    slope: float = _XFEED.slope
    level_in: float = _XFEED.level_in
    level_out: float = _XFEED.level_out


class FilterBuilder:
    """Builds an FFmpeg filter string (ffaudio_filters.h). Every add_* returns the builder, so calls chain; an
    invalid value raises Error saying what was wrong, and leaves the builder as it was. None settings = defaults.

        filters = ffaudio.FilterBuilder().add_equalizer(eq, name="eq").add_limiter().string
    """

    def __init__(self):
        self._fb = _lib.au_fb_create()
        if not self._fb:
            raise Error(Result.ERR_NO_MEMORY)

    def __del__(self):
        if getattr(self, "_fb", None):
            _lib.au_fb_free(self._fb)
            self._fb = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.__del__()

    @property
    def string(self) -> str:
        """The filter string so far ("" when empty)."""
        return _dec(_lib.au_fb_peek(self._fb))

    def __str__(self):
        return self.string

    def clear(self) -> "FilterBuilder":
        _lib.au_fb_clear(self._fb)
        return self

    def _done(self, ret: int) -> "FilterBuilder":
        _check(ret, _dec(_lib.au_fb_last_error(self._fb)))
        return self

    def add(self, filter_string: str) -> "FilterBuilder":
        return self._done(_lib.au_fb_add(self._fb, _enc(filter_string)))

    def add_volume_db(self, gain_db: float, name: Optional[str] = None) -> "FilterBuilder":
        return self._done(_lib.au_fb_add_volume_db(self._fb, _enc(name), gain_db))

    def add_equalizer(self, eq: Optional[Equalizer] = None, name: Optional[str] = None) -> "FilterBuilder":
        n = _to_native(_n.AuEqualizer, eq) if eq is not None else None
        return self._done(_lib.au_fb_add_equalizer(self._fb, _enc(name), ctypes.byref(n) if n else None))

    def add_parametric_eq(self, eq: Optional[ParametricEq] = None, name: Optional[str] = None) -> "FilterBuilder":
        n = eq._native() if eq is not None else None
        return self._done(_lib.au_fb_add_parametric_eq(self._fb, _enc(name), ctypes.byref(n) if n else None))

    def add_tone(self, bass_db: float, treble_db: float, name: Optional[str] = None) -> "FilterBuilder":
        return self._done(_lib.au_fb_add_tone(self._fb, _enc(name), bass_db, treble_db))

    def _add(self, fn, ncls, value, name):
        n = _to_native(ncls, value) if value is not None else None
        return self._done(fn(self._fb, _enc(name), ctypes.byref(n) if n is not None else None))

    def add_stereo(self, stereo: Optional[Stereo] = None, name: Optional[str] = None) -> "FilterBuilder":
        return self._add(_lib.au_fb_add_stereo, _n.AuStereo, stereo, name)

    def add_limiter(self, limiter: Optional[Limiter] = None, name: Optional[str] = None) -> "FilterBuilder":
        return self._add(_lib.au_fb_add_limiter, _n.AuLimiter, limiter, name)

    def add_compressor(self, compressor: Optional[Compressor] = None, name: Optional[str] = None) -> "FilterBuilder":
        return self._add(_lib.au_fb_add_compressor, _n.AuCompressor, compressor, name)

    def add_declick(self, declick: Optional[Declick] = None, name: Optional[str] = None) -> "FilterBuilder":
        return self._add(_lib.au_fb_add_declick, _n.AuDeclick, declick, name)

    def add_declip(self, declip: Optional[Declip] = None, name: Optional[str] = None) -> "FilterBuilder":
        return self._add(_lib.au_fb_add_declip, _n.AuDeclip, declip, name)

    def add_crossfeed(self, crossfeed: Optional[Crossfeed] = None, name: Optional[str] = None) -> "FilterBuilder":
        return self._add(_lib.au_fb_add_crossfeed, _n.AuCrossfeed, crossfeed, name)

    def add_bs2b(self, profile: Bs2bProfile = Bs2bProfile.DEFAULT, name: Optional[str] = None) -> "FilterBuilder":
        return self._done(_lib.au_fb_add_bs2b_profile(self._fb, _enc(name), int(profile)))


#+-----------------------------------------------------+
#| ReplayGain, metadata                                |
#+-----------------------------------------------------+

def set_replaygain(settings: Optional[ReplayGainSettings]) -> None:
    n = settings._native() if settings is not None else None
    _check(_lib.au_set_replaygain(ctypes.byref(n) if n is not None else None))


def track_gain_db(handle: int = 0) -> float:
    v = ctypes.c_double()
    _check(_lib.au_track_gain_db(handle, ctypes.byref(v)))
    return v.value


def probe(path: str) -> MediaInfo:
    err = ctypes.c_int(0)
    p = _lib.au_probe(_enc(path), ctypes.byref(err))
    if not p:
        raise Error(err.value or Result.ERR_FILE, path)
    try:
        return _media_info(p)
    finally:
        _lib.au_media_info_release(p)


def track_info(handle: int = 0) -> Optional[MediaInfo]:
    """The details of a playing or queued track once AU_EVENT_TRACK_OPENED has come; None before."""
    p = _lib.au_track_info(handle)
    if not p:
        return None
    try:
        return _media_info(p)
    finally:
        _lib.au_media_info_release(p)


def media_picture(path: str, type: PictureType = PictureType.ANY) -> Optional[Picture]:
    """An embedded picture (the front cover first for ANY), or None when there is none."""
    err = ctypes.c_int(0)
    p = _lib.au_media_picture(_enc(path), int(type), ctypes.byref(err))
    if not p:
        if err.value and err.value != Result.ERR_NOTHING_TO_DO:
            raise Error(err.value, path)
        return None
    try:
        v = p.contents
        return Picture(ctypes.string_at(v.data, v.size), _dec(v.mime) or "", v.type, v.width, v.height,
                       _dec(v.description) or "")
    finally:
        _lib.au_picture_release(p)


#+-----------------------------------------------------+
#| ReplayGain cache and loudness                       |
#+-----------------------------------------------------+

def gain_cache_set_file(path: Optional[str]) -> None:
    """None = the default location, "" = memory only."""
    _check(_lib.au_gain_cache_set_file(_enc(path)))


def gain_cache_file() -> str:
    buf = ctypes.create_string_buffer(4096)
    _check(_lib.au_gain_cache_get_file(buf, len(buf)))
    return _dec(buf.value)


def gain_cache_get(path: str) -> Optional[ReplayGain]:
    r = _n.AuReplayGain()
    return ReplayGain._of(r) if _lib.au_gain_cache_get(_enc(path), ctypes.byref(r)) else None


def gain_cache_set(path: str, rg: ReplayGain) -> bool:
    """True when it is stored in the file too, False when only in memory (the file could not be written)."""
    n = rg._native()
    ret = _lib.au_gain_cache_set(_enc(path), ctypes.byref(n))
    if ret == Result.ERR_NOT_SAVED:
        return False
    _check(ret, path)
    return True


def gain_cache_remove(path: str) -> bool:
    """True when there was an entry."""
    ret = _lib.au_gain_cache_remove(_enc(path))
    if ret == Result.ERR_NOTHING_TO_DO:
        return False
    if ret != Result.ERR_NOT_SAVED:
        _check(ret, path)
    return True


def gain_cache_clear() -> None:
    ret = _lib.au_gain_cache_clear()
    if ret != Result.ERR_NOT_SAVED:
        _check(ret)


def gain_cache_count() -> int:
    return _lib.au_gain_cache_count()


@dataclass(frozen=True)
class Loudness:
    silent: bool
    integrated_lufs: float
    loudness_range_lu: float
    true_peak: float
    sample_peak: float
    duration: float
    replaygain_gain_db: float
    r128_gain_db: float
    r128_gain_q78: int


def _loudness_options(skip_true_peak: bool, progress):
    o = _n.AuLoudnessOptions(skip_true_peak)
    cb = None
    if progress is not None:
        cb = _n.AuLoudnessProgressFn(lambda fraction, _user: bool(progress(fraction)))
        o.progress = cb
    return o, cb


def loudness_analyze(path: str, skip_true_peak: bool = False,
                     progress: Optional[Callable[[float], bool]] = None) -> Loudness:
    """Measure one file (blocks; progress(fraction) returning False cancels)."""
    o, _cb = _loudness_options(skip_true_peak, progress)
    out = _n.AuLoudness()
    _check(_lib.au_loudness_analyze(_enc(path), ctypes.byref(o), ctypes.byref(out)), path)
    return _from_native(Loudness, out)


def loudness_to_replaygain(track: Loudness, album: Optional[Loudness] = None) -> ReplayGain:
    t = _to_native(_n.AuLoudness, track)
    a = _to_native(_n.AuLoudness, album) if album is not None else None
    return ReplayGain._of(_lib.au_loudness_to_replaygain(ctypes.byref(t), ctypes.byref(a) if a else None))


def gain_cache_scan(path: str, skip_true_peak: bool = False) -> ReplayGain:
    """The cached values, or measure the file (blocks) and store them."""
    o, _cb = _loudness_options(skip_true_peak, None)
    out = _n.AuReplayGain()
    ret = _lib.au_gain_cache_scan(_enc(path), ctypes.byref(o), ctypes.byref(out))
    if ret != Result.ERR_NOT_SAVED:
        _check(ret, path)
    return ReplayGain._of(out)


@dataclass
class GainScanSettings:
    threads: int = 0
    skip_true_peak: bool = False
    # (path, result, ReplayGain or None) on a scan worker thread: keep it short, hand the work to your own thread
    on_cached: Optional[Callable[[str, int, Optional[ReplayGain]], None]] = None
    measure_streams: bool = False


def set_gain_scan(settings: Optional[GainScanSettings]) -> None:
    if settings is None:
        _check(_lib.au_set_gain_scan(None))
        return
    n = _n.AuGainScanSettings(settings.threads, settings.skip_true_peak)
    n.measure_streams = settings.measure_streams
    if settings.on_cached is not None:
        fn = settings.on_cached

        def cached(path, result, rg, _user):
            try:
                fn(_dec(path), result, ReplayGain._of(rg.contents) if rg else None)
            except Exception:   # never into C
                pass

        cb = _n.AuGainCachedFn(cached)
        _keep_alive.append(cb)
        n.on_cached = cb
    _check(_lib.au_set_gain_scan(ctypes.byref(n)))


def _paths(paths: Sequence[str]):
    arr = (ctypes.c_char_p * len(paths))(*[_enc(p) for p in paths])
    return arr, len(paths)


def gain_scan_request(paths: Sequence[str], priority: ScanPriority = ScanPriority.PREFETCH) -> int:
    """Measure files in the background (results go to the cache and on_cached); returns how many were queued."""
    arr, n = _paths(paths)
    return _check(_lib.au_gain_scan_request(arr, n, int(priority)))


def gain_scan_request_album(paths: Sequence[str], priority: ScanPriority = ScanPriority.PREFETCH) -> int:
    arr, n = _paths(paths)
    return _check(_lib.au_gain_scan_request_album(arr, n, int(priority)))


def gain_scan_cancel() -> None:
    _lib.au_gain_scan_cancel()


def gain_scan_pending() -> int:
    return _lib.au_gain_scan_pending()


def gain_scan_stop() -> None:
    _check(_lib.au_gain_scan_stop())


#+-----------------------------------------------------+
#| Meters                                              |
#+-----------------------------------------------------+

@dataclass(frozen=True)
class Levels:
    channels: int
    sample_rate: int
    window_seconds: float
    peak: tuple     # per channel, linear (1.0 = full scale)
    rms: tuple


def levels(window_seconds: float = 0.05) -> Levels:
    """Peak and RMS of the last window_seconds heard (0 < window <= 0.5)."""
    v = _n.AuLevels()
    _check(_lib.au_get_levels(window_seconds, ctypes.byref(v)))
    c = max(0, min(v.channels, 8))
    return Levels(v.channels, v.sample_rate, v.window_seconds, tuple(v.peak[:c]), tuple(v.rms[:c]))


class Spectrum:
    """A spectrum analyzer over what is heard (an FFT of fft_size samples, 256..16384, a power of two)."""

    def __init__(self, fft_size: int = 2048):
        self.fft_size = fft_size
        self._sp = _lib.au_spectrum_create(fft_size)
        if not self._sp:
            raise Error(Result.ERR_INVALID_ARGUMENT, f"fft_size {fft_size}")
        self._mags = (ctypes.c_float * (fft_size // 2 + 1))()
        self.sample_rate = 0

    def __del__(self):
        if getattr(self, "_sp", None):
            _lib.au_spectrum_free(self._sp)
            self._sp = None

    def read(self, channel: int = -1) -> list:
        """fft_size / 2 + 1 magnitudes (linear, a full-scale sine reads 1.0); channel -1 = all."""
        rate = ctypes.c_int(0)
        _check(_lib.au_spectrum_read(self._sp, channel, self._mags, len(self._mags), ctypes.byref(rate)))
        self.sample_rate = rate.value
        return list(self._mags)

    def bands(self, n_bands: int = 32, f_min: float = 30.0, f_max: float = 16000.0, channel: int = -1) -> list:
        """read(), grouped into n_bands log-spaced bands from f_min to f_max Hz (linear magnitudes)."""
        self.read(channel)
        out = (ctypes.c_float * n_bands)()
        if self.sample_rate <= 0:
            return [0.0] * n_bands
        _check(_lib.au_spectrum_bands(self._mags, self.fft_size, self.sample_rate, f_min, f_max, out, n_bands))
        return list(out)


#+-----------------------------------------------------+
#| Playlists and cue sheets                            |
#+-----------------------------------------------------+

@dataclass(frozen=True)
class PlaylistEntry:
    path: str            # ready for play() / queue()
    title: str           # "" when the playlist has none
    duration: float      # -1 when unknown


def _playlist(p) -> list:
    try:
        v = p.contents
        return [PlaylistEntry(_dec(v.entries[i].contents.path), _dec(v.entries[i].contents.title) or "",
                              v.entries[i].contents.duration) for i in range(v.count)]
    finally:
        _lib.au_playlist_file_release(p)


def playlist_read(path_or_url: str) -> list:
    """The entries of an M3U/M3U8/PLS file or URL."""
    err = ctypes.c_int(0)
    p = _lib.au_playlist_read(_enc(path_or_url), ctypes.byref(err))
    if not p:
        raise Error(err.value or Result.ERR_FILE, path_or_url)
    return _playlist(p)


def playlist_parse(text: str, base: Optional[str] = None) -> list:
    """Playlist text; base is where it came from (the playlist's own path or URL), for relative entries."""
    data = _enc(text)
    err = ctypes.c_int(0)
    p = _lib.au_playlist_parse(data, len(data), _enc(base), ctypes.byref(err))
    if not p:
        raise Error(err.value or Result.ERR_INVALID_ARGUMENT)
    return _playlist(p)


@dataclass(frozen=True)
class CueTrack:
    number: int
    path: str
    start_seconds: float
    length_seconds: float     # 0 = to the end of the file
    title: str
    performer: str
    songwriter: str
    isrc: str
    replaygain: ReplayGain

    def play_options(self) -> PlayOptions:
        """The options that play this track: its range, and the sheet's ReplayGain values if any."""
        return PlayOptions(start_seconds=self.start_seconds, length_seconds=self.length_seconds,
                           replaygain=None if self.replaygain.empty else self.replaygain)


@dataclass(frozen=True)
class CueSheet:
    title: str
    performer: str
    genre: str
    date: str
    tracks: tuple


def _cue(p) -> CueSheet:
    try:
        v = p.contents
        tracks = []
        for i in range(v.count):
            t = v.tracks[i].contents
            tracks.append(CueTrack(t.number, _dec(t.path), t.start_seconds, t.length_seconds, _dec(t.title) or "",
                                   _dec(t.performer) or "", _dec(t.songwriter) or "", _dec(t.isrc) or "",
                                   ReplayGain._of(t.replaygain)))
        return CueSheet(_dec(v.title) or "", _dec(v.performer) or "", _dec(v.genre) or "", _dec(v.date) or "",
                        tuple(tracks))
    finally:
        _lib.au_cue_release(p)


def cue_read(path_or_url: str) -> CueSheet:
    """The tracks of a .cue file, of an audio file with an embedded cue sheet, or of a file's chapters."""
    err = ctypes.c_int(0)
    p = _lib.au_cue_read(_enc(path_or_url), ctypes.byref(err))
    if not p:
        raise Error(err.value or Result.ERR_FILE, path_or_url)
    return _cue(p)


__all__ = [n for n in dir() if not n.startswith("_") and n not in ("ctypes", "enum", "math", "threading", "dataclass",
                                                                  "field", "Callable", "Iterable", "Optional",
                                                                  "Sequence")]
