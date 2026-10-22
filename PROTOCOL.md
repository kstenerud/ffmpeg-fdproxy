# ffstreamer, the read protocol: draft 6

**Status:** draft 6, 2026-10-22. **Ready to implement.**
- Draft 5 took in the third audit round
  ([audits/2026-10-22-draft-4-combined.md](audits/2026-10-22-draft-4-combined.md)), accepted by the
  owner with the agent's recommendations.
- Draft 6 takes in the review of that change
  ([audits/2026-10-22-draft-5-review.md](audits/2026-10-22-draft-5-review.md)), which judged it ready
  once its findings were in, with no further review.

§14 lists what is still to set by measurement.

**What changed since draft 4:**
- **`DONE(TERMS)` is sent from the open,** since FFmpeg never calls the close of an open that failed.
  Every other `DONE` comes from the close.
- **An interrupt is `DONE`'s kind `INTERRUPTED`,** and the fftools patch never lets one become exit 0.
  When the parent kills a run, its own reason decides whether that was a stop or a failure.
- **The seccomp filter is written out,** per architecture: `clone3` answers "not implemented" so glibc
  falls back to a `clone` the filter can read. Denying `clone3` outright broke FFmpeg's threads.
  `setpgid` is denied too.
- **The parent kills the process itself, by pidfd,** not its process group, which a hostile FFmpeg
  could leave.
- **The block cache's size is an option the parent sets** from the streams a run reads. 8 blocks
  read a 10-track non-interleaved MP4 400 times over.
- **Pausing holds `READ`s and stops the process (SIGSTOP),** so a paused run uses neither disk nor
  processor.
- **The inherited descriptors are contiguous.**
- **The budget's figures are corrected.**
- **Stops are separated from failures.**
- **`run` is at least 1.**
- **The reserve is (outstanding + 1) × longest `READ`.**

## 1. What it is for

A parent process, here a media server's node, runs FFmpeg to read a film. FFmpeg does not open the
film. The parent hands FFmpeg a channel at its start and serves every byte FFmpeg reads of its
input, by offset. Then:

- **FFmpeg never touches the storage.** A dead network share blocks a thread of the parent, which
  knows and decides. It never blocks FFmpeg in the kernel, where it cannot be killed.
- **Every read is the parent's to schedule:** to order against other work on the same disk, to pace,
  to read ahead, to hold back.
- **FFmpeg needs no path to the film and creates no socket.** Its confinement stays as tight as today:
  Landlock with no media file granted, and seccomp refusing `socket` and `socketpair`.
- **The parent judges every run.** A run whose input could not be read in full never passes as
  complete.

**Scope.** The protocol carries only an input film's bytes:
- **the job stays on FFmpeg's command line,** where its confinement already needs it;
- **FFmpeg writes its own outputs** to local disk;
- **runs that read bytes the parent already holds in memory** (in a media server: re-encoding artwork,
  converting subtitle text) **keep reading them from stdin.** Only runs that read a film from storage
  use ffstreamer.

## 2. Roles

| | The parent | FFmpeg's side |
| --- | --- | --- |
| Decides | what is read, the terms, how long a run may take, whether a run succeeded | what to ask for and when, within the terms |
| Does | creates the channels, launches FFmpeg, reads the storage, sends the bytes | asks, waits, hands the bytes to FFmpeg's demuxer |
| Trusts | nothing FFmpeg sends: FFmpeg may have been taken over by a crafted film | the parent |
| Ends a run | by killing FFmpeg, at any time, for any reason | by exiting |

There is no message to stop, abort, pause or report a read error.
- **The parent pauses a run by holding its `READ`s and stopping the process** (SIGSTOP), and resumes
  it with SIGCONT. Holding the reads stops its disk use. Stopping the process stops it using the
  processor on what it already holds (a paused run uses neither). Holding a `READ` trips no
  timeout inside FFmpeg: a 45-second hold mid-run finished normally (the third audit).
- **The parent stops a run by killing it** (§9.8).
- **Every signal the parent sends a ffstreamer run goes to the process itself, by pidfd,** never to its
  process group: SIGSTOP and SIGCONT to pause, SIGKILL to stop.
- **A pause starts no new storage reads.** Reads already under way finish into the channel's reserve.

**FFmpeg's exit status is the run's verdict.** `DONE` is FFmpeg's account of each channel: advisory,
and not a trust boundary. A compromised FFmpeg can lie in it, as it can in its outputs.

## 3. Versions and compatibility

- Versions are integers from 1. Version 0 is invalid.
- **Anything new is a new version.** There are no optional or ignorable fields and no extensions.
  Within a version, every message has exactly one form.
