"""python -m ffaudio_player [paths or URLs...]   (or player/run.py, which finds everything by itself)"""
from __future__ import annotations

import argparse
import os
import sys


def find_bindings() -> None:
    """The ffaudio module: installed, or next to the player (the source tree: bindings/python; a release
    archive: python/)."""
    try:
        import ffaudio  # noqa: F401
        return
    except ImportError as e:
        if "libffaudio" in str(e):
            raise
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    for rel in ("../bindings/python", "../python"):
        p = os.path.normpath(os.path.join(here, rel))
        if os.path.isdir(os.path.join(p, "ffaudio")):
            sys.path.insert(0, p)
            return


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="ffaudio-player", description="A music player for the terminal, on ffaudio.")
    ap.add_argument("paths", nargs="*", help="files, folders, playlists, cue sheets or URLs to add to the queue")
    ap.add_argument("--play", action="store_true", help="start playing what was given")
    ap.add_argument("--library", metavar="PATH", help="libffaudio to use (else FFAUDIO_LIBRARY, or next to it)")
    ap.add_argument("--settings", metavar="FILE", help="another settings file (default: the per-user one)")
    ap.add_argument("--fresh", action="store_true", help="start with an empty queue")
    args = ap.parse_args(argv)
    if args.library:
        os.environ["FFAUDIO_LIBRARY"] = os.path.abspath(args.library)
    find_bindings()
    try:
        import ffaudio  # noqa: F401
    except ImportError as e:
        print(f"ffaudio player: {e}", file=sys.stderr)
        return 1
    try:
        from textual import __version__  # noqa: F401
    except ImportError:
        print("ffaudio player needs Textual: pip install -r requirements.txt", file=sys.stderr)
        return 1

    from . import settings as settings_module
    from .ui.app import PlayerApp

    if args.settings:
        settings_module.FILE_OVERRIDE = os.path.abspath(args.settings)
    settings = settings_module.Settings.load()
    if args.fresh:
        settings.queue, settings.current = [], -1
    paths = [p if "://" in p else os.path.abspath(os.path.expanduser(p)) for p in args.paths]
    app = PlayerApp(settings, paths, play=args.play or bool(paths))
    result = app.run()
    return 0 if result is None else 1


if __name__ == "__main__":
    sys.exit(main())
