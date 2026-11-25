#!/bin/bash
# Run every suite under chaos delays (docs/threading.md, "Chaos testing"): chaos_campaign.sh <root> <out-dir>
# Needs the chaos and chaos-tsan trees of build_all.sh. Writes one log per run and summary.txt to <out-dir>.
# Read the results with the rules in docs/threading.md: TSan reports and wrong audio are always real; underruns and
# timing windows at medium and heavy intensities are expected.
set -uo pipefail
root=${1:?usage: chaos_campaign.sh <root> <out-dir>}
out=${2:?usage: chaos_campaign.sh <root> <out-dir>}
real=${FFAUDIO_SDL2_LIB:-$HOME/.local/opt/sdl2-2.30/lib}
mkdir -p "$out"
: > "$out/summary.txt"

run() { # tree config label
    (cd "$root/$1" && FFAUDIO_CHAOS=$2 LD_LIBRARY_PATH=$real timeout 3600 ctest -j3 --output-on-failure \
        > "$out/$3.log" 2>&1)
    echo "$3 ($1, $2): $(grep 'tests passed' "$out/$3.log" || echo 'no result'), \
$(grep -c 'WARNING: ThreadSanitizer' "$out/$3.log") TSan reports" | tee -a "$out/summary.txt"
}

for seed in 1 2 3; do run chaos "$seed:10:2000" "light-$seed"; done
for seed in 4 5 6; do run chaos "$seed:30:5000" "medium-$seed"; done
for seed in 7 8; do run chaos "$seed:50:20000" "heavy-$seed"; done
for seed in 11 12 13; do run chaos-tsan "$seed:20:3000" "tsan-$seed"; done
echo DONE >> "$out/summary.txt"
