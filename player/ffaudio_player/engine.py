"""The player without its UI: the queue, playback through ffaudio, gapless and crossfaded transitions, repeat and
shuffle, settings and effects.

Threads: everything here runs on one thread (the UI's). A pump thread only waits for ffaudio's events and hands
each one to that thread with `post`; the probe and loudness workers do the same with what they find. So no state
here needs a lock.

How the queue meets ffaudio: ffaudio plays one track and holds at most one queued after it. When the current track
nears its end, ffaudio says PrepareNext and the engine queues the next item of its own queue (ffaudio.queue()),
which then starts gaplessly or crossfaded. Every track given to ffaudio has a handle; `handles` maps it back to the
queue item until the track has ended.
"""
from __future__ import annotations

import collections
import concurrent.futures
import os
import random
import threading
from typing import Callable, Optional

import ffaudio

from .effects import Effects
from .library import Item
from .settings import Settings

FADE_CURVES = {"equal_power": ffaudio.FadeCurve.EQUAL_POWER, "linear": ffaudio.FadeCurve.LINEAR,
               "s_curve": ffaudio.FadeCurve.S_CURVE, "logarithmic": ffaudio.FadeCurve.LOGARITHMIC,
               "exponential": ffaudio.FadeCurve.EXPONENTIAL}
RG_MODES = {"off": ffaudio.ReplayGainMode.OFF, "track": ffaudio.ReplayGainMode.TRACK,
            "album": ffaudio.ReplayGainMode.ALBUM}
RG_REFERENCE_LUFS = -18.0   # ReplayGain 2.0: the gains in the tags are relative to this


