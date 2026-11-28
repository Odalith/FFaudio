#!/usr/bin/env bash
# Builds the libraries that ship with ffaudio releases, from pinned upstream sources:
#
# soxr 0.1.3 (LGPL 2.1+): the high quality resampler, without OpenMP.
#
# FFmpeg: LGPL only (no --enable-gpl / --enable-nonfree), shared libraries, trimmed to what an audio player needs:
#   - every audio decoder (taken from FFmpeg's own codec list, so new ones are picked up automatically)
#   - every demuxer, parser and bitstream filter (files often carry video or cover art next to the audio)
#   - every audio filter, with soxr and (when its development files are installed) bs2b
#   - the file and pipe protocols, and for streams http, https, tcp, tls, crypto (AES-encrypted HLS) and httpproxy.
#     TLS comes from the system: GnuTLS on Linux (not bundled; FFmpeg stays LGPL 2.1+), schannel on Windows,
#     SecureTransport on macOS
# Not built: encoders, muxers, video decoders and filters, devices, swscale, hardware acceleration, other network
# protocols (rtmp, rtsp, udp, ...) and the ffmpeg/ffprobe programs.
#
# Usage: scripts/build-deps.sh <install-prefix> [work-dir]
# The source tarballs are kept in the work dir (default ./deps-build); releases attach them.
# Environment: JOBS (default: all cores), EXTRA_CONFIGURE (more FFmpeg configure options)
# macOS also gets SDL2 (2.x, zlib license), which it does not have as a system library. Libraries there find each
# other through @rpath (install names) and @loader_path; MACOSX_DEPLOYMENT_TARGET sets the oldest macOS they run on.
#
# Needs: curl, tar, a C compiler, make, cmake, pkg-config, nasm (x86); libbs2b and (Linux) GnuTLS development files
# are optional: without GnuTLS there is no https on Linux. On macOS neither is used (a library from Homebrew would
# make the build depend on Homebrew).
set -euo pipefail

FFMPEG_VERSION=9.0.2
FFMPEG_SHA256=8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
SOXR_VERSION=0.1.3
SOXR_SHA256=b111c15fdc8c029989330ff559184198c161100a59312f5dc19ddeb9b5a15889
# bs2b comes from the system (MIT license); its upstream source only provides the license text, because
# systems do not always install it (MSYS2's static libbs2b has none)
BS2B_VERSION=3.1.0
BS2B_SHA256=6aaafd81aae3898ee40148dd1349aab348db9bfae9767d0e66e0b07ddd4b2528
# macOS only
SDL2_VERSION=2.32.10
SDL2_SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165

prefix="${1:?usage: build-deps.sh <install-prefix> [work-dir]}"
work="${2:-$PWD/deps-build}"
jobs="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
mkdir -p "$prefix" "$work"
prefix="$(cd "$prefix" && pwd)"
work="$(cd "$work" && pwd)"
cd "$work"

# Linux: every library finds the others next to itself (RUNPATH $ORIGIN), so a release archive works wherever it
# is unpacked
# macOS: the same with @rpath install names and an @loader_path rpath
linux=0 macos=0
case "$(uname -s)" in
    Linux)  linux=1 ;;
    Darwin) macos=1 ;;
esac

