# Review of the change from draft 4 to draft 5 (2026-10-22)

The third audit round judged that a review of its fixes would be enough. This is that review, by an
independent reviewer of wire protocols, IPC, seccomp and FFmpeg's internals. Its scratch work, in the
auditor's scratch directory `review5/`, is `sc5.c` (the C filter) and `gshim/main.go` (a copy of the
Go shim with the full filter).

**Verdict:** ready to implement once its findings below are in, with no further review. They are in
draft 6.

## The third round's items

- **Addressed:** M1, M3, M4 and M5; the should-fixes S1, S3, S4, S5, S7 and S9.
- **Partly addressed:** M2 (N1 below), S6 (stall stops unclassified), and S8 (its wording clashed with
  §2's SIGSTOP).

## New findings

| # | Finding | Fix (in draft 6) |
| --- | --- | --- |
| N1 | **Must fix.** An interrupt during the open still exits 0. It never reaches `pb->error`. `main` turns `AVERROR_EXIT` into 0 at `ffmpeg.c:1047`, and the `received_nb_signals ? 255` line runs only after `transcode()` | A fourth place in the patch: `if (ret == AVERROR_EXIT) ret = received_nb_signals ? 255 : 0;`. ffprobe needs nothing: it has no interrupt callback or handlers |
| N2 | When the interrupt fires was stated wrongly. Mid-run, a first signal doesn't fire it. `avio.c:515` can catch it before the protocol is called. `q` never fires it | `DONE` may be `OK` on an interrupted run, and the exit status decides. "Or `q`" dropped. The test expects `INTERRUPTED` or `OK` |
| N3 | No-deadlock premise 5 didn't cover pausing | A pause is bounded by the parent's patience, and starts no new storage reads |
| N4 | Pausing still signalled the process group | SIGSTOP, SIGCONT and SIGKILL all go to the process, by pidfd |
| N5 | The 64-block cap is reached at 42 streams | Cap 1,024. Memory is counted by the parent, and a clamped run is caught by its budget |
| N6 | A failure inside the open after a well-formed `REQUEST` (no memory for the cache) sent no `DONE` | Any open failure after `REQUEST` sends `DONE` from the open, and frees its own state |
| N7 | Stale lines: §10.1's "every path runs the close"; the tests' grandchild and old seccomp list; the change list's "a stop whatever the exit code" | Corrected |

## Checked

- **The seccomp rule is expressible in classic BPF on amd64 and arm64.** `clone`'s flags are `args[0]`,
  whose low word sits at offset 16, tested with `JSET CLONE_THREAD`. The kernel uses only the low 32
  bits.
- **Denying `setpgid` breaks nothing.** Neither binary imports it, and the parent's `Setpgid` runs before
  the shim's exec.
- **The Go shim works under the full filter.** The runtime makes threads with `clone(CLONE_THREAD)`,
  `syscall.Exec` is a bare `execve`, and `clone3` appears only in `forkExec`, which the shim never
  calls after installing the filter. Tested: 64 goroutines, a GC, then exec of an 8-thread ffmpeg:
  exit 0.
- **SIGSTOP with held `READ`s is safe.** The pipe's mutex is released before sleeping, frames are
  atomic, writers block only on their own channel, and SIGKILL works on a stopped process.
- **`(outstanding + 1) × longest READ` is the exact reserve.**

## To confirm by test when implementing

- arm64 on real hardware.
- VAAPI, QSV and NVENC under the filter.
- After N1's fix, a SIGTERM during the protocol's open and during `read_header`.
- `poll` and `EINTR`.
- Exactly one `DONE` for `run` = 0 and for no memory during the open.
- The cache past 41 streams.
- A gap in `ExtraFiles`.
