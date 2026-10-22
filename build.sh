#!/bin/sh
# Builds FFmpeg with the ffstreamer patches: checkout.sh's jellyfin-ffmpeg, at the tag JELLYFIN_TAG
# names with these patches after jellyfin's own series, built by jellyfin's portable builder.
#
#     build.sh ARCH OUT_DIR
#
# ARCH is amd64 or arm64. It needs git and Docker; jellyfin's builder makes its toolchain image
# first, an hour or more the first time and minutes after. The tarball lands in OUT_DIR, and its
# ffmpeg and ffprobe in OUT_DIR/bin-ARCH.
set -eu
if [ $# -ne 2 ] || { [ "$1" != amd64 ] && [ "$1" != arm64 ]; }; then
    echo "usage: $0 amd64|arm64 OUT_DIR" >&2
    exit 2
fi
here=$(cd "$(dirname "$0")" && pwd)
arch=$1
mkdir -p "$2"
out=$(cd "$2" && pwd)
work="$here/build/jellyfin-ffmpeg"

"$here/checkout.sh"
cd "$work"
./build-linux-"$arch" "$out"
case $arch in amd64) name=linux64 ;; arm64) name=linuxarm64 ;; esac
tarball=$(ls -t "$out"/jellyfin-ffmpeg_*_portable_"$name"-gpl.tar.xz | head -1)
rm -rf "$out/bin-$arch" && mkdir -p "$out/bin-$arch"
tar -xJf "$tarball" -C "$out/bin-$arch"
echo "built $tarball; ffmpeg and ffprobe in $out/bin-$arch"
