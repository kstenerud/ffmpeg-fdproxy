# Audit of PROTOCOL.md draft 3, from the wire-protocol and Unix IPC side (2026-10-22)

A second, independent protocol and IPC audit. Its experiments are in the auditor's scratch directory
`audit3/`:
- `merge.c`;
- `soft.c` and `soft2.c`;
- `stop.c`;
- `race.c` and `race2.c`;
- `goclose/`.

**Verdict:**
- **The core is sound:** a frozen HELLO, answers checked first-in-first-out, subtraction-only bounds,
  kill and not abort, `pread` and not `splice`.
- **Six new must-fixes remain.** Two can hang a run with no kill coming, and two of the first audit's
  fixes are wrong in a way an experiment shows. None needs a redesign.

## The first audit's items

Fixed: M1, M2, M5, and the should-fixes S1, S4, S6, S7 and S8 (S8's REQUEST checks). Partly fixed:

| Item | What remains |
| --- | --- |
| M3 | `DONE` can be sent twice, and a malformed `REQUEST` is unclassified (N4) |
| M4 | The in-flight rule races (N1), and the cross-channel case is missing from the no-deadlock argument (N2) |
| M6 | k × size multiplies (S6 below) |
| M7 | `DONE`'s variable length needs a maximum by type (S5 below) |
| S2 | The fallback is 8 KiB, not 64 KiB (N6) |
| S3 | §9.8's two steps contradict each other (N3); creation isn't close-on-exec (N5) |
| S9 | Sanitising is too loose (S8 below) |

## New must-fixes

| # | Finding | Fix |
| --- | --- | --- |
| N1 | **The in-flight rule races.** `race2.c`, N = 1, 20,000 READs: counting down after the header write wrongly judged 8 READs over the limit when FFmpeg frees its slot at the header; counting down after the whole frame, 12,124. Each is a healthy run killed. The spec never says when FFmpeg frees a slot | The parent stops counting a `READ` when it takes it to write, before writing any of the frame (0 in every case). FFmpeg's side frees a slot no earlier than reading the `DATA` header |
| N2 | **A deadlock across channels, made permanent by the watchdog.** ffmpeg 7's scheduler deliberately chokes every input but the one furthest behind (`waiter_wait`). If one parent writer, or a shared buffer pool, blocks on a choked channel's full pipe, the other channel's `READ` is never served. And §9.7 exempts held time from the watchdog, so nothing kills it | One writer per channel, with buffers reserved per channel from the terms. Add the premise: FFmpeg may stop reading any channel indefinitely while it waits on another |
| N3 | **The exit sequence contradicts itself.** Closing "its ends" then draining loses `DONE`. Two readers on one pipe can split a frame. Closing does not free a writer blocked in `write()` (C: E4; Go: once `Fd()` was called, `goclose/`) | After exit, close only the `DATA` write ends. The single reader reads, without blocking, what is already there (it is, at the exit: E6), then stops at end-of-file or "would block". Then close and judge. Set pipe sizes through `SyscallConn().Control`, never `Fd()` |
| N4 | **`DONE` can be sent twice** (a `TERMS` refusal, then at exit). A malformed `REQUEST` is unclassified | `DONE` exactly once per channel. A malformed `REQUEST` is a `PROTOCOL` fault before `REQUEST`, with no `DONE`. `TERMS` is only for a well-formed `REQUEST` whose values break the rules; it echoes the run when valid, else 0. Any frame after `DONE` is logged, and doesn't change the verdict |
| N5 | **The parent's descriptors can leak to another run** launched before it closes its copies. A hostile FFmpeg could read another run's `DATA` or inject `READ`s | Every pipe end is created close-on-exec atomically (`pipe2(O_CLOEXEC)`), and handed to its own child only, by `dup2` at exec |
| N6 | **Past the per-user soft limit, a new pipe holds 8 KiB, not 64 KiB, and can't be grown** (`soft2.c`). The limit is shared with every pipe the node makes | State the real fallback. Size the data pipe from a page budget divided by the channels. Raise `fs.pipe-user-pages-soft` in deployment, and log once on fallback. The request direction still fits: 127 READs and a 1,046-byte DONE fit in 8 KiB |

## Should fix

1. **A storage-read deadline.** On a dead share, a held `READ` is exempt from the watchdog forever.
   Name the deadline that turns "can't read" into a kill.
2. **`DATA` left unread** after a chunk run closes its input with a prefetch outstanding. The writer
   blocks until exit. That is harmless once N2 makes writers per channel, but say it is expected, or
   drain on close.
3. **"`DONE` in any state" is unsafe** if a demuxer thread still waits on a `READ` the parent then
   drops. ffmpeg 7 joins the threads first, so it is fine. State the requirement, and refuse later
   `READ`s locally.
4. **Which kind each channel's `DONE` reports** on a two-input run. Rule: the channel's own fault
   first, then the run's.
5. **`DONE`'s length:** bound it by type before buffering (9 ≤ L ≤ 1,040), then check it exactly.
6. **The budget:**
   - `served / k > size`, so it can't overflow;
   - base it on the bytes the run is expected to need, since 4 × a 50 GB file is a lot for a probe;
   - give it a floor of a few blocks.
7. **Opening:** the descriptor options have no default; the URL must be exactly `ffstreamer:`; and
   the channel opens once per process, refused even after the first session closes.
8. **Sanitising:** decode with replacement, then escape every Cc and Cf character, DEL included.
   That catches C1 controls like `C2 9B` (CSI) and bidi overrides.
9. **Processes that outlive FFmpeg.** seccomp allows `fork` and `setsid`. A grandchild that calls
   `setsid` escapes the group kill and holds descriptors (E6b). Deny `fork`, `vfork`, `clone` without
   `CLONE_THREAD`, and `setsid`, or kill through a cgroup (`cgroup.kill`).
10. **Pausing with SIGSTOP** still reads up to N blocks for the stopped run. Say
    whether that's acceptable.
11. **"A handful outstanding":** int8 allows 127. FIFO makes the wrap harmless; say so, or cap it.

## What is solid, measured

- The request direction cannot block: 3,632 frames of 18 bytes fit in 64 KiB, and 227 in one page.
- A writer against a SIGSTOPped reader sleeps interruptibly, and a SIGKILL of that reader gives
  EPIPE in 2 ms.
- A closed write end delivers its unread data, then end-of-file.
- `DONE` drains after exit without blocking.
- FFmpeg's frames are under `PIPE_BUF`, so a kill can't leave half a frame.
- The subtraction-only bounds, allocating only from the terms, exact lengths, FIFO with one mutex,
  the frozen HELLO, "no `DONE` is a failure", and `pread` with the disk place released before
  writing.
