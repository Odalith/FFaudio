"""The main screen: the library browser, the queue, and the now-playing panel with meters and spectrum."""
from __future__ import annotations

import os
from pathlib import Path
from typing import Iterable, Optional

from rich.style import Style
from rich.text import Text
from textual import on, work
from textual.app import ComposeResult
from textual.binding import Binding
from textual.containers import Horizontal, Vertical
from textual.screen import Screen
from textual.widgets import DataTable, DirectoryTree, Footer, Input, Static

import ffaudio

from .. import library
from ..library import Item
from .widgets import CoverArt, LevelMeter, SeekBar, SpectrumView, fmt_time

# Plain symbols: emoji are drawn at different widths by different terminals
REPEAT_ICONS = {"off": "", "all": "↻ all", "one": "↻ one"}
FIXED_COLUMNS = {"state": 2, "n": 4, "gain": 6, "time": 7}


class Browser(DirectoryTree):
    """Folders, audio files, playlists and cue sheets."""

    BINDINGS = [
        Binding("a", "add", "Add"), Binding("A", "add_play", "Play now"), Binding("i", "add_next", "Play next"),
        Binding("backspace", "up", "Parent folder", show=False),
    ]

    def filter_paths(self, paths: Iterable[Path]) -> Iterable[Path]:
        return [p for p in paths if not p.name.startswith(".") and library.is_listed(str(p))]

    def _target(self) -> Optional[str]:
        node = self.cursor_node
        return str(node.data.path) if node and node.data else None

    def action_add(self) -> None:
        if (p := self._target()):
            self.screen.add_paths([p])

    def action_add_play(self) -> None:
        if (p := self._target()):
            self.screen.add_paths([p], play=True)

    def action_add_next(self) -> None:
        if (p := self._target()):
            self.screen.add_paths([p], next_=True)

    def action_up(self) -> None:
        parent = Path(self.path).resolve().parent
        self.path = parent
        self.screen.app.engine.s.browser_root = str(parent)


class QueueTable(DataTable):
    BINDINGS = [
        Binding("enter", "play_selected", "Play", show=False),
        Binding("delete,x", "remove", "Remove"),
        Binding("ctrl+up,K", "move(-1)", "Move up", show=False), Binding("ctrl+down,J", "move(1)", "Move down",
                                                                          show=False),
        Binding("i", "queue_next", "Play next", show=False),
    ]

    def action_play_selected(self) -> None:
        if (it := self.screen.selected_item()):
            self.app.engine.play_item(it)

    def action_remove(self) -> None:
        if (it := self.screen.selected_item()):
            row = self.cursor_row
            self.app.engine.remove([it])
            self.move_cursor(row=min(row, self.row_count - 1))

    def action_move(self, delta: int) -> None:
        if (it := self.screen.selected_item()):
            j = self.app.engine.move(it, delta)
            self.move_cursor(row=self.screen.row_of(it, default=j))

    def action_queue_next(self) -> None:
        if (it := self.screen.selected_item()):
            eng = self.app.engine
            eng.remove([it])
            eng.add_next([it])


