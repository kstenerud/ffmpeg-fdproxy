# Audit of PROTOCOL.md draft 2, from the wire-protocol and Unix IPC side (2026-10-22)

An independent audit by a protocol and IPC expert. Its experiments are in the auditor's scratch
directory: `hangfs.py` and `splicetest.py`, a FUSE share whose reads hang; `ovf.c`, a
signed-overflow check. Kernel 6.12, gcc at -O2, and jellyfin-ffmpeg's source.

**Verdict:** the core is sound: strict negotiation, kill instead of abort, the parent owning time,
hostility explicit. But it must not ship as written.

## Must fix

| # | Finding | Fix |
| --- | --- | --- |
| M1 | **`splice` from the storage makes FFmpeg unkillable.** `splice` holds FFmpeg's pipe's mutex through the storage read. FFmpeg's `read()` takes that mutex uninterruptibly. Reproduced with a FUSE file whose reads hang: FFmpeg sat in `D` on `pipe_read`, survived SIGKILL, and died only when the share's server was killed. The splice also makes "release the disk place before writing" impossible | The parent never blocks on the storage while holding FFmpeg's pipe: `pread` into its own memory (never an `mmap` of the film), then `write`. If the saving is wanted, stage it: file into a private pipe, release the place, then pipe-to-pipe `splice` |
| M2 | The graceful stop doesn't work as §7.5 assumes. fftools trips its interrupt callback only on a second signal once transcoding has begun, and exits 255 after any signal | Define the stop, or (given the author's "the parent kills") drop the interrupt path and `INTERRUPTED` |
| M3 | `DONE` is contradictory and under-specified | Legal in any state after acceptance, with READs outstanding. Nothing before acceptance. A `PROTOCOL` kind for a parent's fault. Fold DECLINE into `DONE(ERR)`. One mutex per channel around numbering and writing, since `DONE` comes from fftools' main thread and READs from a demuxer thread. Channel state in a registry that outlives the `URLContext` |
| M4 | The parent's in-flight count races its own writer, and could kill a well-behaved FFmpeg. The spec never requires the parent to keep reading requests while a `DATA` write blocks | Read requests independently of writing `DATA`. A `READ` stops counting when its `DATA` header has been written. Put the no-deadlock argument in the spec |
| M5 | §5 checks "an outstanding request's number", which lets swapped answers pass | Check the oldest outstanding request: FIFO |
| M6 | "Negatives catch overflow" is false for computed values. `offset + length` wraps. gcc -O2 deletes an `off + len < 0` test (reproduced). Negating `INT_MIN` is undefined | Checks in non-overflowing form: `0 ≤ offset ≤ size`, `0 < length ≤ min(longest, size − offset)`. Guard the `AVERROR` negation |
| M7 | Frame lengths aren't exact, so trailing bytes are allowed, and two lengths can disagree | Exact lengths for fixed bodies; a variable body's length must equal the sum of its fields; unknown enum values are violations |

## Should fix

1. Drop the 1-second partial-frame deadline. It defends nothing, since silence between frames is allowed anyway.
2. Only the data pipe needs 1 MiB. `F_SETPIPE_SZ` failed with EPERM at the 62nd 1 MiB pipe (the per-user soft limit is 64 MiB). Say what happens then.
3. Descriptor hygiene:
   - CLOEXEC on the channel;
   - `fstat` each descriptor: a FIFO, with the right access mode;
   - refuse a write-mode open;
   - the parent closes its copies of the child's ends;
   - FFmpeg in its own process group;
   - drain `DONE` before judging.
4. ffprobe doesn't ignore SIGPIPE.
5. A crafted manifest naming `pipe:7` or `fd:` could read the channel's descriptor. Launch with `-protocol_whitelist NAME`.
6. Fold ACCEPT and DECLINE into the first READ and `DONE(ERR)`.
7. Freeze the whole HELLO frame, header included:
   - version 0 is invalid;
   - no CAPABILITY before the parent's HELLO;
   - both sides check the other's range.
8. Complete the REQUEST checks: block size, longest READ, in-flight, run, exact length.
9. Sanitise text before logging it.
10. Optional: echo the offset in `DATA`.

## Open questions it raised

- A budget of bytes served per run against crafted re-reading.
- How a deliberate stop is judged.
- Where channel state lives, and the libavformat API fftools uses to reach it.
- Files that grow.
- The wargame rules draft 2 dropped (W4's stall clock, W6).
- **§12.3:** don't make alignment a rule.
- **§12.2:** extra counts in `OK` aren't worth it. `DONE` is advisory, not a trust boundary.

## What it judged good

- Kill rather than abort or error messages.
- One channel per input at named descriptors.
- Strict versioning with a frozen HELLO, and no optional fields.
- Signed little-endian integers.
- Allocation only from the parent's terms.
- Aligned blocks with sequential prefetch.
- "No `DONE` is a failure".
