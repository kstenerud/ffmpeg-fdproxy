#!/bin/sh
# Builds this repository's committed HEAD on another machine, by build.sh there, and fetches the
# tarball and its ffmpeg and ffprobe into OUT_DIR/bin-ARCH, as build.sh does locally.
#
#     tools/remote_build.sh [-f] SSH_TARGET ARCH OUT_DIR
#
# SSH_TARGET is what ssh takes ("-i key user@host" works, quoted as one argument); ARCH is amd64
# or arm64. The host needs git, Docker and the network. Each build is kept on the host under
# ~/ffstreamer-builds/<commit>-<arch>/, so asking again for a commit already built fetches it
# without building; -f builds it again. Only what is committed is built: a tree with uncommitted
# changes to the patches, src/ or the build scripts is refused, since its build would carry the
# commit's name and not be the commit.
set -eu
force=0
if [ "${1:-}" = -f ]; then force=1; shift; fi
if [ $# -ne 3 ] || { [ "$2" != amd64 ] && [ "$2" != arm64 ]; }; then
    echo "usage: $0 [-f] SSH_TARGET amd64|arm64 OUT_DIR" >&2
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
kept="ffstreamer-builds/$commit-$arch"
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
rm -rf "$out/bin-$arch" && mkdir -p "$out/bin-$arch"
tar -xJf "$out/$tarball" -C "$out/bin-$arch"
echo "$commit" > "$out/bin-$arch/COMMIT"
echo "fetched $out/$tarball; ffmpeg and ffprobe of $commit in $out/bin-$arch"
