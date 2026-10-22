# Measurements, 2026-10-22

Measurements for the protocol's open choices. They don't change PROTOCOL.md, which stays at draft 1
until the author says to revise it.

## Transport: pipes against shared memory (`transport.c`)

**Setup:**
- A 1.2 GB file is held in the page cache, so no disk is measured, only the transport.
- A server (the node's side) serves blocks, and a client (FFmpeg's side) copies each block out
  32 KiB at a time, as FFmpeg's I/O layer asks.
- Each row is the better of two runs in a shared sandbox, so expect ±30% noise.

**Modes:**
- **direct:** FFmpeg reads the file itself.
- **pipe:** the server reads into its buffer and writes the block to a pipe.
- **splice:** the server moves the block from the file into the pipe without copying it through its
  own memory.
- **shm:** the server reads into shared memory, and only a 16-byte answer goes through the pipe.

| Mode | Block | Pipe buffer | MB/s | CPU-s per GB |
| --- | --- | --- | --- | --- |
| direct | — | — | ~4,300 | 0.23 |
| pipe | 1 MiB | 64 KiB (default) | ~490 | 1.9 |
| pipe | 1 MiB | 1 MiB | ~940 | 1.4 |
| splice | 1 MiB | 1 MiB | ~1,560 | 0.69 |
| splice | 4 MiB | 1 MiB | ~2,000 | 0.58 |
| shm | 1 MiB | — | ~2,100 | 0.43 |
| shm | 4 MiB | — | ~1,940 | 0.51 |

The full output, both runs, is in `transport-results.txt`.

## Prefetching (`prefetch_sim.py`, `trace.sh`)

**Setup:**
- FFmpeg's real reads of its input (offsets, lengths, and its processing time between reads),
  traced with strace for every run shape a media server uses: `trace.sh FFMPEG_DIR MEDIA_DIR` writes
  them to `traces/`, and `prefetch_sim.py MEDIA_DIR` replays them.
- Each trace is replayed through 1 MiB blocks, with the last four kept, and a disk that serves one
  request at a time.
- **Busy** puts another stream's 1 MiB read before each of ours.
- The full grid (three block sizes, three disks, idle and busy) is in `prefetch-all.md`. The summary
  is `prefetch-summary.md`.

Seconds, 1 MiB blocks, NAS profile (1 ms latency, 4 ms seek, 110 MB/s):

| Run | Busy | No prefetch | Prefetch when sequential | Prefetch always | Wasted blocks (sequential / always) |
| --- | --- | --- | --- | --- | --- |
| Read-through | no | 14.72 | 11.49 | 11.48 | 1 / 1 |
| Read-through | yes | 20.30 | 12.12 | 12.09 | 1 / 1 |
| Prepare (remux) | yes | 11.43 | 8.93 | 8.90 | 1 / 1 |
| Chunk, copy | no | 0.19 | 0.17 | 0.17 | 1 / 3 |
| Chunk, copy | yes | 0.34 | 0.32 | 0.34 | 1 / 3 |
| Chunk, sound | yes | 0.81 | 0.61 | 0.63 | 1 / 3 |
| Probe, MP4 index at end | yes | 0.07 | 0.07 | **0.10** | 0 / 2 |

## Films whose tracks are stored one after another (`interleave.py`)

**Setup:**
- `interleave.py --bin DIR --work DIR` makes five-minute MP4s from FFmpeg's test sources: 2 Mb/s
  video, N stereo AAC tracks at 128 kb/s and a subtitle track, each track in one region of its own
  (`-chunk_duration 2e9 -chunk_size 2e9 -max_interleave_delta 0`), for N of 2, 9, 15 and 23 (4, 11,
  17 and 25 tracks, 85 to 190 MB); and the 11-track film interleaved, as a muxer writes it.