- **`HELLO` is frozen in every version, header included** (§7.1). Every later message takes the form
  of the version both sides work out from it.

## 4. Channels

### 4.1 What a channel is

- **One channel per input film:** two pipes, one each way. The parent creates them and FFmpeg
  inherits them at dedicated descriptors. stdin, stdout and stderr are never used: stdout carries
  results, and a stray write to it would corrupt a channel.
- **Naming and options.** Each input restricts itself to this protocol and names its channel:
  `-protocol_whitelist ffstreamer -ffstreamer_read_fd 5 -ffstreamer_write_fd 6 -i ffstreamer:`.
  - `read_fd` is where FFmpeg reads the parent's messages, and `write_fd` where it writes its own.
  - The whitelist reaches nested opens. A crafted manifest naming `pipe:5` or `fd:` cannot read the
    channel's descriptor directly (checked against FFmpeg's source and reproduced).
  - The options are decoding options. The two descriptors default to -1, and an open with either left
    at -1 fails.
  - `-ffstreamer_cache_blocks N` sets how many blocks FFmpeg's side keeps (§10.2), from 2 to 1,024,
    default 8. The parent sets it from the streams the run reads.
    - The cache's memory is `cache_blocks` × block size, which the parent counts in its memory for the
      run.
    - A run whose stream count would need more than the parent will spend is clamped at the
      parent's limit, and its budget catches a run that then thrashes.
  - The URL must be exactly `ffstreamer:`, with nothing after it, so a relative reference resolved
    against it cannot open.

### 4.2 Descriptors

**The parent:**
- creates every pipe end close-on-exec, atomically (`pipe2(O_CLOEXEC)`);
- hands each child only its own ends, by `dup2` at exec. No other run, launched at any moment, ever
  inherits them;
- hands FFmpeg its inherited descriptors **contiguous, with no gap** (in Go, `ExtraFiles` with no nil
  entry). A missing descriptor would otherwise be filled by the first descriptor FFmpeg opens or
  duplicates itself, which takes the lowest free number: measured, `dup(6)` returned 4. Ordering
  alone can't prevent that, so the `HELLO` round trip is what proves the channel;
- closes its own copies of FFmpeg's ends of the pipes right after FFmpeg starts. Its request reader
  then simply reaches end-of-file once FFmpeg has exited (§9.9).

**FFmpeg's side, on open:**
- sets `FD_CLOEXEC` on both descriptors;
- checks that each is a FIFO opened for its direction;
- refuses to be opened for writing.

That check is a sanity check. The real proof that the parent is on the other end is the `HELLO`
round trip.

**The channel opens once per process.** A second open of the same channel is refused, even after the
first session has closed, since unread `DATA` may still be in the pipe.

### 4.3 Pipe sizes

- **The parent-to-FFmpeg pipe carries the bytes.** The parent sizes it from a budget of pipe memory
  divided among its concurrent channels: up to 1 MiB, since the default 64 KiB halved throughput in
  measurement. It sets the size through Go's `SyscallConn().Control`, never `Fd()`: calling `Fd()`
  puts the descriptor in blocking mode, and then closing it no longer frees a blocked writer.
- **The FFmpeg-to-parent pipe stays at its default.** Requests are tiny.
- **Linux caps a user's pipe memory** (`fs.pipe-user-pages-soft`, 16,384 pages by default). Past the
  cap, `F_SETPIPE_SZ` fails with `EPERM`, and every new pipe holds **8 KiB** and can't be grown
  (measured).
  - The parent's deployment raises the cap.
  - When a channel falls back anyway, the parent logs it once and goes on, since the protocol works
    at 8 KiB, only slower.

## 5. Frames

**The header.** Every message is a frame of a 6-byte header and a body:

| Field | Type | Notes |
| --- | --- | --- |
| type | int8 | §7. 0 is never valid |
| sequence | int8 | §6 |
| length | int32 | the body's length in bytes |

**Integers.** All integers are signed and little-endian. **A negative value on the wire is a
violation.** That catches corrupt or forged fields. It does not make arithmetic safe: §8's checks are
written so they cannot overflow.

**Lengths.**
- Every message of fixed form has one exact body length.
- `DONE`, the one message of variable form, is bounded by its type before it is buffered
  (9 ≤ length ≤ 1,040), then checked to equal the sum of its fields.
- Variable fields are delimited by their own lengths.
- An enum value not defined in the version is a violation.

**Size.** Every frame FFmpeg's side sends is at most 4,096 bytes and is written with one `write()`.
On Linux that write is atomic (`PIPE_BUF`), so a kill can't leave half a frame. It is a convenience:
the parent reads frames however they arrive.

