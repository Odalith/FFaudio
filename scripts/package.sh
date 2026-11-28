#!/usr/bin/env bash
# Assembles a release archive: ffaudio (installed with cmake --install), the libraries from build-deps.sh, the
# third-party libraries they need that a plain system does not have, and every license.
#
# Usage: scripts/package.sh <archive-name> <deps-prefix> <ffaudio-install-dir> <out-dir>
#   e.g. scripts/package.sh ffaudio-4.0.0-linux-x86_64 deps/prefix stage dist
#
# Linux: libraries go to lib/; ffaudio, FFmpeg and soxr are linked with RUNPATH $ORIGIN (FFAUDIO_RELOCATABLE,
#        build-deps.sh), copied system libraries get it with patchelf when they need other bundled libraries.
#        SDL2 and the C library stay system dependencies. The result is checked with ldd.
#        Third-party licenses come from the Debian/Ubuntu package that owns each bundled library.
# Windows (MSYS2): DLLs go to bin/, including SDL2 and the MinGW runtime; licenses from the owning MSYS2 package.
# macOS: libraries go to lib/, SDL2 included (build-deps.sh builds it), all found through @rpath and @loader_path.
#        Nothing is rewritten (that would break the code signatures arm64 needs); the result is checked with otool:
#        every dependency is in the archive or part of macOS.
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
    Darwin)       os=macos ;;
    *)            echo "error: unsupported platform $(uname -s)" >&2; exit 1 ;;
esac

# ffaudio and the dependency builds (headers, libraries, pkg-config files, licenses)
cp -a "$stage/." "$root/"
cp -a "$deps/include" "$deps/lib" "$deps/share" "$root/"
[ -d "$deps/bin" ] && cp -a "$deps/bin" "$root/"
rm -rf "$root/lib/cmake/soxr" "$root/share/ffmpeg" "$root/share/doc" 2>/dev/null || true

# pkg-config files that work wherever the archive is unpacked
for pc in "$root"/lib/pkgconfig/*.pc; do
    sed -i.orig 's|^prefix=.*|prefix=${pcfiledir}/../..|' "$pc" && rm -f "$pc.orig"   # (BSD sed needs a suffix)
done

# The Python bindings and the player (source; the player needs Textual: pip install -r player/requirements.txt)
mkdir -p "$root/python" "$root/player"
cp -a "$src/bindings/python/ffaudio" "$src/bindings/python/README.md" "$root/python/"
cp -a "$src/player/ffaudio_player" "$src/player/run.py" "$src/player/requirements.txt" "$src/player/README.md" \
    "$root/player/"
find "$root/python" "$root/player" -name __pycache__ -type d -prune -exec rm -rf {} +

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
            libgnutls.so.*) return 0 ;;   # https; on practically every desktop Linux, kept up to date by the system
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
elif [ "$os" = macos ]; then
    # Every Mach-O library in lib/: its own name is @rpath/<file>, it looks next to itself (@loader_path), and each
    # library it loads is either in lib/ (as @rpath/<file>) or part of macOS
    system_dylib() { case "$1" in /usr/lib/*|/System/Library/*) return 0 ;; esac; return 1; }
    for lib in "$root"/lib/*.dylib; do
        [ -L "$lib" ] && continue
        id=$(otool -D "$lib" | sed -n 2p)
        case "$id" in @rpath/*) ;; *) echo "error: $lib is named '$id', not @rpath/..." >&2; exit 1 ;; esac
        deps_of=$(otool -L "$lib" | tail -n +2 | awk '{ print $1 }' | grep -vxF "$id" || true)
        if grep -q '^@rpath/' <<< "$deps_of"; then
            otool -l "$lib" | grep -A2 LC_RPATH | grep -q 'path @loader_path ' \
                || { echo "error: $lib loads @rpath libraries but has no @loader_path rpath" >&2; exit 1; }
        fi
        for dep in $deps_of; do
            case "$dep" in
                @rpath/*) [ -e "$root/lib/${dep#@rpath/}" ] || { echo "error: $lib needs $dep, which is not in lib/" >&2; exit 1; } ;;
                *) system_dylib "$dep" || { echo "error: $lib needs $dep, which is neither bundled nor part of macOS" >&2; exit 1; } ;;
            esac
        done
    done
    if [ -e "$root/share/licenses/sdl2/LICENSE.txt" ]; then
        bundled+=("$(head -1 "$root/share/licenses/sdl2/BUILD.txt" | cut -d' ' -f1-2) (zlib license)")
    fi
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
    echo "  include/ffaudio/    ffaudio.h, ffaudio_filters.h, ffaudio_loudness.h, ffaudio_meters.h, ffaudio_playlist.h"
    echo "  include/            FFmpeg's headers"
    echo "  python/ffaudio/     Python bindings (ctypes, no compiler needed): finds the library next to it"
    echo "  player/             a terminal music player built on them: pip install -r player/requirements.txt,"
    echo "                      then python3 player/run.py ~/Music  (player/README.md)"
    if [ "$os" = linux ]; then
        echo "  lib/                libffaudio.so and the libraries it needs (RUNPATH \$ORIGIN)"
        echo "  lib/pkgconfig       pkg-config files: PKG_CONFIG_PATH=<here>/lib/pkgconfig pkg-config --cflags --libs ffaudio"
        echo "  lib/cmake/ffaudio   find_package(ffaudio) with CMAKE_PREFIX_PATH=<here>, target ffaudio::ffaudio"
        echo
        echo "Needs SDL2 (2.24 or newer) from the system, e.g. libsdl2-2.0-0 on Debian/Ubuntu, sdl2 on Arch/Fedora."
        echo "https streams need GnuTLS from the system (libgnutls30 on Debian/Ubuntu, gnutls elsewhere)."
        echo "Built on Ubuntu 24.04: needs glibc 2.39 or newer."
    elif [ "$os" = macos ]; then
        echo "  lib/                libffaudio.dylib and every library it needs (FFmpeg, soxr, SDL2), found through"
        echo "                      @rpath: link with -Wl,-rpath,<here>/lib, or put them next to your executable"
        echo "  lib/pkgconfig       pkg-config files: PKG_CONFIG_PATH=<here>/lib/pkgconfig pkg-config --cflags --libs ffaudio"
        echo "  lib/cmake/ffaudio   find_package(ffaudio) with CMAKE_PREFIX_PATH=<here>, target ffaudio::ffaudio"
        echo
        echo "Needs macOS ${MACOSX_DEPLOYMENT_TARGET:-$(sw_vers -productVersion)} or newer. https uses the system's TLS (SecureTransport)."
        echo "The libraries are not notarized: if macOS refuses to load them after a download, remove the quarantine"
        echo "flag: xattr -dr com.apple.quarantine <this folder>"
        if [ "$(uname -m)" = arm64 ]; then
            echo "Apple silicon (arm64): built and tested on GitHub's macOS runners, not yet on a real Mac."
        fi
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
    for b in ${bundled[@]+"${bundled[@]}"}; do echo "  $b - see share/licenses"; done   # (bash 3.2: empty arrays and set -u)
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
elif [ "$os" = macos ]; then
    archive="$name.tar.gz"   # Archive Utility opens it with a double click; tar keeps the library symlinks
    tar -czf "$archive" "$name"
else
    archive="$name.zip"
    rm -f "$archive"
    zip -qr9 "$archive" "$name"
fi
if command -v sha256sum > /dev/null; then sha256sum "$archive" > "$archive.sha256"; else shasum -a 256 "$archive" > "$archive.sha256"; fi
echo "wrote $out/$archive"
