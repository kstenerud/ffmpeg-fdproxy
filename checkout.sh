#!/bin/sh
# Checks out jellyfin-ffmpeg at the tag JELLYFIN_TAG names into build/jellyfin-ffmpeg, with these
# patches added after jellyfin's own debian/patches/series. build.sh builds it; fuzz/build.sh applies
# its series. It needs git and the network.
#
#     checkout.sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
tag=$(cat "$here/JELLYFIN_TAG")
work="$here/build/jellyfin-ffmpeg"

if [ ! -d "$work/.git" ]; then
    git clone --filter=blob:none https://github.com/jellyfin/jellyfin-ffmpeg "$work"
fi
git -C "$work" fetch --quiet --tags
git -C "$work" checkout --quiet --force "$tag"
# The series as jellyfin ships it, then ours after it.
git -C "$work" checkout --quiet -- debian/patches
for patch in "$here"/patches/*.patch; do
    cp "$patch" "$work/debian/patches/"
    basename "$patch" >> "$work/debian/patches/series"
done
echo "jellyfin-ffmpeg $tag with $(ls "$here"/patches/*.patch | wc -l) patches in $work"
