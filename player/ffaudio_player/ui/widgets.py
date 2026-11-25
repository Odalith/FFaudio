"""The player's own widgets: a slider (Textual has none), the seek bar, level meters, the spectrum and cover art."""
from __future__ import annotations

import io
import math
import time
from typing import Callable, Optional

from rich.style import Style
from rich.text import Text
from textual import events
from textual.binding import Binding
from textual.message import Message
from textual.reactive import reactive
from textual.widget import Widget

try:
    from PIL import Image
except ImportError:   # cover art is optional
    Image = None

BLOCKS = " ▁▂▃▄▅▆▇█"


def fmt_time(seconds: float) -> str:
    if seconds is None or seconds < 0 or math.isnan(seconds):
        return "--:--"
    s = int(seconds)
    h, m, s = s // 3600, s % 3600 // 60, s % 60
    return f"{h}:{m:02d}:{s:02d}" if h else f"{m}:{s:02d}"


def db(linear: float, floor: float = -90.0) -> float:
    return 20 * math.log10(linear) if linear > 1e-9 else floor


def level_color(fraction: float) -> str:
    """Green, then yellow, then red towards full scale."""
    return "#5fd75f" if fraction < 0.7 else "#d7d75f" if fraction < 0.9 else "#ff5f5f"


#+-----------------------------------------------------+
#| Slider                                              |
#+-----------------------------------------------------+

class Slider(Widget, can_focus=True):
    """A value between lo and hi, horizontal or vertical: arrows move it by step (shift: 5 steps), home/end/0, and
    the mouse sets it where it is clicked or dragged. Posts Slider.Changed."""

    DEFAULT_CSS = """
    Slider { height: 1; width: 1fr; min-width: 10; }
    Slider.-vertical { width: 5; height: 1fr; min-height: 5; }
    Slider:focus { background: $boost; }
    """
    BINDINGS = [
        Binding("right,up", "move(1)", "+", show=False), Binding("left,down", "move(-1)", "-", show=False),
        Binding("shift+right,shift+up", "move(5)", show=False), Binding("shift+left,shift+down", "move(-5)", show=False),
        Binding("home", "to(0)", show=False), Binding("end", "to(1)", show=False),
        Binding("0", "reset", show=False),
    ]

    value = reactive(0.0)

    class Changed(Message):
        def __init__(self, slider: "Slider", value: float, final: bool):
            super().__init__()
            self.slider, self.value, self.final = slider, value, final

        @property
        def control(self):
            return self.slider

    def __init__(self, lo: float, hi: float, value: float = 0.0, step: float = 1.0, *, default: float = 0.0,
                 vertical: bool = False, fmt: Callable[[float], str] = lambda v: f"{v:+.0f}", label: str = "",
                 id: Optional[str] = None, classes: str = ""):
        super().__init__(id=id, classes=classes + (" -vertical" if vertical else ""))
        self.lo, self.hi, self.step, self.default, self.vertical, self.fmt, self.label = lo, hi, step, default, \
            vertical, fmt, label
        self.set_reactive(Slider.value, self._clamp(value))
        self._dragging = False

    def _clamp(self, v: float) -> float:
        v = min(max(v, self.lo), self.hi)
        return round(round(v / self.step) * self.step, 6)

    def set(self, v: float, final: bool = True) -> None:
        v = self._clamp(v)
        if v != self.value:
            self.value = v
            self.post_message(self.Changed(self, v, final))

    def action_move(self, steps: int) -> None:
        self.set(self.value + steps * self.step)

    def action_to(self, end: int) -> None:
        self.set(self.hi if end else self.lo)

    def action_reset(self) -> None:
        self.set(self.default)

    def _from_mouse(self, event) -> float:
        if self.vertical:
            h = max(1, self.size.height - 2)
            f = 1 - (event.y - 1) / max(1, h - 1)
        else:
            w = max(1, self.size.width - len(self.label) - 8)
            f = (event.x - len(self.label)) / max(1, w - 1)
        return self.lo + min(max(f, 0), 1) * (self.hi - self.lo)

    def on_mouse_down(self, event: events.MouseDown) -> None:
        self._dragging = True
        self.capture_mouse()
        self.focus()
        self.set(self._from_mouse(event), final=False)

    def on_mouse_move(self, event: events.MouseMove) -> None:
        if self._dragging:
            self.set(self._from_mouse(event), final=False)

    def on_mouse_up(self, event: events.MouseUp) -> None:
        if self._dragging:
            self._dragging = False
            self.release_mouse()
            self.post_message(self.Changed(self, self.value, True))

    def render(self):
        f = (self.value - self.lo) / ((self.hi - self.lo) or 1)
        accent = Style(color=self.app.theme_variables.get("accent", "#ffa62b"))
        dim = Style(color="#555555")
        if self.vertical:
            h = max(3, self.size.height - 2)
            knob = round((1 - f) * (h - 1))
            zero = round((1 - (self.default - self.lo) / ((self.hi - self.lo) or 1)) * (h - 1))
            t = Text(justify="center")
            t.append(self.fmt(self.value)[:5].center(5) + "\n", Style(bold=True))
            for row in range(h):
                lit = (zero <= row <= knob) or (knob <= row <= zero)
                ch = "██" if row == knob else ("▐▌" if lit else "││")
                t.append(" " + ch + " \n", accent if lit or row == knob else dim)
            t.append(self.label.center(5)[:5], Style(dim=True))
            return t
        w = max(4, self.size.width - len(self.label) - 8)
        knob = round(f * (w - 1))
        t = Text()
        t.append(self.label, Style(dim=True))
        t.append("━" * knob, accent)
        t.append("●", accent + Style(bold=True))
        t.append("─" * (w - 1 - knob), dim)
        t.append(" " + self.fmt(self.value).rjust(6))
        return t


