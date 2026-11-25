"""
Tests of the player (CTest runs them as player when Textual is installed):

    FFAUDIO_LIBRARY=<libffaudio>  FFAUDIO_FIXTURES=<fixture dir>  SDL_AUDIODRIVER=dummy
    python3 -m unittest discover -s player/tests

The engine tests play real files through ffaudio (with SDL's dummy driver), without the UI. The UI tests drive the
app with Textual's pilot.
"""
import asyncio
import os
import queue
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [os.path.dirname(HERE), os.path.join(HERE, "..", "..", "bindings", "python")]

import ffaudio  # noqa: E402
from ffaudio_player import settings as settings_module  # noqa: E402
from ffaudio_player.engine import Engine  # noqa: E402
from ffaudio_player.library import Item, expand  # noqa: E402

FIXTURES = os.environ.get("FFAUDIO_FIXTURES", "")
TMP = tempfile.mkdtemp(prefix="ffaudio-player-test-")
settings_module.FILE_OVERRIDE = os.path.join(TMP, "settings.json")


def fx(name):
    return os.path.join(FIXTURES, name)


def fresh_settings(**kw):
    s = settings_module.Settings()
    s.replaygain = "off"
    s.prepare_next = 2.0
    for k, v in kw.items():
        setattr(s, k, v)
    return s


