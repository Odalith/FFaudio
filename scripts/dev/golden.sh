#!/bin/bash
# Compare what the current build plays with what a git ref played: golden.sh <git-ref> [tree]
#   tree: a build tree with golden_render and the fixtures (default build-dev/release)
# The reference is rendered once from <ref> in a temporary worktree and kept as golden/<ref>.f32 (git-ignored); it has
# to come from this machine and FFmpeg build (tests/golden_render.c). Both renders use FFAUDIO_SDL2_LIB when it exists.
# Exit status: 0 identical, 3 different, anything else an error.
set -euo pipefail
src=$(cd "$(dirname "$0")/../.." && pwd)
ref=${1:?usage: golden.sh <git-ref> [tree]}
tree=${2:-$src/build-dev/release}
real=${FFAUDIO_SDL2_LIB:-$HOME/.local/opt/sdl2-2.30/lib}
[ -d "$real" ] && export LD_LIBRARY_PATH=$real
mkdir -p "$src/golden"
reference="$src/golden/$(echo "$ref" | tr '/' '_').f32"
gen=()
command -v ninja >/dev/null && gen=(-G Ninja)

if [ ! -f "$reference" ]; then
    tmp=$(mktemp -d)
    trap 'git -C "$src" worktree remove --force "$tmp/src" >/dev/null 2>&1 || true; rm -rf "$tmp"' EXIT
    git -C "$src" worktree add --detach "$tmp/src" "$ref" >/dev/null
    cmake -S "$tmp/src" -B "$tmp/build" "${gen[@]}" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFFAUDIO_SANITIZER=none \
        -DFFAUDIO_INSTALL=OFF > "$tmp/configure.log" 2>&1 || { cat "$tmp/configure.log"; exit 1; }
    cmake --build "$tmp/build" --target golden_render ffaudio_fixtures -j"$(nproc 2>/dev/null || echo 4)" \
        > "$tmp/build.log" 2>&1 || { tail -30 "$tmp/build.log"; exit 1; }
    "$tmp/build/golden_render" "$tmp/build/fixtures" "$reference"
fi

cmake --build "$tree" --target golden_render ffaudio_fixtures > /dev/null
out=$(mktemp --suffix=.f32)
set +e
"$tree/golden_render" "$tree/fixtures" "$out" "$reference"
status=$?
set -e
rm -f "$out"
exit $status
