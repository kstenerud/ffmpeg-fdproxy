#!/bin/sh
# Checks out FFmpeg at the commit UPSTREAM_COMMIT names into build/ffmpeg-upstream, makes the
# series for FFmpeg's own tree there (tools/upstream.py), and writes it into OUT_DIR as patches to
# send. With --test it then builds the tree in Docker (debian:trixie) and runs the series' FATE test
# and the protocols manual's build. It needs git, Python, the network and, for --test, Docker.
#
#     upstream.sh [--test] OUT_DIR
set -eu
test=no
if [ "${1:-}" = --test ]; then
    test=yes
    shift
fi
if [ $# -ne 1 ]; then
    echo "usage: $0 [--test] OUT_DIR" >&2
    exit 2
fi
here=$(cd "$(dirname "$0")/.." && pwd)
commit=$(cat "$here/UPSTREAM_COMMIT")
work="$here/build/ffmpeg-upstream"

if [ ! -d "$work/.git" ]; then
    git init --quiet "$work"
    git -C "$work" remote add origin https://github.com/FFmpeg/FFmpeg
fi
git -C "$work" fetch --quiet --depth 1 origin "$commit"
git -C "$work" checkout --quiet --force --detach "$commit"
git -C "$work" clean --quiet -fdx
# The series is the owner's: their name on each commit, as on this repository's.
git -C "$work" config user.name "$(git -C "$here" config user.name)"
git -C "$work" config user.email "$(git -C "$here" config user.email)"
python3 "$here/tools/upstream.py" "$work"

mkdir -p "$1"
out=$(cd "$1" && pwd)
rm -f "$out"/*.patch
git -C "$work" format-patch --quiet --output-directory "$out" "$commit"
echo "the series is in $out: $(ls "$out" | wc -l) patches on FFmpeg $(echo "$commit" | cut -c1-12)"
[ "$test" = yes ] || exit 0

docker run --rm -v "$work":/src:ro debian:trixie bash -c '
set -e
apt-get update -qq >/dev/null
apt-get install -y -qq gcc make pkg-config texinfo >/dev/null 2>&1
mkdir /objs && cd /objs
/src/configure --disable-everything --enable-protocol=ffstreamer --disable-autodetect --disable-asm \
    > configure.log 2>&1 || { tail -30 configure.log; exit 1; }
grep -q "^#define CONFIG_FFSTREAMER_PROTOCOL 1" config_components.h \
    || { echo "configure did not enable ffstreamer"; exit 1; }
make -j"$(nproc)" doc/ffmpeg-protocols.html > make.log 2>&1 || { grep -n -B2 -A6 -i error make.log | head -40; exit 1; }
make -j"$(nproc)" libavformat/tests/ffstreamer > build.log 2>&1 || { grep -n -B2 -A6 -i error build.log | head -40; exit 1; }
# The series adds no warning of its own to the build.
if grep -A3 "ffstreamer[^ ]*: warning:" build.log make.log; then exit 1; fi
make fate-ffstreamer SAMPLES= 2>&1 | tail -20
'
