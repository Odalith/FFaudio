#!/bin/bash
# Run every suite of the release, ASan and TSan trees made by build_all.sh against real SDL2 (FFAUDIO_SDL2_LIB,
# default ~/.local/opt/sdl2-2.30/lib), then the release tree once against the system's SDL (e.g. SDL3's sdl2-compat).
#   run_all.sh <root> [tree...]
# Prints the ctest totals and the number of sanitizer reports per run; the logs stay in <root>/<tree>/*.log.
# Exit status: 0 when everything passed without a sanitizer report.
set -uo pipefail
root=${1:?usage: run_all.sh <root> [tree...]}
shift
trees=("$@")
[ ${#trees[@]} -eq 0 ] && trees=(release asan tsan)
real=${FFAUDIO_SDL2_LIB:-$HOME/.local/opt/sdl2-2.30/lib}
jobs=${FFAUDIO_TEST_JOBS:-3}
status=0

summary() { # log label
    local reports
    reports=$(grep -c "WARNING: ThreadSanitizer\|ERROR: AddressSanitizer\|ERROR: LeakSanitizer\|runtime error:" "$1")
    echo "$2: $(grep 'tests passed' "$1" || echo 'no result'), $reports sanitizer reports"
    grep -E "\*\*\*|Failed|Timeout" "$1" | grep -v "^ *0 " || true
    if ! grep -q "100% tests passed" "$1" || [ "$reports" != 0 ]; then return 1; fi
}

if [ -d "$real" ]; then
    for t in "${trees[@]}"; do
        [ -d "$root/$t" ] || { echo "$t: no tree at $root/$t" >&2; status=1; continue; }
        (cd "$root/$t" && LD_LIBRARY_PATH=$real ctest -j"$jobs" --output-on-failure > real_sdl2.log 2>&1)
        summary "$root/$t/real_sdl2.log" "$t, real SDL2" || status=1
    done
else
    echo "no real SDL2 at $real (set FFAUDIO_SDL2_LIB); skipping those runs" >&2
    status=1
fi
if [ -d "$root/release" ]; then
    (cd "$root/release" && ctest -j"$jobs" --output-on-failure > system_sdl.log 2>&1)
    summary "$root/release/system_sdl.log" "release, system SDL" || status=1
fi
exit $status
