#!/bin/sh
# Points apt in a TEST_IMAGE container at Debian's archive as it stood on one day, then updates, so
# every run of the claims test, the fuzzer's build and the upstream check installs the same packages
# (the owner, 2026-10-30: "get everything nailed down"). The image has no CA certificates yet, so the
# TLS peer is not checked; apt still checks the signed Release files against the image's Debian
# keyring and each package's hash against them. A plain `--snapshot` left the live archive in use.
#
#     sh /pin/debian_snapshot.sh        (inside the container, the tools directory mounted at /pin)
set -eu
date=20261029T000000Z
cat > /etc/apt/sources.list.d/debian.sources <<SOURCES
Types: deb
URIs: https://snapshot.debian.org/archive/debian/$date
Suites: trixie trixie-updates
Components: main
Signed-By: /usr/share/keyrings/debian-archive-keyring.pgp
Check-Valid-Until: no

Types: deb
URIs: https://snapshot.debian.org/archive/debian-security/$date
Suites: trixie-security
Components: main
Signed-By: /usr/share/keyrings/debian-archive-keyring.pgp
Check-Valid-Until: no
SOURCES
echo 'Acquire::https::Verify-Peer "false";' > /etc/apt/apt.conf.d/90snapshot-no-ca
apt-get update -qq -o APT::Get::Error-On=any >/dev/null
