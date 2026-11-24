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
# MPEG-TS starts its timestamps at 1.4 s: positions must still count from 0
sine start_offset.ts   440 6 44100 2 -c:a mp2 -f mpegts

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

# Network tests: an HLS segment (ADTS AAC); the playlists are written by test_network with the server's port
sine hls_seg.aac       440 2 44100 2 -c:a aac -f adts
# A constant 128 kbit/s (16000 bytes/s) MP3 without ID3/Xing headers: looped, it is a seamless live stream
sine live.mp3          440 5 44100 2 -c:a libmp3lame -b:a 128k -write_xing 0 -id3v2_version 0
# The same live stream with an ID3 title of its own, like most real MP3 files: ICY titles must win
sine live_id3.mp3      440 5 44100 2 -c:a libmp3lame -b:a 128k -write_xing 0 -metadata title="File Title"

# A chained Ogg stream (how Ogg/Opus radio changes songs): two Vorbis parts with their own titles
sine chain_a.ogg       440 2 44100 2 -c:a libvorbis -metadata title="First Part" -metadata artist="Ogg Artist"
sine chain_b.ogg       660 2 44100 2 -c:a libvorbis -metadata title="Second Part" -metadata artist="Ogg Artist"
cat chain_a.ogg chain_b.ogg > chained.ogg
# The same with Opus parts (granules at 48 kHz, minus each part's pre-skip): 2 s + 1 s
sine chain_c.opus      440 2 48000 2 -c:a libopus
sine chain_d.opus      660 1 48000 2 -c:a libopus
cat chain_c.opus chain_d.opus > chained.opus

# Restoration: a 440 Hz sine (1/8) with a 0.6 click every 8000 samples, and a 1/8 sine with a 20 ms burst at 0.3
# every 0.25 s, hard-clipped at 0.25 (like a loud master: a few percent of the samples clipped; adeclip's cost grows
# steeply with that share)
click="0.125*sin(2*PI*440*t)+if(eq(mod(n\,8000)\,4000)\,0.6\,0)"
clip="clip((0.125+0.175*lt(mod(t\,0.25)\,0.02))*sin(2*PI*440*t)\,-0.25\,0.25)"
ff -f lavfi -i "aevalsrc=${click}|${click}:s=44100:d=1" -c:a pcm_f32le clicks.wav
ff -f lavfi -i "aevalsrc=${clip}|${clip}:s=44100:d=1" -c:a pcm_f32le clipped.wav

# Embedded pictures: a 64x48 PNG front cover and a 40x30 JPEG back cover in FLAC (PICTURE blocks), the front cover in
# MP3 (ID3v2 APIC) and M4A (covr)
ff -f lavfi -i "color=c=red:s=64x48" -frames:v 1 cover.png
ff -f lavfi -i "color=c=blue:s=40x30" -frames:v 1 back.jpg
ff -f lavfi -i "sine=frequency=440:duration=1:sample_rate=44100" -i cover.png -i back.jpg -map 0:a -map 1 -map 2 \
   -c:a flac -c:v copy -disposition:v attached_pic -metadata:s:v:0 comment="Cover (front)" \
   -metadata:s:v:0 title="Front" -metadata:s:v:1 comment="Cover (back)" art.flac
ff -f lavfi -i "sine=frequency=440:duration=1:sample_rate=44100" -i cover.png -map 0:a -map 1 -c:a libmp3lame \
   -c:v copy -disposition:v attached_pic -id3v2_version 3 -metadata:s:v comment="Cover (front)" art.mp3
ff -f lavfi -i "sine=frequency=440:duration=1:sample_rate=44100" -i cover.png -map 0:a -map 1 -c:a aac \
   -c:v copy -disposition:v attached_pic art.m4a

# Broken / non-audio inputs
: > empty.flac
echo "this is not audio" > text.mp3
head -c 4000 s44_stereo.flac > truncated.flac
ff -f lavfi -i "testsrc=duration=1:size=64x64:rate=5" -an video_only.mkv
# Damaged raw JPEG frames, like an encrypted cache file that FFmpeg mistakes for a raw video stream: probing it
# opens the MJPEG decoder, which warns about the data
ff -f lavfi -i "testsrc=duration=1:size=160x120:rate=10" -c:v mjpeg -f mjpeg not_audio.bin
size=$(wc -c < not_audio.bin)
for off in $(seq 60 97 "$size"); do
    printf '\x13\x37\xc0\xde\x00\x00\x01\xb3' | dd of=not_audio.bin bs=1 seek="$off" conv=notrunc status=none
done

touch .stamp
echo "fixtures written to $out"
