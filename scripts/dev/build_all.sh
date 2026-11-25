#!/bin/bash
# Configure and build the development trees: build_all.sh <root> [tree...]
#   trees: release asan tsan chaos chaos-tsan (default: all)
# Every tree lives in <root>/<tree>. Run from anywhere; the source is this script's repository.
set -euo pipefail
src=$(cd "$(dirname "$0")/../.." && pwd)
root=${1:?usage: build_all.sh <root> [release|asan|tsan|chaos|chaos-tsan]...}
shift
trees=("$@")
[ ${#trees[@]} -eq 0 ] && trees=(release asan tsan chaos chaos-tsan)
mkdir -p "$root"
gen=()
command -v ninja >/dev/null && gen=(-G Ninja)

for t in "${trees[@]}"; do
    case $t in
        release)    opts=(-DCMAKE_BUILD_TYPE=RelWithDebInfo -DFFAUDIO_SANITIZER=none) ;;
        asan)       opts=(-DCMAKE_BUILD_TYPE=Debug -DFFAUDIO_SANITIZER=address) ;;
        tsan)       opts=(-DCMAKE_BUILD_TYPE=RelWithDebInfo -DFFAUDIO_SANITIZER=thread) ;;
        chaos)      opts=(-DCMAKE_BUILD_TYPE=RelWithDebInfo -DFFAUDIO_SANITIZER=none -DFFAUDIO_CHAOS=ON
                          -DFFAUDIO_TEST_TIMEOUT=1200) ;;
        chaos-tsan) opts=(-DCMAKE_BUILD_TYPE=RelWithDebInfo -DFFAUDIO_SANITIZER=thread -DFFAUDIO_CHAOS=ON
                          -DFFAUDIO_TEST_TIMEOUT=1200) ;;
        *) echo "unknown tree '$t'" >&2; exit 2 ;;
    esac
    echo "== $t"
    cmake -S "$src" -B "$root/$t" "${gen[@]}" "${opts[@]}" > "$root/$t.configure.log" 2>&1 ||
        { cat "$root/$t.configure.log"; exit 1; }
    if ! cmake --build "$root/$t" -j"$(nproc 2>/dev/null || echo 4)" > "$root/$t.build.log" 2>&1; then
        grep -E "error|Error" "$root/$t.build.log" | head -40
        echo "$t: build failed (see $root/$t.build.log)" >&2
        exit 1
    fi
    grep -E "warning:" "$root/$t.build.log" || true
done
