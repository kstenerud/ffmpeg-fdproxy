#!/usr/bin/env python3
"""Writes the fuzzer's seeds into fuzz/corpus/, in the layout fuzz/ffstreamer_fuzzer.c reads: a
16-byte header, a count of 4-byte operations and the operations, a count of answer actions and the
actions, then (for a raw parent) the bytes the parent sends as they are.

Each seed starts the fuzzer in one of the modes the harness can reach: a responsive parent that
keeps to the protocol, answering by dribs, slowly, wrongly, or not at all; reads of every size and
seeks of every whence, through the protocol, FFmpeg's URL layer or avio; a pipe of requests that is
tiny, full, unread or closed; the interrupt by count; failing system calls; second opens that share
or refuse descriptors; terms and HELLOs that break the rules; and raw parents, whose bytes are
whatever the input says."""
import os, struct

HELLO, REQUEST, DATA = 1, 3, 5
here = os.path.dirname(os.path.abspath(__file__))
film = bytes(i * 7 % 256 for i in range(1000))

# Header byte 0's flags.
RESPONSIVE, AVIO, TINY_BACK, RESOLVE_CLOSE, NONBLOCK, RAW_STALL = 1, 2, 4, 8, 32, 64


def header(flags=RESPONSIVE, block_exp=8, size=1000, size_kind=0, outstanding=2, cache=2,
           chunk_exp=16, interrupt_at=0, resolve=3, perturb=0, longer_reads=False, hello=0,
           inject=0, run=6):
    """The 16 header bytes (see read_header_bytes() in the harness)."""
    h = bytearray(16)
    h[0] = flags
    h[1] = block_exp
    h[2] = size & 0xff
    h[3] = (size >> 8) & 0x3f | size_kind << 6
    h[4] = (outstanding - 1) | perturb << 1 | (0x80 if longer_reads else 0)
    h[5] = cache - 2
    h[6] = chunk_exp
    h[7], h[8] = interrupt_at & 0xff, interrupt_at >> 8
    h[9] = resolve - 1
    h[13] = hello
    h[14] = inject
    h[15] = run
    return bytes(h)


def op(code, a=0, b=0, c=0):
    return bytes([code, a, b, c])


def read(size_hi=0xff, size_lo=0xff, shift=0, via_url=False):
    return op(0, size_hi, size_lo, shift | (4 if via_url else 0))


def read_through(size_hi=0xff, size_lo=0xff, shift=0):
    return op(3, size_hi, size_lo, shift)


def seek(whence, kind, value, via_url=False):
    """whence: 0 SET, 1 CUR, 2 END, 3 AVSEEK_SIZE, 4 invalid; kind: see seek_offset()."""
    return op(4, whence | (0x80 if via_url else 0), kind, value)


def second_open(variant, preset=2):
    return op(6, variant, preset)


def parent(action, value=0):
    """0 close the pipe of requests, 1 fill it and stop reading for value slices (0: for ever),
    2 stop reading it, 3 close the pipe to FFmpeg, 4/5 set O_NONBLOCK on FFmpeg's read/write end,
    6 read again."""
    return op(8, action, value)


INTERRUPT = op(10)
SIZE = op(11)


def seed(head, ops=(), actions=b"", raw=b""):
    return head + bytes([len(ops)]) + b"".join(ops) + bytes([len(actions)]) + actions + raw


# Answer actions (queue_data() in the harness): the low nibble picks, the high one tunes.
OK, DRIBBLE, DELAY, WRONG_NUMBER, WRONG_TYPE, WRONG_LENGTH, TRUNCATE, CLOSE, STALL, HEADER_ONLY = \
    0, 6, 7, 8, 9, 10, 11, 12, 13, 14


def frame(kind, sequence, body=b""):
    return struct.pack("<bbi", kind, sequence, len(body)) + body


def raw_opening(size=1000, block=256, outstanding=2, version=1, run=7):
    hello = frame(HELLO, 0, struct.pack("<hh", 1, 1))
    request = frame(REQUEST, 1, struct.pack("<hqqiib", version, run, size, block, block, outstanding))
    return hello + request


def raw_data(sequence, index, block=256):
    return frame(DATA, sequence, film[index * block:(index + 1) * block])