sha256() {
    if command -v sha256sum > /dev/null; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

# fetch <file> <sha256> <url>... : download from the first URL that works and check the checksum
fetch() {
    local file=$1 want=$2; shift 2
    if [ ! -f "$file" ] || [ "$(sha256 "$file")" != "$want" ]; then
        rm -f "$file"
        for url in "$@"; do
            curl -fsSL --retry 3 -o "$file.part" "$url" && mv "$file.part" "$file" && break
            echo "note: could not download $url" >&2
        done
    fi
    [ -f "$file" ] || { echo "error: could not download $file" >&2; exit 1; }
    local got
    got=$(sha256 "$file")
    [ "$got" = "$want" ] || { echo "error: $file has sha256 $got, expected $want" >&2; exit 1; }
}

#+-----------------------------------------------------+
#| soxr                                                |
#+-----------------------------------------------------+

soxr_tarball="soxr-$SOXR_VERSION-Source.tar.xz"
fetch "$soxr_tarball" "$SOXR_SHA256" \
    "https://downloads.sourceforge.net/project/soxr/$soxr_tarball" \
    "https://deb.debian.org/debian/pool/main/libs/libsoxr/libsoxr_$SOXR_VERSION.orig.tar.xz"
rm -rf "soxr-$SOXR_VERSION-Source" soxr-build
# Without the one symlink (a test helper): MSYS2 can not create symlinks by default
tar xf "$soxr_tarball" --exclude="soxr-$SOXR_VERSION-Source/inst-check-soxr-lsr"
# CMAKE_POLICY_VERSION_MINIMUM: soxr asks for CMake 3.1, which CMake 4 no longer accepts
cmake -S "soxr-$SOXR_VERSION-Source" -B soxr-build -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DBUILD_SHARED_LIBS=ON -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF \
    -DWITH_OPENMP=OFF -DWITH_LSR_BINDINGS=OFF -DWITH_AVFFT=OFF -DWITH_DEV_TRACE=OFF \
    $( [ $linux = 1 ] && echo "-DCMAKE_INSTALL_RPATH=\$ORIGIN" ) \
    $( [ $macos = 1 ] && echo "-DCMAKE_INSTALL_NAME_DIR=@rpath -DCMAKE_INSTALL_RPATH=@loader_path" ) \
    ${CMAKE_GENERATOR:+-G "$CMAKE_GENERATOR"}
cmake --build soxr-build --parallel "$jobs"
cmake --install soxr-build
if [ $macos = 1 ]; then
    # soxr's CMakeLists names its library by the absolute install path (INSTALL_NAME_DIR ${LIB_INSTALL_DIR}), which
    # CMAKE_INSTALL_NAME_DIR does not override: rename it before FFmpeg links to it, and sign it again (ad hoc), as
    # changing a library breaks its signature, which arm64 requires
    for f in "$prefix"/lib/libsoxr*.dylib; do
        [ -L "$f" ] && continue
        install_name_tool -id "@rpath/$(basename "$(otool -D "$f" | sed -n 2p)")" "$f"
        codesign -f -s - "$f"
    done
fi
mkdir -p "$prefix/share/licenses/soxr"
cp "soxr-$SOXR_VERSION-Source/LICENCE" "soxr-$SOXR_VERSION-Source/COPYING.LGPL" "$prefix/share/licenses/soxr/"
echo "soxr $SOXR_VERSION ($soxr_tarball, sha256 $SOXR_SHA256), built without OpenMP" > "$prefix/share/licenses/soxr/BUILD.txt"
export PKG_CONFIG_PATH="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

#+-----------------------------------------------------+
#| SDL2 (macOS)                                        |
#+-----------------------------------------------------+

if [ $macos = 1 ]; then
    sdl_tarball="SDL2-$SDL2_VERSION.tar.gz"
    fetch "$sdl_tarball" "$SDL2_SHA256" "https://github.com/libsdl-org/SDL/releases/download/release-$SDL2_VERSION/$sdl_tarball"
    rm -rf "SDL2-$SDL2_VERSION" sdl2-build
    tar xf "$sdl_tarball"
    cmake -S "SDL2-$SDL2_VERSION" -B sdl2-build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF -DCMAKE_INSTALL_NAME_DIR=@rpath \
        ${CMAKE_GENERATOR:+-G "$CMAKE_GENERATOR"}
    cmake --build sdl2-build --parallel "$jobs"
    cmake --install sdl2-build
    rm -f "$prefix/bin/sdl2-config"   # a script with this machine's paths in it
    mkdir -p "$prefix/share/licenses/sdl2"
    cp "SDL2-$SDL2_VERSION/LICENSE.txt" "$prefix/share/licenses/sdl2/"
    echo "SDL2 $SDL2_VERSION ($sdl_tarball, sha256 $SDL2_SHA256)" > "$prefix/share/licenses/sdl2/BUILD.txt"
fi

#+-----------------------------------------------------+
#| FFmpeg                                              |
#+-----------------------------------------------------+

tarball="ffmpeg-$FFMPEG_VERSION.tar.xz"
fetch "$tarball" "$FFMPEG_SHA256" "https://ffmpeg.org/releases/$tarball"
rm -rf "ffmpeg-$FFMPEG_VERSION"
tar xf "$tarball"
cd "ffmpeg-$FFMPEG_VERSION"

# Audio decoders: the "audio", "PCM", "DPCM" and "ADPCM" sections of allcodecs.c (external libraries come later)
decoders=$(awk '/^\/\* audio codecs \*\//{f=1} /^\/\* subtitles \*\//{f=0} f' libavcodec/allcodecs.c \
           | grep -o 'ff_[a-z0-9_]*_decoder' | sed 's/^ff_//; s/_decoder$//' | sort -u | paste -sd, -)
# Audio filters, sources and sinks, matched like configure does (abuffer/abuffersink are always built)
filters=$(grep -o '^extern const FFFilter ff_\(af\|asrc\|asink\)_[a-z0-9_]*' libavfilter/allfilters.c \
          | sed 's/^extern const FFFilter ff_[a-z]*_//' | sort -u | paste -sd, -)
# amovie (an "avsrc"): reads audio from a file inside a filter graph, e.g. an impulse response for afir. It also
# makes libavfilter link libavformat, which ametadata needs but does not declare (FFmpeg 9: shared Windows builds
# fail with undefined avio_* references without it)
filters="$filters,amovie"
for need in flac alac mp3 mp3float aac vorbis opus wavpack ape pcm_s16le dsd_lsbf; do
    case ",$decoders," in *",$need,"*) ;; *) echo "error: decoder $need missing from the list" >&2; exit 1 ;; esac
