#!/usr/bin/env bash
# Assembles a release archive: ffaudio (installed with cmake --install), the libraries from build-deps.sh, the
# third-party libraries they need that a plain system does not have, and every license.
#
# Usage: scripts/package.sh <archive-name> <deps-prefix> <ffaudio-install-dir> <out-dir>
#   e.g. scripts/package.sh ffaudio-2.0.0-linux-x86_64 deps/prefix stage dist
#
# Linux: libraries go to lib/; ffaudio, FFmpeg and soxr are linked with RUNPATH $ORIGIN (FFAUDIO_RELOCATABLE,
#        build-deps.sh), copied system libraries get it with patchelf when they need other bundled libraries.
#        SDL2 and the C library stay system dependencies. The result is checked with ldd.
#        Third-party licenses come from the Debian/Ubuntu package that owns each bundled library.
# Windows (MSYS2): DLLs go to bin/, including SDL2 and the MinGW runtime; licenses from the owning MSYS2 package.
set -euo pipefail

name="${1:?usage: package.sh <archive-name> <deps-prefix> <ffaudio-install-dir> <out-dir>}"
deps="$(cd "${2:?}" && pwd)"
stage="$(cd "${3:?}" && pwd)"
out="${4:?}"
src="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
root="$out/$name"
rm -rf "$root"
mkdir -p "$root"

case "$(uname -s)" in
    MINGW*|MSYS*) os=windows ;;
    Linux)        os=linux ;;
    *)            echo "error: unsupported platform $(uname -s)" >&2; exit 1 ;;
esac

# ffaudio and the dependency builds (headers, libraries, pkg-config files, licenses)
cp -a "$stage/." "$root/"
cp -a "$deps/include" "$deps/lib" "$deps/share" "$root/"
[ -d "$deps/bin" ] && cp -a "$deps/bin" "$root/"
rm -rf "$root/lib/cmake/soxr" "$root/share/ffmpeg" "$root/share/doc" 2>/dev/null || true

