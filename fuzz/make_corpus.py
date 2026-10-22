#!/usr/bin/env python3
"""Writes the fuzzer's seeds into fuzz/corpus/: what a parent that keeps to the protocol sends the
harness, which reads a 1,000-byte film in 256-byte blocks straight through and then once more from
its middle; and a few that break it, for the fuzzer to mutate from."""
import os, struct

HELLO, REQUEST, DATA = 1, 3, 5
here = os.path.dirname(os.path.abspath(__file__))
film = bytes(i * 7 % 256 for i in range(1000))


def frame(kind, sequence, body=b""):
    return struct.pack("<bbi", kind, sequence, len(body)) + body


def opening(size=1000, block=256, outstanding=2, version=1, run=7):
    hello = frame(HELLO, 0, struct.pack("<hh", 1, 1))
    request = frame(REQUEST, 1, struct.pack("<hqqiib", version, run, size, block, block, outstanding))
    return hello + request


def data(sequence, index, block=256):
    return frame(DATA, sequence, film[index * block:(index + 1) * block])


seeds = {
    # Blocks 0 to 3 straight through (the protocol prefetches each next one), then block 1 again.
    "whole": opening() + b"".join(data(2 + i, i) for i in range(4)) + data(6, 1),
    "opening-only": opening(),
    "hello-only": frame(HELLO, 0, struct.pack("<hh", 1, 1)),
    "no-shared-version": frame(HELLO, 0, struct.pack("<hh", 2, 3)),
    "terms-outside": opening(block=1 << 30),
    "short-data": opening() + frame(DATA, 2, film[:100]),
    "wrong-number": opening() + data(9, 0),
    "empty-film": opening(size=0),
}
os.makedirs(os.path.join(here, "corpus"), exist_ok=True)
for name, seed in seeds.items():
    with open(os.path.join(here, "corpus", name), "wb") as out:
        out.write(seed)
print(f"{len(seeds)} seeds in fuzz/corpus/")
