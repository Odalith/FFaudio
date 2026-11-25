"""The effects, as the two filter strings ffaudio takes: source filters (at the file's own format: repairs) and the
global filters (equalizers, tone, stereo, headphones, dynamics, the limiter last). See the readme's "Audio
processing chain" for where they sit."""
from __future__ import annotations

from typing import Optional

import ffaudio

from .settings import EffectSettings

BAND_LABELS = ["31", "62", "125", "250", "500", "1k", "2k", "4k", "8k", "16k"]
EQ_RANGE = 12.0   # dB either way on the sliders

PRESETS = {
    "flat":     [0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
    "bass":     [6, 5, 4, 2, 0, 0, 0, 0, 0, 0],
    "treble":   [0, 0, 0, 0, 0, 0, 1, 3, 5, 6],
    "v-shape":  [5, 4, 2, 0, -1, -2, -1, 1, 3, 4],
    "vocal":    [-2, -2, -1, 0, 2, 3, 3, 2, 0, -1],
    "loudness": [6, 4, 1, 0, -1, -1, 0, 1, 3, 4],
    "rock":     [4, 3, 2, 0, -1, 0, 1, 2, 3, 3],
    "classical": [3, 2, 1, 0, 0, 0, -1, -1, 1, 2],
    "spoken":   [-4, -3, -1, 1, 3, 4, 3, 1, -1, -3],
}
HEADPHONES = {"off": "Off", "crossfeed": "Crossfeed", "bs2b": "bs2b (default)", "bs2b-chu": "bs2b (Chu Moy)",
              "bs2b-jan": "bs2b (Jan Meier)"}
COMPRESSORS = {"off": "Off", "gentle": "Gentle", "night": "Night mode"}
LIMITERS = {"auto": "Auto", "on": "On", "off": "Off"}   # auto: while something boosts

EQ_NAME = "eq"   # the graphic EQ's instance name: its bands change live with filter commands


class Effects:
    """Builds the filter strings from EffectSettings, and keeps what it built so a band move can be a live command
    instead of a rebuilt graph."""

    def __init__(self, s: EffectSettings):
        self.s = s
        self.peq: Optional[ffaudio.ParametricEq] = None
        self.peq_error = ""
        self.last_global = None
        self.last_source = None
        self.load_peq()

    def load_peq(self) -> None:
        self.peq, self.peq_error = None, ""
        if not self.s.peq_file:
            return
        try:
            with open(self.s.peq_file, encoding="utf-8", errors="replace") as f:
                self.peq = ffaudio.parametric_eq_parse(f.read())
        except (OSError, ffaudio.Error) as e:
            self.peq_error = str(e)

    @property
    def boosts(self) -> bool:
        s = self.s
        eq_boost = s.eq_on and (s.eq_preamp + max(s.eq_gains)) > 0
        peq_boost = s.peq_on and self.peq is not None and any(b.gain_db + self.peq.preamp_db > 0
                                                               for b in self.peq.bands)
        comp = s.compressor != "off"
        return eq_boost or peq_boost or s.bass > 0 or s.treble > 0 or comp or s.width > 1

    @property
    def limiter_active(self) -> bool:
        return self.s.limiter == "on" or (self.s.limiter == "auto" and self.boosts)

    def global_filters(self, stereo_output: bool = True) -> str:
        s = self.s
        fb = ffaudio.FilterBuilder()
        if s.peq_on and self.peq is not None:
            fb.add_parametric_eq(self.peq)
        if s.eq_on:
            eq = ffaudio.equalizer_defaults()
            eq.gain_db = [float(g) for g in s.eq_gains]
            eq.preamp_db = s.eq_preamp
            fb.add_equalizer(eq, name=EQ_NAME)   # named: every band is in the graph, so each can change live
        if s.bass or s.treble:
            fb.add_tone(s.bass, s.treble)
        if stereo_output:
            if s.balance or s.width != 1 or s.mono or s.swap:
                fb.add_stereo(ffaudio.Stereo(balance=s.balance, width=0.0 if s.mono else s.width, swap=s.swap))
            if s.headphones == "crossfeed":
                fb.add_crossfeed()
            elif s.headphones.startswith("bs2b") and ffaudio.filter_available("bs2b"):
                fb.add_bs2b({"bs2b": ffaudio.Bs2bProfile.DEFAULT, "bs2b-chu": ffaudio.Bs2bProfile.CHU_MOY,
                             "bs2b-jan": ffaudio.Bs2bProfile.JAN_MEIER}[s.headphones])
        if s.compressor == "gentle":
            fb.add_compressor()
        elif s.compressor == "night":
            fb.add_compressor(ffaudio.compressor_night())
        if self.limiter_active:
            fb.add_limiter()   # last: the safety net for what the others boost
        return fb.string

    def source_filters(self) -> str:
        fb = ffaudio.FilterBuilder()
        if self.s.declick:
            fb.add_declick()
        if self.s.declip:
            fb.add_declip()
        return fb.string

    def apply(self, stereo_output: bool = True) -> None:
        """Sets what changed (each set rebuilds the tracks' graphs, so unchanged strings are not sent again)."""
        g, src = self.global_filters(stereo_output), self.source_filters()
        if g != self.last_global:
            ffaudio.set_filters(g or None)
            self.last_global = g
        if src != self.last_source:
            ffaudio.set_source_filters(src or None)
            self.last_source = src

    def set_band(self, band: int, gain_db: float, stereo_output: bool = True) -> bool:
        """One graphic EQ band. While the graph keeps its shape this is a live filter command, heard at once with
        nothing rebuilt; it only reaches the tracks already playing, so the caller applies the new string with
        apply() once the changes stop (True = that is still to do). Otherwise the graph is rebuilt right away."""
        before_limiter = self.limiter_active
        self.s.eq_gains[band] = gain_db
        self.s.eq_preset = "custom"
        if self.s.eq_on and self.limiter_active == before_limiter and self.last_global is not None:
            ffaudio.send_filter_command(f"equalizer@{EQ_NAME}_{band}", "g", f"{gain_db:g}")
            return True
        self.apply(stereo_output)
        return False

    def set_preset(self, name: str) -> None:
        if name in PRESETS:
            self.s.eq_preset = name
            self.s.eq_gains = [float(g) for g in PRESETS[name]]

    def describe(self) -> str:
        """One line for the status bar: what is on."""
        s, on = self.s, []
        if s.peq_on and self.peq is not None:
            on.append("PEQ")
        if s.eq_on:
            on.append(f"EQ {s.eq_preset}")
        if s.bass or s.treble:
            on.append("tone")
        if s.mono:
            on.append("mono")
        elif s.width != 1:
            on.append(f"width {s.width:.1f}")
        if s.balance:
            on.append(f"bal {s.balance:+.1f}")
        if s.headphones != "off":
            on.append(HEADPHONES[s.headphones].split(" ")[0])
        if s.compressor != "off":
            on.append(s.compressor)
        if self.limiter_active:
            on.append("limiter")
        if s.declick or s.declip:
            on.append("repair")
        return " · ".join(on) if on else "no effects"