@unittest.skipUnless(FIXTURES and os.path.isdir(FIXTURES), "FFAUDIO_FIXTURES not set")
class EngineTests(unittest.TestCase):
    """The engine on its own: what the UI would see, through its listener."""

    def setUp(self):
        ffaudio.gain_cache_set_file("")
        self.calls = queue.Queue()
        self.events = []
        self.engine = Engine(fresh_settings(), self.calls.put)
        self.engine.on(lambda kind, data: self.events.append((kind, data)))
        self.engine.start()

    def tearDown(self):
        self.engine.close()

    def run_until(self, done, timeout=15.0):
        """Runs what the engine's threads posted, as the UI loop would, until done() is true."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if done():
                return
            try:
                self.calls.get(timeout=0.05)()
            except queue.Empty:
                pass
        self.fail(f"timed out; events: {[k for k, _ in self.events][-30:]}")

    def started(self):
        return [d for k, d in self.events if k == "current" and d is not None]

    def test_plays_the_queue_in_order_gaplessly(self):
        items = [Item(fx("join_a.flac")), Item(fx("join_b.flac")), Item(fx("s44_stereo.flac"))]
        self.engine.add(items, play=True)
        self.run_until(lambda: len(self.started()) == 3 and not self.engine.handles)
        self.assertEqual(self.started(), items)
        self.assertTrue(all(it.info is not None for it in items))   # probed

    def test_a_broken_file_is_skipped_and_reported(self):
        items = [Item(fx("join_a.flac")), Item(fx("does_not_exist.flac")), Item(fx("join_b.flac"))]
        self.engine.add(items, play=True)
        self.run_until(lambda: items[2] in self.started() and not self.engine.handles)
        # Marked (by the probe, or by its failed playback) and gone past; playing it on purpose tries again
        self.assertIn("does_not_exist.flac", items[1].error)
        self.engine.play_item(items[1])
        self.run_until(lambda: any(k == "error" and "does_not_exist" in d for k, d in self.events))

    def test_changing_the_queue_changes_what_comes_next(self):
        a, b, c = Item(fx("long_6s.flac")), Item(fx("join_a.flac")), Item(fx("join_b.flac"))
        self.engine.add([a, b, c], play=True)
        self.run_until(lambda: self.engine.next_handle != 0)   # b is queued in ffaudio
        self.assertIs(self.engine.handles[self.engine.next_handle], b)
        self.engine.remove([b])
        self.assertIs(self.engine.handles[self.engine.next_handle], c)
        self.engine.next()                                     # skip: c takes over
        self.run_until(lambda: self.engine.current is c)

    def test_shuffle_shows_a_shuffled_queue_and_restores_the_truth(self):
        items = [Item(fx(f)) for f in ("join_a.flac", "join_b.flac", "s44_stereo.flac", "s48_stereo.mp3",
                                         "s44_mono.wav", "s48_stereo.ogg")]
        self.engine.add(items)
        self.engine.play_item(items[2])
        self.run_until(lambda: self.engine.current is items[2])
        self.engine.toggle_shuffle()
        shown = list(self.engine.items)
        self.assertIs(shown[0], items[2])                      # the current track first
        self.assertEqual(sorted(it.uid for it in shown), sorted(it.uid for it in items))
        self.assertEqual(self.engine.truth, items)             # the order the user made, kept
        extra = Item(fx("cd_6s.flac"))
        self.engine.add([extra])                               # among the tracks still to come
        self.assertIn(extra, self.engine.items[1:])
        self.assertIs(self.engine.truth[-1], extra)
        self.engine.move(self.engine.items[-1], -1)            # rearranges the shuffle, not the truth
        self.assertEqual(self.engine.truth, items + [extra])
        # It plays the queue as shown, top to bottom
        expected = self.engine.items[1:3]
        self.run_until(lambda: len(self.started()) >= 3, timeout=20)
        self.assertEqual(self.started()[1:3], expected)
        # Saved and restored as shown; off restores the truth with the current track in place
        self.engine.save_session()
        again = Engine(settings_module.Settings.load(), self.calls.put)
        self.assertEqual([it.path for it in again.items], [it.path for it in self.engine.items])
        self.engine.toggle_shuffle()
        self.assertEqual(self.engine.items, items + [extra])
        self.assertEqual(self.engine.cursor, self.engine.items.index(self.engine.current))

    def test_repeat_track_and_queue(self):
        a, b = Item(fx("join_a.flac")), Item(fx("join_b.flac"))
        self.engine.set_repeat("one")
        self.engine.add([a, b], play=True)
        self.run_until(lambda: any(k == "state" for k, _ in self.events) and ffaudio.status().handle != 0)
        time.sleep(2.5)                                        # a 1 s track: it loops, b does not come
        for _ in range(20):
            try:
                self.calls.get_nowait()()
            except queue.Empty:
                break
        self.assertEqual(self.started(), [a])
        self.assertEqual(ffaudio.status().loop_count, -1)
        self.engine.set_repeat("all")                          # then a, b, and a again
        self.run_until(lambda: len(self.started()) >= 3, timeout=20)
        self.assertEqual(self.started()[:3], [a, b, a])

    def test_previous_and_seek(self):
        a, b = Item(fx("long_6s.flac")), Item(fx("cd_6s.flac"))
        self.engine.add([a, b])
        self.engine.play_item(b)
        self.run_until(lambda: self.engine.current is b)
        self.engine.seek_by(4)
        self.run_until(lambda: ffaudio.status().position > 3.5)
        self.engine.previous()   # past 3 s: back to the start
        self.run_until(lambda: ffaudio.status().position < 1.0)
        self.engine.previous()   # at the start: the one before
        self.run_until(lambda: self.engine.current is a)

    def test_equalizer_band_changes_live(self):
        e = self.engine.effects
        e.s.eq_on, e.s.limiter = True, "off"
        self.engine.apply_effects()
        self.engine.add([Item(fx("long_6s.flac"))], play=True)
        self.run_until(lambda: self.engine.current is not None)
        self.assertTrue(e.set_band(3, 4.0))   # a filter command, nothing rebuilt
        self.assertNotIn("g=4", e.last_global)
        self.engine.apply_effects()           # then the string, for the tracks to come
        self.assertIn("g=4", e.last_global)
        e.s.limiter, e.s.eq_gains = "auto", [0.0] * 10
        self.engine.apply_effects()
        self.assertFalse(e.set_band(4, 2.0))  # the first boost brings the limiter in: a new graph
        self.assertIn("alimiter", e.last_global)

    def test_session_is_saved(self):
        self.engine.add([Item(fx("join_a.flac")), Item(fx("cue_tag.flac"), start=1.0, length=2.0, title="T")])
        self.engine.save_session()
        s = settings_module.Settings.load()
        self.assertEqual(len(s.queue), 2)
        self.assertEqual(Item.from_json(s.queue[1]).start, 1.0)


@unittest.skipUnless(FIXTURES and os.path.isdir(FIXTURES), "FFAUDIO_FIXTURES not set")
class LibraryTests(unittest.TestCase):
    def test_cue_sheet_and_folder(self):
        tracks = expand(fx("cue_tag.flac"))
        self.assertGreater(len(tracks), 1)
        self.assertTrue(all(t.is_range for t in tracks))
        self.assertGreater(len(expand(FIXTURES)), 10)

    def test_playlist_text(self):
        p = os.path.join(TMP, "list.m3u8")
        with open(p, "w") as f:
            f.write(f"#EXTM3U\n#EXTINF:1,Band - Song\n{fx('join_a.flac')}\n")
        items = expand(p)
        self.assertEqual(items[0].title, "Band - Song")


try:
    import textual  # noqa: F401
    from ffaudio_player.ui.app import PlayerApp
    from ffaudio_player.ui.main import MainScreen
    from textual.widgets import Input
except ImportError:
    PlayerApp = None


@unittest.skipUnless(PlayerApp and FIXTURES and os.path.isdir(FIXTURES), "Textual or FFAUDIO_FIXTURES missing")
class UITests(unittest.TestCase):
    def setUp(self):
        ffaudio.gain_cache_set_file("")

    def run_app(self, script, paths):
        s = fresh_settings(browser_root=FIXTURES)
        app = PlayerApp(s, paths, play=True)

        async def go():
            async with app.run_test(size=(140, 44)) as pilot:
                await script(app, pilot)

        asyncio.run(go())
        return app

    async def wait(self, pilot, cond, timeout=10.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if cond():
                return
            await pilot.pause(0.1)
        self.fail("timed out")

    def test_play_skip_filter_effects_quit(self):
        async def script(app, pilot):
            eng = app.engine
            await self.wait(pilot, lambda: len(eng.items) == 3 and eng.current is not None)
            self.assertIsInstance(app.screen, MainScreen)
            first = eng.current
            await pilot.press("n")
            await self.wait(pilot, lambda: eng.current is not None and eng.current is not first)

            # The filter: typing a space goes into it, it does not pause
            await pilot.press("slash")
            self.assertIsInstance(app.focused, Input)
            await pilot.press("l", "o", "n", "g")
            await self.wait(pilot, lambda: len(app.main.row_items) == 1)
            paused_before = ffaudio.status().paused
            await pilot.press("space")
            self.assertEqual(app.focused.value, "long ")
            self.assertEqual(ffaudio.status().paused, paused_before)
            await pilot.press("escape")
            self.assertEqual(len(app.main.row_items), 3)

            # Effects: a preset turns the equalizer on, and the filters follow
            await pilot.press("e")
            await pilot.pause(0.3)
            from textual.widgets import Select
            app.screen.query_one("#eq_preset", Select).value = "bass"
            await pilot.pause(0.3)
            self.assertTrue(eng.effects.s.eq_on)
            self.assertIn("equalizer@eq_0", eng.effects.last_global)
            self.assertIn("alimiter", eng.effects.last_global)   # auto: the preset boosts
            await pilot.press("escape")

            await pilot.press("plus")
            self.assertAlmostEqual(eng.s.volume, 1.05)

            # Files: f shows the file in the queue without playing it; g plays it there (no second copy)
            from ffaudio_player.ui.main import Browser, QueueTable
            browser = app.main.query_one(Browser)
            browser.focus()
            target = next(it for it in eng.items if it.path.endswith("long_6s.flac"))
            await self.wait(pilot, lambda: browser.cursor_node is not None)
            node = next(n for n in browser.root.children if n.data and str(n.data.path).endswith("long_6s.flac"))
            browser.move_cursor(node)
            playing = eng.current
            await pilot.press("f")
            await self.wait(pilot, lambda: app.main.selected_item() is target)
            self.assertIs(eng.current, playing)
            browser.focus()
            await pilot.press("g")
            await self.wait(pilot, lambda: eng.current is target)
            self.assertEqual(len(eng.items), 3)

            # The queue's find bar moves the cursor, plays nothing, hides nothing
            app.main.query_one(QueueTable).focus()
            await pilot.press("f")
            self.assertIsInstance(app.focused, Input)
            await pilot.press("j", "o", "i", "n")
            await self.wait(pilot, lambda: app.main.selected_item() is not None and
                            "join_a" in app.main.selected_item().path)
            self.assertEqual(len(app.main.row_items), 3)
            self.assertIs(eng.current, target)
            await pilot.press("escape")
            await pilot.press("q")

        app = self.run_app(script, [fx("long_6s.flac"), fx("cd_6s.flac"), fx("join_a.flac")])
        saved = settings_module.Settings.load()
        self.assertEqual(len(saved.queue), 3)
        self.assertTrue(saved.effects.eq_on)


if __name__ == "__main__":
    unittest.main()
