# The read protocol: draft 2

**Status:** draft 2, 2026-10-22, from the author's discussion of draft 1. It goes to independent audits
next; no patch is written until it has survived them. What is still open is listed in §12.

## 1. What it is for

A parent process, here a media server's node, runs FFmpeg to read a film. FFmpeg does not open the
film. The parent hands FFmpeg a channel at its start and serves every byte FFmpeg reads of its
input, by offset. Then:

- **FFmpeg never touches the storage.** A dead network share blocks a thread of the parent, which
  knows and decides. It never blocks FFmpeg in the kernel, where it cannot be killed.
- **Every read is the parent's to schedule:** to order against other work on the same disk, to pace,
  to read ahead, to share between consumers.
- **FFmpeg needs no path to the film and creates no socket.** Its confinement stays as tight as today:
  Landlock with no media file granted, and seccomp refusing `socket` and `socketpair`.
- **The parent judges every run.** A run whose input could not be read in full never passes as
  complete.

**Scope.** The protocol carries only the input's bytes:

- **The job stays on FFmpeg's command line,** where its confinement already needs it: which outputs
  it may write, whether it uses a GPU.
- **FFmpeg writes its own outputs** to local disk as today: files, and results on stdout.

## 2. Roles

| | The parent (the node) | FFmpeg's side (this patch) |
| --- | --- | --- |
| Decides | what is read, the terms, the time a run may take, and whether a run succeeded | what to ask for and when, within the terms (including prefetching) |
| Does | creates the channels, launches FFmpeg, reads the storage, sends the bytes | asks, waits, hands the bytes to FFmpeg's demuxer |
| Trusts | nothing FFmpeg sends: FFmpeg may have been taken over by a crafted film | the parent |
| Ends a run | by killing FFmpeg, for any reason it chooses, at any time | by exiting, after sending `DONE` |

There is no message to stop or abort. The parent kills FFmpeg instead (SIGKILL of its process group).

## 3. Channels

- **One channel per input:** two pipes, one each way, created by the parent and inherited by FFmpeg
  at dedicated descriptors. stdin, stdout and stderr are never used: stdout carries results, and a
  stray write to it would corrupt a channel.
- The parent sets each pipe's buffer to 1 MiB (`F_SETPIPE_SZ`). The default 64 KiB halved throughput
  in measurement ([bench/RESULTS.md](bench/RESULTS.md)).
- Each input names its channel's descriptors as protocol options:
  `-NAME_read_fd 7 -NAME_write_fd 8 -i NAME:`. Here `read_fd` is where FFmpeg reads the parent's
  messages, and `write_fd` where it writes its own.
- Opening a channel a second time in one run, for example a demuxer reopening its input or a crafted
  concat list naming it, fails at once, and FFmpeg's side sends nothing more on it.
