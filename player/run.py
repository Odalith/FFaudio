#!/usr/bin/env python3
"""Starts the player from wherever it is (the source tree or a release archive): python3 player/run.py [paths...]"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ffaudio_player.__main__ import main  # noqa: E402

sys.exit(main())
