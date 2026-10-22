#!/bin/sh
# Builds the libFuzzer harness of ffstreamer's FFmpeg side, with AddressSanitizer and
# UndefinedBehaviorSanitizer, against a minimal FFmpeg built the same way from jellyfin-ffmpeg: either
# build.sh's checkout, whose debian/patches/series it applies with quilt into OUT_DIR/src, or a tree
# with the series already applied.
#
#     fuzz/build.sh SOURCE_TREE OUT_DIR
#
# It runs in Docker (debian:trixie, clang); `make fuzz` builds it and runs it.
set -eu
if [ $# -ne 2 ]; then
    echo "usage: $0 SOURCE_TREE OUT_DIR" >&2
    exit 2
fi
source=$(cd "$1" && pwd)
mkdir -p "$2"
out=$(cd "$2" && pwd)
here=$(cd "$(dirname "$0")" && pwd)
docker run --rm -v "$source":/src:ro -v "$out":/out -v "$here":/fuzz:ro debian:trixie bash -c '
set -e
apt-get update -qq >/dev/null
apt-get install -y -qq clang make pkg-config quilt >/dev/null 2>&1
src=/src
if [ ! -d /src/.pc ]; then
    if [ ! -f /out/src/.pc/applied-patches ]; then
        rm -rf /out/src && mkdir -p /out/src
        tar -C /src --exclude=.git -cf - . | tar -C /out/src -xf -
        (cd /out/src && QUILT_PATCHES=debian/patches quilt push -a -q > /dev/null)
    fi
    src=/out/src
fi
mkdir -p /out/objs && cd /out/objs
flags="-fsanitize=address,undefined,fuzzer-no-link -fno-sanitize-recover=undefined -g -O1"
if [ ! -f config.h ]; then
    "$src"/configure --cc=clang --cxx=clang++ --disable-everything --disable-programs --disable-doc \
        --disable-autodetect --disable-asm --disable-stripping \
        --extra-cflags="$flags" --extra-ldflags="-fsanitize=address,undefined" > configure.log 2>&1 \
        || { tail -30 configure.log; exit 1; }
fi
make -j"$(nproc)" libavutil/libavutil.a libavcodec/libavcodec.a libavformat/libavformat.a > make.log 2>&1 \
    || { grep -n -B2 -A6 error make.log | head -40; exit 1; }
clang -fsanitize=address,undefined,fuzzer -fno-sanitize-recover=undefined -g -O1 \
    -I/out/objs -I"$src" /fuzz/ffstreamer_fuzzer.c \
    libavformat/libavformat.a libavcodec/libavcodec.a libavutil/libavutil.a -lm -lpthread \
    -o /out/ffstreamer_fuzzer
echo built /out/ffstreamer_fuzzer
'
