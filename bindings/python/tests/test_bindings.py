"""
Tests of the Python bindings (CTest runs them as python_bindings):

    FFAUDIO_LIBRARY=<libffaudio>  FFAUDIO_LAYOUT=<the built tests/layout.c>  FFAUDIO_FIXTURES=<fixture dir>
    SDL_AUDIODRIVER=dummy python3 -m unittest discover -s bindings/python/tests

The layout test is the important one: ctypes takes the struct declarations on trust, and a field out of place
reads garbage without any error.
"""
import ctypes
import math
import os
import subprocess
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import ffaudio  # noqa: E402
from ffaudio import _native as n  # noqa: E402

FIXTURES = os.environ.get("FFAUDIO_FIXTURES", "")


def fx(name):
    return os.path.join(FIXTURES, name)


def needs_fixtures(test):
    return unittest.skipUnless(FIXTURES and os.path.isdir(FIXTURES), "FFAUDIO_FIXTURES not set")(test)


class Layout(unittest.TestCase):
    @unittest.skipUnless(os.environ.get("FFAUDIO_LAYOUT"), "FFAUDIO_LAYOUT not set")
    def test_every_struct_as_c_lays_it_out(self):
        out = subprocess.run([os.environ["FFAUDIO_LAYOUT"]], check=True, capture_output=True, text=True).stdout
        c = dict(line.rsplit(" ", 1) for line in out.splitlines())
        checked = 0
        for s in n.STRUCTS:
            self.assertEqual(int(c[s.__name__]), ctypes.sizeof(s), f"sizeof({s.__name__})")
            checked += 1

            def walk(cls, prefix, base):
                nonlocal checked
                for name, t in cls._fields_:
                    off = base + getattr(cls, name).offset
                    key = f"{s.__name__}.{prefix}{name}"
                    self.assertEqual(int(c[key]), off, f"offsetof({key})")
                    checked += 1
                    if isinstance(t, type) and issubclass(t, (ctypes.Structure, ctypes.Union)):
                        walk(t, prefix + name + ".", off)

            walk(s, "", 0)
        self.assertEqual(checked, len(c), "layout.c prints something the declarations lack: run make_layout.py")

    def test_event_has_its_abi_size(self):
        self.assertEqual(ctypes.sizeof(n.AuEvent), 72)


class WithoutPlayer(unittest.TestCase):
    def test_version_and_results(self):
        self.assertTrue(ffaudio.version().startswith("4."))
        self.assertEqual(ffaudio.result_string(ffaudio.Result.ERR_NOT_SAVED), "not saved to the cache file")
        with self.assertRaises(ffaudio.Error) as e:
            ffaudio.set_volume(0.5)   # no player
        self.assertEqual(e.exception.code, ffaudio.Result.ERR_NOT_INITIALIZED)

    def test_filter_builder(self):
        eq = ffaudio.equalizer_defaults()
        eq.gain_db[0] = 3
        fb = ffaudio.FilterBuilder().add_equalizer(eq, name="eq").add_limiter()
        self.assertIn("equalizer@eq_0", fb.string)
        self.assertIn("alimiter", fb.string)
        with self.assertRaises(ffaudio.Error) as e:
            fb.add_stereo(ffaudio.Stereo(width=9))
        self.assertIn("width", str(e.exception))
        self.assertTrue(ffaudio.validate_filters(fb.string))
        self.assertFalse(ffaudio.validate_filters("no_such_filter"))
        self.assertEqual(ffaudio.FilterBuilder().add_equalizer().string, "")   # None = defaults: flat

    def test_parametric_eq(self):
        peq = ffaudio.parametric_eq_parse("Preamp: -6.4 dB\nFilter 1: ON LSC Fc 105 Hz Gain 5.6 dB Q 0.70\n")
        self.assertAlmostEqual(peq.preamp_db, -6.4)
        self.assertEqual(peq.bands[0].type, ffaudio.PeqType.LOW_SHELF)
        with self.assertRaises(ffaudio.Error):
            ffaudio.parametric_eq_parse("Filter 1: ON XX Fc 1 Hz\n")

    def test_playlist_parse(self):
        entries = ffaudio.playlist_parse("#EXTM3U\n#EXTINF:215,Apashe - Majesty\nsongs/a.flac\n",
                                         "/music/lists/mix.m3u8")
        self.assertEqual(entries[0].path, "/music/lists/songs/a.flac")
        self.assertEqual(entries[0].title, "Apashe - Majesty")
        self.assertEqual(entries[0].duration, 215)

    @needs_fixtures
    def test_probe(self):
        m = ffaudio.probe(fx("rg_tagged.flac"))
        self.assertEqual(m.codec, "flac")
        self.assertEqual(m.title, "Test Song")
        self.assertAlmostEqual(m.replaygain.track_gain_db, -6.0)
        with self.assertRaises(ffaudio.Error) as e:
            ffaudio.probe(fx("does_not_exist.flac"))
        self.assertEqual(e.exception.code, ffaudio.Result.ERR_FILE)

    @needs_fixtures
    def test_cue_sheet(self):
        sheet = ffaudio.cue_read(fx("cue_tag.flac"))
        self.assertGreater(len(sheet.tracks), 1)
        t = sheet.tracks[1]
        self.assertGreater(t.start_seconds, 0)
        self.assertEqual(t.play_options().start_seconds, t.start_seconds)

    @needs_fixtures
    def test_loudness(self):
        l = ffaudio.loudness_analyze(fx("s44_stereo.flac"))
        self.assertFalse(l.silent)
        self.assertLess(l.integrated_lufs, 0)
        rg = ffaudio.loudness_to_replaygain(l)
        self.assertAlmostEqual(rg.track_gain_db, l.replaygain_gain_db, places=4)


