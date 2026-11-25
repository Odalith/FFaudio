"""The screens over the main one: effects, settings, open, save, help and the log."""
from __future__ import annotations

import os

from rich.text import Text
from textual import on
from textual.app import ComposeResult
from textual.binding import Binding
from textual.containers import Grid, Horizontal, Vertical, VerticalScroll
from textual.screen import ModalScreen
from textual.widgets import Button, Input, Label, RichLog, Select, Static, Switch

import ffaudio

from .. import effects as fx
from .widgets import Slider

CLOSE = [Binding("escape", "dismiss_screen", "Close"), Binding("q", "dismiss_screen", "Close", show=False)]


class Panel(ModalScreen):
    BINDINGS = CLOSE

    def action_dismiss_screen(self) -> None:
        self.dismiss()


def row(label: str, widget, hint: str = "") -> Horizontal:
    parts = [Label(label, classes="row-label"), widget]
    if hint:
        parts.append(Label(hint, classes="row-hint"))
    return Horizontal(*parts, classes="row")


def switch(id_: str, value: bool) -> Switch:
    return Switch(value=value, id=id_)


def select(id_: str, options: dict, value) -> Select:
    return Select([(label, key) for key, label in options.items()], value=value, allow_blank=False, id=id_,
                  compact=True)


#+-----------------------------------------------------+
#| Effects                                             |
#+-----------------------------------------------------+

class EffectsScreen(Panel):
    """Everything in the filters, live: what changes is heard within half a second (an EQ band at once)."""

    def compose(self) -> ComposeResult:
        e = self.app.engine.effects
        s = e.s
        with Vertical(id="effects", classes="panel"):
            yield Static("Effects", classes="panel-title")
            with Horizontal(id="eq-head"):
                yield Label("Graphic equalizer", classes="section")
                yield switch("eq_on", s.eq_on)
                yield select("eq_preset", {k: k for k in [*fx.PRESETS, "custom"]}, s.eq_preset)
            with Horizontal(id="eq-bands"):
                yield Slider(-fx.EQ_RANGE, fx.EQ_RANGE, s.eq_preamp, 0.5, vertical=True, label="pre", id="eq_preamp",
                             fmt=lambda v: f"{v:+.1f}")
                for i, name in enumerate(fx.BAND_LABELS):
                    yield Slider(-fx.EQ_RANGE, fx.EQ_RANGE, s.eq_gains[i], 0.5, vertical=True, label=name,
                                 id=f"band{i}", fmt=lambda v: f"{v:+.1f}")
            with Grid(id="fx-grid"):
                with Vertical(classes="box"):
                    yield Label("Headphone correction", classes="section")
                    yield row("On", switch("peq_on", s.peq_on))
                    yield Input(value=s.peq_file, placeholder="AutoEq / EqualizerAPO ParametricEQ.txt (enter)",
                                id="peq_file", compact=True)
                    yield Static(self._peq_status(), id="peq_status", classes="hint")
                with Vertical(classes="box"):
                    yield Label("Tone and stereo", classes="section")
                    yield Slider(-12, 12, s.bass, 0.5, label="Bass    ", id="bass", fmt=lambda v: f"{v:+.1f} dB")
                    yield Slider(-12, 12, s.treble, 0.5, label="Treble  ", id="treble", fmt=lambda v: f"{v:+.1f} dB")
                    yield Slider(-1, 1, s.balance, 0.05, label="Balance ", id="balance", fmt=_balance)
                    yield Slider(0, 2, s.width, 0.05, default=1, label="Width   ", id="width",
                                 fmt=lambda v: f"{v:.2f}")
                    yield row("Mono", switch("mono", s.mono))
                    yield row("Swap L/R", switch("swap", s.swap))
                with Vertical(classes="box"):
                    yield Label("Headphones and dynamics", classes="section")
                    hp = dict(fx.HEADPHONES)
                    if not ffaudio.filter_available("bs2b"):
                        hp = {k: v for k, v in hp.items() if not k.startswith("bs2b")}
                    yield row("Crossfeed", select("headphones", hp, s.headphones if s.headphones in hp else "off"))
                    yield row("Compressor", select("compressor", fx.COMPRESSORS, s.compressor))
                    yield row("Limiter", select("limiter", fx.LIMITERS, s.limiter))
                    yield Label("Restoration (before resampling)", classes="section")
                    yield row("Declick", switch("declick", s.declick), "crackle")
                    yield row("Declip", switch("declip", s.declip), "clipping")
            yield Static(id="filter-string", classes="hint")

    def on_mount(self) -> None:
        self._show_string()

    def _peq_status(self) -> Text:
        e = self.app.engine.effects
        if e.peq_error:
            return Text(e.peq_error, style="#ff8787")
        if e.peq:
            return Text(f"{len(e.peq.bands)} bands, preamp {e.peq.preamp_db:+.1f} dB", style="dim")
        return Text("none loaded", style="dim")

    def _show_string(self) -> None:
        e = self.app.engine.effects
        g, src = e.global_filters(self.app.engine.stereo_output), e.source_filters()
        t = Text("filters: ", style="dim bold")
        t.append(g or "(none)", style="dim")
        if src:
            t.append("\nsource:  ", style="dim bold")
            t.append(src, style="dim")
        self.query_one("#filter-string", Static).update(t)
        self.app.main.refresh_status()

    def _apply(self) -> None:
        self.app.engine.apply_effects()
        self._show_string()

    @on(Slider.Changed)
    def _slider(self, event: Slider.Changed) -> None:
        e, s = self.app.engine.effects, self.app.engine.effects.s
        sid = event.slider.id
        if sid.startswith("band"):
            if e.set_band(int(sid[4:]), event.value, self.app.engine.stereo_output):
                self.app.schedule_effects_sync()
            self.query_one("#eq_preset", Select).value = "custom"
            self._show_string()
            return
        setattr(s, {"eq_preamp": "eq_preamp"}.get(sid, sid), event.value)
        if event.final or sid != "eq_preamp":
            self._apply()

    @on(Switch.Changed)
    def _switch(self, event: Switch.Changed) -> None:
        setattr(self.app.engine.effects.s, event.switch.id, event.value)
        self._apply()

    @on(Select.Changed)
    def _select(self, event: Select.Changed) -> None:
        e = self.app.engine.effects
        sid = event.select.id
        if sid == "eq_preset":
            if event.value == "custom" or event.value == e.s.eq_preset:
                return
            e.set_preset(event.value)
            for i, g in enumerate(e.s.eq_gains):
                sl = self.query_one(f"#band{i}", Slider)
                sl.set_reactive(Slider.value, g)
                sl.refresh()
            if not e.s.eq_on:
                e.s.eq_on = True
                self.query_one("#eq_on", Switch).value = True
        else:
            setattr(e.s, sid, event.value)
        self._apply()

    @on(Input.Submitted, "#peq_file")
    def _peq_file(self, event: Input.Submitted) -> None:
        e = self.app.engine.effects
        e.s.peq_file = os.path.expanduser(event.value.strip())
        e.load_peq()
        if e.peq and not e.s.peq_on:
            e.s.peq_on = True
            self.query_one("#peq_on", Switch).value = True
        self.query_one("#peq_status", Static).update(self._peq_status())
        self._apply()


