#!/bin/sh
# Builds the libFuzzer harness of fdproxy's FFmpeg side, and test/claims_test.c, with
# AddressSanitizer and UndefinedBehaviorSanitizer, against a minimal FFmpeg built the same way from jellyfin-ffmpeg: either
# checkout.sh's checkout, whose debian/patches/series it applies with quilt into OUT_DIR/src, or a tree
# with the series already applied.
#
#     fuzz/build.sh SOURCE_TREE OUT_DIR
#
# It runs in Docker (TEST_IMAGE, Debian trixie by digest, its packages pinned; clang); `make fuzz` builds the harness and runs it, and
# `make test-claims` the test.
set -eu
if [ $# -ne 2 ]; then
    echo "usage: $0 SOURCE_TREE OUT_DIR" >&2
    exit 2
fi
source=$(cd "$1" && pwd)
mkdir -p "$2"
out=$(cd "$2" && pwd)
here=$(cd "$(dirname "$0")" && pwd)
docker run --rm -v "$source":/src:ro -v "$out":/out -v "$here":/fuzz:ro -v "$here/../test":/test:ro \
    -v "$here/../tools":/pin:ro "$(cat "$here/../TEST_IMAGE")" bash -c '
set -e
sh /pin/debian_snapshot.sh
apt-get install -y -qq clang make pkg-config quilt >/dev/null 2>&1
src=/src
series=$(cat /src/debian/patches/series /src/debian/patches/*.patch | sha256sum)
if [ ! -d /src/.pc ]; then
    # The applied copy is made again whenever the series or a patch changed, or it would fuzz old
    # code. At fuzz 0, so a patch whose context moved after a jellyfin bump fails here rather than
    # landing a few lines off.
    if [ ! -f /out/src/.pc/applied-patches ] || [ "$(cat /out/src.series 2>/dev/null)" != "$series" ]; then
        rm -rf /out/src && mkdir -p /out/src
        tar -C /src --exclude=.git -cf - . | tar -C /out/src -xf -
        (cd /out/src && QUILT_PATCHES=debian/patches quilt push -a -q --fuzz=0 > /dev/null)
        echo "$series" > /out/src.series
    fi
    src=/out/src
fi
# Configured again, from nothing, whenever the series changed: a new tag or a patch to configure
# would otherwise build with the old configuration.
if [ "$(cat /out/objs.series 2>/dev/null)" != "$series" ]; then
    rm -rf /out/objs
fi
mkdir -p /out/objs && cd /out/objs
flags="-fsanitize=address,undefined,fuzzer-no-link -fno-sanitize-recover=undefined -g -O1"
if [ ! -f config.h ]; then
    "$src"/configure --cc=clang --cxx=clang++ --disable-everything --disable-programs --disable-doc \
        --disable-autodetect --disable-asm --disable-stripping \
        --extra-cflags="$flags" --extra-ldflags="-fsanitize=address,undefined" > configure.log 2>&1 \
        || { tail -30 configure.log; exit 1; }
    echo "$series" > /out/objs.series
fi
make -j"$(nproc)" libavutil/libavutil.a libavcodec/libavcodec.a libavformat/libavformat.a > make.log 2>&1 \
    || { grep -n -B2 -A6 error make.log | head -40; exit 1; }
clang -fsanitize=address,undefined,fuzzer -fno-sanitize-recover=undefined -g -O1 \
    -I/out/objs -I"$src" /fuzz/fdproxy_fuzzer.c \
    libavformat/libavformat.a libavcodec/libavcodec.a libavutil/libavutil.a -lm -lpthread \
    -o /out/fdproxy_fuzzer
clang -fsanitize=address,undefined -fno-sanitize-recover=undefined -g -O1 \
    -I/out/objs -I"$src" /test/claims_test.c \
    libavformat/libavformat.a libavcodec/libavcodec.a libavutil/libavutil.a -lm -lpthread \
    -o /out/claims_test
echo built /out/fdproxy_fuzzer /out/claims_test
'