**Reading.** Each side reads what is in the pipe into its own buffer and carves frames out of it. A
short read and two frames in one read are both normal.

## 6. Sequence numbers

- FFmpeg's side numbers every message it sends. `HELLO` is 0, and each later message takes the next
  number, wrapping from 127 back to 0: `(n + 1) mod 128`.
- **Every parent message is an answer, and echoes the number of the message it answers.**
- **Answers are first-in-first-out:**
  - an answer must echo the oldest outstanding request on its channel;
  - FFmpeg's side checks that before using an answer;
  - the parent checks that every message it receives carries the number it expects next.
- `READ`s outstanding are capped at 16 (§7.3). FIFO checking makes a wrap harmless in any case.

FFmpeg's side holds **one mutex per channel** over taking a number and writing the frame. fftools
never reads one input from two threads at once (§10.1), but libavformat's other users might.

## 7. Messages

| Type | Code | From | Answered by | Body (bytes) |
| --- | --- | --- | --- | --- |
| `HELLO` | 1 | both | FFmpeg's, by the parent's `HELLO` | exactly 4 |
| `CAPABILITY` | 2 | FFmpeg | `REQUEST` | exactly 9 |
| `REQUEST` | 3 | parent | the first `READ`, or `DONE` | exactly 27 |
| `READ` | 4 | FFmpeg | `DATA` | exactly 12 |
| `DATA` | 5 | parent | — | exactly the `READ`'s length |
| `DONE` | 6 | FFmpeg | — | 9 to 1,040 |

### 7.1 `HELLO`

**Frozen in every version.** The whole frame is `type 1, sequence 0, length 4`, then:

| Field | Type |
| --- | --- |
| lowest version this side speaks | int16, at least 1 |
| highest version this side speaks | int16, at least the lowest |

**FFmpeg's side** sends its `HELLO` as soon as the channel is opened.

**The parent:**
- kills FFmpeg if FFmpeg's range is invalid, or if the two ranges don't overlap, logging both;
- otherwise answers with its own `HELLO`, echoing 0.

**FFmpeg's side** checks the parent's range the same way, and fails the open if it is invalid or does
not overlap. It sends nothing more before the parent's `HELLO` has arrived.

Each side works out the version itself:

    version = min(the two highest)    valid when version >= max(the two lowest)

### 7.2 `CAPABILITY` (version 1)

| Field | Type | Rule |
| --- | --- | --- |
| preferred block size | int32 | > 0 |
| largest `DATA` body it accepts | int32 | ≥ the preferred block size |
| most `READ`s it can have outstanding | int8 | 1 to 16 |

### 7.3 `REQUEST` (version 1)

The parent's terms for this input. It echoes `CAPABILITY`'s number.

| Field | Type | Rule |
| --- | --- | --- |
| version | int16 | the version both sides worked out; a cross-check |
| run | int64 | ≥ 1; the parent's identifier for the run, echoed in `DONE` |
| file size | int64 | ≥ 0 |
| block size | int32 | > 0, and ≤ the largest body `CAPABILITY` gave |
| longest `READ` | int32 | ≥ the block size, and ≤ the largest body |
| `READ`s outstanding | int8 | ≥ 1, and ≤ what `CAPABILITY` gave |

**A parent that can't work within `CAPABILITY`** kills FFmpeg.

**FFmpeg's side:**
- **A malformed `REQUEST`:** the wrong type, number or length, or a negative field. That is a protocol
  fault before the terms exist. The open fails, and no `DONE` is sent.
- **A well-formed `REQUEST` whose values break the rules:** it sends `DONE` (`ERR`, `TERMS`) from the
  open, and the open fails. The `DONE` echoes the run when it is at least 1, and 0 otherwise.
- **A good `REQUEST`:** its first `READ` accepts the terms.

### 7.4 `READ` and `DATA`

| `READ` field | Type |
| --- | --- |
| offset | int64 |
| length | int32 |

**A `READ` must hold to these** (checked as in §8):
- `0 ≤ offset`;
- `0 < length ≤ min(longest READ, file size − offset)`;
- the outstanding `READ`s within the agreed count.

The parent answers each `READ` with `DATA`, echoing its number, in the order of the `READ`s. A
`DATA` body is exactly the requested bytes. FFmpeg's side checks the type, the number (the oldest
outstanding), and that the length equals its `READ`'s, before using a byte.

**Counting what is outstanding:**
- The parent stops counting a `READ` as outstanding **when it takes it to write**, before it writes
  any of the `DATA` frame.
- FFmpeg's side frees a slot **no earlier than reading the `DATA` header**.

