#!/bin/bash
# Builds the patched tree, unconfined and without external libraries, for testing the protocol.
set -e
apt-get update -qq >/dev/null
apt-get install -y -qq build-essential nasm pkg-config >/dev/null 2>&1
mkdir -p /build && cd /build
/src/configure --prefix=/out --disable-doc --disable-debug --enable-gpl --disable-autodetect \
  --enable-protocol=ffstreamer >/build/configure.log 2>&1 || { tail -30 /build/configure.log; exit 1; }
grep -q "CONFIG_FFSTREAMER_PROTOCOL 1" config_components.h && echo "ffstreamer: enabled" || { echo "ffstreamer: NOT enabled"; exit 1; }
make -j12 >/build/make.log 2>&1 || { grep -n "error" /build/make.log | head -30; exit 1; }
make install >/dev/null
echo built; /out/bin/ffmpeg -hide_banner -protocols | grep -A50 Input | grep -w ffstreamer
