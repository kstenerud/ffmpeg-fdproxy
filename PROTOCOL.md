# The read protocol: draft 1, for discussion

**Status:** draft 1, 2026-10-22. Nothing here is decided. Each choice that is open says so, gives its
options, and gives a recommendation (**R**). After discussion it goes through several independent
audits before any patch is written.

## 1. What it is for

A parent process (here, a media server's node) runs FFmpeg to read a film. Instead of FFmpeg opening
the film, the parent hands FFmpeg descriptors at its start and serves every read FFmpeg makes, by
byte offset. Then:

- **FFmpeg never touches the storage.** A dead network share blocks a thread of the parent, which
  knows and decides; never FFmpeg, in the kernel, where it cannot be killed.
- **Every read is the parent's to schedule:** to order against other work on the same disk, pace,
  share between consumers, read ahead, or refuse.
- **FFmpeg needs no path to the film and no socket.** Its confinement can stay as tight as it is
  today (Landlock with no media file granted; seccomp refusing `socket` and `socketpair`).
- **The parent knows what was read.** A run whose input broke can never pass as complete.

It reads only. Writing FFmpeg's outputs this way is out of scope for version 1. Its message types are
reserved (§5.6) so that a later version can add it.

## 2. Requirements

Each comes from a test or an audit: the wargame of an LD_PRELOAD shim speaking the same kind of
channel (an earlier wargame, not published), and
the tests before it.

| # | Requirement | Why |
| --- | --- | --- |
| Q1 | Seekable: any offset, any time, and the size | Probes read an MP4's index at its end; chunk runs seek; pipes can't (tested) |
| Q2 | No socket created by FFmpeg, and no filesystem path | FFmpeg's seccomp refuses sockets (N1); Landlock grants no media file |
| Q3 | Never desynchronises: an answer is always matched to its request, or the channel is dead for good | A late answer taken for the next one gave 559 of 35,413 frames, exit 0 (tested) |
| Q4 | Every failure of the channel or the storage fails the run visibly | FFmpeg exits 0 with a partial result when its input breaks (tested, more than once) |
| Q5 | FFmpeg stays interruptible while it waits: a stop ends it cleanly, with its outputs' trailers written | A read waiting on a pipe ignored SIGINT and SIGTERM (tested) |
| Q6 | The parent owns every timeout. FFmpeg's side never gives up on its own | A side that gives up desynchronises, or turns a slow disk into a failed film |
| Q7 | Bounded: no message, allocation or number of open inputs is unbounded, on either side | A hostile file asks for ~2 GiB reads; a hostile concat reopens without end (tested) |
| Q8 | Performant: the parent sees reads large enough to schedule well, and the overhead is small beside decoding | One scheduled read per 32 KiB made a 6 s chunk take 19.5 s under load (simulated) |
| Q9 | Several inputs in one run never block or corrupt each other | Two inputs on one channel crossed bytes (tested) |
| Q10 | Versioned: an FFmpeg and a parent of different versions refuse each other plainly | The binaries and the node are released separately |
| Q11 | General: nothing in FFmpeg's side knows the parent, so it can be offered upstream | Smaller fork; others have the same need |

## 3. Where it sits in FFmpeg

**A URL protocol** in libavformat: `libavformat/<name>.c`, registered in `protocols.c` and its
Makefile, about 300 lines. The FFmpeg CLI and every demuxer are untouched. An input is named
`<name>:` with its descriptors as protocol options, the way the `fd:` protocol takes `-fd`.

**A second, small patch to fftools.** When an input fails with an I/O error, FFmpeg exits non-zero,
where today it finishes the outputs and exits 0. This is defence in depth: the parent judges success
from what it served (Q4) whatever FFmpeg says.

**Open: the protocol's name.**
- `fdpair:` says what it is, and suits upstream.
- `ipcread:` says what it does.

**R:** a generic name, `fdpair:`, unless the design moves away from a descriptor pair.

## 4. The choices to make

### 4.1 Transport

| Option | For | Against |
| --- | --- | --- |
| **a. Two pipes per input**, one each way, created by the parent and inherited | Works on every Unix and on Windows (anonymous pipes); no socket anywhere; simplest | A pipe is a byte stream, so framing is the protocol's job; data copies through the kernel twice |
| **b. A `SOCK_SEQPACKET` socketpair per input**, created by the parent and inherited | Message boundaries kept by the kernel, so a frame arrives whole or not at all; one descriptor | Still a socket inside FFmpeg, though one it didn't create (seccomp forbids creating, not using); Linux-only semantics; harder to argue N1 still holds |
| **c. Shared memory** (a `memfd` the parent maps and hands down) **for data, a pipe for control** | One copy fewer: the parent reads the disk straight into memory FFmpeg reads from | Much more to get right: buffer ownership, recycling, a hostile FFmpeg scribbling on shared pages; worth it only if copying shows in profiles |

**R: a.** Two pipes, with framing as the protocol's job (§5). Measured: pipes carry well over a
GB/s at 1 MiB writes, above any disk the node reads. Keep (c) in reserve, behind the version number,
if profiles ever show the copies.

### 4.2 Channels: one per input, or one per run with handles

**R: one channel per input.**
- The parent knows a run's inputs when it builds the command, so it hands each `-i` its own pair.
- One input waiting on a dead share can't block another (Q9). No two threads ever share a channel's
  framing (W2).
- An attempt to open the same channel twice (a demuxer reopening its input, a hostile concat) is
  refused, loudly (Q7).

### 4.3 Who decides the size of a read

FFmpeg's I/O layer asks for 32 KiB at a time. Its packets can ask for 24 MB in one read.

| Option | How |
| --- | --- |
| **a. FFmpeg's side reads in blocks** | It asks for a fixed block (1 MiB, an option) aligned to the block size, keeps the last block or two, and answers FFmpeg's small reads from them. The parent sees 1 MiB reads |
| **b. The parent reads ahead** | It answers the 32 KiB asked, but reads 1 MiB from the disk and keeps the rest for the next request. Every 32 KiB still costs a round trip of the channel |
| **c. Both** | Blocks on FFmpeg's side, and read-ahead on the parent's for sequential runs |

**R: c.**
- FFmpeg's side asks in blocks: a round trip per MiB, not per 32 KiB.
- The parent reads ahead what its scheduler and the run's pattern justify: one more block for a
  sequential read, none for a probe.
- The block size is negotiated (§5.2): the parent says its preferred block and its largest answer.

### 4.4 Requests in flight

| Option | For | Against |
| --- | --- | --- |
| **a. One at a time per channel** | Simplest; desync impossible | Disk latency and processing never overlap |
| **b. At most two:** the block FFmpeg needs, and the next one prefetched while FFmpeg works | Overlap for sequential reads; still bounded | A seek makes the prefetch worthless. Its answer is still read and dropped, matched by sequence number, never abandoned |

**R: b, with the limit negotiated** (1 or 2). A prefetch is only a hint: the parent may answer it at
the back of its queue, or with "not now" (§5.4). And since the parent reads ahead too (4.3), version 1
could start with one in flight and measure.

### 4.5 Waiting, stopping and timeouts

**The rule: FFmpeg's side never times out a request (Q6).**
- It waits for the answer in short polls (100 ms), and between polls it checks FFmpeg's interrupt
  callback.
- When FFmpeg is asked to stop (`q`, SIGINT, SIGTERM, or the API's interrupt), the read returns
  `AVERROR_EXIT`. FFmpeg ends its outputs cleanly, with trailers written (Q5).
- The request is left behind, so the channel is closed, not reused.

The parent owns every time limit:
- It ends a run by answering the waiting read with an error, then, if that doesn't end it, with
  SIGKILL.
- It never treats a quiet channel as dead: a paused or slow FFmpeg is quiet for hours.
- It detects FFmpeg's end by its exit, not by end-of-file on a pipe.

**Open:** should the protocol honour FFmpeg's own `-rw_timeout`? **R: no.** The parent owns time, and
two timers disagree.

### 4.6 Errors

An answer's status says what happened, and FFmpeg's side maps each to its error:

| Status | Means | FFmpeg sees |
| --- | --- | --- |
| `OK` | Data follows, between 1 byte and the length asked | the bytes |
| `EOF` | The offset is at or past the end | `AVERROR_EOF` |
| `IO` | The storage failed the read (a dead disk, a bad sector) | `AVERROR(EIO)` |
| `CHANGED` | The file changed since it was opened: replaced, truncated, grown | `AVERROR(ESTALE)` |
| `STOP` | The parent is ending the run | `AVERROR_EXIT`: FFmpeg ends cleanly |
| `REFUSED` | The request broke the protocol | `AVERROR(EPROTO)`, and the channel is closed |

The parent records every status other than `OK` and `EOF` as a failure of the run (Q4). It never
answers `IO` for a disk that is only slow: slowness is a wait.

### 4.7 The file's identity

The open's answer carries the file's size and an opaque generation (the parent's choice: device,
inode, size and modification time, hashed). The parent checks the file between reads. When it has
changed, every later read is `CHANGED`. A film half-read from one file and half from its replacement
can't pass as either.

## 5. The wire format (draft)

All integers little-endian. Each side writes whole frames and reads them whole, looping over short
reads and writes and over `EINTR`. A malformed or unexpected frame closes the channel for good, on
either side: there is no resynchronising.

### 5.1 Framing

Every frame starts with the same 16-byte header:

| Field | Bytes | Notes |
| --- | --- | --- |
| magic | 4 | `"FDPR"` |
| type | 2 | §5.3 |
| flags | 2 | 0 in version 1; a set bit not known is an error |
| sequence | 4 | Requests count from 1 on each channel; an answer echoes its request's |
| body length | 4 | The bytes after the header, at most the negotiated maximum |

### 5.2 Opening: HELLO

FFmpeg's side writes `HELLO` as soon as the input is opened:
- protocol versions it speaks, lowest and highest;
- the largest frame it accepts;
- what it wants to read (a short name it was given in the URL, at most 255 bytes, never a path).

The parent answers `WELCOME`:
- the version chosen;
- its block size;
- its largest answer;
- requests in flight allowed;
- the file's size;
- its generation.

Or it answers `REFUSED`, with a reason code. There is no further negotiation.

### 5.3 Frame types

| Type | From | Body |
| --- | --- | --- |
| `HELLO` (1) | FFmpeg | versions, largest frame, name |
| `WELCOME` (2) | parent | version, block, largest answer, in flight, size, generation |
| `READ` (3) | FFmpeg | offset (8), length (4), hint (4): `NEEDED` or `PREFETCH` |
| `DATA` (4) | parent | offset echoed (8), status (4), then 0 to length bytes |
| `CLOSE` (5) | FFmpeg | nothing. The parent answers nothing, and the channel ends |
| `REFUSED` (6) | either | reason (4) |
| 7–15 | — | reserved: writing (§5.6), statistics, keep-alive |

### 5.4 Rules

- **Lengths.** A `READ` asks for at most the negotiated largest answer. A `DATA` carries at most what
  its `READ` asked, and only with status `OK`. A frame over the negotiated maximum is `REFUSED`.
- **Offsets.** An offset is never negative. An offset past the end is answered `EOF`, not
  `REFUSED`, because FFmpeg probes past the end legitimately.
- **Sequence.** Answers come in the order of their requests on a channel. An answer whose sequence
  or offset doesn't match the oldest request outstanding closes the channel.
- **Prefetch.** A `PREFETCH` may be answered with `OK` and fewer bytes, or with zero bytes and `OK`,
  meaning "not now". A `NEEDED` is never answered with zero bytes unless `EOF`.
- **Seeking is local** to FFmpeg's side: it keeps the position, and `AVSEEK_SIZE` is the size from
  `WELCOME`.
- **One reader per channel.** FFmpeg's side holds a lock from writing a request to reading its
  answer's last byte, though FFmpeg's I/O layer already calls one input from one thread at a time.
- **No allocation from the peer's numbers.** Each side sizes its buffers from the negotiated maximum,
  never from a length the other sent.

### 5.5 What the parent must validate

The FFmpeg it serves may have been taken over by a crafted film. So the parent treats every frame as
hostile:
- the magic and the version;
- the type expected now;
- the flags, which must be zero;
- the sequence;
- a length within the maximum;
- an offset that is not negative;
- one `HELLO` per channel;
- the requests in flight within the allowed count.

Any violation ends the run. **It reads only the file it chose for that channel**: the name in `HELLO`
is matched against what the parent itself said it would serve, and a name it didn't offer is
`REFUSED`.

### 5.6 Writing, later

Writing outputs (chunk stores, prepared copies) through the parent would give the parent the same
control over writes. That matters if a prepared directory ever sits on a share. Version 1 reserves
types for it and builds nothing.

## 6. Performance (estimates, to measure)

- **Round trips.** A request costs two system calls and two context switches on each side, on the
  order of 10–30 µs. At 1 MiB blocks, 1 GB/s is 1,000 round trips a second, a few percent of a core.
- **Copies.** Page cache to the parent's buffer, to the pipe, to FFmpeg's buffer is two copies more
  than FFmpeg reading the file. The interposer's stand-in measured ~0.9 CPU-s per GB, in Python; Go
  should do better. The parent can cut one copy with `splice` from file to pipe when it isn't sharing
  the bytes.
- **Latency.** Blocks and prefetch hide the round trip behind processing for sequential reads. A seek
  pays one round trip, plus the disk.
- **To measure before version 1 is fixed:**
  - throughput and CPU per GB against direct reads, on a local disk and on NFS;
  - a probe's time;
  - a chunk run's time to its first output, with the disk idle and under background load;
  - block sizes from 256 KiB to 4 MiB;
  - one in flight against two.

## 7. Testing

- **A reference server** (in `test/`) that speaks the protocol and can be slow, dead, wrong, hostile,
  or short at will. Every behaviour above is a test against it.
- **Every run shape a media server uses**, read directly and through the protocol, compared byte for byte:
  probe, read-through, subtitles, chunk copy, encode and sound, HLS, artwork. Encodes run at one
  thread, since x264 with VBV differs between direct runs too.
- **The hostile cases from the wargame:** two inputs, a reopening concat, huge reads, garbage
  frames, a pipelining client, death of either side mid-frame, SIGSTOP mid-answer, a stop while a
  read waits.
- **Confined:** under Landlock with no media file granted, and seccomp refusing sockets.
- **On every jellyfin-ffmpeg release the patches move to:** the whole suite, on amd64 and arm64.

## 8. For discussion

1. **The name** (§3).
2. **The transport** (§4.1): pipes, a socketpair handed down, or shared memory. R: pipes.
3. **Requests in flight** (§4.4): one, or two with prefetch. R: negotiated; start with one and
   measure.
4. **Whether a stop should end FFmpeg cleanly** (§4.5), which means the protocol polls its interrupt
   callback every 100 ms while waiting. R: yes.
5. **The fftools patch** (§3), exiting non-zero when an input failed. R: yes, as defence in depth.
6. **Writing** (§5.6): reserve it and build nothing in version 1? R: yes.
7. **Anything the protocol should carry that it doesn't:**
   - a keep-alive (R: no, because the parent owns time);
   - statistics (R: later);
   - a way for FFmpeg's side to say its read is for a seek, so the parent can put it first (R:
     worth discussing, since the parent can also tell a seek from an offset that jumps).
