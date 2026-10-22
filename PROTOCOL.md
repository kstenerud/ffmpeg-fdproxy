# ffstreamer, the read protocol: draft 3

**Status:** draft 3, 2026-10-22. It takes in:
- the author's decisions after draft 2: `DONE`'s `OK` carries no byte count, and block alignment is
  not a rule;
- every must-fix and the should-fixes of the first audit
  ([audits/2026-10-22-draft-2-protocol.md](audits/2026-10-22-draft-2-protocol.md)), which the author
  accepted.

It goes to further audits next. No patch is written until it has survived them. What is still open
is in §13.

**What changed since draft 2:**
- The parent never splices from the storage (§9). A splice made FFmpeg unkillable on a dead share,
  which the audit reproduced.
- `ACCEPT` and `DECLINE` are gone: the first `READ` accepts, and `DONE` declines.
- The graceful stop and `INTERRUPTED` are gone.
- `DONE`'s rules are complete (§7.5).
- Answers are checked first-in-first-out.
- Overflow-safe checks (§8).
- Exact lengths.
- The in-flight rule and the no-deadlock argument (§10).
- Descriptor hygiene (§4).
- A budget on the bytes served (§9).
- The partial-frame deadline is gone.

## 1. What it is for

A parent process, here a media server's node, runs FFmpeg to read a film. FFmpeg does not open the
film. The parent hands FFmpeg a channel at its start and serves every byte FFmpeg reads of its
input, by offset. Then:

- **FFmpeg never touches the storage.** A dead network share blocks a thread of the parent, which
  knows and decides. It never blocks FFmpeg in the kernel, where it cannot be killed.
- **Every read is the parent's to schedule:** to order against other work on the same disk, to pace,
  to read ahead.
- **FFmpeg needs no path to the film and creates no socket.** Its confinement stays as tight as today:
  Landlock with no media file granted, and seccomp refusing `socket` and `socketpair`.
- **The parent judges every run.** A run whose input could not be read in full never passes as
  complete.

**Scope.** The protocol carries only the input's bytes:
- **the job stays on FFmpeg's command line,** where its confinement already needs it;
- **FFmpeg writes its own outputs** to local disk.

## 2. Roles

| | The parent | FFmpeg's side |
| --- | --- | --- |
| Decides | what is read, the terms, how long a run may take, whether a run succeeded | what to ask for and when, within the terms |
| Does | creates the channels, launches FFmpeg, reads the storage, sends the bytes | asks, waits, hands the bytes to FFmpeg's demuxer |
| Trusts | nothing FFmpeg sends: FFmpeg may have been taken over by a crafted film | the parent |
| Ends a run | by killing FFmpeg (SIGKILL of its process group), at any time, for any reason | by exiting, after sending `DONE` |

There is no message to stop, abort or report a read error. The parent kills.

`DONE` is FFmpeg's account of the run, **advisory and not a trust boundary.** A compromised FFmpeg
can lie in it as it can in its outputs.

## 3. Versions and compatibility

- Versions are integers from 1. Version 0 is invalid.
- **Anything new is a new version.** There are no optional or ignorable fields and no extensions.
  Within a version, every message has exactly one form.
- **`HELLO` is frozen in every version, header included** (§6.1). Every later message takes the form
  of the version both sides work out from it.

## 4. Channels

- **One channel per input:** two pipes, one each way, created by the parent and inherited by FFmpeg
  at dedicated descriptors. stdin, stdout and stderr are never used: stdout carries results, and a
  stray write to it would corrupt a channel.
- **Naming.** Each input names its channel's descriptors and restricts itself to this protocol:
  `-protocol_whitelist ffstreamer -ffstreamer_read_fd 7 -ffstreamer_write_fd 8 -i ffstreamer:`. Here `read_fd` is where FFmpeg
  reads the parent's messages, and `write_fd` where it writes its own. The whitelist reaches nested
  opens, so a crafted manifest naming `pipe:7` or `fd:` cannot read the channel's descriptor directly.
