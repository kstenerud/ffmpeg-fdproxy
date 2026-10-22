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
  traced with strace for every run shape a media server uses. The traces are in `traces/`.
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