done
for need in aformat aresample atempo anequalizer equalizer stereotools alimiter acompressor; do
    case ",$filters," in *",$need,"*) ;; *) echo "error: filter $need missing from the list" >&2; exit 1 ;; esac
done

# configure looks for soxr with the compiler (not pkg-config), hence the -I/-L for the prefix below
optional=(--enable-libsoxr)
if [ $macos = 0 ] && pkg-config --exists libbs2b; then
    optional+=(--enable-libbs2b)
    (
        cd "$work"
        bs2b_tarball="libbs2b-$BS2B_VERSION.tar.gz"
        fetch "$bs2b_tarball" "$BS2B_SHA256" "https://downloads.sourceforge.net/project/bs2b/libbs2b/$BS2B_VERSION/$bs2b_tarball"
        mkdir -p "$prefix/share/licenses/bs2b"
        for f in COPYING AUTHORS; do
            tar -xzOf "$bs2b_tarball" "libbs2b-$BS2B_VERSION/$f" > "$prefix/share/licenses/bs2b/$f"
        done
        echo "bs2b $(pkg-config --modversion libbs2b) from the system (MIT); license from $bs2b_tarball" \
            > "$prefix/share/licenses/bs2b/BUILD.txt"
    )
else
    echo "note: libbs2b not found, building without it"
fi
case "$(uname -s)" in
    MINGW*|MSYS*|Darwin) ;;   # schannel / SecureTransport, below
    *) if pkg-config --exists gnutls; then optional+=(--enable-gnutls)
       else echo "note: GnuTLS not found, building without https"; fi ;;
esac

case "$(uname -s)" in
    MINGW*|MSYS*) platform=(--enable-w32threads --enable-schannel) ;;
    # SecureTransport: Apple's TLS, part of the system (Security.framework). @rpath install names, and every library
    # looks next to itself
    Darwin)       platform=(--enable-pthreads --enable-securetransport --install-name-dir=@rpath
                            --extra-ldflags=-Wl,-rpath,@loader_path) ;;
    # $ORIGIN passes four interpreters: configure's eval (\\\$\$\$\$ -> \$$$$), make twice (library.mak expands
    # its link rule with $(eval), $$$$ -> $$ -> $) and the shell running the link command (\$ -> $)
    *)            platform=(--enable-pthreads '--extra-ldflags=-Wl,-rpath,\\\$\$\$\$ORIGIN') ;;
esac

# shellcheck disable=SC2086 # EXTRA_CONFIGURE is a list of options
./configure \
    --prefix="$prefix" \
    --enable-shared --disable-static --enable-pic \
    --disable-programs --disable-doc --disable-debug \
    --disable-autodetect --enable-zlib "${platform[@]}" \
    --disable-everything \
    --disable-avdevice --disable-swscale \
    --enable-decoder="$decoders" \
    --enable-demuxers --enable-parsers --enable-bsfs \
    --enable-filter="$filters" \
    --enable-protocol=file,pipe,http,https,tcp,tls,crypto,httpproxy \
    --extra-cflags="-I$prefix/include" --extra-ldflags="-L$prefix/lib" \
    "${optional[@]}" ${EXTRA_CONFIGURE:-} \
    | tee "$work/configure.log"

grep -q '^License: LGPL' "$work/configure.log" || { echo "error: the configuration is not LGPL" >&2; exit 1; }

make -j"$jobs"
make install

# What was built and under which license, next to the libraries
mkdir -p "$prefix/share/licenses/ffmpeg"
cp COPYING.LGPLv2.1 LICENSE.md "$prefix/share/licenses/ffmpeg/"
{
    echo "FFmpeg $FFMPEG_VERSION (https://ffmpeg.org/releases/$tarball, sha256 $FFMPEG_SHA256)"
    echo "built by ffaudio's scripts/build-deps.sh with:"
    sed -n 's/^#define FFMPEG_CONFIGURATION "\(.*\)"$/\1/p' config.h
    grep '^License:' "$work/configure.log"
} > "$prefix/share/licenses/ffmpeg/BUILD.txt"
echo "FFmpeg $FFMPEG_VERSION installed to $prefix"