- **Pipe sizes.**
  - The parent asks for 1 MiB on the parent-to-FFmpeg pipe (`F_SETPIPE_SZ`), where the bytes flow. The
    default 64 KiB halved throughput in measurement.
  - If the kernel refuses (`EPERM`: a user's pipe buffers are capped, 64 MiB by default), the parent
    logs it and goes on at the default.
  - The FFmpeg-to-parent pipe stays at its default: requests are tiny.

**FFmpeg's side, on opening the channel:**
- sets `FD_CLOEXEC` on both descriptors;
- checks that each is a FIFO opened for the right direction (read-only for `read_fd`, write-only for
  `write_fd`);
- refuses to be opened for writing;
- refuses a second open of the same channel, while the first session goes on.

A descriptor that isn't what it should be fails the open. Without the check, a missed inheritance
could let FFmpeg's own earlier `open` land on that number and write `HELLO` into an output file.

**The parent:**
- launches FFmpeg in its own process group;
- closes its copies of FFmpeg's ends of the pipes once FFmpeg has started.

## 5. Frames

**The header.** Every message is a frame of a 6-byte header and a body:

| Field | Type | Notes |
| --- | --- | --- |
| type | int8 | §7. 0 is never valid |
| sequence | int8 | §6 |
| length | int32 | the body's length in bytes |

**Integers.** All integers are signed and little-endian. **A negative value on the wire is a
violation.** That catches corrupt or forged fields. It does not make arithmetic safe: §8's checks are
written so they can't overflow.

**Lengths are exact.**
- Every message of fixed form has one exact body length.
- A message with a variable field must have a length equal to the sum of its fields, each variable
  field delimited by its own length before its bytes.
- An enum value not defined in the version is a violation.
- A side checks the length before reading the body.

**Size.** Every frame FFmpeg's side sends is at most 4,096 bytes and is written with a single
`write()`. On Linux that write is atomic (`PIPE_BUF`). This is a convenience, not something the
protocol depends on: the parent reads frames however they arrive.

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
- At most a handful of requests are outstanding (§7.4), so a wrap can never confuse two answers.

On FFmpeg's side, **one mutex per channel covers taking the next number and writing the frame.**
`READ`s come from a demuxer thread and `DONE` from fftools' main thread, and without it two frames
could reach the pipe out of their numbering order.

## 7. Messages

| Type | Code | From | Answered by | Body (exact bytes) |
| --- | --- | --- | --- | --- |
| `HELLO` | 1 | both | FFmpeg's, by the parent's `HELLO` | 4 |
| `CAPABILITY` | 2 | FFmpeg | `REQUEST` | 9 |
| `REQUEST` | 3 | parent | the first `READ`, or `DONE` | 27 |
| `READ` | 4 | FFmpeg | `DATA` | 12 |
| `DATA` | 5 | parent | — | the `READ`'s length |
| `DONE` | 6 | FFmpeg | — | 9, or 16 plus the message |

### 7.1 `HELLO`

**Frozen in every version.** The whole frame is `type 1, sequence 0, length 4`, then:

| Field | Type |
| --- | --- |
| lowest version this side speaks | int16, at least 1 |
| highest version this side speaks | int16, at least the lowest |

**FFmpeg's side** sends its `HELLO` as soon as the channel is opened.

**The parent:**
- kills FFmpeg if FFmpeg's range is invalid (0, or lowest above highest);
- kills it if the two ranges don't overlap, logging both ranges;
- otherwise answers with its own `HELLO`, echoing 0.

**FFmpeg's side** checks the parent's range the same way, and fails the open if it is invalid or does
not overlap. It sends nothing before the parent's `HELLO` has arrived, since `CAPABILITY`'s form
depends on the version.

Each side works out the version itself:

    version = min(the two highest)    valid when version >= max(the two lowest)

### 7.2 `CAPABILITY` (version 1)

| Field | Type | Notes |
| --- | --- | --- |
| preferred block size | int32 | > 0 |
| largest `DATA` body it accepts | int32 | ≥ the preferred block size |
| most `READ`s it can have outstanding | int8 | ≥ 1 |

### 7.3 `REQUEST` (version 1)

The parent's terms for this input. It echoes `CAPABILITY`'s number.

| Field | Type | The rule |
| --- | --- | --- |
| version | int16 | the version both sides worked out; a cross-check |
| run | int64 | ≥ 0; the parent's identifier for the run, echoed in `DONE` |
| file size | int64 | ≥ 0 |
| block size | int32 | > 0, and ≤ the largest body `CAPABILITY` gave |
| longest `READ` | int32 | ≥ the block size, and ≤ the largest body |
| `READ`s outstanding | int8 | ≥ 1, and ≤ what `CAPABILITY` gave |

**A parent that can't work within `CAPABILITY`** kills FFmpeg.

**FFmpeg's side checks every rule above,** with the exact length.
- **If the terms are good,** its first `READ` accepts them.
- **If they are not,** it sends `DONE` with `ERR` and kind `TERMS` instead, and the open fails.

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

**When the parent can't read,** because the storage failed or the file changed since `REQUEST`, it
does not answer. It kills FFmpeg. The parent checks the file's identity itself (device, inode, size
and modification time). A file that grows or changes during a run is a changed file.