- Each run shape a media server makes goes through `test/refparent.py`, which counts the bytes it
  serves: a probe, a read-through (the video copied, one track's loudness), a prepare copying every
  track, a chunk of video, a chunk of the last sound track, and the subtitle track.
- The figures are bytes served over the film's size; "timeout" is past 180 seconds.
- On an amd64 workstation, the patched jellyfin-ffmpeg 8.1.2-5, 2026-10-22.

| Film | Tracks | Run | 1 MiB × 8 | 1 MiB × 24 | 1 MiB × 32 | 1 MiB × 48 | 256 KiB × 64 | 256 KiB × 128 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| one region each | 4 | probe | 0.10 | 0.10 | 0.10 | 0.10 | 0.08 | 0.08 |
| one region each | 4 | read-through | 1.04 | 1.02 | 1.02 | 1.02 | 1.01 | 1.01 |
| one region each | 4 | prepare, every track | 11.33 | 1.07 | 1.04 | 1.04 | 1.03 | 1.03 |
| one region each | 11 | probe | 24.66 | 24.66 | 14.10 | 0.33 | 22.99 | 13.46 |
| one region each | 11 | read-through | 25.33 | 25.33 | 14.77 | 0.99 | 23.67 | 14.13 |
| one region each | 11 | prepare, every track | | timeout | 166.95 | 1.15 | timeout | 14.32 |
| one region each | 11 | chunk, video copy | | 24.68 | 14.13 | 0.36 | 23.01 | 13.47 |
| one region each | 11 | subtitles | | 24.67 | 14.11 | 0.33 | 23.00 | 13.46 |
| one region each | 17 | probe | | 5.96 | 0.50 | 0.31 | 11.99 | 0.26 |
| one region each | 17 | read-through | | 6.51 | 1.05 | 0.85 | 12.53 | 0.80 |
| one region each | 17 | prepare, every track | | timeout | 2.42 | 1.60 | timeout | 1.46 |
| one region each | 25 | probe | | 8.15 | 0.34 | 0.33 | 6.47 | 0.31 |
| one region each | 25 | read-through | | 8.58 | 0.78 | 0.76 | 6.90 | 0.74 |
| one region each | 25 | prepare, every track | | timeout | timeout | timeout | timeout | timeout |
| interleaved | 11 | probe | | 0.02 | 0.02 | 0.02 | 0.01 | 0.01 |
| interleaved | 11 | read-through | | 1.02 | 1.02 | 1.02 | 1.00 | 1.00 |
| interleaved | 11 | prepare, every track | | 1.02 | 1.02 | 1.02 | 1.00 | 1.00 |

The other runs (chunks of sound, the 17- and 25-track films' chunks and subtitles) follow their
film's probe. The 1 MiB × 8 column is the default and the old formula's size for few streams.

**What this shows:**
- **An interleaved film never thrashes,** at any size measured: about once through, and a probe a
  few percent of the film.
- **A film whose tracks are stored one after another thrashes when the cache is short of what
  FFmpeg sweeps, and it falls off a cliff, not a slope.** FFmpeg reads a few seconds of every track
  as it opens a film, whatever the run maps, and then goes on reading from every mapped track's
  region. The trace (`refparent.py --trace`) of the 11-track probe at 32 blocks reads blocks 71 to
  104 in turn, over and over: 34 blocks swept with 32 kept, so the least recently used block is
  always the next one wanted, and every read misses.
- **What a film needs follows its layout in bytes, not its count of tracks, and what counts is the
  bytes kept, not the blocks.** The 11-track film needs more than 32 MiB where the 17- and 25-track
  ones need 32; and 128 blocks of 256 KiB do as 32 of 1 MiB do on every film.
- **A prepare that maps every track needs about two blocks for each, and no size measured is
  enough for all:** the 25-track prepare was served over 20 GB of a 190 MB film at 48 blocks of 1
  MiB before it was stopped.
- **The old formula, 1.5 × the streams the run reads + 2, is wrong twice:** a run reads every track
  while it opens a film, whatever it maps, and two blocks a track are the least.

## Pipe size (`refparent.py --pipe-bytes`)

A read-through copying the video of the suite's 119 MB `film.mkv` through the reference parent, seven
runs each, on an amd64 workstation with a fuzzer running beside it:

| Data pipe | Milliseconds (median) | MB/s |
| --- | --- | --- |
| 8 KiB (past the user's pipe memory) | 388 | ~305 |
| 64 KiB (the default) | 239 | ~500 |
| 1 MiB | 196 | ~605 |

At 8 KiB the protocol reads at half the speed of 1 MiB, still well past what a disk or a NAS gives a
film, so the fallback costs processor time more than a run's speed.

## Choosing a block to evict: least recently used against random

The same films and runs through two development builds of FFmpeg (no external libraries), one keeping
the least recently used block, as the patch does, one evicting a random block other than the one
being read. Bytes served over the film's size, LRU / random:

| Film | Tracks | Run | 1 MiB × 24 | 1 MiB × 32 | 1 MiB × 48 | 256 KiB × 128 |
| --- | --- | --- | --- | --- | --- | --- |
| one region each | 4 | prepare, every track | 1.07 / 1.44 | 1.04 / 1.31 | 1.04 / 1.14 | 1.03 / 1.26 |
| one region each | 11 | probe | 24.66 / 9.53 | 14.10 / 2.09 | 0.33 / 0.33 | 13.46 / 0.92 |
| one region each | 11 | read-through | 25.33 / 10.25 | 14.77 / 2.77 | 0.99 / 0.98 | 14.13 / 1.59 |
| one region each | 11 | prepare, every track | timeout / timeout | 166.95 / 148.15 | 1.15 / 2.50 | 14.32 / 8.64 |
| one region each | 17 | probe | 5.96 / 1.83 | 0.50 / 0.79 | 0.31 / 0.31 | 0.26 / 0.31 |
| one region each | 17 | prepare, every track | timeout / 868.91 | 2.42 / 13.13 | 1.60 / 2.96 | 1.46 / 2.74 |
| one region each | 25 | probe | 8.15 / 2.10 | 0.34 / 0.67 | 0.33 / 0.40 | 0.31 / 0.47 |
| one region each | 25 | prepare, every track | timeout / timeout | timeout / timeout | timeout / timeout | timeout / timeout |
| interleaved | 11 | every run | as LRU | as LRU | as LRU | as LRU |

**Random eviction is no fix.** It softens the cliff where the cache is short of the opening sweep
(24.66 to 9.53), but costs up to five times as much where least recently used fits (the 17-track
prepare, 2.42 to 13.13), and neither finishes the 25-track prepare.

**Still open:** a cache sized from the film's tracks (at least two blocks each, so the parent must
know how many the film has), or a policy that knows FFmpeg reads from one place per track (a block
or two kept per region read). Until then a parent's budget kills a run that thrashes, so such a
film fails to prepare rather than read gigabytes.