The parent's count is then never above FFmpeg's, so a well-behaved FFmpeg can never be judged over the
limit. A hostile one gains at most one queued request, which is still bounded. (Measured: counting
down after the header was written wrongly judged 8 reads in 20,000 over the limit; counting down on
taking the read, none.)

**When the parent can't read,** because the storage failed, the read passed its deadline (§9.7), or
the file changed since `REQUEST`, it does not answer. It kills FFmpeg. The parent checks the file's
identity itself (device, inode, size, modification time). A file that grows or changes during a run
is a changed file.

Alignment is FFmpeg's side's own business. The parent serves any range that holds to the rules.

### 7.5 `DONE`

FFmpeg's account of one channel, like a `Result`.

| Field | Type | Notes |
| --- | --- | --- |
| run | int64 | echoes `REQUEST`, or 0 (§7.3) |
| result | int8 | 0 `OK`, 1 `ERR` |
| *if `ERR`:* kind | int8 | the table below |
| *if `ERR`:* code | int32 | FFmpeg's error code, negated. A code that isn't negative, or is `INT32_MIN`, is sent as 0 |
| *if `ERR`:* message length | int16 | 0 to 1,024 |
| *if `ERR`:* message | that many bytes | for the logs |

| Kind | Code | Means |
| --- | --- | --- |
| `TERMS` | 1 | FFmpeg's side declined `REQUEST`'s terms |
| `CHANNEL` | 2 | the channel to the parent ended (end-of-file, or a broken pipe): the parent died |
| `PROTOCOL` | 3 | the parent broke the protocol: a wrong type, number or length |
| `INTERRUPTED` | 4 | FFmpeg's interrupt callback fired while the channel waited: a signal from outside the parent (its own `q` never fires it) |

`OK` means this channel saw no fault. The interrupt doesn't always reach a channel:
- fftools fires it only on a second signal once the run has started;
- FFmpeg's I/O layer may catch it before calling the protocol at all.

So an interrupted run may end with `DONE` `OK` and a non-zero exit. The exit status decides. Whether the run as a whole succeeded is FFmpeg's exit status.
fftools carries one merged error with no record of where it came from, so `DONE` can't classify a
failure of decoding or writing (§10.4).

**When it is sent:**
- **Exactly once per channel that received a well-formed `REQUEST`:**
  - from the open, when the open fails after the `REQUEST` arrived, whether by a `TERMS` refusal or by
    a resource failure such as no memory for the cache (also sent as `TERMS`, with its code). FFmpeg
    never calls the close of an open that failed (`ffurl_connect` sets `is_connected` only on success),
    so the open's failure path also frees its own state and closes its descriptors;
  - from the close, otherwise.
- **Never before a well-formed `REQUEST`.** If the opening exchange fails, the open fails, FFmpeg
  exits non-zero, and the missing `DONE` already says so.
- **After a fault on the channel** (`CHANNEL`, `PROTOCOL`, `INTERRUPTED`), `DONE` is the only message
  sent, as `ERR` with the fault's kind.
- **A path that skips the close** (an `exit()` mid-run, a signal, a crash) sends no `DONE`. Each of
  those also exits non-zero or is killed.

The close runs only after fftools has joined the threads that read (§10.1), so no thread is still
waiting on a `READ` when `DONE` goes out. Once `DONE` is sent, FFmpeg's side refuses any further read
locally.

**The parent, on `DONE`:**
- stops serving that channel, and drops any `READ` still waiting;
- doesn't treat a `DONE` arriving after `READ`s it hasn't answered as a fault;
- logs any frame after `DONE` as a violation, which doesn't change the verdict, since FFmpeg is
  exiting.

## 8. Checks that cannot overflow

Signed fields stop forged negatives, not arithmetic overflow. Added, `offset + length` can wrap. In
C, gcc at -O2 deletes a test like `offset + length < 0`, because signed overflow is undefined
(reproduced in the first audit). Every bound is checked by subtraction or division from a known
non-negative value, never by addition or multiplication:

    0 <= offset  &&  offset <= size  &&  0 < length  &&  length <= longest  &&  length <= size - offset
    over budget  <=>  served / k > allowed        (never served > k * allowed)

The same holds for every check on either side. A side never computes a size or allocates from a
number the other side sent. It allocates only from the terms it set or accepted.

## 9. The parent's side

