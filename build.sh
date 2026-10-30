#!/bin/sh
# Builds FFmpeg with the fdproxy patches: checkout.sh's jellyfin-ffmpeg, at the tag JELLYFIN_TAG
# names with these patches after jellyfin's own series, built by jellyfin's portable builder.
#
#     build.sh ARCH OUT_DIR
#
# ARCH is amd64 or arm64. It needs git and Docker; jellyfin's builder makes its toolchain image
# first, an hour or more the first time and minutes after, until the builder changes. The tarball
# lands in OUT_DIR, and its ffmpeg and ffprobe in OUT_DIR/bin-ARCH.
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
case $arch in amd64) name=linux64 ;; arm64) name=linuxarm64 ;; esac
# jellyfin's build-linux-ARCH makes its image (toolchain and libraries) only when none is tagged, so
# one made by another tag's builder, or before a builder patch, would be used as it stands. Each
# image is tagged with a digest of the builder that made it; one without this builder's digest is
# removed, and made again. The image is named as jellyfin's builder names it (util/vars.sh).
image=ghcr.io/$(echo "${GITHUB_REPOSITORY:-jellyfin/jellyfin-ffmpeg}" | tr '[:upper:]' '[:lower:]')/$name-gpl
builder=$({ git rev-parse HEAD:builder; git diff HEAD -- builder; } | sha256sum | cut -c1-16)
if ! docker image inspect "$image:builder-$builder" > /dev/null 2>&1; then
    docker image ls --format '{{.Repository}}:{{.Tag}}' "$image" | xargs -r docker image rm
fi
./build-linux-"$arch" "$out"
docker tag "$image:latest" "$image:builder-$builder"
tarball=$(ls -t "$out"/jellyfin-ffmpeg_*_portable_"$name"-gpl.tar.xz | head -1)
rm -rf "$out/bin-$arch" && mkdir -p "$out/bin-$arch"
tar -xJf "$tarball" -C "$out/bin-$arch"
echo "built $tarball; ffmpeg and ffprobe in $out/bin-$arch"
