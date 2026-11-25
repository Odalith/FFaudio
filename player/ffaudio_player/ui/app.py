"""The Textual app: owns the engine, routes what it reports to the screens, and the screens' commands."""
from __future__ import annotations

from typing import Callable, Iterable, Optional

from textual.app import App, SystemCommand
from textual.message import Message
from textual.screen import Screen

import ffaudio

from ..engine import Engine
from ..settings import Settings
from .main import MainScreen
from .screens import EffectsScreen, HelpScreen, LogScreen, OpenScreen, SaveScreen, SettingsScreen


class Call(Message):
    """A function for the UI thread, from another thread (post_message is thread-safe and never waits)."""

    def __init__(self, fn: Callable[[], None]):
        super().__init__()
        self.fn = fn


class PlayerApp(App):
    CSS_PATH = "player.tcss"
    TITLE = "ffaudio player"
    ENABLE_COMMAND_PALETTE = True

    def __init__(self, settings: Settings, paths: Optional[list] = None, play: bool = False):
        super().__init__()
        self.settings = settings
        self.initial_paths, self.initial_play = paths or [], play
        self.engine = Engine(settings, lambda fn: self.post_message(Call(fn)))
        self.main: Optional[MainScreen] = None
        self.spectrum: Optional[ffaudio.Spectrum] = None
        self._sync_timer = None
        self._last_current = None

    def on_mount(self) -> None:
        if self.settings.theme in self.available_themes:
            self.theme = self.settings.theme
        self.engine.on(self._engine_event)
        try:
            self.engine.start()
        except ffaudio.Error as e:
            self.exit(message=f"ffaudio could not start: {e}")
            return
        self.spectrum = ffaudio.Spectrum(4096)
        self.main = MainScreen()
        self.push_screen(self.main)
        self.set_interval(1 / 30, self._tick)
        self.set_interval(0.5, self._slow_tick)
        if self.initial_paths:
            self.main.add_paths(self.initial_paths, play=self.initial_play)

    def on_call(self, message: Call) -> None:
        message.fn()

    def _tick(self) -> None:
        if self.main and self.screen_stack and self.main in self.screen_stack:
            self.main.tick()

    def _slow_tick(self) -> None:
        if self.main:
            self.main.refresh_status()
            it = self.engine.current
            self.sub_title = it.display_title if it else ""

    #+-----------------------------------------------------+
    #| What the engine reports                             |
    #+-----------------------------------------------------+

    def _engine_event(self, kind: str, data) -> None:
        m = self.main
        if m is None or not m.is_mounted:
            if kind == "error":
                self.notify(str(data), severity="error")
            return
        if kind == "queue":
            m.rebuild_queue()
        elif kind == "item":
            m.refresh_item(data)
            if data is self.engine.current:
                m.refresh_now()
        elif kind == "current":
            for it in (self._last_current, data):
                if it is not None:
                    m.refresh_item(it)
            self._last_current = data
            m.refresh_now()
            m.refresh_status()
            if data is not None:
                m.load_art(data)
                self.title = f"{data.display_title} · ffaudio player"
        elif kind == "state":
            m.refresh_status()
            if self.engine.current is not None:
                m.refresh_item(self.engine.current)
        elif kind == "device":
            m.refresh_topbar()
        elif kind == "meta":
            m.refresh_now()
        elif kind == "log":
            for s in self.screen_stack:
                if isinstance(s, LogScreen):
                    s.add(self.engine.log[-1])
        elif kind == "error":
            self.notify(str(data), severity="error", title="ffaudio", timeout=6)
        elif kind == "scan":
            for s in self.screen_stack:
                if isinstance(s, SettingsScreen):
                    s.scan_progress(data)
            if data == 0:
                self.notify("Loudness measured", timeout=3)
                m.rebuild_queue()

    def schedule_effects_sync(self) -> None:
        """After live EQ commands: give ffaudio the new string once the slider stops (for the tracks to come)."""
        if self._sync_timer is not None:
            self._sync_timer.stop()
        self._sync_timer = self.set_timer(0.8, self.engine.apply_effects)

    #+-----------------------------------------------------+
    #| Commands                                            |
    #+-----------------------------------------------------+

    def action_effects(self) -> None:
        self.push_screen(EffectsScreen())

    def action_settings(self) -> None:
        self.push_screen(SettingsScreen())

    def action_open(self) -> None:
        self.push_screen(OpenScreen())

    def action_help(self) -> None:
        self.push_screen(HelpScreen())

    def action_log(self) -> None:
        self.push_screen(LogScreen())

    def action_save_playlist(self) -> None:
        self.push_screen(SaveScreen())

    def action_loudness(self) -> None:
        n = self.engine.measure_loudness()
        self.notify(f"Measuring the loudness of {n} file{'s' if n != 1 else ''} in the background" if n else
                    "Everything in the queue is measured already", timeout=4)

    def action_clear_queue(self) -> None:
        self.engine.clear()

    def action_sort(self, key: str) -> None:
        self.engine.sort(key)

    async def action_quit(self) -> None:
        self.settings.theme = self.theme
        self.engine.close()
        self.exit()

    def get_system_commands(self, screen: Screen) -> Iterable[SystemCommand]:
        yield from super().get_system_commands(screen)
        e = self.engine
        yield SystemCommand("Effects", "Equalizers, tone, stereo, crossfeed, dynamics, repairs", self.action_effects)
        yield SystemCommand("Settings", "Device, output format, loudness, crossfades, speed", self.action_settings)
        yield SystemCommand("Open file, folder or URL", "Add to the queue", self.action_open)
        yield SystemCommand("Save the queue as a playlist", "An M3U8 file", self.action_save_playlist)
        yield SystemCommand("Measure loudness", "ReplayGain values for the queue, in the background",
                            self.action_loudness)
        yield SystemCommand("Clear the queue", "Remove every track", self.action_clear_queue)
        for key in ("artist", "album", "title", "path"):
            yield SystemCommand(f"Sort the queue by {key}", "", lambda k=key: self.action_sort(k))
        yield SystemCommand("Repeat: next mode", f"now {e.s.repeat}", e.cycle_repeat)
        yield SystemCommand("Shuffle on/off", "", e.toggle_shuffle)
        yield SystemCommand("Stop", "Stop playback", e.stop)
        yield SystemCommand("Log", "Warnings and errors", self.action_log)
        yield SystemCommand("Keys", "Every key", self.action_help)