1. **It launches FFmpeg confined,** in its own process group, with the job, the whitelist and the
   descriptors on its command line, and its descriptors as in §4.2.
   - **No process outlives or escapes FFmpeg.** Its seccomp filter, beside today's denials:

     | System call | Answer | Why |
     | --- | --- | --- |
     | `clone3` | `ENOSYS` | seccomp can't read `clone3`'s flags. glibc 2.34+ calls it first, and falls back to `clone` only on `ENOSYS`. `EPERM` here broke FFmpeg's threads (measured: exit 255) |
     | `clone` without `CLONE_THREAD` in its flags (`args[0]`) | `EPERM` | creating a process; threads pass |
     | `fork`, `vfork` (amd64; arm64 has neither) | `EPERM` | creating a process |
     | `setsid`, `setpgid` | `EPERM` | leaving its session or process group |

     The filter differs by architecture, keeps today's refusal of other architectures (x32 on amd64),
     and is tested on amd64 and arm64.
     - Measured on amd64 with the pinned binaries: threads work, ffmpeg and ffprobe run normally,
       and `fork`, `posix_spawn`, `system`, `popen` and `setsid` all fail.
     - Neither binary imports any way to start a process.
     - Hardware encoding is tested under the filter on real devices: NVIDIA's driver is reported to
       launch a helper (`nvidia-modprobe`) when its device nodes are missing.
2. **It treats every frame from FFmpeg as hostile.** On any violation (§5–§8), it logs exactly what
   was wrong and kills FFmpeg. It sends nothing.
3. **Per channel, one reader of requests and one writer of `DATA`, each its own goroutine, and the
   channel's own buffer reserve:** (outstanding + 1) × the longest `READ`, from the terms. That covers
   one `DATA` being written while the outstanding `READ`s wait. A storage read waits for a free buffer
   in its channel's reserve, never in another's.
   - **No `DATA` write ever waits on another channel or another run.** FFmpeg's scheduler stops
     reading every input but the one furthest behind, by design. A writer shared between channels
     would block on a paused channel's full pipe and starve the one FFmpeg is waiting for.
   - The reader sees every request, including a violation, while its writer is blocked.
   - Silence between frames is never a failure.
4. **It reads the storage into its own memory, then writes.**
   - It reads with `pread` into the channel's buffer: never `splice` from the file into FFmpeg's
     pipe, and never an `mmap` of the film.
   - A `splice` holds FFmpeg's pipe locked while the storage read waits. FFmpeg's `read()` of that
     pipe then waits on the lock uninterruptibly, and a dead share leaves FFmpeg unkillable
     (reproduced in the first audit).
   - It releases its place on the disk as soon as its read of the storage returns, before writing to
     the pipe, so a paused or slow FFmpeg never holds the disk.
5. **It counts outstanding `READ`s as §7.4 says.**
6. **It keeps a budget of bytes served per run,** by kind of run: what the run is expected to need,
   times a margin, with a floor so a small file or block rounding can't trip it. For example:
   - **a probe:** a floor of about 64 MiB plus a fraction of the file's size. A probe reads the whole
     index, about 17 MB for 2.5 hours of a 10-track MP4 (the third audit);
   - **a whole-film read:** the file's size;
   - **a chunk run:** the bytes of its span, plus the bytes the file's probe was served (which covers
     its index), plus the floor.
   A run over its budget is killed and logged. The figures are the parent's settings, and are tested
   against a badly interleaved MP4 before they're fixed (§12).
7. **A storage read has a deadline:** the stall window of its place (the parent's own watchdog of its disks).
   A read past its deadline is a read that failed. The parent kills the run and marks the place as the
   gate does.
   - A storage read the parent has abandoned keeps its place and its buffer until it returns, so a
     dead share pins at most one thread per place the disk has.
   - The parent's watchdog counts the bytes it serves as progress. The time it holds a `READ` back
     itself, for pacing or pausing, doesn't count as FFmpeg standing still. A held storage read is
     bounded by its deadline, so nothing waits forever.
8. **It owns all time.** It kills FFmpeg, the process itself, by pidfd (in Go, `os.Process.Kill`), never
   by its process group, which a hostile FFmpeg could leave. Its only stopping signal is SIGKILL: no
   SIGINT or SIGTERM first, since nothing the run would write after them is kept. A parent's usual
   stall sequence (SIGINT, then SIGTERM, then SIGKILL) is skipped for these runs, and its pausing
   (SIGSTOP and SIGCONT) moves from the process group to the process. It kills:
   - when it can't read, or the file changed;
   - when the run is over its budget or no longer wanted;
   - when its own limits pass.

   It signals only before it has reaped the process.
9. **It knows a run has ended when the process exits,** never from end-of-file on a pipe. Then, per
   channel:
   1. it closes its `DATA` write end;
   2. its request reader goes on reading, without blocking, what is already in the pipe: everything
      FFmpeg wrote before it exited is already there. It stops at end-of-file or "would block";
   3. it closes the request pipe.

   Then it judges. There is only ever one reader per pipe.
10. **It judges.** A run succeeded only if:
    - FFmpeg exited 0;
    - every channel's `DONE` said `OK` and echoed the run;
    - the parent itself had no read failure.

    Exiting without `DONE` is a failure. When the parent kills a run, its own reason decides,
    whatever the exit code:
    - **a stop:** the run is no longer wanted, the parent paused it past its patience, or the parent's
      watchdog or time limits ended it. It is recorded as a stop, not a failure;
    - **a failure:**
      - a violation of the protocol;
      - a storage read that failed or passed its deadline;
      - a changed file;
      - the budget passed.

      The failure is recorded with that reason.
11. **It sanitises text before logging it.** It decodes as UTF-8 with replacement, then escapes every
    character in the Unicode categories Cc and Cf, DEL included. That catches C0 and C1 controls, CSI
    in either form (`0x9B`, or `C2 9B`) and bidi overrides. Invalid UTF-8 is not a violation.

## 10. FFmpeg's side

### 10.1 Facts about fftools that the design rests on

These were checked against jellyfin-ffmpeg 8.1.3's source and its pinned binaries (see
[the internals audit](audits/2026-10-22-draft-3-ffmpeg-internals.md)).