def _balance(v: float) -> str:
    return "centre" if abs(v) < 0.01 else (f"L {-v:.2f}" if v < 0 else f"R {v:.2f}")


#+-----------------------------------------------------+
#| Settings                                            |
#+-----------------------------------------------------+

TARGETS = {-18.0: "-18 LUFS · ReplayGain 2.0 reference", -16.0: "-16 LUFS · Apple Music, podcasts",
           -14.0: "-14 LUFS · Spotify, YouTube, Tidal", -11.0: "-11 LUFS · Spotify \"loud\"",
           -23.0: "-23 LUFS · EBU R128 broadcast"}
CURVES = {"equal_power": "Equal power", "linear": "Linear", "s_curve": "S-curve", "logarithmic": "Logarithmic",
          "exponential": "Exponential"}


class SettingsScreen(Panel):
    def compose(self) -> ComposeResult:
        s = self.app.engine.s
        devices = {"": "System default (follows it)"}
        for d in ffaudio.list_devices():
            devices[d] = d
        with VerticalScroll(id="settings", classes="panel"):
            yield Static("Settings", classes="panel-title")
            yield Label("Output", classes="section")
            yield row("Device", select("device", devices, s.device or ""))
            yield row("Channels", select("channels", {0: "as the device", 1: "mono", 2: "stereo", 6: "5.1",
                                                     8: "7.1"}, s.channels))
            yield row("Sample rate", select("sample_rate", {0: "as the device", 44100: "44.1 kHz",
                                                           48000: "48 kHz", 88200: "88.2 kHz", 96000: "96 kHz",
                                                           192000: "192 kHz"}, s.sample_rate))
            yield row("Downmix", select("downmix", {"full": "full level (like mpv)",
                                                   "normalized": "normalized (never clips)"}, s.downmix),
                      "surround on fewer speakers")
            yield row("Upmix", switch("upmix", s.upmix), "stereo files over every speaker")
            yield Label("Loudness", classes="section")
            yield row("ReplayGain", select("replaygain", {"off": "off", "track": "track", "album": "album"},
                                           s.replaygain))
            yield row("Target", select("loudness_target", TARGETS, s.loudness_target))
            yield Slider(-18, 0, s.fallback_gain_db, 0.5, default=-6, label="Untagged files ", id="fallback_gain_db",
                         fmt=lambda v: f"{v:+.1f} dB")
            yield row("Prevent clipping", switch("prevent_clipping", s.prevent_clipping),
                      "lower the gain so the peak stays below full scale")
            yield row("Measure untagged", switch("measure_missing", s.measure_missing),
                      "in the background, into the ReplayGain cache")
            yield Horizontal(Button("Measure the queue now", id="measure", compact=True), Static(id="scan-status", classes="hint"),
                             classes="row")
            yield Label("Transitions", classes="section")
            yield Slider(0, 12, s.crossfade, 0.5, label="Crossfade      ", id="crossfade",
                         fmt=lambda v: "gapless" if v == 0 else f"{v:g} s")
            yield row("Curve", select("crossfade_curve", CURVES, s.crossfade_curve))
            yield Slider(0, 3, s.skip_fade, 0.05, default=0.3, label="Skip/stop fade ", id="skip_fade",
                         fmt=lambda v: "cut" if v == 0 else f"{v:.2f} s")
            yield Slider(0, 2, s.pause_fade, 0.05, default=0.15, label="Pause fade     ", id="pause_fade",
                         fmt=lambda v: "cut" if v == 0 else f"{v:.2f} s")
            yield Label("Speed", classes="section")
            yield Slider(0.25, 4, s.speed, 0.05, default=1, label="Speed          ", id="speed",
                         fmt=lambda v: f"×{v:.2f}")
            yield row("Keep the pitch", switch("keep_pitch", s.keep_pitch), "off: tape-style")
            yield Label("About", classes="section")
            yield Static(self._about(), classes="hint")

    def _about(self) -> Text:
        try:
            cache = ffaudio.gain_cache_file() or "memory only"
        except ffaudio.Error:
            cache = "?"
        return Text(f"ffaudio {ffaudio.version()} · settings {self.app.engine.s.path}\nReplayGain cache {cache} "
                    f"({ffaudio.gain_cache_count()} entries)")

    @on(Select.Changed)
    def _select(self, event: Select.Changed) -> None:
        eng, s, sid = self.app.engine, self.app.engine.s, event.select.id
        setattr(s, sid, (event.value or None) if sid == "device" else event.value)
        if sid == "device":
            ffaudio.set_device(s.device)
        elif sid in ("channels", "sample_rate", "downmix"):
            eng.apply_output()
        elif sid in ("replaygain", "loudness_target"):
            eng.apply_replaygain()
            self.app.main.refresh_topbar()
        elif sid == "crossfade_curve":
            eng.apply_crossfade()
        self.app.main.refresh_status()

    @on(Switch.Changed)
    def _switch(self, event: Switch.Changed) -> None:
        eng, s, sid = self.app.engine, self.app.engine.s, event.switch.id
        setattr(s, sid, event.value)
        if sid == "upmix":
            eng.apply_output()
        elif sid in ("prevent_clipping", "measure_missing"):
            eng.apply_replaygain()
        elif sid == "keep_pitch":
            eng.set_speed(s.speed, s.keep_pitch)
        self.app.main.refresh_status()

    @on(Slider.Changed)
    def _slider(self, event: Slider.Changed) -> None:
        eng, s, sid = self.app.engine, self.app.engine.s, event.slider.id
        if sid == "speed":
            eng.set_speed(event.value)
        else:
            setattr(s, sid, event.value)
            if event.final:
                eng.apply_replaygain() if sid == "fallback_gain_db" else eng.apply_crossfade()
        self.app.main.refresh_status()

    @on(Button.Pressed, "#measure")
    def _measure(self) -> None:
        n = self.app.engine.measure_loudness()
        self.query_one("#scan-status", Static).update(f"{n} file{'s' if n != 1 else ''} queued for measuring")

    def scan_progress(self, pending: int) -> None:
        self.query_one("#scan-status", Static).update(f"{pending} left to measure" if pending else "done")


