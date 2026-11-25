"""What can be put in the queue: audio files, folders, playlists, cue sheets (and files with chapters), URLs.

Nothing here knows about the UI; expanding a path may block (it reads playlists and cue sheets), so the UI calls it
from a worker thread.
"""
from __future__ import annotations

import itertools
import os
import re
from dataclasses import dataclass, field
from typing import Iterable, Optional

import ffaudio

AUDIO_EXTENSIONS = {
    ".flac", ".mp3", ".ogg", ".oga", ".opus", ".m4a", ".m4b", ".mp4", ".aac", ".wav", ".wave", ".aif", ".aiff",
    ".aifc", ".wv", ".ape", ".mpc", ".wma", ".dsf", ".dff", ".tta", ".mka", ".webm", ".ac3", ".dts", ".caf", ".alac",
}
PLAYLIST_EXTENSIONS = {".m3u", ".m3u8", ".pls"}
CUE_EXTENSIONS = {".cue"}
# Files that often hold chapters (audiobooks): read as several tracks when they have them
CHAPTER_EXTENSIONS = {".m4b", ".mka"}

_uid = itertools.count(1)


@dataclass(eq=False)
class Item:
    """One entry of the queue: a file or URL, or a range of a file (a cue sheet track, a chapter)."""
    path: str
    start: float = 0.0                  # range in the file (seconds); 0, 0 = the whole file
    length: float = 0.0
    title: str = ""                     # known before probing (cue sheet, playlist), else from the tags
    artist: str = ""
    album: str = ""
    duration: float = -1.0              # -1 = unknown
    number: int = 0                     # track number (cue sheet)
    replaygain: Optional[ffaudio.ReplayGain] = None   # from a cue sheet's REM REPLAYGAIN_* lines
    info: Optional[ffaudio.MediaInfo] = None          # probed in the background
    error: str = ""                     # why it could not be played, the last time
    uid: int = field(default_factory=lambda: next(_uid))

    @property
    def is_url(self) -> bool:
        return ffaudio.is_network_url(self.path)

    @property
    def is_range(self) -> bool:
        return self.start > 0 or self.length > 0

    def play_options(self) -> Optional[ffaudio.PlayOptions]:
        if not self.is_range and self.replaygain is None:
            return None
        return ffaudio.PlayOptions(start_seconds=self.start, length_seconds=self.length, replaygain=self.replaygain)

    # What the UI shows, from the best source there is
    @property
    def display_title(self) -> str:
        if self.title:
            return self.title
        if self.info and self.info.title:
            return self.info.title
        if self.is_url:
            return self.path
        return os.path.splitext(os.path.basename(self.path))[0]

    @property
    def display_artist(self) -> str:
        return self.artist or (self.info.artist if self.info and self.info.artist else "")

    @property
    def display_album(self) -> str:
        return self.album or (self.info.album if self.info and self.info.album else "")

    @property
    def display_duration(self) -> float:
        if self.length > 0:
            return self.length
        if self.duration > 0:
            return self.duration
        if self.info and self.info.duration > 0:
            return max(0.0, self.info.duration - self.start)
        return -1.0

    def to_json(self) -> dict:
        d = {"path": self.path}
        for k in ("start", "length", "title", "artist", "album", "duration", "number"):
            v = getattr(self, k)
            if v not in (0, 0.0, "", -1.0):
                d[k] = v
        if self.replaygain is not None:
            d["replaygain"] = [self.replaygain.track_gain_db, self.replaygain.track_peak,
                               self.replaygain.album_gain_db, self.replaygain.album_peak]
        return d

    @staticmethod
    def from_json(d: dict) -> "Item":
        rg = d.get("replaygain")
        return Item(path=d["path"], start=d.get("start", 0.0), length=d.get("length", 0.0), title=d.get("title", ""),
                    artist=d.get("artist", ""), album=d.get("album", ""), duration=d.get("duration", -1.0),
                    number=d.get("number", 0),
                    replaygain=ffaudio.ReplayGain(*[float("nan") if v is None else v for v in rg]) if rg else None)