- **ffmpeg reads an input from two threads, never at once.** The main thread opens it, probes it,
  runs `avformat_find_stream_info`, and makes `-ss` seeks, in `ifile_open`. Then the input's demux
  thread reads its packets, including the `-stream_loop` seek. The demux thread starts after
  `ifile_open`, and is joined (`sch_stop`) before the trailers are written and before inputs are
  closed.
- **ffprobe does all of it on one thread.**
- **Inputs close only at the end:** in ffmpeg's `ffmpeg_cleanup` → `ifile_close` →
  `avformat_close_input`, after the joins, and in ffprobe's `probe_file`. A failed open closes its
  context too. Every path on which `DONE` can be sent runs either the protocol's close, or (for an open that fails
after the `REQUEST`) the open's own failure path.
- **No run shape a media server uses opens its input more than once:** counted for probe, read-through,
  subtitle extraction, chunk copy and encode with `-ss`/`-to` into the segment muxer, HLS, and
  `-stream_loop`.
- **Today, ffmpeg and ffprobe exit 0 after an input failed mid-read** (reproduced): in the demux loop,
  in `avformat_find_stream_info`, and in ffprobe's packet loop. FFmpeg records every non-end-of-file
  read error on the input's I/O context (`pb->error`), and keeps it there.

### 10.2 Patch 1: the protocol (libavformat/ffstreamer.c)

1. **It opens the channel** (§4) and makes the opening exchange (§7.1–§7.3).
2. **It reads in blocks.**
   - It asks for whole blocks, aligned to the block size, the last one trimmed at the file's size.
   - It keeps the last `cache_blocks` blocks (§4.1). The parent sets it to at least 1.5 × the streams
     the run reads + 2, since a file whose tracks are stored one after another is read from that many
     places at once.
   - Measured on a 10-track MP4 with no interleaving, under prepare's mapping of every track, the
     bytes fetched were these multiples of the file's size:

     | Blocks kept | 8 | 12 | 16 |
     | --- | --- | --- | --- |
     | Bytes fetched | 403× | 4.3× | 1.2× |

     An interleaved file is about 1.1× at 8.
   - It answers a read of any size from them. FFmpeg's I/O layer usually asks 32 KiB, but reads a
     packet larger than its buffer at full size, and grows its buffer while probing. A read may come
     back short; FFmpeg loops.
   - It does not set `max_packet_size`, which would change FFmpeg's own buffering.
3. **Seeking and end-of-file are local.**
   - It handles `SEEK_SET`, `SEEK_CUR`, `SEEK_END` and `AVSEEK_SIZE`, the last answered with the file's
     size. FFmpeg strips `AVSEEK_FORCE` before calling it.
   - A read at or past the end returns `AVERROR_EOF`, never 0: a 0 makes FFmpeg's retry loop spin.
   - It never returns `AVERROR(EAGAIN)`, which makes FFmpeg busy-wait.
4. **It prefetches when reading straight through:** when the block just needed follows the one needed
   before it, it asks for the next as well, within the outstanding limit. Measured on FFmpeg's own read
   traces, this keeps nearly all of always-prefetching's gain and never costs more than no prefetch
   ([bench/RESULTS.md](bench/RESULTS.md)).
