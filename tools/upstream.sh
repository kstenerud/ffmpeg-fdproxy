#!/bin/sh
# Checks out FFmpeg at the commit UPSTREAM_COMMIT names into build/ffmpeg-upstream, makes the two
# series for FFmpeg's own tree there (tools/upstream.py), each on FFmpeg alone, and writes them into
# OUT_DIR/fftools and OUT_DIR/protocol as patches to send apart. With --test it then makes both on one
# tree, builds it in Docker (debian:trixie) and runs the FATE test and the protocols manual's build. It needs git, Python, the network and, for --test, Docker.
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
# The series is the owner's: their name on each commit, as on this repository's.
git -C "$work" config user.name "$(git -C "$here" config user.name)"
git -C "$work" config user.email "$(git -C "$here" config user.email)"
mkdir -p "$1"
out=$(cd "$1" && pwd)
rm -rf "$out"/*.patch "$out/fftools" "$out/protocol"

# FFmpeg as UPSTREAM_COMMIT names it, then the series named.
make_series() {
    git -C "$work" checkout --quiet --force --detach "$commit"
    git -C "$work" clean --quiet -fdx
    python3 "$here/tools/upstream.py" "$work" "$1"
}
for series in fftools protocol; do
    make_series $series
    git -C "$work" format-patch --quiet --output-directory "$out/$series" "$commit"
    echo "the $series series is in $out/$series: $(ls "$out/$series" | wc -l) patches on FFmpeg $(echo "$commit" | cut -c1-12)"
done
[ "$test" = yes ] || exit 0
make_series all

docker run --rm -v "$work":/src:ro debian:trixie bash -c '
set -e
apt-get update -qq >/dev/null
apt-get install -y -qq gcc make pkg-config texinfo >/dev/null 2>&1
mkdir /objs && cd /objs
/src/configure --disable-everything --enable-protocol=fdproxy --disable-autodetect --disable-asm \
    > configure.log 2>&1 || { tail -30 configure.log; exit 1; }
grep -q "^#define CONFIG_FDPROXY_PROTOCOL 1" config_components.h \
    || { echo "configure did not enable fdproxy"; exit 1; }
make -j"$(nproc)" doc/ffmpeg-protocols.html > make.log 2>&1 || { grep -n -B2 -A6 -i error make.log | head -40; exit 1; }
make -j"$(nproc)" libavformat/tests/fdproxy > build.log 2>&1 || { grep -n -B2 -A6 -i error build.log | head -40; exit 1; }
# The series adds no warning of its own to the build.
if grep -A3 "fdproxy[^ ]*: warning:" build.log make.log; then exit 1; fi
# On a failure, what the test printed, whole: the reference to compare, or to write anew.
make fate-fdproxy SAMPLES= > fate.log 2>&1 || {
    tail -5 fate.log; echo "--- what the test printed:"; cat tests/data/fate/fdproxy; exit 1; }
tail -1 fate.log
'