Alignment is FFmpeg's side's own business. The parent serves any range that holds to the rules.

### 7.5 `DONE`

FFmpeg's verdict, like a `Result`.

| Field | Type | Notes |
| --- | --- | --- |
| run | int64 | echoes `REQUEST` |
| result | int8 | 0 `OK`, 1 `ERR` |
| *if `ERR`:* kind | int8 | the table below |
| *if `ERR`:* code | int32 | FFmpeg's error code, negated. A code that isn't negative, or is `INT32_MIN`, is sent as 0 |
| *if `ERR`:* message length | int16 | 0 to 1,024 |
| *if `ERR`:* message | that many bytes | for the logs |

| Kind | Code | Means |
| --- | --- | --- |
| `TERMS` | 1 | FFmpeg's side declined `REQUEST`'s terms |
| `CHANNEL` | 2 | the channel to the parent ended: end-of-file or a broken pipe, so the parent died |
| `PROTOCOL` | 3 | the parent broke the protocol: a wrong type, number or length |
| `OPTIONS` | 4 | FFmpeg refused its command line after this channel opened (an output's options) |
| `MEDIA` | 5 | the content could not be demuxed, decoded or encoded |
| `OUTPUT` | 6 | writing an output failed (for example, a full disk) |
| `INTERNAL` | 7 | anything else |

**When it is sent:**
- **Never before `REQUEST`.** If the opening exchange fails, nothing more is sent; the open fails,
  FFmpeg exits non-zero, and the missing `DONE` already says so.
- **After `REQUEST`, it may be sent in any state, with `READ`s outstanding.** It is the last message
  on the channel.
- **At FFmpeg's exit, on every channel that got a `REQUEST`.** That includes channels the demuxer has
  closed, and channels where FFmpeg's side saw a fault (`CHANNEL`, `PROTOCOL`). On those, `DONE` is
  the only message sent after the fault.

**The parent, on `DONE`:**
- stops serving that channel;
- drops any `READ` still waiting;
- doesn't treat a `DONE` arriving after `READ`s it hasn't answered as a fault.

## 8. Checks that cannot overflow

Signed fields stop forged negatives, not arithmetic overflow. Added, `offset + length` can wrap. In
C, gcc at -O2 deletes a test like `offset + length < 0`, because signed overflow is undefined (the
audit reproduced it). Every bound is therefore checked by subtraction from a known non-negative
value, never by addition:

    0 <= offset  &&  offset <= size  &&  0 < length  &&  length <= longest  &&  length <= size - offset

The same holds for every check on either side. A side never computes a size or allocates from a
number the other side sent. It allocates only from the terms it set or accepted.

## 9. The parent's side

1. **It launches FFmpeg** with the job, the whitelist and the descriptors on its command line,
   confined, in its own process group, and closes its copies of FFmpeg's pipe ends.
2. **It treats every frame from FFmpeg as hostile.** On any violation (§5–§8), it logs exactly what
   was wrong and kills FFmpeg. It sends nothing.
3. **It reads FFmpeg's requests independently of writing `DATA`,** so it sees every request, including
   a violation, while a `DATA` write is blocked. Silence between frames is never a failure.
4. **It reads the storage into its own memory, then writes.**
   - It reads with `pread` into a buffer it owns: never `splice` from the file into FFmpeg's pipe, and
     never an `mmap` of the film.
   - A `splice` holds FFmpeg's pipe locked while the storage read waits. FFmpeg's `read()` of that
     pipe then waits on the lock uninterruptibly, and a dead share leaves FFmpeg unkillable
     (reproduced in the audit).
   - It releases its place on the disk as soon as its read of the storage returns, before writing to
     the pipe, so a paused or slow FFmpeg never holds the disk.
   - A read the storage hasn't answered keeps its place and its own buffer until it returns, so a dead
     share pins at most one thread per place the disk has.
5. **A `READ` stops counting as outstanding when its `DATA` header has been written** (§10).
6. **It keeps a budget of bytes served per run:** at most k × the file size, with k = 4 by default,
   a setting. A run over its budget is killed and logged. A crafted file that makes FFmpeg re-read
   blocks over and over would otherwise compete for the disk without end.
7. **It owns all time.** It kills FFmpeg:
   - when it can't read;
   - when the file changed;
   - when the run is no longer wanted;
   - when its own limits pass.

   Its watchdog counts the bytes it serves as progress, and doesn't count time it holds a `READ`
   itself as FFmpeg standing still.
8. **It knows a run has ended when the process exits,** never from end-of-file on a pipe. Then:
   1. it closes its ends of the channels, which frees a writer stuck behind anything still holding
      them;
   2. it reads what is left in each request pipe without blocking, to collect `DONE`;
   3. only then does it judge.
9. **It judges.** A run succeeded only if:
   - FFmpeg exited 0;
   - every channel's `DONE` said `OK` and echoed the run;
   - the parent itself had no read failure.

   A run the parent killed is a stop, not a failure, and the parent records it as one. Exiting without
   `DONE` is a failure.
10. **It sanitises text before logging it.** Text from FFmpeg is hostile: control characters, escapes,
    newlines that could forge log lines. It escapes them. Invalid UTF-8 is not a violation.

## 10. FFmpeg's side

**Patch 1: the protocol,** a URL protocol in libavformat.

1. **It opens the channel** (§4) and does the opening exchange (§7.1–§7.3).
2. **It reads in blocks.**
   - It asks for whole blocks, aligned to the block size, the last one trimmed at the file's size.
   - It keeps the last few (4) and answers FFmpeg's small reads from them.
   - FFmpeg's I/O layer asks 32 KiB at a time, so this is what keeps the parent's reads large.
3. **Seeking and end-of-file are local.** It keeps the position, `AVSEEK_SIZE` answers with the file's
   size, and a read at or past the end returns end-of-file without asking.
4. **It prefetches when reading straight through:** when the block just needed follows the one needed
   before it, it asks for the next as well, within the outstanding limit. A prefetched block never
   used is kept until it falls out of the cache.
   - Measured on FFmpeg's own read traces: this keeps nearly all of always-prefetching's gain (a
     read-through on a busy NAS took 12.1 s against 20.3 s).
   - It never costs more than no prefetch ([bench/RESULTS.md](bench/RESULTS.md)).
5. **It never times out.** It waits for the parent as long as the parent likes. Only the parent's kill
   or the channel ending ends a wait. On the channel ending, the read returns `AVERROR(EIO)`, and
   `DONE` will say `CHANNEL`.
6. **It checks every answer,** as in §7.4. A mismatch is the parent's fault. It fails the input, and
   on that channel sends only `DONE` (`PROTOCOL`) after it.
7. **Channel state outlives the input.** It lives in a registry that outlives FFmpeg's input context,
   so `DONE` can be sent on a channel the demuxer has already closed. libavformat exports one function
   through which fftools sends `DONE` on every channel. Its name and form are for the patch.

**Patch 2: fftools**, small:
- at FFmpeg's single exit point, it sends `DONE` on every channel that got a `REQUEST`;
- it makes FFmpeg exit non-zero when an input failed, where FFmpeg today finishes its outputs and
  exits 0;
- ffprobe gets the same, and also ignores `SIGPIPE`, which ffmpeg already does and ffprobe doesn't;
- for use upstream, the protocol also blocks `SIGPIPE` around its own writes, whatever the program.

**No deadlock.** The parent reads requests on a path independent of its `DATA` writes (§9.3). So the
request direction cannot block, for three reasons:
- FFmpeg's requests are bounded: at most the outstanding count, each 18 bytes, plus one `DONE` under
  4,096 bytes;
- small writes merge into the pipe's pages, so they fit its default buffer many times over;
- whenever FFmpeg's side waits, it reads the `DATA` pipe in order.

The `DATA` direction blocks only while FFmpeg is busy or paused, which the parent never mistakes for
a failure (§9.3). Each premise is a rule both sides must keep.

## 11. Performance

Measured in [bench/](bench/RESULTS.md), on a file in the page cache, so the transport alone:

| Transport | MB/s | CPU-s per GB |
| --- | --- | --- |
| FFmpeg reads the file itself | ~4,300 | 0.23 |
| A pipe with a 1 MiB buffer, the parent reading into its memory and writing (chosen) | ~940 | 1.4 |
| The same with `splice` from the file (not used: §9.4) | ~1,500–2,000 | 0.6–0.7 |
| Shared memory (not chosen) | ~2,000 | 0.45 |

The chosen path costs about 15% of one core at a NAS's 110 MB/s.
- If that ever matters, a staged `splice` keeps the safety: the file into a pipe private to the
  parent, then pipe-to-pipe into FFmpeg's. A stuck storage read then holds only the private pipe.
  It is not in version 1.
- A request costs a round trip of tens of microseconds, and the header 6 bytes per block, so a block
  of 1 MiB keeps both negligible.

**Still to measure:**
- a real NAS under real competing load;
- a chunk run's time to its first output;
- block sizes from 256 KiB to 4 MiB;
- one `READ` outstanding against two.

## 12. Testing

- **A reference parent** (in `test/`) that speaks the protocol and can be slow, silent, wrong or short,
  and kill at any point.
- **The protocol's own suite:**
  - every rule in §3–§10, from both sides;
  - partial frames, and two frames in one read;
  - a wrapping sequence, and answers out of order;
  - every violation the parent must catch;
  - the overflow cases of §8, built at -O2;
  - `DONE` from the main thread racing `READ`s from the demuxer thread.
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
  - a reopening concat, and a manifest naming `pipe:7`;
  - huge, negative and out-of-range fields;
  - a pipelining FFmpeg;
  - half frames;
  - death of either side mid-frame;
  - SIGSTOP mid-answer;
  - wrong or missing descriptors;
  - a run that re-reads past its budget.
- **A dead share:** a storage read that never returns (a FUSE file, as the audit used) must leave
  FFmpeg killable at every point.
- **Confined:** under Landlock with no media file granted, and seccomp refusing sockets.
- **At every jellyfin-ffmpeg release the patches move to:** the whole suite, on amd64 and arm64.

## 13. Still open

1. **The libavformat function** fftools uses to send `DONE` (§10.7). It needs a public symbol, which
   matters for upstream.
2. **The budget's k** (§9.6), and whether it should differ by kind of run: a probe reads little, and a
   read-through reads the file once.

The protocol's name is `ffstreamer` (the author, 2026-10-22). The URL is `ffstreamer:`, and its options
are `-ffstreamer_read_fd` and `-ffstreamer_write_fd`.