#+-----------------------------------------------------+
#| Seek bar                                            |
#+-----------------------------------------------------+

class SeekBar(Widget):
    """Elapsed ━━━━●──── total; click (or drag) to seek."""

    DEFAULT_CSS = "SeekBar { height: 1; }"

    class Seek(Message):
        def __init__(self, fraction: float):
            super().__init__()
            self.fraction = fraction

    def __init__(self, **kw):
        super().__init__(**kw)
        self.position, self.duration, self.live, self.buffered = -1.0, -1.0, False, -1.0

    def show(self, position: float, duration: float, live: bool = False, buffered: float = -1.0) -> None:
        self.position, self.duration, self.live, self.buffered = position, duration, live, buffered
        self.refresh()

    def render(self):
        accent = self.app.theme_variables.get("accent", "#ffa62b")
        left = fmt_time(self.position) if self.position >= 0 else ""
        if self.live or self.duration <= 0:
            right = "● LIVE" if self.live else "--:--"
            bar_w = max(4, self.size.width - len(left) - len(right) - 2)
            t = Text()
            t.append(left.rjust(7) + " ", Style(bold=True))
            t.append("─" * bar_w, Style(color="#555555"))
            t.append(" " + right, Style(color="#ff5f5f" if self.live else "#888888", bold=self.live))
            return t
        right = fmt_time(self.duration)
        bar_w = max(4, self.size.width - 16)
        f = min(max(self.position / self.duration, 0), 1)
        knob = round(f * (bar_w - 1))
        t = Text()
        t.append(left.rjust(7) + " ", Style(bold=True))
        t.append("━" * knob, Style(color=accent))
        t.append("●", Style(color=accent, bold=True))
        t.append("─" * (bar_w - 1 - knob), Style(color="#555555"))
        t.append(" " + right.ljust(7), Style(dim=True))
        return t

    def _fraction(self, x: int) -> float:
        bar_w = max(4, self.size.width - 16)
        return min(max((x - 8) / max(1, bar_w - 1), 0), 1)

    def on_mouse_down(self, event: events.MouseDown) -> None:
        if self.duration > 0 and not self.live:
            self.post_message(self.Seek(self._fraction(event.x)))


#+-----------------------------------------------------+
#| Level meters and spectrum                           |
#+-----------------------------------------------------+

CHANNEL_NAMES = {1: ["M"], 2: ["L", "R"], 6: ["FL", "FR", "C", "LF", "SL", "SR"],
                 8: ["FL", "FR", "C", "LF", "BL", "BR", "SL", "SR"]}


class LevelMeter(Widget):
    """Peak (the bar's tip, held a moment) and RMS (the filled part) per channel, -48..0 dBFS."""

    DEFAULT_CSS = "LevelMeter { height: auto; min-height: 3; }"
    FLOOR = -48.0

    def __init__(self, **kw):
        super().__init__(**kw)
        self.peak, self.rms, self.hold, self.hold_t, self.rms_s = [], [], [], [], []

    def show(self, peak: tuple, rms: tuple) -> None:
        now = time.monotonic()
        n = len(peak)
        if len(self.hold) != n:
            self.hold, self.hold_t, self.rms_s = [self.FLOOR] * n, [0.0] * n, [self.FLOOR] * n
        self.peak = [db(p) for p in peak]
        for i, r in enumerate(rms):
            # RMS rises at once and falls smoothly, as a VU needle would
            self.rms_s[i] = max(db(r), self.rms_s[i] - 1.5)
            if self.peak[i] >= self.hold[i] or now - self.hold_t[i] > 1.5:
                self.hold[i], self.hold_t[i] = self.peak[i], now
        relayout = len(self.rms) != n
        self.rms = list(self.rms_s)
        self.refresh(layout=relayout)

    def get_content_height(self, container, viewport, width) -> int:
        return max(2, len(self.rms)) + 1

    def render(self):
        w = max(8, self.size.width - 9)
        names = CHANNEL_NAMES.get(len(self.rms), [str(i + 1) for i in range(len(self.rms))])
        t = Text()
        if not self.rms:
            for name in ("L", "R"):
                t.append(f"{name:>2} ", Style(dim=True))
                t.append("·" * w + "\n", Style(color="#333333"))
        for i, r in enumerate(self.rms):
            frac = lambda v: min(max((v - self.FLOOR) / -self.FLOOR, 0), 1)
            fill, tip = round(frac(r) * w), min(w - 1, round(frac(self.hold[i]) * w))
            t.append(f"{names[i] if i < len(names) else i + 1:>2} ", Style(dim=True))
            for x in range(w):
                c = level_color(x / w)
                if x < fill:
                    t.append("█", Style(color=c))
                elif x == tip and self.hold[i] > self.FLOOR:
                    t.append("▌", Style(color=c, bold=True))
                else:
                    t.append("·", Style(color="#333333"))
            pk = self.hold[i]
            t.append(f" {pk:5.1f}" if pk > self.FLOOR else "   -∞", Style(color="#ff5f5f" if pk > -0.1 else "#888888"))
            t.append("\n")
        scale = Text("   ")
        marks = {0: "-48", round(w * 0.25): "-36", round(w * 0.5): "-24", round(w * 0.75): "-12", w - 1: "0"}
        x = 0
        while x < w:
            if x in marks:
                scale.append(marks[x], Style(dim=True))
                x += len(marks[x])
            else:
                scale.append(" ")
                x += 1
        t.append(scale)
        return t


