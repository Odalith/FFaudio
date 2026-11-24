#!/bin/bash
# make_cue_album.sh <album-dir> <out-dir> [sample-rate]
#
# Makes a test album for cue sheet playback: album.flac, the album's tracks joined into one 24-bit FLAC "image", and
# album.cue with where each track starts. Tracks at another sample rate than the image's (the most common rate of the
# album, or the one given) are resampled with soxr; channel counts must match. The album's own files are only read.
set -euo pipefail
[ $# -ge 2 ] || { echo "usage: $0 <album-dir> <out-dir> [sample-rate]" >&2; exit 2; }
src=$1 out=$2 rate=${3:-}

mapfile -t files < <(find "$src" -maxdepth 1 -type f \( -iname '*.flac' -o -iname '*.wav' -o -iname '*.m4a' \
                       -o -iname '*.mp3' -o -iname '*.ogg' -o -iname '*.opus' -o -iname '*.wv' -o -iname '*.ape' \) | sort)
[ ${#files[@]} -gt 0 ] || { echo "no audio files in $src" >&2; exit 1; }

probe() { ffprobe -v error -select_streams a:0 -show_entries "stream=$2" -of csv=p=0 "$1"; }
tag() { ffprobe -v error -show_entries "format_tags=$2" -of csv=p=0 "$1"; }
cue_text() { printf '%s' "$1" | tr '"' "'"; }   # cue values are quoted and can not hold a "

[ -n "$rate" ] || rate=$(for f in "${files[@]}"; do probe "$f" sample_rate; done | sort | uniq -c | sort -rn | awk 'NR==1 {print $2}')
channels=$(probe "${files[0]}" channels)

mkdir -p "$out"
work=$(mktemp -d "$out/.work.XXXXXX")
trap 'rm -rf "$work"' EXIT

album=$(tag "${files[0]}" album); [ -n "$album" ] || album=$(basename "$src")
artist=$(tag "${files[0]}" album_artist); [ -n "$artist" ] || artist=$(tag "${files[0]}" artist)
cue="$out/album.cue"
{
    printf 'REM COMMENT "made by make_cue_album.sh from %s"\n' "$(cue_text "$(basename "$src")")"
    [ -n "$artist" ] && printf 'PERFORMER "%s"\n' "$(cue_text "$artist")"
    printf 'TITLE "%s"\n' "$(cue_text "$album")"
    printf 'FILE "album.flac" WAVE\n'
} > "$cue"

total=0
n=0
: > "$work/list.txt"
for f in "${files[@]}"; do
    n=$((n + 1))
    [ "$(probe "$f" channels)" = "$channels" ] || { echo "$f: not $channels channels" >&2; exit 1; }
    part="$work/$(printf '%03d' $n).wav"
    if [ "$(probe "$f" sample_rate)" = "$rate" ]; then
        ffmpeg -v error -nostdin -i "$f" -map 0:a:0 -c:a pcm_f32le "$part"
    else
        ffmpeg -v error -nostdin -i "$f" -map 0:a:0 -af "aresample=resampler=soxr:precision=28:osr=$rate" \
               -c:a pcm_f32le "$part"
    fi
    samples=$(probe "$part" duration_ts)
    frames=$(( (total * 75 + rate / 2) / rate ))          # the start, in CD frames (1/75 s), rounded
    title=$(tag "$f" title); [ -n "$title" ] || title=$(basename "${f%.*}")
    performer=$(tag "$f" artist)
    {
        printf '  TRACK %02d AUDIO\n' $n
        printf '    TITLE "%s"\n' "$(cue_text "$title")"
        [ -n "$performer" ] && [ "$performer" != "$artist" ] && printf '    PERFORMER "%s"\n' "$(cue_text "$performer")"
        printf '    INDEX 01 %02d:%02d:%02d\n' $((frames / 4500)) $((frames / 75 % 60)) $((frames % 75))
    } >> "$cue"
    printf "file '%s'\n" "$part" >> "$work/list.txt"
    printf '%2d. %-45s %6s Hz -> %d samples, starts at %d:%02d:%02d\n' $n "$title" "$(probe "$f" sample_rate)" \
           "$samples" $((frames / 4500)) $((frames / 75 % 60)) $((frames % 75))
    total=$((total + samples))
done

ffmpeg -v error -nostdin -f concat -safe 0 -i "$work/list.txt" -c:a flac -sample_fmt s32 -bits_per_raw_sample 24 \
       -metadata album="$album" -metadata artist="$artist" -y "$out/album.flac"
echo "$out/album.flac: $total samples at $rate Hz ($(( total / rate / 60 )):$(printf '%02d' $(( total / rate % 60 )))), $n tracks"
echo "$out/album.cue"