@needs_fixtures
class Playback(unittest.TestCase):
    def setUp(self):
        ffaudio.gain_cache_set_file("")   # memory only
        ffaudio.init("ffaudio python tests")

    def tearDown(self):
        ffaudio.shutdown()

    def events_until(self, cls, handle=None, timeout=10.0):
        seen = []
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            ev = ffaudio.wait_event(0.1)
            if ev is None:
                continue
            seen.append(ev)
            if isinstance(ev, cls) and (handle is None or ev.handle == handle):
                return ev, seen
        self.fail(f"no {cls.__name__} within {timeout} s; got {[type(e).__name__ for e in seen]}")

    def test_play_to_the_end(self):
        h = ffaudio.play(fx("s44_stereo.flac"))
        dev, _ = self.events_until(ffaudio.Device)
        self.assertTrue(dev.ok and dev.sample_rate > 0)
        opened, _ = self.events_until(ffaudio.TrackOpened, h)
        self.assertGreater(opened.duration, 0)
        info = ffaudio.track_info(h)
        self.assertIsNotNone(info)
        self.events_until(ffaudio.TrackStarted, h)
        self.assertEqual(ffaudio.status().handle, h)
        ended, _ = self.events_until(ffaudio.TrackEnded, h)
        self.assertEqual(ended.reason, ffaudio.End.FINISHED)
        self.assertEqual(ended.error, 0)

    def test_failure_says_why(self):
        h = ffaudio.play(fx("does_not_exist.flac"))
        ended, _ = self.events_until(ffaudio.TrackEnded, h)
        self.assertEqual(ended.reason, ffaudio.End.ERROR)
        self.assertEqual(ended.error, ffaudio.Result.ERR_FILE)
        self.assertIn("does_not_exist.flac", ended.message)

    def test_options_settings_and_meters(self):
        ffaudio.set_volume(1.5)
        ffaudio.set_crossfade(ffaudio.Crossfade(on_skip=ffaudio.Fade(0.2)))
        ffaudio.set_replaygain(ffaudio.ReplayGainSettings(mode=ffaudio.ReplayGainMode.TRACK, preamp_db=4))
        with self.assertRaises(ffaudio.Error):
            ffaudio.set_volume(2.5)
        with self.assertRaises(ffaudio.Error):
            ffaudio.play(fx("long_6s.flac"), ffaudio.PlayOptions(filters="no_such_filter"))
        h = ffaudio.play(fx("long_6s.flac"), ffaudio.PlayOptions(start_seconds=1, length_seconds=2,
                                                                 filters="volume=0.5"))
        opened, _ = self.events_until(ffaudio.TrackOpened, h)
        self.assertAlmostEqual(opened.duration, 2.0, places=3)
        self.events_until(ffaudio.TrackStarted, h)
        ffaudio.levels(0.05)   # the first call turns the meters on
        time.sleep(0.4)
        st = ffaudio.status()
        self.assertEqual(st.volume, 1.5)
        lv = ffaudio.levels(0.05)
        self.assertEqual(lv.channels, st.channels)
        sp = ffaudio.Spectrum(1024)
        self.assertEqual(len(sp.read()), 513)
        self.assertEqual(len(sp.bands(16)), 16)
        ffaudio.stop()
        self.events_until(ffaudio.TrackEnded, h)


if __name__ == "__main__":
    unittest.main()