# pkg-config files that work wherever the archive is unpacked
for pc in "$root"/lib/pkgconfig/*.pc; do
    sed -i 's|^prefix=.*|prefix=${pcfiledir}/../..|' "$pc"
done

mkdir -p "$root/share/licenses/ffaudio"
cp "$src/license.txt" "$root/share/licenses/ffaudio/"
cp "$src/readme.md" "$src/CHANGELOG.md" "$root/"

bundled=()   # third-party libraries that were added, for the README

if [ "$os" = linux ]; then
    # Libraries every Linux system has, or that the application provides (SDL2: one copy per process)
    system_lib() {
        case "$1" in
            linux-vdso.so.*|ld-linux*.so.*|libc.so.*|libm.so.*|libpthread.so.*|libdl.so.*|librt.so.*) return 0 ;;
            libgcc_s.so.*|libstdc++.so.*|libz.so.*|libSDL2-2.0.so.*) return 0 ;;
        esac
        return 1
    }
    # Copy the direct dependencies of every library in lib/ that are neither bundled nor system libraries
    changed=1
    while [ $changed = 1 ]; do
        changed=0
        for so in "$root"/lib/*.so*; do
            [ -L "$so" ] && continue
            for need in $(readelf -d "$so" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
                system_lib "$need" && continue
                [ -e "$root/lib/$need" ] && continue
                path=$(ldd "$so" | awk -v n="$need" '$1 == n { print $3 }')
                [ -n "$path" ] && [ -f "$path" ] || { echo "error: $need (needed by $so) not found" >&2; exit 1; }
                cp -L "$path" "$root/lib/$need"
                bundled+=("$need")
                # Its license, from the package that installed it (Debian/Ubuntu, or Arch for local tests)
                real=$(readlink -f "$path")
                if pkg=$(dpkg -S "$real" 2>/dev/null | head -1 | cut -d: -f1) && [ -f "/usr/share/doc/$pkg/copyright" ]; then
                    mkdir -p "$root/share/licenses/$pkg"
                    cp "/usr/share/doc/$pkg/copyright" "$root/share/licenses/$pkg/"
                elif pkg=$(pacman -Qqo "$real" 2>/dev/null) && [ -d "/usr/share/licenses/$pkg" ]; then
                    cp -r "/usr/share/licenses/$pkg" "$root/share/licenses/"
                else
                    echo "warning: no license found for $need" >&2
                fi
                changed=1
            done
        done
    done
    # Copied system libraries that need other bundled ones look next to themselves
    for need in "${bundled[@]}"; do
        if readelf -d "$root/lib/$need" | grep -q '(NEEDED)' && command -v patchelf > /dev/null; then
            patchelf --set-rpath '$ORIGIN' "$root/lib/$need"
        fi
    done
    # Check, for the direct dependencies of every library in lib/ (with no library path set): bundled ones load
    # from the archive, the rest are system libraries. What system libraries need in turn (e.g. SDL2's ALSA and
    # PulseAudio) is the system's business.
    for so in "$root"/lib/*.so*; do
        [ -L "$so" ] && continue
        resolved=$(env -u LD_LIBRARY_PATH ldd "$so")
        for dep in $(readelf -d "$so" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
            path=$(awk -v n="$dep" '$1 == n && $2 == "=>" { print $3 }' <<< "$resolved")
            if [ -e "$root/lib/$dep" ]; then
                case "$path" in "$root"/lib/*) ;; *) echo "error: $so loads $dep from '$path', not from the archive" >&2; exit 1 ;; esac
            elif ! system_lib "$dep"; then
                echo "error: $so needs $dep, which is neither bundled nor a system library" >&2
                exit 1
            fi
        done
    done
else
    msys="${MINGW_PREFIX:-/ucrt64}"
    msys_bin="$msys/bin"
    # The license directory of the MSYS2 package that owns a file
    msys_license() {
        local pkg
        pkg=$(pacman -Qqo "$1" 2>/dev/null | head -1) || pkg=""
        if [ -n "$pkg" ] && [ -d "$msys/share/licenses/${pkg#mingw-w64-ucrt-x86_64-}" ]; then
            cp -r "$msys/share/licenses/${pkg#mingw-w64-ucrt-x86_64-}" "$root/share/licenses/"
        else
            echo "warning: no license directory for ${pkg:-the owner of $1}" >&2
        fi
    }
    bundle_dll() { # bundle_dll <name>: copy it from MSYS2 (with its license); false for Windows' own DLLs
        local path="$msys_bin/$1"
        [ -e "$root/bin/$1" ] && return 0
        [ -f "$path" ] || return 1
        cp "$path" "$root/bin/"
        bundled+=("$1")
        msys_license "$path"
    }
    # DLLs from the MSYS2 environment (not Windows' own) that the DLLs in bin/ import, recursively
    bundle_imports() {
        local changed=1 dll need
        while [ $changed = 1 ]; do
            changed=0
            for dll in "$root"/bin/*.dll; do
                for need in $(objdump -p "$dll" | sed -n 's/^\s*DLL Name: \(.*\)$/\1/p' | tr -d '\r'); do
                    [ -e "$root/bin/$need" ] && continue
                    bundle_dll "$need" && changed=1
                done
            done
        done
    }
    bundle_imports
    # Loaded at run time, so not in any import table: MSYS2's SDL2.dll is sdl2-compat, which runs on SDL3.dll
    if [ -e "$root/bin/SDL2.dll" ] && strings -a "$root/bin/SDL2.dll" | grep -qx 'SDL3.dll'; then
        bundle_dll SDL3.dll || { echo "error: SDL2.dll needs SDL3.dll, which is not installed" >&2; exit 1; }
        bundle_imports
    fi
    # Libraries FFmpeg linked statically (MSYS2 has only a static libbs2b): their code is in the DLLs, so their
    # licenses come along
    if grep -q -- '--enable-libbs2b' "$root/share/licenses/ffmpeg/BUILD.txt" && [ ! -e "$root/bin/libbs2b-0.dll" ]; then
        # build-deps.sh installs bs2b's license (MSYS2's package has none)
        [ -f "$root/share/licenses/bs2b/COPYING" ] || { echo "error: bs2b is linked in but its license is missing" >&2; exit 1; }
        bundled+=("bs2b (linked into avfilter-*.dll) - MIT, see share/licenses/bs2b")
    fi
    # Import libraries are not needed to run, but let applications link against the bundled DLLs
    rm -f "$root"/lib/*.def 2>/dev/null || true
fi

# What is inside and under which terms
{
    echo "$name"
    echo
    echo "ffaudio $(sed -n 's/^project(ffAudio VERSION \([0-9.]*\).*/\1/p' "$src/CMakeLists.txt") - LGPL 2.1 or later, see share/licenses/ffaudio."
    echo "https://github.com/Odalith/ffAudio"
    echo
    echo "Contents"
    echo "  include/            ffaudio.h, ffaudio_filters.h, ffaudio_loudness.h (and FFmpeg's headers)"
    if [ "$os" = linux ]; then
        echo "  lib/                libffaudio.so and the libraries it needs (RUNPATH \$ORIGIN)"
        echo "  lib/pkgconfig       pkg-config files: PKG_CONFIG_PATH=<here>/lib/pkgconfig pkg-config --cflags --libs ffaudio"
        echo "  lib/cmake/ffaudio   find_package(ffaudio) with CMAKE_PREFIX_PATH=<here>, target ffaudio::ffaudio"
        echo
        echo "Needs SDL2 (2.24 or newer) from the system, e.g. libsdl2-2.0-0 on Debian/Ubuntu, sdl2 on Arch/Fedora."
        echo "Built on Ubuntu 24.04: needs glibc 2.39 or newer."
    else
        echo "  bin/                libffaudio.dll and every DLL it needs (FFmpeg, soxr, SDL2, MinGW runtime)."
        echo "                      SDL2.dll is sdl2-compat, which runs on SDL3.dll: ship both"
        echo "  lib/                import libraries (.dll.a) for MinGW; pkg-config and CMake files as on Linux"
        echo
        echo "Built with MSYS2 UCRT64 (GCC). Ship the DLLs from bin/ next to your executable."
    fi
    echo
    echo "Bundled third-party libraries"
    echo "  FFmpeg - LGPL 2.1 or later; how it was configured: share/licenses/ffmpeg/BUILD.txt"
    echo "  soxr   - LGPL 2.1 or later; share/licenses/soxr"
    for b in "${bundled[@]}"; do echo "  $b - see share/licenses"; done
    echo
    echo "Source code"
    echo "  The FFmpeg and soxr source tarballs these libraries were built from are attached to the same release"
    echo "  as this archive; scripts/build-deps.sh in the ffaudio repository rebuilds them. You may replace the"
    echo "  bundled LGPL libraries with compatible builds of your own."
} > "$root/README.txt"

cd "$out"
if [ "$os" = linux ]; then
    archive="$name.tar.xz"
    tar -cJf "$archive" "$name"
else
    archive="$name.zip"
    rm -f "$archive"
    zip -qr9 "$archive" "$name"
fi
sha256sum "$archive" > "$archive.sha256"
echo "wrote $out/$archive"