#+-----------------------------------------------------+
#| Open, save, help, log                               |
#+-----------------------------------------------------+

class OpenScreen(Panel):
    def compose(self) -> ComposeResult:
        with Vertical(id="open", classes="panel dialog"):
            yield Static("Open a file, folder, playlist or URL", classes="panel-title")
            yield Input(placeholder="/music/album  ·  ~/list.m3u8  ·  https://radio.example.com/stream.mp3",
                        id="open-path", compact=True)
            yield Horizontal(Button("Add to queue", id="add", variant="primary", compact=True), Button("Play now", id="play", compact=True),
                             Button("Play next", id="next", compact=True), classes="buttons")

    def on_mount(self) -> None:
        self.query_one(Input).focus()

    def _go(self, how: str) -> None:
        p = self.query_one(Input).value.strip()
        if not p:
            return
        if not ffaudio.is_network_url(p):
            p = os.path.abspath(os.path.expanduser(p))
        self.dismiss()
        self.app.main.add_paths([p], play=how == "play", next_=how == "next")

    @on(Input.Submitted)
    def _submit(self) -> None:
        self._go("add")

    @on(Button.Pressed)
    def _button(self, event: Button.Pressed) -> None:
        self._go(event.button.id)


class SaveScreen(Panel):
    def compose(self) -> ComposeResult:
        with Vertical(id="save", classes="panel dialog"):
            yield Static("Save the queue as a playlist", classes="panel-title")
            yield Input(value=os.path.join(os.path.expanduser("~"), "queue.m3u8"), id="save-path", compact=True)
            yield Horizontal(Button("Save", id="save-go", variant="primary", compact=True), classes="buttons")

    @on(Input.Submitted)
    @on(Button.Pressed)
    def _save(self) -> None:
        from .. import library
        p = os.path.expanduser(self.query_one(Input).value.strip())
        try:
            library.write_m3u(p, self.app.engine.items)
        except OSError as e:
            self.app.notify(str(e), severity="error")
            return
        self.app.notify(f"Saved {len(self.app.engine.items)} tracks to {p}")
        self.dismiss()


