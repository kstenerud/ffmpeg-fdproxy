#!/bin/bash
# Builds the patched tree, unconfined and without external libraries, for testing the protocol.
# /build persists between runs, so a rebuild compiles only what changed.
set -e
apt-get update -qq >/dev/null
apt-get install -y -qq build-essential nasm pkg-config >/dev/null 2>&1
mkdir -p /build && cd /build
if [ ! -f config_components.h ] || [ /src/configure -nt config_components.h ]; then
  /src/configure --prefix=/out --disable-doc --disable-debug --enable-gpl --disable-autodetect \
    > /build/configure.log 2>&1 || { tail -30 /build/configure.log; exit 1; }
fi
grep -q "CONFIG_FFSTREAMER_PROTOCOL 1" config_components.h && echo "ffstreamer: enabled" || { echo "ffstreamer: NOT enabled"; exit 1; }
if ! make -j12 > /build/make.log 2>&1; then echo "MAKE FAILED"; grep -n -B2 -A6 "error" /build/make.log | head -60; exit 1; fi
if ! make install > /build/install.log 2>&1; then echo "INSTALL FAILED"; tail -20 /build/install.log; exit 1; fi
echo built; /out/bin/ffmpeg -hide_banner -protocols 2>/dev/null | grep -w ffstreamer
