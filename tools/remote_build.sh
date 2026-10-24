#!/bin/sh
# Builds this repository's committed HEAD on another machine, by build.sh there, and fetches the
# tarball and its ffmpeg and ffprobe into OUT_DIR/bin-ARCH, as build.sh does locally.
#
#     tools/remote_build.sh [-f] [-m MEDIA_DIR] SSH_TARGET ARCH OUT_DIR
#
# SSH_TARGET is what ssh takes ("-i key user@host" works, quoted as one argument); ARCH is amd64
# or arm64. The host needs git, Docker and the network. Each build is kept on the host under
# ~/fdproxy-builds/<commit>-<arch>/, so asking again for a commit already built fetches it
# without building; -f builds it again. Only what is committed is built: a tree with uncommitted
# changes to the patches, src/ or the build scripts is refused, since its build would carry the
# commit's name and not be the commit.
#
# The host is the cache machine: it does the one-time heavy work, and other machines copy what it
# made. With -m it also makes the suite's films there (test/make_media.py with that build's ffmpeg,
# minutes of encoding), keeps them beside the build, and fetches them into MEDIA_DIR.
set -eu
force=0
media=
while :; do
    case "${1:-}" in
        -f) force=1; shift ;;
        -m) media=$2; shift 2 ;;
        *) break ;;
    esac
done
if [ $# -ne 3 ] || { [ "$2" != amd64 ] && [ "$2" != arm64 ]; }; then
    echo "usage: $0 [-f] [-m MEDIA_DIR] SSH_TARGET amd64|arm64 OUT_DIR" >&2
    exit 2
fi
target=$1
arch=$2
here=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$3"
out=$(cd "$3" && pwd)
cd "$here"
if [ -n "$(git status --porcelain -- patches src build.sh checkout.sh JELLYFIN_TAG)" ]; then
    echo "uncommitted changes to what a build reads; commit them first" >&2
    exit 1
fi
commit=$(git rev-parse --short=12 HEAD)
kept="fdproxy-builds/$commit-$arch"
case $arch in amd64) name=linux64 ;; arm64) name=linuxarm64 ;; esac

# shellcheck disable=SC2086 # the target may carry ssh's own options
if [ "$force" = 1 ] || ! ssh $target "ls $kept/*_portable_$name-gpl.tar.xz" >/dev/null 2>&1; then
    echo "building $commit for $arch on $target"
    git archive --format=tar --prefix="$kept/src/" HEAD | ssh $target "rm -rf $kept && tar -xf -"
    # The build runs detached, so a dropped connection does not end it; this waits for its end.
    ssh $target "cd $kept && nohup sh -c 'src/build.sh $arch . > build.log 2>&1; echo \$? > build.exit' \
        > /dev/null 2>&1 &"
    while ! ssh $target "test -f $kept/build.exit"; do sleep 60; done
    status=$(ssh $target "cat $kept/build.exit")
    if [ "$status" != 0 ]; then
        ssh $target "tail -30 $kept/build.log" >&2
        echo "the build on $target failed ($status); its log is $kept/build.log there" >&2
        exit 1
    fi
else
    echo "$commit for $arch is built on $target already"
fi
tarball=$(ssh $target "cd $kept && ls *_portable_$name-gpl.tar.xz")
host=${target##* }
options=${target%"$host"}
# shellcheck disable=SC2086
scp -q $options "$host:$kept/$tarball" "$out/"
# The folder's contents are replaced, not the folder, which a container may have mounted.
mkdir -p "$out/bin-$arch" && find "$out/bin-$arch" -mindepth 1 -delete
tar -xJf "$out/$tarball" -C "$out/bin-$arch"
echo "$commit" > "$out/bin-$arch/COMMIT"
echo "fetched $out/$tarball; ffmpeg and ffprobe of $commit in $out/bin-$arch"

if [ -n "$media" ]; then
    # shellcheck disable=SC2086
    if [ "$force" = 1 ] || ! ssh $target "test -f $kept/media/.made"; then
        echo "making the suite's films on $target"
        ssh $target "cd $kept && rm -rf media && python3 src/test/make_media.py --ffmpeg bin-$arch/ffmpeg media \
            && touch media/.made"
    fi
    mkdir -p "$media"
    # shellcheck disable=SC2086
    scp -q $options "$host:$kept/media/*" "$media/"
    echo "fetched the suite's films of $commit into $media"
fi