HELP_INTRO = ("A full example of ffaudio in use: gapless playback and crossfades, ReplayGain, cue sheets, streams, "
              "live filters, level meters and a spectrum, all through its Python bindings.")
HELP = [
    ("Playback", [
        ("space", "play / pause"),
        ("n  p", "next / previous (previous restarts a track past 3 s)"),
        ("← →", "seek 5 s;  shift+← →  or  , .  30 s;  0 – 9  jump to 0 – 90 %;  or click the bar"),
        ("+  -", "volume, up to 200 %;  m  mute"),
        ("[  ]", "slower / faster;  \\  keep the pitch, or tape-style"),
        ("r  s", "repeat off → all → one;  shuffle"),
    ]),
    ("Queue and files", [
        ("enter", "play the selected track (in the files: play that file)"),
        ("a  A  i", "in the files: add · play now · play next"),
        ("x  del", "remove from the queue;  ctrl+↑ ↓  or  K J  move it"),
        ("/", "filter the queue;  esc  clears the filter"),
        ("u", "open a file, folder, playlist, cue sheet or URL"),
        ("ctrl+s", "save the queue as a playlist;  ctrl+l  measure its loudness"),
        ("b  tab", "show / hide the files;  next pane"),
    ]),
    ("Screens", [
        ("e", "effects: equalizers, tone, stereo, crossfeed, compressor, limiter, repairs"),
        ("o", "settings: device, output format, loudness target, crossfades, speed"),
        ("l  ?", "the log;  these keys"),
        ("ctrl+p", "every command, searchable"),
        ("q", "quit (the queue, settings and effects are kept)"),
    ]),
]


def help_table():
    from rich.table import Table
    from rich.console import Group
    parts = [Text("ffaudio player\n", style="bold"), Text(HELP_INTRO + "\n", style="dim")]
    for title, keys in HELP:
        t = Table(title=title, title_justify="left", title_style="bold", show_header=False, box=None,
                  padding=(0, 2), expand=True)
        t.add_column(style="bold", width=10, no_wrap=True)
        t.add_column()
        for k, v in keys:
            t.add_row(k, v)
        parts += [t, Text("")]
    return Group(*parts)


class HelpScreen(Panel):
    def compose(self) -> ComposeResult:
        with VerticalScroll(id="help", classes="panel"):
            yield Static(help_table())


class LogScreen(Panel):
    def compose(self) -> ComposeResult:
        with Vertical(id="log", classes="panel"):
            yield Static("Log (warnings and errors from ffaudio and FFmpeg)", classes="panel-title")
            yield RichLog(id="log-view", wrap=True, markup=False)

    def on_mount(self) -> None:
        view = self.query_one(RichLog)
        for line in self.app.engine.log:
            view.write(line)
        if not self.app.engine.log:
            view.write("(nothing yet)")

    def add(self, line: str) -> None:
        self.query_one(RichLog).write(line)