5. **It waits without a timeout, but stays interruptible.**
   - While waiting, it polls in slices of 100 ms and checks FFmpeg's interrupt callback
     (`ff_check_interrupt`). When that fires (a signal), the read returns `AVERROR_EXIT`. FFmpeg's
     handlers make `poll` return `EINTR`, which the loop takes as a reason to look again, not a fault.
   - Without that check, a blocked read ignores the callback, since ffmpeg's signal handlers restart
     interrupted calls. ffmpeg's own stop would then hang its scheduler's join on the blocked thread.
   - This is protection, not a stop the parent uses: the parent kills.
6. **A fault is sticky.** Once a read has failed, whether by the channel ending (`CHANNEL`), a
   mismatched answer (`PROTOCOL`), or the interrupt, every later read on the channel returns the same
   error. A demuxer that ignores one error can't read on.
7. **Writing a frame:** one `write()`, guarded against SIGPIPE whatever the program:
   1. note whether SIGPIPE is already pending;
   2. block it with `pthread_sigmask`;
   3. write;
   4. on `EPIPE`, if it wasn't already pending, consume it with a zero-timeout `sigtimedwait`;
   5. restore the mask.

   ffmpeg already ignores SIGPIPE, and ffprobe needs nothing more than this.
8. **The close sends `DONE`** (§7.5), unless it has already been sent, then closes its descriptors. It
   does not drain `DATA` still owed. The parent's writer for that channel blocks until FFmpeg exits,
   which harms nothing, since writers are per channel (§9.3).

### 10.3 Patch 2: fftools (about 15 lines)

It makes an input's read error the run's exit status, where ffmpeg and ffprobe exit 0 today. It
promotes `pb->error` (when negative and not `EAGAIN`) in the three places below. An `AVERROR_EXIT`,
which is an interrupt, is promoted as `AVERROR(EINTR)`.

**A fourth place covers an interrupt during an input's open,** which never reaches `pb->error`.
- Unchanged, ffmpeg's `main` turns `AVERROR_EXIT` into exit 0 (`ffmpeg.c:1047`; reproduced: a SIGTERM
  during an input's open exited 0).
- The patch makes it `if (ret == AVERROR_EXIT) ret = received_nb_signals ? 255 : 0;`, so a signal
  always exits non-zero, and `-h` and `-version` still exit 0.
- ffprobe needs nothing: it installs no interrupt callback and no handlers, so a signal simply ends it.
- after `input_thread`'s loop, in `ffmpeg_demux.c`;
- after `avformat_find_stream_info` in `ifile_open`;
- in ffprobe's `probe_file`, before its end.

It is generic, and could be offered upstream on its own merits.

### 10.4 What fftools can't do

fftools merges every thread's error into one code with no record of its source. So whether a failure
came from demuxing, decoding or writing an output can't be told from inside FFmpeg, and `DONE` doesn't
try. Its `OK` and `ERR` are about the channel alone, and the exit status about the run.

### 10.5 Building

- The patches go after jellyfin-ffmpeg's own series (`debian/patches/series`, applied by
  `quilt push -a`), and are generated against the patched tree. jellyfin's 0028, 0077 and 0089 touch
  `ffmpeg_demux.c`.
- **Registration:**
  - one `extern` line in `libavformat/protocols.c`;
  - one `OBJS` line in `libavformat/Makefile`;
  - one `ffstreamer_protocol_deps=…` line in `configure`, so builds without POSIX pipes (win64) drop
    the protocol rather than fail.
- jellyfin's configure has no `--disable-everything`, so the protocol is built by default.
- No public header changes and no new exported symbol.

**The patch's shape:**

| File | Lines |
| --- | --- |
| `libavformat/ffstreamer.c` (new) | 600–800 |
| registration (`protocols.c`, `Makefile`, `configure`) | 3 |
| `doc/protocols.texi` | ~30 |
| `fftools/ffmpeg_demux.c` | ~10 |
| `fftools/ffmpeg.c` | ~2 |
| `fftools/ffprobe.c` | ~4 |

## 11. No deadlock

Five premises, each a rule both sides must keep:
1. **The parent reads each channel's requests on their own path,** independent of every writer
   (§9.3).
2. **FFmpeg's requests are bounded:** at most 16 outstanding, each 18 bytes, plus one `DONE` of at
   most 1,046 bytes. That fits even an 8 KiB pipe, since small writes merge into the pipe's pages
   (measured: 127 `READ`s and a `DONE` fitted in 8 KiB).
3. **Whenever FFmpeg's side waits, it reads that channel's `DATA` pipe in order.**
4. **FFmpeg may stop reading any channel indefinitely while it waits on another.** fftools' scheduler
   does exactly that. So the parent's writers are per channel, with their own buffers, and no write
   waits on another channel (§9.3).
