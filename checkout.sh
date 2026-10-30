#!/bin/sh
# Checks out jellyfin-ffmpeg at the tag JELLYFIN_TAG names, which must be the commit JELLYFIN_COMMIT
# names, into build/jellyfin-ffmpeg, with these patches added after jellyfin's own
# debian/patches/series, and builder-patches/ applied to jellyfin's builder, so that every source its
# images take is named exactly and checked. build.sh builds it; fuzz/build.sh applies
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
# A tag can be moved; the commit it named when it was chosen cannot.
if [ "$(git -C "$work" rev-parse HEAD)" != "$(cat "$here/JELLYFIN_COMMIT")" ]; then
    echo "jellyfin-ffmpeg's $tag is $(git -C "$work" rev-parse HEAD), not JELLYFIN_COMMIT's $(cat "$here/JELLYFIN_COMMIT")" >&2
    exit 1
fi
# jellyfin's builder with ours applied: it pins what jellyfin's leaves to the day of the build.
git -C "$work" checkout --quiet -- builder
for patch in "$here"/builder-patches/*.patch; do
    git -C "$work" apply "$patch"
done
# The series as jellyfin ships it, then ours after it.
git -C "$work" checkout --quiet -- debian/patches
for patch in "$here"/patches/*.patch; do
    cp "$patch" "$work/debian/patches/"
    basename "$patch" >> "$work/debian/patches/series"
done
echo "jellyfin-ffmpeg $tag with $(ls "$here"/patches/*.patch | wc -l) patches and $(ls "$here"/builder-patches/*.patch | wc -l) to its builder in $work"
