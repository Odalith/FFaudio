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

# Two whole-cycle 440 Hz tracks: played gaplessly they form one continuous sine
sine join_a.flac       440 1 48000 2 -sample_fmt s16
sine join_b.flac       440 1 48000 2 -sample_fmt s16
sine join_c_44k.flac   440 1 44100 2 -sample_fmt s16

# 16-bit 5.1 with a 440 Hz sine (-18.06 dBFS) in a single channel, for downmix level and routing tests
for c in FL FC LFE SL; do
    ff -f lavfi -i "sine=frequency=440:duration=1:sample_rate=48000" \
       -af "pan=5.1(side)|${c}=c0" -sample_fmt s16 "surround_${c}.flac"
done

# Tagged files for metadata / ReplayGain tests. The sine is at -18.06 dBFS in both channels.
TAGS=(-metadata title="Test Song" -metadata artist="Test Artist" -metadata album="Test Album"
      -metadata album_artist="Various" -metadata track=3/12 -metadata disc=1/2 -metadata date=2024
      -metadata genre=Rock -metadata REPLAYGAIN_TRACK_GAIN="-6.00 dB" -metadata REPLAYGAIN_TRACK_PEAK=0.125
      -metadata REPLAYGAIN_ALBUM_GAIN="-3.00 dB" -metadata REPLAYGAIN_ALBUM_PEAK=0.2)
rg_sine() { # name seconds [ffmpeg output args...]
    local name=$1 secs=$2; shift 2
    ff -f lavfi -i "sine=frequency=440:duration=${secs}:sample_rate=48000" -af "pan=stereo|c0=c0|c1=c0" "$@" "$name"
}
rg_sine rg_tagged.flac 1 -sample_fmt s16 "${TAGS[@]}"
rg_sine rg_tagged.mp3  1 "${TAGS[@]}"
rg_sine rg_long.flac   3 -sample_fmt s16 "${TAGS[@]}"
# Opus R128 gain -2816 = -11 dB re -23 LUFS = -6 dB re ReplayGain's -18 LUFS
rg_sine rg_r128.opus   1 -c:a libopus -b:a 192k -metadata title="Opus Song" -metadata R128_TRACK_GAIN=-2816
# +12 dB with a peak of 0.5: clipping prevention limits the gain to +6.02 dB
rg_sine rg_hot.flac    1 -sample_fmt s16 -metadata REPLAYGAIN_TRACK_GAIN="+12.00 dB" -metadata REPLAYGAIN_TRACK_PEAK=0.5
rg_sine rg_untagged.flac 1 -sample_fmt s16

# Loudness reference signals (EBU Tech 3341 / 3342 style): 1 kHz sines in both channels, amplitude in dBFS
ld_sine() { # name amplitude seconds rate
    ff -f lavfi -i "aevalsrc=exprs='$2*sin(2*PI*1000*t)|$2*sin(2*PI*1000*t)':s=$4:d=$3" -sample_fmt s16 "$1"
}
ld_sine ld_m23_48k.flac 0.0707946 10 48000   # -23 dBFS -> -23.0 LUFS
ld_sine ld_m33_48k.flac 0.0223872 10 48000   # -33 dBFS -> -33.0 LUFS
ld_sine ld_m23_44k.flac 0.0707946 10 44100
# -36 / -23 / -36 dBFS for 5 / 20 / 5 s: the relative gate removes the quiet parts -> -23.0
ff -f lavfi -i "aevalsrc=exprs='if(between(t,5,25),0.0707946,0.0158489)*sin(2*PI*1000*t)|if(between(t,5,25),0.0707946,0.0158489)*sin(2*PI*1000*t)':s=48000:d=30" -sample_fmt s16 ld_gate.flac
# 10 s at -20 dBFS then 10 s at -30 dBFS -> loudness range 10 LU
ff -f lavfi -i "aevalsrc=exprs='if(lt(t,10),0.1,0.0316228)*sin(2*PI*1000*t)|if(lt(t,10),0.1,0.0316228)*sin(2*PI*1000*t)':s=48000:d=20" -sample_fmt s16 ld_lra10.flac
# 12 kHz at 48 kHz, 45 degree phase: samples only reach 0.707 of the waveform's peak
ff -f lavfi -i "aevalsrc=exprs='0.5*sin(2*PI*12000*t+PI/4)|0.5*sin(2*PI*12000*t+PI/4)':s=48000:d=3" -c:a pcm_f32le ld_truepeak.wav
# 5.1 with the -23 dBFS sine only in front left, and only in side left (surround channels weigh +1.5 dB)
ff -f lavfi -i "aevalsrc=exprs='0.0707946*sin(2*PI*1000*t)':s=48000:d=10" -af "pan=5.1(side)|FL=c0" -sample_fmt s16 ld_fl51.flac
ff -f lavfi -i "aevalsrc=exprs='0.0707946*sin(2*PI*1000*t)':s=48000:d=10" -af "pan=5.1(side)|SL=c0" -sample_fmt s16 ld_sl51.flac
ff -f lavfi -i "anullsrc=r=48000:cl=stereo" -t 3 ld_silence.flac
# Many distinct paths for the measurement queue tests (hard links: no extra space)
for i in $(seq -w 1 24); do ln -f ld_m23_48k.flac "scan_$i.flac"; done

# Broken / non-audio inputs
: > empty.flac
echo "this is not audio" > text.mp3
head -c 4000 s44_stereo.flac > truncated.flac
ff -f lavfi -i "testsrc=duration=1:size=64x64:rate=5" -an video_only.mkv

touch .stamp
echo "fixtures written to $out"