class SpectrumView(Widget):
    """Log-spaced bands as bars of eighth blocks, -72..0 dB; quick to rise, slower to fall, with peak caps."""

    DEFAULT_CSS = "SpectrumView { height: 1fr; min-height: 3; }"
    FLOOR = -72.0

    def __init__(self, **kw):
        super().__init__(**kw)
        self.values: list[float] = []
        self.caps: list[float] = []

    @property
    def bands_wanted(self) -> int:
        return max(8, min(64, self.size.width // 2))

    def show(self, bands: list) -> None:
        n = len(bands)
        if len(self.values) != n:
            self.values, self.caps = [0.0] * n, [0.0] * n
        for i, m in enumerate(bands):
            # Magnitudes are scaled so a full-scale sine reads 1.0; a band sums a few bins
            v = min(max((db(m) - self.FLOOR) / -self.FLOOR, 0.0), 1.0)
            self.values[i] = v if v > self.values[i] else max(v, self.values[i] - 0.04)
            self.caps[i] = max(self.values[i], self.caps[i] - 0.008)
        self.refresh()

    def render(self):
        h = max(1, self.size.height)
        n = len(self.values)
        t = Text()
        if not n:
            return Text("\n" * (h - 1) + "▁" * self.size.width, Style(color="#333333"))
        bar = max(1, self.size.width // n - 1)
        for row in range(h):
            level_top = (h - row) / h
            for i in range(n):
                v, cap = self.values[i] * h, self.caps[i] * h
                cell = v - (h - row - 1)
                color = level_color(level_top)
                if cell >= 1:
                    t.append("█" * bar, Style(color=color))
                elif cell > 0:
                    t.append(BLOCKS[max(1, round(cell * 8))] * bar, Style(color=color))
                elif h - row - 1 <= cap < h - row and cap > 0.02 * h:
                    t.append("▔" * bar, Style(color="#aaaaaa"))
                else:
                    t.append(" " * bar)
                t.append(" ")
            if row < h - 1:
                t.append("\n")
        return t


#+-----------------------------------------------------+
#| Cover art                                           |
#+-----------------------------------------------------+

class CoverArt(Widget):
    """A picture drawn with half blocks (two pixels per cell, any terminal); a placeholder without Pillow or art."""

    DEFAULT_CSS = "CoverArt { width: 20; height: 10; }"

    def __init__(self, **kw):
        super().__init__(**kw)
        self.data: Optional[bytes] = None
        self.initials = "♪"
        self._cache: Optional[tuple] = None

    def show(self, data: Optional[bytes], initials: str = "♪") -> None:
        self.data, self.initials, self._cache = data, initials or "♪", None
        self.refresh()

    def render(self):
        w, h = self.size.width, self.size.height
        if self.data and Image is not None:
            if self._cache and self._cache[0] == (w, h):
                return self._cache[1]
            try:
                img = Image.open(io.BytesIO(self.data)).convert("RGB")
                img.thumbnail((w, h * 2), Image.LANCZOS)
                iw, ih = img.size
                ih -= ih % 2
                px = img.load()
                t = Text()
                pad = (w - iw) // 2
                top_pad = (h - ih // 2) // 2
                t.append("\n" * top_pad)
                for y in range(0, ih, 2):
                    t.append(" " * pad)
                    for x in range(iw):
                        a, b = px[x, y], px[x, y + 1]
                        t.append("▀", Style(color=f"rgb({a[0]},{a[1]},{a[2]})", bgcolor=f"rgb({b[0]},{b[1]},{b[2]})"))
                    t.append("\n")
                self._cache = ((w, h), t)
                return t
            except Exception:
                pass
        accent = self.app.theme_variables.get("accent", "#ffa62b")
        t = Text(justify="center")
        mid = h // 2
        for row in range(h):
            if row == mid:
                t.append(self.initials[:max(1, w - 2)].center(w), Style(color=accent, bold=True))
            else:
                t.append("░" * w, Style(color="#2a2a2a"))
            if row < h - 1:
                t.append("\n")
        return t