- The parent writes into the pipe with `splice` from the file where it can, which saves a copy
  (measured: about 0.6–0.7 CPU-seconds per GB against 1.4 for a copy through the parent's memory).

## 4. Frames

**The header.** Every message is a frame with a 6-byte header, followed by the body:

| Field | Type | Notes |
| --- | --- | --- |
| type | int8 | §6. 0 is never a valid type |
| sequence | int8 | §5 |
| length | int32 | the body's length in bytes; the type sets its bounds |

**Integers.** All integers are signed and little-endian, and a negative value anywhere is a violation.
The rule catches overflow and corrupted framing early.

**Delimiting.** The type determines everything in the body. Every variable-length field is
delimited by its own length (an int16 before a text's bytes), even where the frame's length could
imply it.

**Sizes.**
- Every frame FFmpeg's side sends is at most 4,096 bytes and is written with a single `write()`.
  The kernel then delivers it whole (`PIPE_BUF`), so a well-behaved FFmpeg can never leave the parent
  holding half a frame.
- `DATA` frames are as long as their `READ` asked, up to the agreed maximum (§6.4), and arrive over
  several pipe-fulls.

**Reading.** Each side reads what is in the pipe into its own buffer and carves frames out of it. A
short read and two frames in one read are both normal. A side never reads a frame body without first
reading its header.

## 5. Sequence numbers

FFmpeg's side numbers every message it sends:
- `HELLO` is 0, and each later message the next number;
- numbers wrap from 127 back to 0: `(n + 1) mod 128`;
- every message takes a number, even one that gets no answer.

The parent's every message is an answer, and echoes the number of the message it answers. Each
answer maps to exactly one request. The parent checks that every number is the one it expects next;
FFmpeg's side checks that every answer echoes an outstanding request's number. At most a handful of
requests are outstanding at once, so a wrap can never confuse two answers.

## 6. Messages

| Type | Code | From | Answered by | Body |
| --- | --- | --- | --- | --- |
| `HELLO` | 1 | both | the parent's `HELLO` | §6.1 |
| `CAPABILITY` | 2 | FFmpeg | `REQUEST` | §6.2 |
| `REQUEST` | 3 | parent | `ACCEPT` or `DECLINE` (as the next message, not an echo) | §6.3 |
| `ACCEPT` | 4 | FFmpeg | — | empty |
| `DECLINE` | 5 | FFmpeg | — | §6.5 |
| `READ` | 6 | FFmpeg | `DATA` | §6.4 |
| `DATA` | 7 | parent | — | the bytes |
| `DONE` | 8 | FFmpeg | — | §6.6 |

### 6.1 `HELLO`

The only message whose form never changes, in any version. Its body is exactly 4 bytes:

| Field | Type |
| --- | --- |
| lowest protocol version this side speaks | int16 |
| highest version this side speaks | int16 |

FFmpeg's side sends its `HELLO` (sequence 0) as soon as the input is opened. The parent reads it, and
then:

- **If the ranges overlap,** it answers with its own `HELLO`, echoing 0.
- **If they don't,** it logs both ranges and kills FFmpeg, without answering.

Each side then works out the version itself:

    version = min(FFmpeg's highest, the parent's highest)
    valid when version >= max(FFmpeg's lowest, the parent's lowest)

Every later message is in that version's form.

### 6.2 `CAPABILITY` (version 1)

What FFmpeg's side can do, and what it prefers:

| Field | Type | Notes |
| --- | --- | --- |
| preferred block size | int32 | bytes; 1 MiB in this patch |
| largest `DATA` body it accepts | int32 | bytes; what it can buffer |
| most `READ`s it can have in flight | int8 | 2 in this patch |

### 6.3 `REQUEST` (version 1)

The parent's terms for this input. It echoes `CAPABILITY`'s sequence.

| Field | Type | Notes |
| --- | --- | --- |
| version | int16 | the version both sides worked out; a cross-check |
| run | int64 | the parent's identifier for the run, echoed in `DONE` |
| file size | int64 | bytes; reads stay within it |
| block size | int32 | bytes, at most the largest body FFmpeg's side accepts |
| longest `READ` | int32 | bytes, at least the block size and at most the largest body |
| `READ`s in flight | int8 | 1 or more, at most what `CAPABILITY` said |

The parent chooses the terms within what `CAPABILITY` allows. A parent that cannot work within
them kills FFmpeg.

FFmpeg's side checks the terms and answers with its next message:
- `ACCEPT`, then starts reading;
- or `DECLINE` (§6.5), then exits.

**The terms checked:**
- the version matches its own reckoning;
- the sizes are within its capability;
- the file size is not negative.

### 6.4 `READ` and `DATA`

`READ` asks for a range of the file:

| Field | Type |
| --- | --- |
| offset | int64 |
| length | int32 |

**The rules a `READ` must follow:**
- `length` is greater than 0 and at most the longest `READ`;
- `offset + length` is at most the file size;
- the outstanding `READ`s are within the agreed count.

The parent answers each `READ` with `DATA`, echoing its sequence. A `DATA` body is the requested
bytes and nothing else, so its length is exactly the `READ`'s. Answers come in the order of their
requests on a channel.

**When the parent can't read** (the storage failed, or the file changed since `REQUEST`, which the
parent checks by its own identity of the file), it does not answer. It kills FFmpeg. FFmpeg's side
never hears of a read failure, because nothing it could do with a partial input is of use to the
parent.

### 6.5 `DECLINE`

| Field | Type |
| --- | --- |
| reason length | int16, at most 1,024 |
| reason | that many bytes, UTF-8 |

### 6.6 `DONE`

FFmpeg's verdict, like a `Result`. It is sent at FFmpeg's exit on every channel the run opened, and
then FFmpeg exits.

| Field | Type | Notes |
| --- | --- | --- |
| run | int64 | echoes `REQUEST` |
| result | int8 | 0 `OK`, 1 `ERR` |
| *if `OK`:* bytes read | int64 | through this channel |
| *if `ERR`:* kind | int8 | the table below |
| *if `ERR`:* code | int32 | FFmpeg's error code (`AVERROR`), negated, since FFmpeg's codes are negative |
| *if `ERR`:* message length | int16 | at most 1,024 |
| *if `ERR`:* message | that many bytes | UTF-8, for the logs |

| Kind | Code | Means |
| --- | --- | --- |
| `CHANNEL` | 1 | the channel to the parent ended (the parent died) |
| `OPTIONS` | 2 | FFmpeg refused its command line before reading |
| `MEDIA` | 3 | the content could not be demuxed, decoded or encoded |
| `OUTPUT` | 4 | writing an output failed (for example, a full disk) |
| `INTERRUPTED` | 5 | FFmpeg was asked to stop by its own means |
| `INTERNAL` | 6 | anything else |

## 7. FFmpeg's side

**Patch 1: the protocol,** a URL protocol in libavformat.

1. **On open,** it does the opening exchange (§6.1–§6.3), and refuses to be opened twice on one
   channel.
2. **It reads in blocks.**
   - It asks for whole blocks, aligned to the block size, the last block trimmed at the file's size.
   - It keeps the last few blocks (4 in this patch) and answers FFmpeg's small reads from them.
   - FFmpeg's I/O layer asks 32 KiB at a time, so this is what keeps the parent's reads large.
3. **Seeking and end-of-file are local.** It keeps the position. The size comes from `REQUEST`, and
   `AVSEEK_SIZE` answers it. A read at or past the end returns `AVERROR_EOF` without asking.
4. **It prefetches when reading straight through.** When the block just needed follows the one needed
   before it, it asks for the next block too, within the in-flight limit. A prefetched block that is
   never used is read, kept until it falls out of the cache, and dropped.
   - Measured on FFmpeg's own read traces: prefetching only when sequential keeps nearly all of
     always-prefetching's gain. A read-through on a busy NAS took 12.1 s against 20.3 s.
   - It never costs more than no prefetch: at most one wasted block per run, where always-prefetching
     wasted up to three and slowed a probe by 40% ([bench/RESULTS.md](bench/RESULTS.md)).
5. **It never times out.** While it waits, it polls every 100 ms and checks FFmpeg's interrupt
   callback.
   - **When FFmpeg is asked to stop by its own means,** the read returns `AVERROR_EXIT`, and the
     channel is abandoned with a request outstanding: it sends only `DONE` after that.
   - **When the channel ends** (end-of-file on its read side, or a broken pipe on its write side),
     the read returns `AVERROR(EIO)`, and `DONE` will say `CHANNEL`.
6. **It checks every answer:**
   - the type and sequence expected;
   - a `DATA` length equal to its `READ`'s;
   - nothing longer than it accepts.

   Any mismatch is the parent's bug: it fails the input and sends nothing more on that channel.

**Patch 2: fftools,** small.
- At FFmpeg's single exit point, it sends `DONE` on every channel the run opened.
- It makes FFmpeg exit non-zero when an input failed, where FFmpeg today finishes its outputs and
  exits 0.
- ffprobe gets the same.

## 8. The parent's side

1. **It creates each input's pipes,** sets their buffers, and launches FFmpeg with the job and the
   descriptors on its command line, confined.
2. **It treats every frame from FFmpeg as hostile.** It checks:
   - the type expected now;
   - the sequence expected next;
   - every integer for being negative;
   - the length within the type's bounds and the 4,096-byte limit;
   - `HELLO`'s exact length;
   - each `READ` within the file, within the longest `READ`, and within the in-flight count.

   **On any violation,** it logs exactly what was wrong and kills FFmpeg. It sends nothing. It never
   allocates from a number FFmpeg sent, only from the terms it set.
3. **A frame, once begun, must finish within a deadline** (1 second). A well-behaved FFmpeg sends
   each frame in one atomic `write()`, so a partial frame that stays partial is a violation.
   **Silence between frames is never a failure:** a paused or slow FFmpeg is quiet for as long as it
   likes.
4. **It serves each `READ` through its disk scheduler.** It releases its place on the disk as soon as
   its read of the storage returns, before writing to the pipe, so an FFmpeg that is paused or slow to
   drain never holds the disk. A read the storage hasn't answered keeps its place, and its own buffer,
   until it returns, so a dead share pins at most one thread per place the disk has.
5. **It owns all time.** It ends a run by killing FFmpeg:
   - when it can't read;
   - when the file changed;
   - when a run is no longer wanted;
   - when its own limits pass.
6. **It knows a run has ended when the process exits,** never from end-of-file on a pipe.
7. **It is the judge.** A run succeeded only if:
   - FFmpeg exited 0;
   - `DONE` said `OK` on every channel;
   - the parent itself had no read failure;
   - the `DONE`'s run matches.

   Exiting without `DONE` is a failure: a crash, or a kill.

## 9. Performance

Measured in [bench/](bench/RESULTS.md), on a file in the page cache, so the transport alone:

| Transport | MB/s | CPU-s per GB |
| --- | --- | --- |
| FFmpeg reads the file itself | ~4,300 | 0.23 |
| Pipe, 1 MiB buffer, copy | ~940 | 1.4 |
| Pipe, 1 MiB buffer, `splice` | ~1,500–2,000 | 0.6–0.7 |
| Shared memory (not chosen) | ~2,000 | 0.45 |

Every row is faster than the disks the parent reads: a 1 Gb/s NAS gives about 110 MB/s, an NVMe drive
2–3 GB/s. Shared memory was set aside: its gain (about 5 CPU-seconds on a 20 GB read-through) isn't
worth its bookkeeping of who owns each block and its Linux-only path. A request costs a round trip of
tens of microseconds, so a block of 1 MiB keeps it negligible.

**Still to measure:**
- on a real NAS, with real competing load;
- a chunk run's time to its first output;
- block sizes from 256 KiB to 4 MiB;
- one `READ` in flight against two.

## 10. Testing

- **A reference parent** (in `test/`) that speaks the protocol and can be slow, silent, wrong, or
  short, and kill at any point.
- **The protocol's own suite:** every rule in §4–§8, from both sides; partial frames; two frames in
  one read; a wrapping sequence; every violation the parent must catch.
- **Every run shape a media server uses,** read directly and through the protocol, compared byte for byte:
  - probe;
  - read-through;
  - subtitles;
  - chunk copy, encode and sound;
  - HLS;
  - artwork.

  Encodes run at one thread, since an x264 encode with VBV differs between direct runs too.
- **Hostile cases:**
  - two inputs;
  - a reopening concat;
  - huge, negative and out-of-range fields;
  - a pipelining FFmpeg;
  - half frames;
  - death of either side mid-frame;
  - SIGSTOP mid-answer.
- **Confined:** under Landlock with no media file granted, and seccomp refusing sockets.
- **At every jellyfin-ffmpeg release** the patches move to: the whole suite, on amd64 and arm64.

## 11. Not in version 1

- **Writing outputs through the parent.** Outputs belong on fast local disk, which FFmpeg writes
  itself.
- **Shared memory** for the bytes.
- **A stop or abort message.** The parent kills.
- **Read errors sent to FFmpeg.** The parent kills.
- **A magic number.** The parent creates the channel and names its descriptors itself, so nothing
  else is ever on the other end.

## 12. Still open

1. **The name** (`NAME` above). Something generic, such as `fdpair:`, suits offering it upstream.
2. **Whether `DONE`'s `OK` carries more than the bytes read,** for cross-checking: for example,
   packets demuxed or frames written.
3. **Whether `READ`s must be block-aligned.** Draft 2 has FFmpeg's side ask in aligned blocks, but the
   parent doesn't yet refuse an unaligned one. Making it a rule makes violations plainer. It also
   makes a later change to how FFmpeg's side reads a protocol change.