class MainScreen(Screen):
    """Keys that would get in the way of typing are turned off while an input has focus (check_action)."""

    BINDINGS = [
        Binding("space", "play_pause", "Play/Pause", priority=True),
        Binding("n", "next", "Next"), Binding("p", "previous", "Prev"),
        Binding("left", "seek(-5)", "-5 s", show=False, priority=True),
        Binding("right", "seek(5)", "+5 s", show=False, priority=True),
        Binding("shift+left,comma", "seek(-30)", "-30 s", show=False),
        Binding("shift+right,full_stop", "seek(30)", "+30 s", show=False),
        Binding("plus,equals_sign", "volume(0.05)", "Vol+", show=False),
        Binding("minus,underscore", "volume(-0.05)", "Vol-", show=False),
        Binding("m", "mute", "Mute", show=False),
        Binding("left_square_bracket", "speed(-0.05)", "Slower", show=False),
        Binding("right_square_bracket", "speed(0.05)", "Faster", show=False),
        Binding("backslash", "pitch", "Pitch", show=False),
        Binding("r", "repeat", "Repeat", show=False), Binding("s", "shuffle", "Shuffle", show=False),
        Binding("e", "app.effects", "Effects"), Binding("o", "app.settings", "Settings"),
        Binding("u", "app.open", "Open"), Binding("slash", "filter", "Filter", show=False),
        Binding("b", "toggle_browser", "Files", show=False), Binding("l", "app.log", "Log", show=False),
        Binding("question_mark,f1", "app.help", "Help"), Binding("q", "app.quit", "Quit"),
        Binding("ctrl+l", "app.loudness", "Measure loudness", show=False),
        Binding("ctrl+s", "app.save_playlist", "Save queue", show=False),
        Binding("escape", "clear_filter", show=False),
    ] + [Binding(str(d), f"seek_fraction({d / 10})", show=False) for d in range(10)]

    TYPING_KEYS = {"play_pause", "seek", "volume", "speed", "mute", "pitch", "repeat", "shuffle", "next",
                   "previous", "toggle_browser", "filter", "seek_fraction"}

    def __init__(self):
        super().__init__()
        self.row_items: list[Item] = []
        self.filter_text = ""

    def compose(self) -> ComposeResult:
        eng = self.app.engine
        with Horizontal(id="topbar"):
            yield Static(id="topbar-left")
            yield Static(id="topbar-right")
        with Horizontal(id="body"):
            with Vertical(id="browser-pane"):
                root = eng.s.browser_root if os.path.isdir(eng.s.browser_root or "") else _music_dir()
                yield Browser(root, id="browser")
            with Vertical(id="queue-pane"):
                yield Input(placeholder="filter the queue (title, artist, album)…", id="filter")
                yield QueueTable(id="queue", cursor_type="row", zebra_stripes=True)
        with Horizontal(id="now"):
            yield CoverArt(id="art")
            with Vertical(id="info"):
                yield Static(id="title")
                yield Static(id="subtitle")
                yield Static(id="format")
                yield SeekBar(id="seek")
                yield Static(id="status")
            with Vertical(id="meters"):
                yield LevelMeter(id="levels")
                yield SpectrumView(id="spectrum")
        yield Footer()

    def on_mount(self) -> None:
        self._columns_for = -1
        self.layout_columns()
        self.query_one("#browser-pane").border_title = "Files"
        self.query_one("#now").border_title = "Now playing"
        self.query_one("#filter").display = False
        self.rebuild_queue()
        self.refresh_now()
        self.refresh_topbar()
        self.query_one(QueueTable).focus()

    # Typing in the filter: the playback keys go to the input instead
    def check_action(self, action: str, parameters: tuple) -> Optional[bool]:
        if action in self.TYPING_KEYS and isinstance(self.app.focused, Input):
            return False
        return True

    #+-----------------------------------------------------+
    #| The queue table                                     |
    #+-----------------------------------------------------+

    def _matches(self, it: Item) -> bool:
        f = self.filter_text.lower()
        return not f or any(f in s.lower() for s in (it.display_title, it.display_artist, it.display_album, it.path))

    def _cells(self, it: Item, n: int):
        eng = self.app.engine
        st = ffaudio.status()
        playing = it is eng.current
        state = Text("")
        if playing:
            state = Text("‖" if st.paused else "►", Style(color=self.app.theme_variables.get("accent"), bold=True))
        elif it.error:
            state = Text("✗", Style(color="#ff5f5f", bold=True))
        style = Style(bold=True, color=self.app.theme_variables.get("accent")) if playing else (
            Style(color="#ff8787") if it.error else Style())
        return (state, Text(str(n), Style(dim=True)), Text(it.display_title, style), Text(it.display_artist, style),
                Text(it.display_album, style), Text(self._gain(it), Style(dim=True)),
                Text(fmt_time(it.display_duration) if it.display_duration > 0 else ("live" if it.is_url else ""),
                     Style(dim=True), justify="right"))

    def _gain(self, it: Item) -> str:
        rg = it.replaygain
        if rg is None or rg.empty:
            rg = it.info.replaygain if it.info and not it.info.replaygain.empty else None
        if rg is None and not it.is_url:
            rg = ffaudio.gain_cache_get(it.path)
        if rg is None or rg.empty:
            return ""
        album = self.app.engine.s.replaygain == "album"
        v = rg.album_gain_db if album and rg.album_gain_db == rg.album_gain_db else rg.track_gain_db
        return f"{v:+.1f}" if v == v else ""

    def layout_columns(self) -> None:
        """Title, artist and album share what the fixed columns leave of the pane's width."""
        t = self.query_one(QueueTable)
        width = self.query_one("#queue-pane").size.width or self.size.width * 2 // 3
        if width == self._columns_for:
            return
        self._columns_for = width
        free = max(30, width - 4 - sum(FIXED_COLUMNS.values()) - 2 * 7)
        title, artist = free * 45 // 100, free * 27 // 100
        t.clear(columns=True)
        t.add_column(" ", key="state", width=FIXED_COLUMNS["state"])
        t.add_column("#", key="n", width=FIXED_COLUMNS["n"])
        t.add_column("Title", key="title", width=title)
        t.add_column("Artist", key="artist", width=artist)
        t.add_column("Album", key="album", width=free - title - artist)
        t.add_column("Gain", key="gain", width=FIXED_COLUMNS["gain"])
        t.add_column(Text("Time", justify="right"), key="time", width=FIXED_COLUMNS["time"])
        self.rebuild_queue()

    def rebuild_queue(self) -> None:
        t = self.query_one(QueueTable)
        eng = self.app.engine
        row = t.cursor_row
        t.clear()
        self.row_items = [it for it in eng.items if self._matches(it)]
        for it in self.row_items:
            t.add_row(*self._cells(it, eng.items.index(it) + 1), key=str(it.uid))
        if self.row_items:
            t.move_cursor(row=min(max(row, 0), len(self.row_items) - 1))
        self.update_queue_title()

    def update_queue_title(self) -> None:
        eng = self.app.engine
        n, total = len(eng.items), eng.total_duration()
        shown = f" · {len(self.row_items)} shown" if self.filter_text else ""
        self.query_one("#queue-pane").border_title = (f"Queue · {n} track{'s' if n != 1 else ''}"
                                                      f"{' · ' + fmt_time(total) if total else ''}{shown}")
        self._queue_subtitle()

    def _queue_subtitle(self) -> None:
        s = self.app.engine.s
        bits = [x for x in (REPEAT_ICONS[s.repeat], "🔀 shuffle" if s.shuffle else "") if x]
        self.query_one("#queue-pane").border_subtitle = "  ".join(bits)

    def refresh_item(self, it: Item) -> None:
        if it not in self.row_items:
            return
        t = self.query_one(QueueTable)
        cells = self._cells(it, self.app.engine.items.index(it) + 1 if it in self.app.engine.items else 0)
        for key, value in zip(("state", "n", "title", "artist", "album", "gain", "time"), cells):
            try:
                t.update_cell(str(it.uid), key, value)
            except Exception:
                return
        self.update_queue_title()

    def selected_item(self) -> Optional[Item]:
        t = self.query_one(QueueTable)
        if not self.row_items or t.cursor_row < 0 or t.cursor_row >= len(self.row_items):
            return None
        return self.row_items[t.cursor_row]

    def row_of(self, it: Item, default: int = 0) -> int:
        return self.row_items.index(it) if it in self.row_items else default

    @on(DataTable.RowSelected, "#queue")
    def _row_selected(self, event: DataTable.RowSelected) -> None:
        if (it := self.selected_item()):
            self.app.engine.play_item(it)

    #+-----------------------------------------------------+
    #| Adding                                              |
    #+-----------------------------------------------------+

    @on(DirectoryTree.FileSelected, "#browser")
    def _file_selected(self, event: DirectoryTree.FileSelected) -> None:
        self.add_paths([str(event.path)], play=True)

    @work(thread=True, exclusive=False, group="expand")
    def add_paths(self, paths: list, play: bool = False, next_: bool = False) -> None:
        """Reads the folders, playlists and cue sheets on a worker thread, then adds on the UI thread."""
        self.app.call_from_thread(self.app.notify, "Reading " + ", ".join(os.path.basename(p.rstrip("/\\")) or p
                                                                          for p in paths)[:80], timeout=2)
        items, errors = library.expand_all(paths)
        self.app.call_from_thread(self._added, items, errors, play, next_)

    def _added(self, items: list, errors: list, play: bool, next_: bool) -> None:
        eng = self.app.engine
        for e in errors:
            self.app.notify(e, severity="error", title="Could not add")
        if not items:
            if not errors:
                self.app.notify("Nothing playable there", severity="warning")
            return
        if next_:
            eng.add_next(items)
        else:
            # Into an empty queue with nothing playing: start it
            eng.add(items, play=play or (not ffaudio.status().handle and not eng.items))
        self.app.notify(f"Added {len(items)} track{'s' if len(items) != 1 else ''}", timeout=2)

    #+-----------------------------------------------------+
    #| Now playing                                         |
    #+-----------------------------------------------------+

    def refresh_now(self) -> None:
        eng = self.app.engine
        it = eng.current
        accent = self.app.theme_variables.get("accent")
        title, sub, fmt = self.query_one("#title", Static), self.query_one("#subtitle", Static), \
            self.query_one("#format", Static)
        if it is None:
            title.update(Text("Nothing playing", Style(bold=True, dim=True)))
            sub.update(Text("Add music from the files on the left (a), or open a file or URL (u)", Style(dim=True)))
            fmt.update("")
            self.query_one(CoverArt).show(None)
            return
        info = it.info
        name = eng.stream_title or it.display_title
        title.update(Text(name, Style(bold=True, color=accent)))
        parts = [x for x in (it.display_artist, it.display_album) if x]
        year = info.tag("date", "")[:4] if info else ""
        sub.update(Text(" — ".join(parts) + (f" ({year})" if year else "") if parts else
                        (it.display_title if eng.stream_title else ""), Style(italic=True)))
        fmt.update(Text(self._format_line(it), Style(dim=True)))

    def _format_line(self, it: Item) -> str:
        info = it.info
        bits = []
        if info:
            bits.append((info.codec or "").upper())
            if info.sample_rate:
                bits.append(f"{info.sample_rate / 1000:g} kHz" + (f"/{info.bits_per_sample}-bit"
                                                                  if info.bits_per_sample else ""))
            if info.channel_layout:
                bits.append(info.channel_layout)
            if info.bit_rate > 0:
                bits.append(f"{info.bit_rate // 1000} kbps")
        try:
            g = ffaudio.track_gain_db(0)
            if self.app.engine.s.replaygain != "off":
                bits.append(f"gain {g:+.1f} dB")
        except ffaudio.Error:
            pass
        if it.is_range:
            bits.append(f"track {it.number}" if it.number else "range")
        return " · ".join(b for b in bits if b)

    @work(thread=True, exclusive=True, group="art")
    def load_art(self, item: Item) -> None:
        data = None
        if not item.is_url:
            try:
                pic = ffaudio.media_picture(item.path)
                data = pic.data if pic else None
            except ffaudio.Error:
                data = None
            if data is None:   # a picture next to the files
                folder = os.path.dirname(item.path)
                for name in ("cover.jpg", "cover.png", "folder.jpg", "front.jpg", "Cover.jpg", "Folder.jpg",
                             "AlbumArt.jpg"):
                    p = os.path.join(folder, name)
                    if os.path.isfile(p):
                        with open(p, "rb") as f:
                            data = f.read()
                        break
        initials = "".join(w[0] for w in (item.display_album or item.display_title).split()[:3]).upper()
        self.app.call_from_thread(self.query_one(CoverArt).show, data, initials)

    def tick(self) -> None:
        """30 times a second: position, meters, spectrum (ffaudio's getters never wait)."""
        eng = self.app.engine
        st = ffaudio.status()
        live = bool(eng.current and eng.current.info and eng.current.info.live)
        buf = eng.buffering.buffered_seconds if eng.buffering else -1
        self.query_one(SeekBar).show(st.position if st.handle else -1, st.duration, live, buf)
        try:
            lv = ffaudio.levels(0.05)
            self.query_one(LevelMeter).show(lv.peak, lv.rms)
        except ffaudio.Error:
            pass
        sv = self.query_one(SpectrumView)
        try:
            sv.show(self.app.spectrum.bands(sv.bands_wanted, 30, 16000))
        except ffaudio.Error:
            pass

    def refresh_status(self) -> None:
        eng, s = self.app.engine, self.app.engine.s
        st = ffaudio.status()
        accent = self.app.theme_variables.get("accent")
        parts: list[Text] = []
        if eng.buffering:
            parts.append(Text(f"… buffering {eng.buffering.buffered_seconds:.1f} s", Style(color="#d7d75f", bold=True)))
        elif st.handle:
            parts.append(Text("‖ Paused" if st.paused else "► Playing", Style(color=accent, bold=True)))
        else:
            parts.append(Text("■ Stopped", Style(dim=True)))
        parts.append(Text.assemble(("vol ", Style(dim=True)), ("muted" if s.muted else f"{round(s.volume * 100)}%",
                                   Style(color="#ff8787" if s.volume > 1 or s.muted else None, bold=True))))
        if s.speed != 1:
            parts.append(Text.assemble(("speed ", Style(dim=True)),
                                       (f"×{s.speed:.2f}{'' if s.keep_pitch else ' tape'}", Style(bold=True))))
        parts.append(Text(f"crossfade {s.crossfade:g} s" if s.crossfade > 0 else "gapless", Style(dim=True)))
        if s.repeat != "off":
            parts.append(Text(REPEAT_ICONS[s.repeat], Style(color=accent)))
        if s.shuffle:
            parts.append(Text("⇄ shuffle", Style(color=accent)))
        parts.append(Text(eng.effects.describe(), Style(dim=True)))
        t = Text()
        for i, part in enumerate(parts):
            if i:
                t.append(" · ", Style(color="#555555"))
            t.append_text(part)
        self.query_one("#status", Static).update(t)
        self._queue_subtitle()

    def refresh_topbar(self) -> None:
        eng, s = self.app.engine, self.app.engine.s
        accent = self.app.theme_variables.get("accent")
        t = Text()
        t.append(" ffaudio ", Style(bold=True, color="black", bgcolor=accent))
        t.append(" player ", Style(bold=True))
        right = Text()
        d = eng.device
        if d is None:
            right.append("no device yet", Style(dim=True))
        elif not d.ok:
            right.append("⚠ no output device", Style(color="#ff5f5f", bold=True))
        else:
            right.append(f"♪ {d.name or 'system default'}", Style())
            right.append(f" · {d.sample_rate / 1000:g} kHz · {d.channels} ch", Style(dim=True))
        if s.replaygain != "off":
            right.append(f"   ReplayGain {s.replaygain} · {s.loudness_target:g} LUFS", Style(dim=True))
        self.query_one("#topbar-left", Static).update(t)
        self.query_one("#topbar-right", Static).update(right)

    def on_resize(self) -> None:
        self.call_after_refresh(self.layout_columns)

    #+-----------------------------------------------------+
    #| Actions                                             |
    #+-----------------------------------------------------+

    def action_play_pause(self) -> None:
        self.app.engine.play_pause()

    def action_next(self) -> None:
        self.app.engine.next()

    def action_previous(self) -> None:
        self.app.engine.previous()

    def action_seek(self, seconds: float) -> None:
        self.app.engine.seek_by(seconds)

    def action_seek_fraction(self, fraction: float) -> None:
        self.app.engine.seek_fraction(fraction)

    def action_volume(self, delta: float) -> None:
        eng = self.app.engine
        eng.set_volume(eng.s.volume + delta)
        self.app.notify(f"Volume {round(eng.s.volume * 100)}%" + (" (can clip above 100%)" if eng.s.volume > 1
                                                                   else ""), timeout=1)

    def action_mute(self) -> None:
        self.app.engine.toggle_mute()

    def action_speed(self, delta: float) -> None:
        eng = self.app.engine
        eng.set_speed(eng.s.speed + delta)

    def action_pitch(self) -> None:
        eng = self.app.engine
        eng.set_speed(eng.s.speed, not eng.s.keep_pitch)
        self.app.notify("Speed keeps the pitch" if eng.s.keep_pitch else "Tape-style speed (pitch follows)",
                        timeout=2)

    def action_repeat(self) -> None:
        self.app.engine.cycle_repeat()
        self.app.notify(f"Repeat {self.app.engine.s.repeat}", timeout=1.5)

    def action_shuffle(self) -> None:
        self.app.engine.toggle_shuffle()
        self.app.notify("Shuffle on" if self.app.engine.s.shuffle else "Shuffle off", timeout=1.5)

    def action_toggle_browser(self) -> None:
        pane = self.query_one("#browser-pane")
        pane.display = not pane.display
        self.call_after_refresh(self.layout_columns)

    def action_filter(self) -> None:
        f = self.query_one("#filter", Input)
        f.display = True
        f.focus()

    def action_clear_filter(self) -> None:
        f = self.query_one("#filter", Input)
        if f.display:
            f.value = ""
            f.display = False
            self.filter_text = ""
            self.rebuild_queue()
            self.query_one(QueueTable).focus()

    @on(Input.Changed, "#filter")
    def _filter_changed(self, event: Input.Changed) -> None:
        self.filter_text = event.value
        self.rebuild_queue()

    @on(Input.Submitted, "#filter")
    def _filter_done(self, event: Input.Submitted) -> None:
        self.query_one(QueueTable).focus()

    @on(SeekBar.Seek)
    def _seek(self, event: SeekBar.Seek) -> None:
        self.app.engine.seek_fraction(event.fraction)


def _music_dir() -> str:
    for p in (os.path.expanduser("~/Music"), os.path.expanduser("~/music"), os.path.expanduser("~")):
        if os.path.isdir(p):
            return p
    return os.getcwd()
