"""The player's settings and saved session, as JSON in the per-user config folder.

Unknown or damaged values fall back to the defaults, so an old or hand-edited file never stops the player.
"""
from __future__ import annotations

import json
import os
import sys
from dataclasses import asdict, dataclass, field, fields
from typing import Optional


FILE_OVERRIDE: Optional[str] = None   # --settings


def config_dir() -> str:
    if sys.platform == "win32":
        base = os.environ.get("APPDATA") or os.path.expanduser("~")
    elif sys.platform == "darwin":
        base = os.path.expanduser("~/Library/Application Support")
    else:
        base = os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config")
    return os.path.join(base, "ffaudio-player")


@dataclass
class EffectSettings:
    eq_on: bool = False
    eq_preset: str = "flat"
    eq_gains: list = field(default_factory=lambda: [0.0] * 10)
    eq_preamp: float = 0.0
    peq_file: str = ""                  # an EqualizerAPO / AutoEq ParametricEQ.txt
    peq_on: bool = False
    bass: float = 0.0
    treble: float = 0.0
    balance: float = 0.0
    width: float = 1.0
    mono: bool = False
    swap: bool = False
    headphones: str = "off"             # off, crossfeed, bs2b, bs2b-chu, bs2b-jan
    compressor: str = "off"             # off, gentle, night
    limiter: str = "auto"               # auto (while something boosts), on, off
    declick: bool = False
    declip: bool = False


@dataclass
class Settings:
    volume: float = 1.0
    muted: bool = False
    speed: float = 1.0
    keep_pitch: bool = True
    repeat: str = "off"                 # off, all, one
    shuffle: bool = False
    device: Optional[str] = None        # None = the system default
    channels: int = 0                   # output format; 0 = the device's
    sample_rate: int = 0
    downmix: str = "full"               # full, normalized
    upmix: bool = False
    replaygain: str = "track"           # off, track, album
    loudness_target: float = -14.0      # LUFS: -18 = ReplayGain 2.0, -14 = most streaming services
    fallback_gain_db: float = -6.0      # for files without ReplayGain values
    prevent_clipping: bool = True
    measure_missing: bool = False       # measure files without values in the background (writes the cache)
    crossfade: float = 0.0              # seconds between tracks; 0 = gapless
    crossfade_curve: str = "equal_power"
    skip_fade: float = 0.3              # skip, play another track, stop
    pause_fade: float = 0.15
    prepare_next: float = 10.0
    browser_root: str = ""
    theme: str = "tokyo-night"
    effects: EffectSettings = field(default_factory=EffectSettings)
    # The session
    queue: list = field(default_factory=list)
    current: int = -1                   # index into queue
    position: float = 0.0

    @property
    def path(self) -> str:
        return FILE_OVERRIDE or os.path.join(config_dir(), "settings.json")

    @staticmethod
    def load(path: Optional[str] = None) -> "Settings":
        s = Settings()
        try:
            with open(path or s.path, encoding="utf-8") as f:
                data = json.load(f)
        except (OSError, ValueError):
            return s
        _fill(s, data)
        return s

    def save(self, path: Optional[str] = None) -> None:
        target = path or self.path
        os.makedirs(os.path.dirname(target), exist_ok=True)
        tmp = target + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(asdict(self), f, indent=1)
        os.replace(tmp, target)


def _fill(obj, data: dict) -> None:
    if not isinstance(data, dict):
        return
    for f in fields(obj):
        if f.name not in data:
            continue
        cur, new = getattr(obj, f.name), data[f.name]
        if isinstance(cur, EffectSettings):
            _fill(cur, new)
        elif cur is None or isinstance(new, type(cur)) or (isinstance(cur, float) and isinstance(new, int)):
            if isinstance(cur, list) and cur and len(new) != len(cur) and f.name == "eq_gains":
                continue
            setattr(obj, f.name, float(new) if isinstance(cur, float) else new)
