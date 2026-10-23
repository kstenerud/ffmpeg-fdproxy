ffstreamer
==========

A protocol for feeding a file to FFmpeg through two inherited pipes, so that FFmpeg reads its input
without ever opening it.


### Features

* **FFmpeg never touches the storage.** A storage read that hangs blocks the parent, which can see
  it and decide, never FFmpeg in the kernel, where it cannot be killed.
* **Every read is the parent's to schedule:** to order against other work on the same disk, to pace,
  to read ahead, or to hold back.
* **Confinement stays tight.** FFmpeg needs no path to its input and creates no socket.
* **The parent judges every run.** A run whose input could not be read whole never passes as
  complete.
* **Nothing FFmpeg sends is trusted.** Every frame is bounded and checked, without arithmetic that
  can overflow, before it is used.
* **Minimal:** six messages, fixed forms, one version.


### Contents

* [Terms and Conventions](#terms-and-conventions)
* [Use Case](#use-case)
* [General Operation](#general-operation)
    * [Roles](#roles)
    * [A Session](#a-session)
* [Channels](#channels)
    * [Naming the Input](#naming-the-input)
    * [Descriptors](#descriptors)
    * [Pipe Sizes](#pipe-sizes)
* [Frames](#frames)
    * [Frame Header](#frame-header)
    * [Integers](#integers)
    * [Lengths](#lengths)
    * [Frame Size](#frame-size)
    * [Sequence Numbers](#sequence-numbers)
* [Messages](#messages)
    * [Message Types](#message-types)
    * [HELLO](#hello)
    * [CAPABILITY](#capability)
    * [REQUEST](#request)
    * [READ](#read)
    * [DATA](#data)
    * [DONE](#done)
        * [Error Kinds](#error-kinds)
        * [When DONE Is Sent](#when-done-is-sent)
* [Versions](#versions)
* [Checks Without Overflow](#checks-without-overflow)
* [The Parent](#the-parent)
    * [Launching](#launching)
    * [Serving](#serving)
    * [Budget](#budget)
    * [Read Deadline](#read-deadline)
    * [Pausing](#pausing)
    * [Stopping](#stopping)
    * [End of a Run](#end-of-a-run)
    * [Judging a Run](#judging-a-run)
    * [Logging Text](#logging-text)
* [FFmpeg's Side](#ffmpegs-side)
    * [Opening](#opening)
    * [Reading in Blocks](#reading-in-blocks)
    * [Seeking and End of File](#seeking-and-end-of-file)
    * [Prefetching](#prefetching)
    * [Waiting and Interrupts](#waiting-and-interrupts)
    * [Sticky Faults](#sticky-faults)
    * [Writing a Frame](#writing-a-frame)
    * [Closing](#closing)
    * [Exit Status](#exit-status)
* [No Deadlock](#no-deadlock)
* [Security Rules](#security-rules)
    * [Resource Limits](#resource-limits)
* [Full Example](#full-example)
* [Formal Grammar](#formal-grammar)
* [License](#license)



Terms and Conventions
---------------------

**The following bolded, capitalized terms have specific meanings in this document**:

| Term             | Meaning                                                                                          |
| ---------------- | ------------------------------------------------------------------------------------------------ |
| **MUST (NOT)**   | If this directive is not adhered to, the document or implementation is invalid.                  |
| **SHOULD (NOT)** | Every effort should be made to follow this directive, but it's still conformant if not followed. |
| **MAY (NOT)**    | It is up to the implementation to decide whether to do something or not.                         |
| **CAN**          | Refers to a possibility which **MUST** be accommodated by the implementation.                    |
| **CANNOT**       | Refers to a situation which **MUST NOT** be allowed by the implementation.                       |

**Additional terms**:

| Term              | Meaning                                                                                      |
| ----------------- | -------------------------------------------------------------------------------------------- |
| **parent**        | The process that launches FFmpeg and serves its input, by offset.                            |
| **FFmpeg's side** | The `ffstreamer` protocol inside FFmpeg (`libavformat/ffstreamer.c`), and the fftools change of [Exit Status](#exit-status). |
| **channel**       | The two pipes that carry one input of one FFmpeg process.                                    |
| **run**           | One FFmpeg process, from launch to exit.                                                     |
| **violation**     | Anything FFmpeg sends that breaks this specification.                                        |

**Additional conventions**:

 * All integers are signed, two's complement, and little endian.
 * Byte sequences are written in hexadecimal, two digits per byte, in the order they are sent, with
   `//` comments.
 * FFmpeg's error codes are its `AVERROR` values: negative `errno` values and FFmpeg's own tags.



Use Case
--------

A parent, such as a media server, runs FFmpeg on files it stores, possibly on a network share, and
runs it confined: FFmpeg reads files that anyone could have crafted, so it gets no network, no other
process, and as little of the filesystem as possible.

Granting FFmpeg its input by path leaves two problems:

* **A storage read that never returns** (a network share whose host is gone) leaves FFmpeg waiting
  in the kernel, where no signal ends it, holding whatever it holds.
* **The parent can't schedule FFmpeg's reads,** which compete with every other reader of the same
  disk.

ffstreamer moves every read of the input into the parent:

```
+------------------------------------+
|              FFmpeg                |
|   demuxer  <--  ffstreamer: input  |
+---------------^------------|-------+
                |            | READ (offset, length)
           DATA |            v
+---------------|--------------------+
|              parent                |
|   checks, schedules, reads storage |
+------------------------------------+
                |
             storage
```

**Scope.** The protocol carries only an input file's bytes:

* the job stays on FFmpeg's command line, where its confinement already needs it;
* FFmpeg writes its outputs itself;
* a run that reads bytes the parent already holds in memory (re-encoding an image, converting
  subtitle text) **SHOULD** keep reading them from stdin. ffstreamer is for files read from
  storage.



General Operation
-----------------

### Roles

|            | The parent                                                                 | FFmpeg's side                                      |
| ---------- | -------------------------------------------------------------------------- | -------------------------------------------------- |
| Decides    | what is read, the terms, how long a run may take, whether a run succeeded  | what to ask for and when, within the terms         |
| Does       | creates the channels, launches FFmpeg, reads the storage, sends the bytes  | asks, waits, hands the bytes to FFmpeg's demuxer   |
| Trusts     | nothing FFmpeg sends: FFmpeg may have been taken over by a crafted file    | the parent                                         |
| Ends a run | by killing FFmpeg, at any time, for any reason                             | by exiting                                         |

There is no message to stop, abort, pause or report a read error:

* The parent pauses a run by holding its `READ`s and stopping the process ([Pausing](#pausing)).
* The parent stops a run by killing it ([Stopping](#stopping)).
* **FFmpeg's exit status is the run's verdict.** `DONE` is FFmpeg's account of its channel:
  advisory, and not a trust boundary. A compromised FFmpeg can lie in it, as it can in its outputs.


### A Session

FFmpeg's side speaks first and numbers what it sends; every message from the parent answers one.

| FFmpeg's side                      | Parent                                   |
| ---------------------------------- | ---------------------------------------- |
| `HELLO` 0 (versions it speaks)     |                                          |
|                                    | `HELLO` 0 (versions it speaks)           |
| `CAPABILITY` 1 (what it can take)  |                                          |
|                                    | `REQUEST` 1 (the terms)                  |
| `READ` 2                           |                                          |
| `READ` 3                           | `DATA` 2                                 |
| `READ` 4                           | `DATA` 3                                 |
|                                    | `DATA` 4                                 |
| ...                                | ...                                      |
| `DONE` n (its account)             |                                          |
| (exits)                            | (judges the run)                         |



Channels
--------

A channel is two pipes, one each way, for one input file of one FFmpeg process. The parent creates
them, and FFmpeg inherits them at descriptors of its own. stdin, stdout and stderr are never a
channel: stdout carries results, and a stray write there would corrupt a channel.


### Naming the Input

Each input restricts itself to this protocol and names its channel:

    -protocol_whitelist ffstreamer -ffstreamer_read_fd 4 -ffstreamer_write_fd 5 -ffstreamer_cache_blocks 48 -i ffstreamer:

| Option                    | Meaning                                                                                        |
| ------------------------- | ---------------------------------------------------------------------------------------------- |
| `-ffstreamer_read_fd`     | The descriptor FFmpeg reads the parent's messages from. Default -1.                           |
| `-ffstreamer_write_fd`    | The descriptor FFmpeg writes its own messages to. Default -1.                                  |
| `-ffstreamer_cache_blocks`| How many blocks FFmpeg's side keeps ([Reading in Blocks](#reading-in-blocks)), 2 to 1,024. Default 32. |

* The options are decoding options. An open with either descriptor left at -1 **MUST** fail.
* The URL **MUST** be exactly `ffstreamer:`, with nothing after it, so a relative reference resolved
  against it cannot open.
* The parent **MUST** name the whitelist. It reaches nested opens, so a crafted manifest naming
  `pipe:5` or `fd:` cannot read the channel's descriptor directly.
* The cache's memory is `cache_blocks` × the block size; the parent counts it in what a run may use.


### Descriptors

**The parent**:

* **MUST** create every pipe end close-on-exec, atomically (`pipe2(O_CLOEXEC)`).
* **MUST** hand each child only its own ends, by `dup2` at exec, so no other run, launched at any
  moment, inherits them.
* **MUST** hand FFmpeg its inherited descriptors contiguous, with no gap. A missing descriptor would
  be filled by the first descriptor FFmpeg opens or duplicates itself, which takes the lowest free
  number. Ordering alone can't prevent that; the `HELLO` round trip is what proves the channel.
* **MUST** close its own copies of FFmpeg's ends right after FFmpeg starts, so its reader of
  requests reaches end-of-file once FFmpeg has exited ([End of a Run](#end-of-a-run)).

**FFmpeg's side, on open**:

* **MUST** set `FD_CLOEXEC` on both descriptors;
* **MUST** check that each is a FIFO opened for its direction;
* **MUST** refuse to be opened for writing.

That check is a sanity check. The proof that the parent is on the other end is the `HELLO` round
trip.

**A channel carries one session.** FFmpeg's side **MUST** refuse to open a channel whose
parent-to-FFmpeg pipe is already open in the process, through whichever descriptor reaches it, and
**MUST** close both its descriptors when the session closes, so no later open reaches that pipe
again through them: unread `DATA` may still be in it. It knows a pipe by `fstat`'s device and inode,
not by descriptor number, which the process reuses for new pipes.


### Pipe Sizes

* **The parent-to-FFmpeg pipe carries the bytes.** The parent **SHOULD** size it from a budget of
  pipe memory divided among its concurrent channels, up to 1 MiB: at the default 64 KiB, throughput
  halves. A parent written in Go **MUST** set the size through `SyscallConn().Control`, never `Fd()`,
  which puts the descriptor in blocking mode, after which closing it no longer frees a blocked writer.
* **The FFmpeg-to-parent pipe stays at its default.** Requests are tiny.
* **Linux caps a user's pipe memory** (`fs.pipe-user-pages-soft`, 16,384 pages by default). Past the
  cap, `F_SETPIPE_SZ` fails with `EPERM`, and every new pipe holds 8 KiB and can't be grown. The
  protocol works at 8 KiB, at about half the speed. A parent that falls back **SHOULD** log it once,
  and its deployment **SHOULD** raise the cap if it runs many channels at once.



Frames
------

Every message is a frame: a 6-byte header, then a body.


### Frame Header

| Field    | Type  | Notes                                                       |
| -------- | ----- | ----------------------------------------------------------- |
| type     | int8  | The [message type](#message-types). 0 is never valid.       |
| sequence | int8  | 0 to 127 ([Sequence Numbers](#sequence-numbers))            |
| length   | int32 | The body's length in bytes                                  |

**Example**:

    04 02 0c 00 00 00                                // READ, number 2, a 12-byte body


### Integers

All integers are signed and little endian. **A negative value is a violation**, wherever it
appears. That catches corrupt or forged fields. It does not make arithmetic safe: see
[Checks Without Overflow](#checks-without-overflow).


### Lengths

* Every message of fixed form has one exact body length ([Message Types](#message-types)).
* `DONE`, the one message of variable form, **MUST** be bounded by its type before it is buffered
  (9 ≤ length ≤ 1,040), then checked to equal the sum of its fields.
* A variable field is delimited by its own length.
* An enumerated value not defined in the version is a violation.


### Frame Size

Every frame FFmpeg's side sends is at most 4,096 bytes and **MUST** be written with one `write()`.
On Linux that write is atomic (`PIPE_BUF`), so a kill can't leave half a frame. This is a
convenience: each side **MUST** read frames however they arrive. A short read, and two frames in
one read, are both normal; each side reads what is in the pipe into its own buffer and carves frames
out of it.


### Sequence Numbers

* FFmpeg's side numbers every message it sends. `HELLO` is 0, and each later message takes the next
  number, wrapping from 127 back to 0: `(n + 1) mod 128`.
* **Every parent message is an answer, and echoes the number of the message it answers.**
* **Answers are first-in-first-out:** an answer **MUST** echo the oldest outstanding request on its
  channel. FFmpeg's side **MUST** check that before using an answer, and the parent **MUST** check
  that every message it receives carries the number it expects next.
* `READ`s outstanding are at most 16 ([CAPABILITY](#capability)), so a wrap is harmless; FIFO
  checking would make it so in any case.

FFmpeg's side **MUST** hold one mutex per channel over taking a number and writing the frame.



Messages
--------

### Message Types

| Type         | Code | From          | Answered by                              | Body (bytes)                 |
| ------------ | ---- | ------------- | ---------------------------------------- | ---------------------------- |
| `HELLO`      | 1    | both          | FFmpeg's, by the parent's `HELLO`        | exactly 4                    |
| `CAPABILITY` | 2    | FFmpeg's side | `REQUEST`                                | exactly 9                    |
| `REQUEST`    | 3    | parent        | the first `READ`, or `DONE`              | exactly 27                   |
| `READ`       | 4    | FFmpeg's side | `DATA`                                   | exactly 12                   |
| `DATA`       | 5    | parent        | —                                        | exactly the `READ`'s length  |
| `DONE`       | 6    | FFmpeg's side | —                                        | 9 to 1,040                   |


### HELLO

**Frozen in every version.** The whole frame is type 1, sequence 0, length 4, then:

| Field                            | Type  | Rule                     |
| -------------------------------- | ----- | ------------------------ |
| lowest version this side speaks  | int16 | at least 1               |
| highest version this side speaks | int16 | at least the lowest      |

* FFmpeg's side sends its `HELLO` as soon as the channel is opened.
* The parent **MUST** kill FFmpeg if FFmpeg's range is invalid or the two ranges don't overlap, and
  **SHOULD** log both; otherwise it answers with its own `HELLO`, echoing 0.
* FFmpeg's side **MUST** check the parent's range the same way, and fail the open if it is invalid
  or doesn't overlap. It **MUST NOT** send anything more before the parent's `HELLO` has arrived.

Each side works out the version itself ([Versions](#versions)).

**Example**:

    01 00 04 00 00 00                                // HELLO, number 0, 4 bytes
       01 00                                         //   lowest version: 1
       01 00                                         //   highest version: 1


### CAPABILITY

What FFmpeg's side can take. Version 1.

| Field                                | Type  | Rule                          |
| ------------------------------------ | ----- | ----------------------------- |
| preferred block size                 | int32 | > 0                           |
| largest `DATA` body it accepts       | int32 | ≥ the preferred block size    |
| most `READ`s it can have outstanding | int8  | 1 to 16                       |

**Example**:

    02 01 09 00 00 00                                // CAPABILITY, number 1, 9 bytes
       00 00 10 00                                   //   preferred block: 1 MiB
       00 00 40 00                                   //   largest DATA: 4 MiB
       02                                            //   READs outstanding: 2


### REQUEST

The parent's terms for this input. Version 1. It echoes `CAPABILITY`'s number.

| Field              | Type  | Rule                                                                   |
| ------------------ | ----- | ---------------------------------------------------------------------- |
| version            | int16 | the version both sides worked out; a cross-check                       |
| run                | int64 | ≥ 1; the parent's identifier for the run, echoed in `DONE`             |
| file size          | int64 | 0 to 2^62 ([Checks Without Overflow](#checks-without-overflow))        |
| block size         | int32 | > 0, and ≤ the largest body `CAPABILITY` gave                          |
| longest `READ`     | int32 | ≥ the block size, and ≤ the largest body                               |
| `READ`s outstanding| int8  | ≥ 1, and ≤ what `CAPABILITY` gave                                      |

* The parent chooses the block size. It **SHOULD** take the preferred one, and **MAY** choose a
  smaller one to bound what it holds for a run.
* A parent that can't work within `CAPABILITY` **MUST** kill FFmpeg.

**FFmpeg's side**:

* **A malformed `REQUEST`** (the wrong type, number or length, or a negative field) is a protocol
  fault before the terms exist: the open **MUST** fail, and no `DONE` is sent.
* **A well-formed `REQUEST` whose values break the rules:** it **MUST** send `DONE` (`ERR`, `TERMS`)
  from the open, and fail the open. That `DONE` echoes the run when it is at least 1, and 0
  otherwise.
* **A good `REQUEST`:** its first `READ` accepts the terms.

**Example** (a 2,500,000-byte file, run 7):

    03 01 1b 00 00 00                                // REQUEST, number 1, 27 bytes
       01 00                                         //   version: 1
       07 00 00 00 00 00 00 00                       //   run: 7
       a0 25 26 00 00 00 00 00                       //   file size: 2,500,000
       00 00 10 00                                   //   block size: 1 MiB
       00 00 10 00                                   //   longest READ: 1 MiB
       02                                            //   READs outstanding: 2


### READ

A range of the file.

| Field  | Type  |
| ------ | ----- |
| offset | int64 |
| length | int32 |

A `READ` **MUST** hold to these, checked as in [Checks Without Overflow](#checks-without-overflow):

* `0 ≤ offset`;
* `0 < length ≤ min(longest READ, file size − offset)`;
* the outstanding `READ`s within the agreed count.

Alignment is FFmpeg's side's own business. The parent serves any range that holds to the rules.

**Counting what is outstanding**:

* The parent stops counting a `READ` as outstanding **when it takes it to answer**, before it writes
  any of the `DATA` frame.
* FFmpeg's side frees a slot **no earlier than reading the `DATA` header**.

The parent's count is then never above FFmpeg's, so a well-behaved FFmpeg is never judged over the
limit. A hostile one gains at most one queued request, which is still bounded.

**Example**:

    04 02 0c 00 00 00                                // READ, number 2, 12 bytes
       00 00 00 00 00 00 00 00                       //   offset: 0
       00 00 10 00                                   //   length: 1 MiB


### DATA

The parent answers each `READ` with `DATA`, echoing its number, in the order of the `READ`s. Its
body is exactly the bytes requested. FFmpeg's side **MUST** check the type, the number (the oldest
outstanding) and that the length equals its `READ`'s, before using a byte.

**When the parent can't read**, because the storage failed, the read passed its
[deadline](#read-deadline), or the file changed since `REQUEST`, it **MUST NOT** answer. It kills
FFmpeg. The parent checks the file's identity itself (device, inode, size, modification time); a
file that grows or changes during a run is a changed file.

**Example**:

    05 02 00 00 10 00                                // DATA, number 2, 1 MiB
       ...                                           //   the file's bytes 0 to 1,048,575


### DONE

FFmpeg's account of its channel.

| Field                    | Type             | Notes                                                                 |
| ------------------------ | ---------------- | --------------------------------------------------------------------- |
| run                      | int64            | echoes `REQUEST`, or 0 ([REQUEST](#request))                          |
| result                   | int8             | 0 `OK`, 1 `ERR`                                                       |
| *if `ERR`:* kind         | int8             | [Error Kinds](#error-kinds)                                           |
| *if `ERR`:* code         | int32            | FFmpeg's error code, negated. A code that isn't negative, or is `INT32_MIN`, is sent as 0 |
| *if `ERR`:* message length | int16          | 0 to 1,024                                                            |
| *if `ERR`:* message      | that many bytes  | for the logs ([Logging Text](#logging-text))                          |

`OK` means this channel saw no fault. Whether the run as a whole succeeded is FFmpeg's exit status.
fftools merges every thread's error into one code with no record of where it came from, so `DONE`
can't classify a failure of decoding or writing: its `OK` and `ERR` are about the channel alone.

**Examples**:

    06 05 09 00 00 00                                // DONE, number 5, 9 bytes
       07 00 00 00 00 00 00 00                       //   run: 7
       00                                            //   OK

    06 05 22 00 00 00                                // DONE, number 5, 34 bytes
       07 00 00 00 00 00 00 00                       //   run: 7
       01                                            //   ERR
       02                                            //   kind: CHANNEL
       05 00 00 00                                   //   code: 5 (EIO)
       12 00                                         //   message: 18 bytes
       49 6e 70 75 74 2f 6f 75 74 70 75 74 20 65 72  //   "Input/output error"
       72 6f 72


#### Error Kinds

| Kind          | Code | Means                                                                                        |
| ------------- | ---- | -------------------------------------------------------------------------------------------- |
| `TERMS`       | 1    | FFmpeg's side declined `REQUEST`'s terms, or failed to set up for them (no memory for its cache) |
| `CHANNEL`     | 2    | the channel to the parent ended (end-of-file, or a broken pipe): the parent died             |
| `PROTOCOL`    | 3    | the parent broke the protocol: a wrong type, number or length                                |
| `INTERRUPTED` | 4    | FFmpeg's interrupt callback fired while the channel waited: a signal from outside the parent |

The interrupt doesn't always reach a channel: fftools fires it only on a second signal once a run
has started, and FFmpeg's I/O layer may catch it before calling the protocol. An interrupted run may
therefore end with `DONE` `OK` and a non-zero exit. The exit status decides.


#### When DONE Is Sent

* **Exactly once for each channel that received a well-formed `REQUEST`:**
    * from the open, when the open fails after the `REQUEST` arrived, by a `TERMS` refusal or by a
      resource failure (also sent as `TERMS`, with its code). FFmpeg never calls the close of an open
      that failed, so the open's failure path also frees its own state and closes its descriptors;
    * from the close, otherwise.
* **Never before a well-formed `REQUEST`.** If the opening exchange fails, the open fails, FFmpeg
  exits non-zero, and the missing `DONE` already says so.
* **After a fault on the channel** (`CHANNEL`, `PROTOCOL`, `INTERRUPTED`), `DONE` is the only message
  sent, as `ERR` with the fault's kind.
* **A path that skips the close** (an `exit()` mid-run, a signal, a crash) sends no `DONE`. Each of
  those also exits non-zero or is killed.

Once `DONE` is sent, FFmpeg's side **MUST** refuse any further read locally.

**The parent, on `DONE`**:

* stops serving that channel, and drops any `READ` still waiting;
* **MUST NOT** treat a `DONE` arriving after `READ`s it hasn't answered as a fault;
* **SHOULD** log any frame after `DONE` as a violation, which doesn't change the verdict, since
  FFmpeg is exiting.



Versions
--------

* Versions are integers from 1. Version 0 is invalid.
* **Anything new is a new version.** There are no optional or ignorable fields and no extensions.
  Within a version, every message has exactly one form.
* **`HELLO` is frozen in every version, header included.** Every later message takes the form of the
  version both sides work out from it:

        version = min(the two highest)    valid when version >= max(the two lowest)

This document specifies version 1.



Checks Without Overflow
-----------------------

Signed fields stop forged negatives, not arithmetic overflow: added, `offset + length` can wrap, and
in C, signed overflow is undefined, so a compiler **CAN** delete a test like `offset + length < 0`.
Every bound **MUST** be checked by subtraction or division from a known non-negative value, never by
addition or multiplication:

    0 <= offset  &&  offset <= size  &&  0 < length  &&  length <= longest  &&  length <= size - offset
    over budget  <=>  served / k > allowed        (never served > k * allowed)

A side **MUST NOT** compute a size or allocate from a number the other side sent. It allocates only
from the terms it set or accepted.

**The file size is at most 2^62,** far past any file, so the file size plus a block, a `READ` or a
seek from the end stays below the largest int64 however a side adds them. A parent **MUST NOT** send
a larger one, and FFmpeg's side **MUST** answer one with `DONE` (`ERR`, `TERMS`). Every other field
that is ever added to is bounded far below that: a block and a `READ` at most the largest body,
4 MiB on FFmpeg's side. The bound is headroom; the checks above still never add.



The Parent
----------

### Launching

The parent **MUST** launch FFmpeg confined, in its own process group, with the job, the whitelist
and the descriptors on its command line, its descriptors as in [Descriptors](#descriptors).

**No process outlives or escapes FFmpeg.** Its seccomp filter, beside any other denials:

| System call                                     | Answer   | Why                                                                                     |
| ----------------------------------------------- | -------- | --------------------------------------------------------------------------------------- |
| `clone3`                                        | `ENOSYS` | seccomp can't read `clone3`'s flags. glibc 2.34+ calls it first, and falls back to `clone` only on `ENOSYS`; `EPERM` here breaks FFmpeg's threads |
| `clone` without `CLONE_THREAD` in its flags     | `EPERM`  | creating a process; threads pass                                                        |
| `fork`, `vfork` (amd64; arm64 has neither)      | `EPERM`  | creating a process                                                                      |
| `setsid`, `setpgid`                             | `EPERM`  | leaving its session or process group                                                    |

The filter differs by architecture and **MUST** refuse other architectures (x32 on amd64).
Hardware encoders **SHOULD** be tested under it on real devices: some drivers start helpers.


### Serving

* **Every frame from FFmpeg is hostile.** On any violation the parent **MUST** kill FFmpeg, and
  **SHOULD** log exactly what was wrong. It sends nothing more.
* **Per channel, one reader of requests and one writer of `DATA`,** each its own thread, with the
  channel's own buffer reserve of (outstanding + 1) × the longest `READ`, from the terms. A storage
  read waits for a free buffer in its own channel's reserve, never in another's.
    * **No `DATA` write ever waits on another channel or another run.** FFmpeg's scheduler stops
      reading every input but the one furthest behind, by design: a writer shared between channels
      would block on a paused channel's full pipe and starve the one FFmpeg waits for.
    * The reader sees every request, a violation included, while its writer is blocked.
    * Silence between frames is never a failure.
* **The parent reads the storage into its own memory, then writes.** It **MUST NOT** `splice` from
  the file into FFmpeg's pipe or `mmap` the file: a `splice` holds FFmpeg's pipe locked while the
  storage read waits, and FFmpeg's `read()` of that pipe then waits uninterruptibly, so a dead share
  leaves FFmpeg unkillable.
* It **SHOULD** release its place on the storage as soon as its read returns, before writing to the
  pipe, so a paused or slow FFmpeg never holds the disk.
* It counts outstanding `READ`s as [READ](#read) says.


### Budget

The parent **SHOULD** keep a budget of bytes served per run, by kind of run: what the run is
expected to need, times a margin, with a floor so a small file or block rounding can't trip it. A
run over its budget is killed. For example:

| Run                          | Budget                                                       |
| ---------------------------- | ------------------------------------------------------------ |
| a probe                      | a sixteenth of the file, over a floor of 64 MiB              |
| a read of the whole file     | the file and a quarter, over the floor                       |

A run that thrashes FFmpeg's cache ([Reading in Blocks](#reading-in-blocks)) passes its budget and
is killed rather than read the file many times over.


### Read Deadline

A storage read **SHOULD** have a deadline: how long the parent lets that storage go without
answering. A read past its deadline is a read that failed, and the parent kills the run.

* A storage read the parent has given up on keeps its buffer until it returns, so a dead share pins
  at most one thread per channel.
* Time the parent holds a `READ` back itself, for pacing or pausing, doesn't count as FFmpeg standing
  still.


### Pausing

The parent pauses a run by holding its `READ`s and stopping the process with SIGSTOP, and resumes it
with SIGCONT. Holding the reads stops its storage use; stopping the process stops it using the
processor on what it already holds. Holding a `READ` trips no timeout inside FFmpeg. A pause starts
no new storage read; reads already under way finish into the channel's reserve.


### Stopping

**The parent owns all time.** It ends a run by killing it:

* **MUST** signal the process itself, by pidfd where the system has one, never its process group,
  which a hostile FFmpeg could leave. SIGSTOP and SIGCONT go the same way.
* **SHOULD** use SIGKILL alone: nothing the run would write after a SIGINT or SIGTERM is kept.
* **MUST** signal only before it has reaped the process.

It kills when it can't read or the file changed, when the run passes its budget or is no longer
wanted, and when its own limits pass.


### End of a Run

The parent **MUST** take a run's end from the process's exit, never from end-of-file on a pipe. Then,
for each channel:

1. it closes its `DATA` write end;
2. its reader of requests goes on reading, without blocking, what is already in the pipe: everything
   FFmpeg wrote before it exited is there. It stops at end-of-file or "would block";
3. it closes the request pipe.

Then it judges. There is only ever one reader per pipe.


### Judging a Run

A run succeeded only if:

* FFmpeg exited 0;
* every channel's `DONE` said `OK` and echoed the run;
* the parent itself had no reason to fail it.

Exiting without `DONE` is a failure. When the parent kills a run, its own reason decides, whatever
the exit status:

* **a stop:** the run is no longer wanted, was paused past the parent's patience, or met the
  parent's time limits;
* **a failure:** a violation, a storage read that failed or passed its deadline, a changed file, or
  the budget passed.


### Logging Text

Before logging any text FFmpeg sent, the parent **MUST** decode it as UTF-8 with replacement, then
escape every character in the Unicode categories Cc and Cf, DEL included. That catches C0 and C1
controls, CSI in either form (`0x9B`, or `C2 9B`) and bidirectional overrides. Invalid UTF-8 is not a
violation.



FFmpeg's Side
-------------

### Opening

The protocol opens the channel ([Descriptors](#descriptors)) and makes the opening exchange:
`HELLO`, the parent's `HELLO`, `CAPABILITY`, `REQUEST`.


### Reading in Blocks

* It asks for whole blocks, aligned to the block size, the last one trimmed at the file's size.
* It keeps the last `cache_blocks` blocks, and answers a read of any size from them. FFmpeg's I/O
  layer usually asks 32 KiB, but reads a packet larger than its buffer at full size, and grows its
  buffer while probing. A read **MAY** come back short; FFmpeg loops.
* It **MUST NOT** set `max_packet_size`, which would change FFmpeg's own buffering.
* **FFmpeg MUST treat the input as local,** as it treats `file`: a seek costs nothing, since the
  parent serves any offset. FFmpeg assumes any protocol it doesn't know as local is a network one,
  and when it opens an indexed file it then enlarges its buffer up to 16 MiB and reads forward
  rather than seek up to 8 MiB, reading megabytes it never needs (`ff_configure_buffers_for_index`).

**Sizing the cache.** FFmpeg reads a few seconds of every track while it opens a file, whatever the
run maps, and a run mapping several tracks reads from each of them as it goes. In a file whose tracks
are stored one after another, each track is a place of its own, and the cache **SHOULD** hold about a
block for each place read at once, and a few more for prefetching: a cache smaller than that thrashes,
each block evicted just before it is needed again. An interleaved file needs a few blocks. The parent
**SHOULD** set `cache_blocks` for the files it serves; see [DESIGN.md](DESIGN.md#the-block-cache) for
what has been measured.


### Seeking and End of File

* It handles `SEEK_SET`, `SEEK_CUR`, `SEEK_END` and `AVSEEK_SIZE`, the last answered with the file's
  size. A seek whose sum would pass the largest int64 **MUST** fail with `EINVAL`.
* A read at or past the end **MUST** return `AVERROR_EOF`, never 0: a 0 makes FFmpeg's retry loop
  spin.
* It **MUST NOT** return `AVERROR(EAGAIN)`, which makes FFmpeg busy-wait.


### Prefetching

When the block just needed follows the one needed before it, it **SHOULD** also ask for the next,
within the outstanding limit.


### Waiting and Interrupts

It waits without a timeout, but **MUST** stay interruptible: while waiting it polls in slices of
100 ms and checks FFmpeg's interrupt callback (`ff_check_interrupt`). When that fires, the read
returns `AVERROR_EXIT`. FFmpeg's signal handlers make `poll` return `EINTR`, which the loop takes as
a reason to look again, not a fault. Without the check, a blocked read ignores the callback, and
ffmpeg's own stop would hang on the blocked thread. This is protection, not a stop the parent uses:
the parent kills.


### Sticky Faults

Once a read has failed, by the channel ending (`CHANNEL`), a mismatched answer (`PROTOCOL`) or the
interrupt, every later read on the channel **MUST** return the same error, so a demuxer that ignores
one error can't read on.


### Writing a Frame

One `write()`, guarded against SIGPIPE whatever the program:

1. note whether SIGPIPE is already pending;
2. block it with `pthread_sigmask`;
3. write;
4. on `EPIPE`, if it wasn't already pending, consume it with a zero-timeout `sigtimedwait`;
5. restore the mask.


### Closing

The close sends `DONE` ([When DONE Is Sent](#when-done-is-sent)), unless it has been sent already,
then closes its descriptors. It does not drain `DATA` still owed: the parent's writer for that
channel blocks until FFmpeg exits, which harms nothing, since writers are per channel.


### Exit Status

An input's read error **MUST** be the run's exit status. Unchanged, ffmpeg and ffprobe exit 0 after
an input failed mid-read: FFmpeg records the error on the input's I/O context (`pb->error`) and goes
on as if the input had ended. fftools **MUST** promote `pb->error`, when negative and not `EAGAIN`:

* after the demux thread's loop (`ffmpeg_demux.c`);
* after `avformat_find_stream_info` in `ifile_open`;
* in ffprobe's `probe_file`, before its end.

An `AVERROR_EXIT`, which is an interrupt, is promoted as `AVERROR(EINTR)`. And since ffmpeg's `main`
turns `AVERROR_EXIT` into exit 0, a signal during an input's open **MUST** still exit non-zero:
`if (ret == AVERROR_EXIT) ret = received_nb_signals ? 255 : 0;`.



No Deadlock
-----------

Five premises, each a rule both sides **MUST** keep:

1. **The parent reads each channel's requests on their own path,** independent of every writer
   ([Serving](#serving)).
2. **FFmpeg's requests are bounded:** at most 16 outstanding, each 18 bytes, plus one `DONE` of at
   most 1,046 bytes. That fits even an 8 KiB pipe.
3. **Whenever FFmpeg's side waits, it reads that channel's `DATA` pipe in order.**
4. **FFmpeg may stop reading any channel indefinitely while it waits on another.** fftools' scheduler
   does exactly that, so the parent's writers are per channel, with their own buffers.
5. **Nothing the parent holds waits forever:** a storage read has its [deadline](#read-deadline), a
   pause the parent's patience, and a run its [budget](#budget).

Under these, the request direction can't block; a `DATA` write blocks only on its own channel, while
FFmpeg is busy elsewhere, paused or exiting, none of which the parent mistakes for a failure; and
the parent's held reads are bounded.



Security Rules
--------------

FFmpeg reads files anyone could have crafted, so the parent **MUST** assume any FFmpeg it runs has
been taken over. ffstreamer gives such an FFmpeg nothing but the bytes of its own input:

* it holds no path to the file, and opens nothing to read it;
* it can ask only for ranges inside the file, within its terms, and only as many at once as agreed;
* every violation ends its run, and its `DONE` is never believed over its exit status or the
  parent's own account.

**Note**: what a hostile FFmpeg can still do is spend what its terms allow: its budget of bytes, its
cache, its time. The parent sets all three.


### Resource Limits

| Resource                             | Limit                         | Notes                                                    |
| ------------------------------------ | ----------------------------- | -------------------------------------------------------- |
| File size                            | 2^62 bytes                    | [Checks Without Overflow](#checks-without-overflow)      |
| Block size, longest `READ`           | the largest body `CAPABILITY` gave | 4 MiB on FFmpeg's side; a parent **MAY** cap it lower |
| `READ`s outstanding                  | 16                            | and no more than both sides agreed                       |
| `DONE` body                          | 1,040 bytes                   | bounded before it is buffered                            |
| `DONE` message                       | 1,024 bytes                   |                                                          |
| Frames FFmpeg's side sends           | 4,096 bytes                   | one `write()` each                                       |
| `cache_blocks`                       | 2 to 1,024                    | the cache's memory is this × the block size              |
| Bytes served per run                 | the parent's [budget](#budget) |                                                         |
| Each storage read                    | the parent's [deadline](#read-deadline) |                                                |



Full Example
------------

FFmpeg reads a 2,500,000-byte file straight through, as run 7, in blocks of 1 MiB with two `READ`s
outstanding.

**FFmpeg's side → parent**, then **parent → FFmpeg's side**, in the order the frames are sent:

```text
→  01 00 04 00 00 00  01 00 01 00                   // HELLO 0: versions 1 to 1
←  01 00 04 00 00 00  01 00 01 00                   // HELLO 0: versions 1 to 1
→  02 01 09 00 00 00                                // CAPABILITY 1
      00 00 10 00  00 00 40 00  02                  //   block 1 MiB, largest 4 MiB, 2 outstanding
←  03 01 1b 00 00 00                                // REQUEST 1
      01 00                                         //   version 1
      07 00 00 00 00 00 00 00                       //   run 7
      a0 25 26 00 00 00 00 00                       //   file size 2,500,000
      00 00 10 00  00 00 10 00  02                  //   block 1 MiB, longest READ 1 MiB, 2 outstanding
→  04 02 0c 00 00 00                                // READ 2
      00 00 00 00 00 00 00 00  00 00 10 00          //   bytes 0 to 1,048,575
→  04 03 0c 00 00 00                                // READ 3 (prefetch)
      00 00 10 00 00 00 00 00  00 00 10 00          //   bytes 1,048,576 to 2,097,151
←  05 02 00 00 10 00  ...                           // DATA 2: 1,048,576 bytes
←  05 03 00 00 10 00  ...                           // DATA 3: 1,048,576 bytes
→  04 04 0c 00 00 00                                // READ 4
      00 00 20 00 00 00 00 00  a0 25 06 00          //   bytes 2,097,152 to the end: 402,848
←  05 04 a0 25 06 00  ...                           // DATA 4: 402,848 bytes
→  06 05 09 00 00 00                                // DONE 5
      07 00 00 00 00 00 00 00  00                   //   run 7, OK
```

FFmpeg then exits 0, and the parent, having had no reason to fail it, judges the run a success.



Formal Grammar
--------------

The following [Dogma](https://github.com/kstenerud/dogma/blob/master/v1/dogma_v1.0.md) grammar is
**normative**. In case of any discrepancy between the prose descriptions in this document and the
formal grammar, the grammar takes precedence. It is also kept on its own in
[ffstreamer.dogma](ffstreamer.dogma), which **MUST** stay identical to this copy.

**Note**: the grammar defines the bytes on each pipe. The rules that pair the two pipes in time (the
`READ`s outstanding), what each side does on a violation, and the checks of
[Checks Without Overflow](#checks-without-overflow) are stated in the prose.

```dogma
dogma_v1 utf-8
- identifier  = ffstreamer_v1
- description = ffstreamer: the read protocol between FFmpeg and the parent that serves its input, version 1
- reference   = ffstreamer-specification.md
- dogma       = https://github.com/kstenerud/dogma/blob/master/v1/dogma_v1.0.md

# A channel is two pipes, one each way (Channels). This grammar describes the bytes on either
# one, from the first byte to the writer's end. Which of the two a pipe is comes from the descriptor
# it was handed, never from what it carries.
#
# Not expressible here, and stated in the specification's prose:
# - the number of READs outstanding at once (READ), which pairs the two pipes in time;
# - that the parent sends nothing more after a violation, and kills FFmpeg instead (Serving);
# - that FFmpeg sends no DONE before a well-formed REQUEST has arrived (When DONE Is Sent);
# - the checks a reader must make without overflowing (Checks Without Overflow).

ffstreamer_pipe = byte_order(lsb, [
                    pipe = written_by_ffmpeg : ffmpeg_to_parent;
                    pipe = written_by_parent : parent_to_ffmpeg;
                  ]);

written_by_ffmpeg = 1;
written_by_parent = 2;
pipe: uinteger    = """
Which pipe of the channel this is: `written_by_ffmpeg` for the one FFmpeg writes and the parent
reads (FFmpeg's -ffstreamer_write_fd), `written_by_parent` for the one the parent writes and FFmpeg
reads (FFmpeg's -ffstreamer_read_fd).
""";


###############################################################################
# FFmpeg to the parent
###############################################################################

# FFmpeg numbers every frame it sends: HELLO is 0, and each later frame takes the next number,
# wrapping from 127 to 0. A pipe may end after any whole frame: FFmpeg writes each frame with one
# write() of at most 4,096 bytes, so a kill never leaves half of one (Frame Size).
ffmpeg_to_parent          = hello
                          & (capability(1) & reads_then_done(2)?)?
                          & eod
                          ;

# READs until DONE, or until FFmpeg exits without one (When DONE Is Sent: a kill, a crash, an exit() mid-run).
reads_then_done(sequence) = read(sequence) & reads_then_done(next(sequence))?
                          | done(sequence)
                          ;
next(sequence)            = (sequence + 1) % 128;


###############################################################################
# The parent to FFmpeg
###############################################################################

# Every parent frame is an answer, and echoes the number of the frame it answers, oldest first (Sequence Numbers).
# REQUEST answers CAPABILITY (1). The DATA frames answer FFmpeg's READs in their order, so they
# carry the numbers from 2 on. The parent answers no READ after DONE, and none once it has decided
# to kill FFmpeg, so the DATA frames may stop short of the READs.
parent_to_ffmpeg          = hello
                          & (request(1) & answers(2)?)?
                          & eod
                          ;
answers(sequence)         = data(sequence) & answers(next(sequence))?;


###############################################################################
# The frame (Frame Header): a 6-byte header, then the body
###############################################################################

frame(type, sequence, body_lengths, body) = frame_type(type)
                                          & frame_sequence(sequence)
                                          & frame_body_length(var(body_length, body_lengths))
                                          & sized(body_length * 8, body)
                                          ;

frame_type(type)              = i8(type);         # which message: never 0
frame_sequence(sequence)      = i8(sequence);     # 0 to 127 (Sequence Numbers)
frame_body_length(length)     = i32(length);      # the body's length in bytes

type_hello      = 1;
type_capability = 2;
type_request    = 3;
type_read       = 4;
type_data       = 5;
type_done       = 6;


###############################################################################
# HELLO, from both sides
###############################################################################

# Frozen in every version, its header included. The parent's echoes FFmpeg's sequence, 0.
hello                        = frame(type_hello, 0, hello_body_length, hello_body);
hello_body_length            = 4;
hello_body                   = lowest_version_spoken(var(lowest, 1~))
                             & highest_version_spoken(lowest~)
                             ;
lowest_version_spoken(v)     = i16(v);
highest_version_spoken(v)    = i16(v);


###############################################################################
# CAPABILITY, from FFmpeg
###############################################################################

capability(sequence)         = frame(type_capability, sequence, capability_body_length, capability_body);
capability_body_length       = 9;
capability_body              = preferred_block_size(var(preferred, 1~))
                             & largest_data_body_accepted(preferred~)
                             & most_reads_outstanding(1~16)
                             ;
preferred_block_size(v)       = i32(v);
largest_data_body_accepted(v) = i32(v);
most_reads_outstanding(v)     = i8(v);


###############################################################################
# REQUEST, from the parent: the terms
###############################################################################

request(sequence)            = frame(type_request, sequence, request_body_length, request_body);
request_body_length          = 27;
request_body                 = version(negotiated_version)
                             & run(1~)
                             & file_size(0~4611686018427387904)
                             & block_size(var(block, 1~capability_largest_body))
                             & longest_read(block~capability_largest_body)
                             & reads_outstanding(1~capability_outstanding)
                             ;
version(v)                   = i16(v);   # the version both sides worked out; a cross-check
run(v)                       = i64(v);   # the parent's identifier for the run, echoed in DONE
file_size(v)                 = i64(v);   # the input's size in bytes, at most 2^62 (Checks Without Overflow)
block_size(v)                = i32(v);   # the size of the blocks FFmpeg's side asks for
longest_read(v)              = i32(v);   # the most bytes one READ may ask for
reads_outstanding(v)         = i8(v);    # the most READs FFmpeg's side may have unanswered


###############################################################################
# READ, from FFmpeg
###############################################################################

read(sequence)               = frame(type_read, sequence, read_body_length, read_body);
read_body_length             = 12;
read_body                    = read_offset(var(offset, 0~terms_file_size))
                             & read_length((1~terms_longest_read) ! ((terms_file_size - offset + 1)~))
                             ;
read_offset(v)               = i64(v);   # where in the input to read from
read_length(v)               = i32(v);   # how many bytes; never past the end of the input


###############################################################################
# DATA, from the parent
###############################################################################

data(sequence)               = frame(type_data, sequence, length_read(sequence), data_body);
data_body                    = input_byte*;   # exactly the bytes the READ asked for
input_byte                   = uint(8, ~);


###############################################################################
# DONE, from FFmpeg: its account of the channel
###############################################################################

done(sequence)               = frame(type_done, sequence, done_body_lengths, done_body);
done_body_lengths            = 9~1040;
done_body                    = done_run(terms_run | 0)   # 0 when REQUEST's run broke its rule
                             & (done_ok | done_err)
                             ;
done_run(v)                  = i64(v);

done_ok                      = result(result_ok);
done_err                     = result(result_err)
                             & error_kind(kind_terms | kind_channel | kind_protocol | kind_interrupted)
                             & error_code(0~)
                             & message_length(var(length, 0~1024))
                             & message(length)
                             ;
result(v)                    = i8(v);
error_kind(v)                = i8(v);
error_code(v)                = i32(v);   # FFmpeg's error code, negated; 0 when it had none to give
message_length(v)            = i16(v);
message(length)              = message_byte{length};   # for the logs; any bytes, sanitised (Logging Text)
message_byte                 = uint(8, ~);

result_ok        = 0;
result_err       = 1;
kind_terms       = 1; # FFmpeg's side declined REQUEST's terms, or could not take them up
kind_channel     = 2; # the channel to the parent ended: the parent died
kind_protocol    = 3; # the parent broke the protocol: a wrong type, number or length
kind_interrupted = 4; # FFmpeg's interrupt callback fired while the channel waited


###############################################################################
# Values one pipe takes from the other
###############################################################################

negotiated_version: uinteger = """
The version both sides worked out from the two HELLOs (Versions): the lower of the two highest, valid
when it is at least the higher of the two lowest. Version 1 is the one this grammar describes.
""";

capability_largest_body: uinteger = """
The largest DATA body FFmpeg's CAPABILITY said it accepts.
""";

capability_outstanding: uinteger = """
The most READs outstanding FFmpeg's CAPABILITY said it can have.
""";

terms_run: uinteger = """
The run REQUEST gave.
""";

terms_file_size: uinteger = """
The file size REQUEST gave.
""";

terms_longest_read: uinteger = """
The longest READ REQUEST allowed.
""";

length_read(sequence: uinteger): uinteger = """
The length of the READ numbered `sequence` that this DATA answers: the oldest READ outstanding.
""";


###############################################################################
# Integers: signed, two's complement, little-endian (Integers)
###############################################################################

i8(v)   = sint(8, v);
i16(v)  = ordered(sint(16, v));
i32(v)  = ordered(sint(32, v));
i64(v)  = ordered(sint(64, v));
```



License
-------

Copyright (c) 2026 Karl Stenerud. All rights reserved.

Distributed under the [Creative Commons Attribution License](https://creativecommons.org/licenses/by/4.0/legalcode) ([license deed](https://creativecommons.org/licenses/by/4.0)).
