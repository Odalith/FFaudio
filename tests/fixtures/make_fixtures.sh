#!/usr/bin/env bash
# Generates small deterministic audio fixtures for the ffaudio test suite.
# Usage: make_fixtures.sh <output-dir>
set -euo pipefail
out="${1:?output dir required}"
mkdir -p "$out"
cd "$out"

ff() { ffmpeg -hide_banner -loglevel error -nostdin -y "$@"; }
sine() { # name freq seconds rate channels [extra ffmpeg output args...]
    local name=$1 freq=$2 secs=$3 rate=$4 ch=$5; shift 5
    ff -f lavfi -i "sine=frequency=${freq}:duration=${secs}:sample_rate=${rate}" -ac "$ch" "$@" "$name"
}

# Short tracks used by most tests (1 s keeps the suite fast with real-time drivers)
sine s44_stereo.flac   440 1 44100 2 -sample_fmt s16
sine s48_stereo.mp3    660 1 48000 2
sine s96_stereo.flac   550 1 96000 2 -sample_fmt s32
sine s44_mono.wav      330 1 44100 1
sine s48_51.flac       770 1 48000 6
sine s44_flt.wav       880 1 44100 2 -c:a pcm_f32le
sine s48_stereo.ogg    990 1 48000 2 -c:a libvorbis
sine s44_stereo.m4a    220 1 44100 2 -c:a aac

# Longer track for seek / pause / crossfade tests
sine long_6s.flac      440 6 44100 2 -sample_fmt s16

# Broken / non-audio inputs
: > empty.flac
echo "this is not audio" > text.mp3
head -c 4000 s44_stereo.flac > truncated.flac
ff -f lavfi -i "testsrc=duration=1:size=64x64:rate=5" -an video_only.mkv

touch .stamp
echo "fixtures written to $out"