def natural_key(s: str):
    """"2 Song" before "10 Song"."""
    return [int(t) if t.isdigit() else t.lower() for t in re.split(r"(\d+)", s)]


def kind(path: str) -> str:
    """"url", "dir", "cue", "playlist", "audio" or "other"."""
    if ffaudio.is_network_url(path):
        ext = os.path.splitext(path.split("?")[0])[1].lower()
        return "playlist" if ext in PLAYLIST_EXTENSIONS else "url"
    if os.path.isdir(path):
        return "dir"
    ext = os.path.splitext(path)[1].lower()
    if ext in CUE_EXTENSIONS:
        return "cue"
    if ext in PLAYLIST_EXTENSIONS:
        return "playlist"
    if ext in AUDIO_EXTENSIONS:
        return "audio"
    return "other"


def is_listed(path: str) -> bool:
    """Whether the file browser shows it."""
    return os.path.isdir(path) or kind(path) in ("cue", "playlist", "audio")


def _cue_items(sheet: ffaudio.CueSheet) -> list[Item]:
    return [Item(path=t.path, start=t.start_seconds, length=t.length_seconds, title=t.title,
                 artist=t.performer or sheet.performer, album=sheet.title, number=t.number,
                 replaygain=None if t.replaygain.empty else t.replaygain)
            for t in sheet.tracks]


def _audio_file(path: str, look_inside: bool) -> list[Item]:
    if look_inside:
        # Chapters, or an embedded cue sheet: its tracks
        try:
            sheet = ffaudio.cue_read(path)
            if len(sheet.tracks) > 1:
                return _cue_items(sheet)
        except ffaudio.Error:
            pass
    return [Item(path=path)]


def _folder(path: str) -> list[Item]:
    items: list[Item] = []
    for root, dirs, files in os.walk(path):
        dirs.sort(key=natural_key)
        files = sorted(files, key=natural_key)
        full = [os.path.join(root, f) for f in files]
        # A cue sheet stands for the audio files it uses (an album image): those are not added again
        covered: set[str] = set()
        for f in full:
            if kind(f) == "cue":
                try:
                    tracks = _cue_items(ffaudio.cue_read(f))
                except ffaudio.Error:
                    continue
                items.extend(tracks)
                covered.update(os.path.normcase(os.path.abspath(t.path)) for t in tracks)
        for f in full:
            if kind(f) == "audio" and os.path.normcase(os.path.abspath(f)) not in covered:
                items.extend(_audio_file(f, os.path.splitext(f)[1].lower() in CHAPTER_EXTENSIONS))
    return items


def expand(path: str) -> list[Item]:
    """The queue items a path stands for; raises ffaudio.Error when it can not be read."""
    k = kind(path)
    if k == "url":
        return [Item(path=path)]
    if k == "dir":
        return _folder(path)
    if k == "cue":
        return _cue_items(ffaudio.cue_read(path))
    if k == "playlist":
        return [Item(path=e.path, title=e.title, duration=e.duration) for e in ffaudio.playlist_read(path)]
    if k == "audio":
        return _audio_file(path, True)
    raise ffaudio.Error(ffaudio.Result.ERR_UNSUPPORTED, path)


def expand_all(paths: Iterable[str]) -> tuple[list[Item], list[str]]:
    """Every item of every path, and the errors of those that could not be read."""
    items, errors = [], []
    for p in paths:
        try:
            items.extend(expand(p))
        except ffaudio.Error as e:
            errors.append(f"{p}: {e}")
    return items, errors


def write_m3u(path: str, items: list[Item]) -> None:
    """The queue as an extended M3U (UTF-8). Ranges of a file are written as the whole file (M3U has none)."""
    with open(path, "w", encoding="utf-8") as f:
        f.write("#EXTM3U\n")
        seen_range_files = set()
        for it in items:
            if it.is_range:
                if it.path in seen_range_files:
                    continue
                seen_range_files.add(it.path)
            d = it.display_duration
            name = " - ".join(x for x in (it.display_artist, it.display_title) if x)
            f.write(f"#EXTINF:{int(d) if d > 0 else -1},{name}\n{it.path}\n")