class Engine:
    def __init__(self, settings: Settings, post: Callable[[Callable[[], None]], None]):
        self.s = settings
        self.post = post                        # runs a function on the engine's thread
        self.effects = Effects(settings.effects)
        self.items: list[Item] = [Item.from_json(d) for d in settings.queue if isinstance(d, dict) and "path" in d]
        self.current: Optional[Item] = None     # the one being heard
        self.handles: dict[int, Item] = {}      # every track ffaudio has and has not ended yet
        self.next_handle = 0                    # the track queued in ffaudio after the current one
        self.history: list[Item] = []           # what was heard, for previous()
        self.played: set[int] = set()           # uids heard in this round of shuffle
        self.cursor = 0                         # where the current item is (or was) in `items`
        self.device: Optional[ffaudio.Device] = None
        self.buffering: Optional[ffaudio.Buffering] = None
        self.stream_title = ""                  # a stream's "now playing"
        self.log = collections.deque(maxlen=500)
        self._listeners: list[Callable[[str, object], None]] = []
        self._running = False
        self._pump: Optional[threading.Thread] = None
        self._probe = concurrent.futures.ThreadPoolExecutor(max_workers=2, thread_name_prefix="probe")
        self._probing: set[int] = set()
        self._resume: Optional[tuple[Item, float]] = None
        if 0 <= settings.current < len(self.items):
            self.cursor = settings.current
            self._resume = (self.items[settings.current], settings.position)

    #+-----------------------------------------------------+
    #| Listeners (the UI)                                  |
    #+-----------------------------------------------------+

    def on(self, fn: Callable[[str, object], None]) -> None:
        """fn(kind, data) on the engine's thread. Kinds: queue, item (an Item changed), current, state, device,
        devices, log, error (a message), meta, scan."""
        self._listeners.append(fn)

    def _emit(self, kind: str, data: object = None) -> None:
        for fn in self._listeners:
            fn(kind, data)

    def _fail(self, what: str, e: Exception) -> None:
        self._emit("error", f"{what}: {e}")

    #+-----------------------------------------------------+
    #| Lifetime                                            |
    #+-----------------------------------------------------+

    def start(self) -> None:
        ffaudio.init("ffaudio player")
        ffaudio.set_gain_scan(ffaudio.GainScanSettings(on_cached=self._on_cached_thread))
        self.apply_all()
        ffaudio.open_device()   # at once, so the device and its format show before anything plays
        self._running = True
        self._pump = threading.Thread(target=self._pump_events, name="ffaudio events", daemon=True)
        self._pump.start()
        self.probe(self.items)

    def close(self) -> None:
        self.save_session()
        self._running = False
        try:
            ffaudio.gain_scan_cancel()
            ffaudio.shutdown()         # wakes the pump: wait_event() returns None from now on
        except ffaudio.Error:
            pass
        if self._pump:
            self._pump.join(timeout=2)
        self._probe.shutdown(wait=False, cancel_futures=True)
        try:
            ffaudio.gain_scan_stop()
        except ffaudio.Error:
            pass

    def save_session(self) -> None:
        self.s.queue = [it.to_json() for it in self.items]
        cur = self.current if self.current in self.items else None
        self.s.current = self.items.index(cur) if cur else -1
        st = ffaudio.status() if self._running else None
        self.s.position = st.position if st and st.position > 0 and cur else 0.0
        try:
            self.s.save()
        except OSError as e:
            self.log.append(f"could not save the settings: {e}")

    def _pump_events(self) -> None:
        while self._running:
            ev = ffaudio.wait_event(0.25)
            if ev is not None:
                self.post(lambda ev=ev: self._handle(ev))

    #+-----------------------------------------------------+
    #| Settings                                            |
    #+-----------------------------------------------------+

    def apply_all(self) -> None:
        s = self.s
        ffaudio.set_volume(min(max(s.volume, 0.0), 2.0))
        ffaudio.set_muted(s.muted)
        ffaudio.set_speed(s.speed, s.keep_pitch)
        ffaudio.set_loop_count(-1 if s.repeat == "one" else 0)
        ffaudio.set_prepare_next(max(1.0, s.prepare_next))
        try:
            ffaudio.set_device(s.device)
        except ffaudio.Error:
            ffaudio.set_device(None)
        self.apply_output()
        self.apply_replaygain()
        self.apply_crossfade()
        self.apply_effects()

    def apply_output(self) -> None:
        s = self.s
        try:
            ffaudio.set_output_format(s.channels, s.sample_rate)
        except ffaudio.Error as e:
            self._fail("output format", e)
        ffaudio.set_downmix(ffaudio.Downmix.NORMALIZED if s.downmix == "normalized" else ffaudio.Downmix.FULL_LEVEL)
        ffaudio.set_upmix(ffaudio.Upmix.SURROUND if s.upmix else ffaudio.Upmix.NONE)

    def apply_replaygain(self) -> None:
        s = self.s
        preamp = min(max(s.loudness_target - RG_REFERENCE_LUFS, -15.0), 15.0)
        try:
            ffaudio.set_replaygain(ffaudio.ReplayGainSettings(
                mode=RG_MODES.get(s.replaygain, ffaudio.ReplayGainMode.TRACK), preamp_db=preamp,
                fallback_gain_db=min(max(s.fallback_gain_db + preamp, -30.0), 15.0),
                prevent_clipping=s.prevent_clipping, measure_missing=s.measure_missing))
        except ffaudio.Error as e:
            self._fail("ReplayGain", e)

    def apply_crossfade(self) -> None:
        s = self.s
        curve = FADE_CURVES.get(s.crossfade_curve, ffaudio.FadeCurve.EQUAL_POWER)
        quick = ffaudio.Fade(s.skip_fade, ffaudio.FadeCurve.EQUAL_POWER)
        ffaudio.set_crossfade(ffaudio.Crossfade(on_track_end=ffaudio.Fade(s.crossfade, curve), on_skip=quick,
                                                on_play=quick, on_stop=quick,
                                                on_pause=ffaudio.Fade(s.pause_fade, ffaudio.FadeCurve.EQUAL_POWER)))

    @property
    def stereo_output(self) -> bool:
        return self.device is None or self.device.channels == 2

    def apply_effects(self) -> None:
        try:
            self.effects.apply(self.stereo_output)
        except ffaudio.Error as e:
            self._fail("effects", e)
        self._emit("state")

    def set_volume(self, v: float) -> None:
        self.s.volume = round(min(max(v, 0.0), 2.0), 2)
        ffaudio.set_volume(self.s.volume)
        if self.s.muted:
            self.toggle_mute()
        self._emit("state")

    def toggle_mute(self) -> None:
        self.s.muted = not self.s.muted
        ffaudio.set_muted(self.s.muted)
        self._emit("state")

    def set_speed(self, speed: float, keep_pitch: Optional[bool] = None) -> None:
        self.s.speed = round(min(max(speed, 0.25), 4.0), 2)
        if keep_pitch is not None:
            self.s.keep_pitch = keep_pitch
        ffaudio.set_speed(self.s.speed, self.s.keep_pitch)
        self._emit("state")

    def cycle_repeat(self) -> None:
        self.s.repeat = {"off": "all", "all": "one", "one": "off"}[self.s.repeat]
        ffaudio.set_loop_count(-1 if self.s.repeat == "one" else 0)
        self._requeue()
        self._emit("state")

    def toggle_shuffle(self) -> None:
        self.s.shuffle = not self.s.shuffle
        self.played = {self.current.uid} if self.current else set()
        self._requeue()
        self._emit("state")

    #+-----------------------------------------------------+
    #| Playback                                            |
    #+-----------------------------------------------------+

    def play_item(self, item: Item, at: float = 0.0) -> None:
        """Play it now (crossfading from what plays, per the play fade); what follows it is queued later."""
        item.error = ""   # asked for: try it again
        try:
            ffaudio.clear_queue()            # au_play() keeps a queued track: it would follow this one
            h = ffaudio.play(item.path, item.play_options())
        except ffaudio.Error as e:
            item.error = str(e)
            self._emit("item", item)
            self._fail(item.display_title, e)
            return
        self.handles[h] = item
        self.next_handle = 0
        self._resume = (item, at) if at > 0 else None
        if item in self.items:
            self.cursor = self.items.index(item)

    def play_pause(self) -> None:
        st = ffaudio.status()
        if st.handle:
            ffaudio.pause(not st.paused)
        elif self._resume:
            self.play_item(*self._resume)
        elif self.items:
            self.play_item(self.items[min(self.cursor, len(self.items) - 1)])
        self._emit("state")

    def stop(self) -> None:
        ffaudio.stop()
        self._emit("state")

    def next(self) -> None:
        if self.next_handle:
            ffaudio.skip()                   # the queued track takes over, per the skip fade
            return
        nxt = self._next_item(self.current, user=True)
        if nxt:
            self.play_item(nxt)

    def previous(self) -> None:
        st = ffaudio.status()
        if st.handle and st.position > 3:
            ffaudio.seek(0)
            return
        prev = None
        if self.s.shuffle:
            past = [it for it in self.history[:-1] if it in self.items]
            prev = past[-1] if past else None
            if prev:
                self.history = self.history[:-2]   # it is pushed again when it starts
        elif self.current in self.items:
            i = self.items.index(self.current)
            prev = self.items[i - 1] if i > 0 else (self.items[-1] if self.s.repeat == "all" else None)
        elif self.items:
            prev = self.items[max(0, min(self.cursor, len(self.items)) - 1)]
        if prev:
            self.play_item(prev)
        elif st.handle:
            ffaudio.seek(0)

    def seek_by(self, seconds: float) -> None:
        st = ffaudio.status()
        if st.handle:
            target = max(0.0, st.position + seconds)
            if 0 < st.duration <= target:
                self.next()
            else:
                ffaudio.seek(target)

    def seek_fraction(self, fraction: float) -> None:
        if ffaudio.status().handle:
            ffaudio.seek_fraction(min(max(fraction, 0.0), 1.0))

    def _next_item(self, after: Optional[Item], user: bool = False) -> Optional[Item]:
        if not self.items:
            return None
        if self.s.shuffle:
            pool = [it for it in self.items if it.uid not in self.played and it is not after]
            if not pool and (self.s.repeat == "all" or user):
                self.played = set()
                pool = [it for it in self.items if it is not after] or list(self.items)
            return random.choice(pool) if pool else None
        if after is not None and after in self.items:
            i = self.items.index(after) + 1
        else:
            i = min(self.cursor, len(self.items))   # it was removed: what took its place
        if i >= len(self.items):
            return self.items[0] if self.s.repeat == "all" or (user and self.s.repeat != "off") else None
        return self.items[i]

    def _queue_next(self, after: Optional[Item]) -> None:
        for _ in range(len(self.items)):
            nxt = self._next_item(after)
            if nxt is None:
                return
            if nxt.error:   # failed already (ffaudio then asks again, after the same track): go past it
                after = nxt
                continue
            try:
                h = ffaudio.queue(nxt.path, nxt.play_options())
            except ffaudio.Error as e:
                nxt.error = str(e)
                self._emit("item", nxt)
                after = nxt
                continue
            self.handles[h] = nxt
            self.next_handle = h
            return

    def _requeue(self) -> None:
        """The queue or the order changed: what ffaudio holds as next may no longer be it."""
        if not self.next_handle:
            return
        want = self._next_item(self.current)
        if want is self.handles.get(self.next_handle):
            return
        if want is None:
            ffaudio.clear_queue()
            self.next_handle = 0
        else:
            self._queue_next(self.current)   # replaces the queued track

    #+-----------------------------------------------------+
    #| Events                                              |
    #+-----------------------------------------------------+

    def _handle(self, ev: ffaudio.Event) -> None:
        if isinstance(ev, ffaudio.Log):
            if ev.level >= ffaudio.LogLevel.WARNING:
                self.log.append(f"[{ev.level.name.lower()}] {ev.message}")
                self._emit("log", ev)
        elif isinstance(ev, ffaudio.Device):
            self.device = ev
            if not ev.ok:
                self._emit("error", f"no output device could be opened ({ev.name or 'system default'})")
            self.apply_effects()   # stereo-only effects follow the channel count
            self._emit("device", ev)
        elif isinstance(ev, ffaudio.DevicesChanged):
            self._emit("devices")
        elif isinstance(ev, ffaudio.TrackOpened):
            item = self.handles.get(ev.handle)
            if item:
                if ev.duration > 0:
                    item.duration = ev.duration if not item.is_range else item.duration
                if item.info is None:
                    item.info = ffaudio.track_info(ev.handle)
                self._emit("item", item)
        elif isinstance(ev, ffaudio.TrackStarted):
            item = self.handles.get(ev.handle)
            if ev.handle == self.next_handle:
                self.next_handle = 0
            if item:
                self.current, item.error, self.stream_title, self.buffering = item, "", "", None
                if item in self.items:
                    self.cursor = self.items.index(item)
                self.history.append(item)
                self.played.add(item.uid)
                if self._resume and self._resume[0] is item:
                    ffaudio.seek(self._resume[1])
                self._resume = None
                self._emit("current", item)
        elif isinstance(ev, ffaudio.TrackEnded):
            item = self.handles.pop(ev.handle, None)
            if ev.handle == self.next_handle:
                self.next_handle = 0
            if item and ev.reason == ffaudio.End.ERROR:
                item.error = ev.message or ffaudio.result_string(ev.error)
                self._emit("item", item)
                self._emit("error", f"{item.display_title}: {item.error}")
            if not self.handles:
                self.current = None if ev.reason != ffaudio.End.ERROR or item is self.current else self.current
                self._emit("current", None)
            self._emit("state")
        elif isinstance(ev, ffaudio.PrepareNext):
            after = self.handles.get(ev.handle) or self.current
            if ev.handle == 0 and not self.handles:
                after = self.current   # a failed track, nothing playing: go on after it
            self._queue_next(after)
        elif isinstance(ev, ffaudio.Buffering):
            self.buffering = ev if ev.active else None
            self._emit("state")
        elif isinstance(ev, ffaudio.StreamMetadata):
            self.stream_title = " - ".join(x for x in (ev.artist, ev.title) if x)
            self._emit("meta", self.stream_title)
        elif isinstance(ev, ffaudio.Restarted) or isinstance(ev, ffaudio.DurationChanged):
            self._emit("state")

    #+-----------------------------------------------------+
    #| The queue                                           |
    #+-----------------------------------------------------+

    def add(self, items: list[Item], at: Optional[int] = None, play: bool = False) -> None:
        if not items:
            return
        pos = len(self.items) if at is None else max(0, min(at, len(self.items)))
        self.items[pos:pos] = items
        if pos <= self.cursor and self.current is not None:
            self.cursor += len(items)
        self.probe(items)
        self._emit("queue")
        if play:
            self.play_item(items[0])
        else:
            self._requeue()
            if not ffaudio.status().handle and not self.next_handle and self.current is None and len(items) == len(
                    self.items):
                self.cursor = 0

    def add_next(self, items: list[Item]) -> None:
        """After the current item."""
        at = self.items.index(self.current) + 1 if self.current in self.items else self.cursor
        self.add(items, at=at)

    def remove(self, items: list[Item]) -> None:
        gone = set(id(it) for it in items)
        before = [it for it in self.items[:self.cursor] if id(it) in gone]
        self.items = [it for it in self.items if id(it) not in gone]
        self.cursor = max(0, self.cursor - len(before))
        self._emit("queue")
        self._requeue()

    def move(self, item: Item, delta: int) -> int:
        i = self.items.index(item)
        j = max(0, min(i + delta, len(self.items) - 1))
        if i != j:
            self.items.insert(j, self.items.pop(i))
            if self.current in self.items:
                self.cursor = self.items.index(self.current)
            self._emit("queue")
            self._requeue()
        return j

    def clear(self) -> None:
        self.items = []
        self.cursor = 0
        self._emit("queue")
        self._requeue()

    def sort(self, key: str) -> None:
        keys = {"title": lambda it: it.display_title.lower(), "artist": lambda it: (it.display_artist.lower(),
                it.display_album.lower(), it.info.disc_number if it.info else 0,
                it.info.track_number if it.info else it.number), "album": lambda it: (it.display_album.lower(),
                it.info.disc_number if it.info else 0, it.info.track_number if it.info else it.number),
                "path": lambda it: (it.path, it.start)}
        self.items.sort(key=keys[key])
        if self.current in self.items:
            self.cursor = self.items.index(self.current)
        self._emit("queue")
        self._requeue()

    def total_duration(self) -> float:
        return sum(max(0.0, it.display_duration) for it in self.items)

    #+-----------------------------------------------------+
    #| Background work                                     |
    #+-----------------------------------------------------+

    def probe(self, items: list[Item]) -> None:
        """Read tags and durations in the background, a few files at a time."""
        for it in items:
            if it.info is None and not it.is_url and it.uid not in self._probing:
                self._probing.add(it.uid)
                self._probe.submit(self._probe_one, it)

    def _probe_one(self, item: Item) -> None:
        try:
            info = ffaudio.probe(item.path)
        except ffaudio.Error as e:
            err = str(e)
            self.post(lambda: self._probed(item, None, err))
            return
        self.post(lambda: self._probed(item, info, ""))

    def _probed(self, item: Item, info: Optional[ffaudio.MediaInfo], error: str) -> None:
        self._probing.discard(item.uid)
        if info is not None:
            item.info = info
        elif error:
            item.error = error
        self._emit("item", item)

    def measure_loudness(self, items: Optional[list[Item]] = None) -> int:
        """Queue the local files for the ReplayGain cache: by folder, each measured as an album. Returns how many."""
        items = [it for it in (items or self.items) if not it.is_url]
        albums: dict[str, list[str]] = {}
        for it in items:
            paths = albums.setdefault(os.path.dirname(it.path), [])
            if it.path not in paths:
                paths.append(it.path)
        n = 0
        for paths in albums.values():
            try:
                n += ffaudio.gain_scan_request_album(paths) if len(paths) > 1 else ffaudio.gain_scan_request(paths)
            except ffaudio.Error as e:
                self._fail("loudness", e)
        self._emit("scan", ffaudio.gain_scan_pending())
        return n

    def _on_cached_thread(self, path: str, result: int, rg: Optional[ffaudio.ReplayGain]) -> None:
        # A scan worker thread: hand it over
        self.post(lambda: self._on_cached(path, result, rg))

    def _on_cached(self, path: str, result: int, rg) -> None:
        if result not in (0, ffaudio.Result.ERR_NOT_SAVED):
            self.log.append(f"loudness of {path}: {ffaudio.result_string(result)}")
        for it in self.items:
            if it.path == path:
                self._emit("item", it)
        self._emit("scan", ffaudio.gain_scan_pending())