5. **Nothing the parent holds waits forever:**
   - a storage read has its deadline (§9.7);
   - a pause has the parent's patience (§9.10);
   - a run has its budget of bytes (§9.6).

Under these:
- **the request direction can't block;**
- **a `DATA` write blocks only on its own channel,** while FFmpeg is busy with something else, paused,
  or exiting, none of which the parent mistakes for a failure;
- **the parent's held reads are bounded.**

## 12. Testing

- **A reference parent** (in `test/`) that speaks the protocol and can be slow, silent, wrong or short,
  and kill at any point.
- **The protocol's own suite:**
  - every rule in §3–§11, from both sides;
  - partial frames, and two frames in one read;
  - a wrapping sequence, and answers out of order;
  - every violation the parent must catch;
  - the overflow cases of §8, built at -O2;
  - the in-flight count at its limit, with slots freed at the header;
  - a malformed `REQUEST`, and a declined one, with exactly one `DONE`.
- **Every run shape that reads a film,** read directly and through the protocol, compared byte for
  byte: probe, read-through, subtitle extraction, chunk copy, encode and sound, and HLS. Encodes run
  at one thread, since an x264 encode with VBV differs between direct runs too.
- **The exit-status patch:** an input cut mid-read in the demux loop, in `find_stream_info`, and in
  ffprobe's packet loop each exit non-zero.
- **Several inputs:** two inputs with one choked by fftools' scheduler while the other is served. No
  hang.
- **Hostile cases:**
  - a reopening concat, and a manifest naming `pipe:5`;
  - huge, negative and out-of-range fields;
  - a pipelining FFmpeg;
  - half frames;
  - death of either side mid-frame;
  - a stop while a read waits (the interrupt);
  - wrong, missing and defaulted descriptors;
  - a URL with a suffix;
  - a second open after close;
  - each way of starting a process, which must fail (§9.1).
- **Descriptors:** a run launched while another is being set up never inherits its pipes.
- **Resources:** channels at the 8 KiB fallback past the per-user pipe cap still work.
- **The cache and the budget:** against MP4s whose tracks are stored one after another, at 10 and more
  streams, with the cache set from the stream count, before the budget's figures are fixed.
- **The seccomp filter, on amd64 and arm64:** threads work; every way of starting a process fails;
  `setsid` and `setpgid` fail; hardware encoders work on real devices.
- **Killing:** an FFmpeg that tried to leave its process group is still killed by pidfd.
- **An interrupt during the protocol's open, during `read_header`, during probing, and mid-run:** each
  ends with a non-zero exit, and with `DONE` `INTERRUPTED` or `OK` where `DONE` is sent.
- **Exactly one `DONE`** for a `run` of 0, and for a failure to allocate during the open.
- **The cache** at more than 41 streams.
- **Pausing:** repeated SIGSTOP and SIGCONT while the channel waits.
- **Descriptors:** a gap in the inherited descriptors is refused by the parent before launch.
- **A dead share:** a storage read that never returns (a FUSE file) must leave FFmpeg killable at
  every point, and the run must end at the read's deadline.
- **Confined:** under Landlock with no media file granted, and seccomp as in §9.1.
- **At every jellyfin-ffmpeg release the patches move to:** the whole suite, on amd64 and arm64.

## 13. Performance

Measured in [bench/](bench/RESULTS.md), on a file in the page cache, so the transport alone:

| Transport | MB/s | CPU-s per GB |
| --- | --- | --- |
| FFmpeg reads the file itself | ~4,300 | 0.23 |
| A pipe with a 1 MiB buffer, the parent reading into its memory and writing (chosen) | ~940 | 1.4 |
| The same with `splice` from the file (not used: §9.4) | ~1,500–2,000 | 0.6–0.7 |
| Shared memory (not chosen) | ~2,000 | 0.45 |

The chosen path costs about 15% of one core at a NAS's 110 MB/s. If that ever matters, a staged
`splice` keeps the safety: the file into a pipe private to the parent, then pipe-to-pipe into
FFmpeg's. It is not in version 1.

**Still to measure:**
- a real NAS under real competing load;
- a chunk run's time to its first output;
- block sizes from 256 KiB to 4 MiB;
- one `READ` outstanding against two;
- the 8 KiB fallback's throughput.

## 14. Still open

1. **The budget's figures per kind of run** (§9.6), set after the cache and interleaving tests.
2. **The pipe-memory budget** the parent divides among its channels (§4.3), and the value its
   deployment raises `fs.pipe-user-pages-soft` to.
3. **The exact formula for `cache_blocks`** (§10.2), confirmed by those same tests.