RAW = 0
# One byte from each of blocks 0 to 7, then back down from 6: past a small cache's capacity, and
# then reusing what least-recently-used eviction keeps.
blocks = list(range(8)) + list(range(6, -1, -1))
cycling = [o for i in blocks for o in (seek(0, 1, i * 3), read(0, 0))] + [seek(1, 0, 100),
                                                                          read(0, 3), SIZE]

seeds = {
    # A parent that keeps to the protocol, and the old harness's script: straight through, from
    # the middle, and seeks at the edges of the arithmetic.
    "responsive-default": seed(header()),
    "responsive-avio": seed(header(RESPONSIVE | AVIO), [read_through(0x7f, 0xff), seek(0, 8, 128),
                                                        read(1, 0), seek(3, 0, 0), seek(1, 0, 0),
                                                        read(0, 77)]),
    "responsive-url-layer": seed(header(), [read(0, 99, 0, True), seek(2, 0, 120, True),
                                            read(0, 99, 0, True), SIZE]),
    # Caches of 6 to 40 blocks of 16 bytes, read at strides: least-recently-used eviction past 3 blocks.
    "lru-many-blocks": seed(header(block_exp=4, cache=6, outstanding=1), cycling),
    "lru-two-outstanding": seed(header(block_exp=4, cache=7, outstanding=2), cycling),
    "lru-large-cache": seed(header(block_exp=4, cache=40, outstanding=2), cycling * 2),
    # Blocks of 1 MiB over a 2 MB film: whole DATA frames far larger than a pipe.
    "large-blocks": seed(header(block_exp=20, size=16000, size_kind=1, cache=3),
                         [read_through(0xff, 0xff, 2), seek(0, 1, 1), read(0xff, 0xff, 3)]),
    "odd-block-size": seed(header(block_exp=8 | 0x40, size=7000, cache=5),
                           [read_through(0, 100), seek(2, 3, 255), read_through(0, 255)]),
    # A parent that sends a byte at a time, and the interrupt fired in the middle of it.
    "dribble": seed(header(chunk_exp=0), [read_through(0, 200)]),
    "dribble-interrupted": seed(header(chunk_exp=0, interrupt_at=300), [read_through(0, 200)]),
    "interrupt-during-open": seed(header(interrupt_at=3)),
    "interrupt-op": seed(header(), [read(0, 50), INTERRUPT, read(0xff, 0xff), read(0, 1)]),
    # Answers that come slowly, wrongly, short, or never; a stall ended by the interrupt or by the
    # parent closing its pipe.
    "slow-answers": seed(header(), [read_through(0, 128)], bytes([DELAY | 0x30, OK, DELAY | 0x70])),
    "wrong-number": seed(header(), [read(0, 10), read(0, 10), seek(0, 0, 128), read(0, 10)],
                         bytes([WRONG_NUMBER])),
    "wrong-type": seed(header(), [read_through(0, 255)], bytes([OK, WRONG_TYPE | 0x40])),
    "wrong-length": seed(header(), [read_through(0, 255)], bytes([OK, WRONG_LENGTH | 0x10])),
    "wrong-length-short": seed(header(), [read_through(0, 255)], bytes([OK, OK, WRONG_LENGTH])),
    "truncated": seed(header(), [read_through(0, 255)], bytes([OK, TRUNCATE | 0x80])),
    "parent-closes": seed(header(), [read_through(0, 255)], bytes([OK, OK, CLOSE])),
    "stall-interrupted": seed(header(resolve=2), [read_through(0, 255)], bytes([OK, STALL])),
    "stall-closed": seed(header(RESPONSIVE | RESOLVE_CLOSE), [read_through(0, 255)],
                         bytes([HEADER_ONLY])),
    # The pipe of requests: a page, filled and unread for a while or for ever, or closed (EPIPE).
    "back-full-resumes": seed(header(RESPONSIVE | TINY_BACK, block_exp=2, cache=2),
                              [read(0, 4), parent(1, 4), read_through(0, 4)]),
    "back-full-for-ever": seed(header(RESPONSIVE | TINY_BACK, block_exp=2),
                               [read(0, 4), parent(1, 0), read_through(0, 4)]),
    "back-full-closed": seed(header(RESPONSIVE | TINY_BACK | RESOLVE_CLOSE, block_exp=2),
                             [read(0, 4), parent(1, 0), read_through(0, 4)]),
    "back-unread": seed(header(block_exp=2), [read(0, 4), parent(2), read_through(0, 4)]),
    "back-closed": seed(header(), [read(0, 10), parent(0), read_through(0, 255), read(0, 1)]),
    "to-ffmpeg-closed": seed(header(), [read(0, 10), parent(3), read_through(0, 255)]),
    "nonblocking-midway": seed(header(block_exp=4), [read(0, 10), parent(4), parent(5),
                                                     read_through(0, 255)]),
    "nonblocking-handover": seed(header(RESPONSIVE | NONBLOCK)),
    # System calls that fail: EINTR and EAGAIN retried, the rest faults.
    "syscalls-fail": seed(header(inject=0x80 | 31), [read_through(0, 100)]),
    "syscalls-fail-later": seed(header(inject=0x80 | 0x20 | 20), [read_through(0, 100)]),
    "syscalls-fail-often": seed(header(inject=0x80 | 0x40), [read_through(0, 20)]),
    # Second opens: sharing the live session's pipes (EBUSY), refused for their arguments, and on
    # fresh pipes, whose opening is empty, a HELLO, good terms with a DATA, or bad terms.
    "second-opens": seed(header(), [read(0, 10)] + [second_open(v, 2) for v in range(12)] +
                         [read(0, 10)]),
    "second-opens-fresh": seed(header(), [second_open(2, p) for p in range(4)] + [read(0, 9)]),
    "second-open-after-close": seed(header(size=0), [second_open(1), second_open(2, 2)]),
    # Terms that break the rules (perturbations 5 to 15) and HELLOs that share no version.
    **{f"terms-{p}": seed(header(perturb=p)) for p in range(5, 16)},
    **{f"hello-{h}": seed(header(hello=h)) for h in (4, 5, 6, 7)},
    "longest-read": seed(header(longer_reads=True)),
    "empty-film": seed(header(size=0), [read(0, 1), seek(2, 0, 127), read(0, 1)]),
    # A size at the top of what REQUEST may give: seeks and reads at the end of the arithmetic.
    "size-at-the-bound": seed(header(size=0, size_kind=3, block_exp=12),
                              [seek(2, 0, 127), read(0, 9), seek(0, 3, 1), read(0, 9), SIZE,
                               seek(1, 5, 0), seek(2, 5, 0), seek(1, 6, 0)]),
    "size-near-the-bound": seed(header(size=4095, size_kind=3, block_exp=12),
                                [seek(2, 0, 100), read_through(0xff, 0xff)]),
    # Raw parents: whatever bytes the input says, delivered whole or by dribs, then closed or
    # stalled (ended by the interrupt).
    "raw-whole": seed(header(RAW), raw=raw_opening() + b"".join(raw_data(2 + i, i) for i in range(4))
                      + raw_data(6, 1)),
    "raw-dribble-stall": seed(header(RAW | RAW_STALL, chunk_exp=0),
                              raw=raw_opening() + raw_data(2, 0)),
    "raw-opening-only": seed(header(RAW), raw=raw_opening()),
    "raw-no-shared-version": seed(header(RAW), raw=frame(HELLO, 0, struct.pack("<hh", 2, 3))),
    "raw-terms-outside": seed(header(RAW), raw=raw_opening(block=1 << 30)),
    "raw-short-data": seed(header(RAW), raw=raw_opening() + frame(DATA, 2, film[:100])),
    # The long fuzz found rounding up to whole blocks overflowing at a size near INT64_MAX
    # (2026-10-22).
    "raw-size-near-int64-max": seed(header(RAW), raw=raw_opening(size=2**63 - 1) + raw_data(2, 0)),
    "raw-size-at-the-bound": seed(header(RAW), raw=raw_opening(size=2**62) + raw_data(2, 0)),
}
corpus = os.path.join(here, "corpus")
os.makedirs(corpus, exist_ok=True)
for stale in os.listdir(corpus):
    os.remove(os.path.join(corpus, stale))
for name, data in seeds.items():
    with open(os.path.join(corpus, name), "wb") as out:
        out.write(data)
print(f"{len(seeds)} seeds in fuzz/corpus/")
