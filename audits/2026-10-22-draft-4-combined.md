# Audit of PROTOCOL.md draft 4, the third round: protocol, IPC and FFmpeg internals (2026-10-22)

An independent auditor covering both the protocol and FFmpeg's internals. Its experiments are in the
auditor's scratch directory `audit4/`:
- `sc.c`, seccomp filters;
- `pg.c`, leaving the process group;
- `sim.py` with the `tr/` traces, cache replay;
- `bad1.mp4` and `bad2.mp4`, non-interleaved MP4s.

**Verdict:** close, but not yet ready. Five must-fixes, each a change of text or one parameter. Three
come from draft 4's own changes. After they are folded in, a review of the diff is enough.

## Draft 3's findings, against draft 4

**Fixed:**
- N1, with its proof checked;
- N2, N3, N5 and N6;
- the internals audit's M1, M3, S1–S4, S7, S8 and S10;
- the protocol audit's S1–S5, S7, S8 and S11.

**Partly fixed:**
- N4 and the internals audit's M2: see M1 below;
- the budget: see S2 below;
- escaping processes: see M3 and M4 below.

**Wrong:**
- the internals audit's S6 (numbering): see S1 below;
- S9 (8 blocks): see M5 below.

**Checked, and fine:**
- `url_close` runs on the main thread after the joins: on a normal end, an error, `-t`/`-to`, an open
  failing after connect, and ffprobe;
- a user's `q` doesn't exit non-zero;
- holding a `READ` trips no FFmpeg timeout (a 45 s hold mid-run finished with exit 0).

## Must fix

| # | Finding | Fix |
| --- | --- | --- |
| M1 | `url_close` is never called when the open fails (`avio.c:256`, `ffurl_closep:596`), and a `TERMS` refusal happens inside the open | `DONE` is sent once: from the open for `TERMS`, from the close otherwise |
| M2 | The interrupt has no `DONE` kind. An interrupt during the open phase exits 0: `main` turns `AVERROR_EXIT` into 0 (reproduced: SIGTERM while opening a silent input, exit 0). Combined with a `DONE` of `OK`, the parent would judge success with no output. Reachable today, since a parent's stall stop may send SIGINT first | A kind `INTERRUPTED` (4), the sticky fault's kind sent as `ERR`. The fftools patch promotes `AVERROR_EXIT` as `AVERROR(EINTR)`. A stall stop is judged a stop whatever the exit code |
| M3 | seccomp can't read `clone3`'s flags, and glibc ≥ 2.34 falls back to `clone` only on `ENOSYS`. Measured: `clone3`→`EPERM` breaks threads (ffmpeg exits 255). `clone3`→`ENOSYS`, plus `clone` refused without `CLONE_THREAD`, plus `fork`/`vfork`/`setsid` refused, works, and blocks `posix_spawn`, `fork`, `system`, `popen` and `setsid`. arm64 has no `fork` or `vfork` system calls | Write out the filter, `clone3`→`ENOSYS`, per architecture, tested on both |
| M4 | A hostile FFmpeg can leave its process group (`setpgid` into the node's group), and then a group kill returns `ESRCH` (reproduced) | Kill the process itself through a pidfd. Also deny `setpgid` |
| M5 | 8 blocks thrash on a non-interleaved MP4 with many tracks: `bad2.mp4`, video and 9 audio tracks, prepare's mapping, fetched **402.8×** its size at 8 blocks, 4.3× at 12, 1.2× at 16. Prepare maps every audio track, and read-through measures every track | A decoding option, `-ffstreamer_cache_blocks`, set by the parent to at least 1.5 × the streams read + 2, with a default of 8 and a bound. Test at 10+ streams |

## Should fix

- **S1.** Numbering the channel "below" the progress pipe doesn't help. A missing descriptor is
  filled by the first dup or open at the lowest free number (measured: `dup(6)=4`). Require
  contiguous inherited descriptors with no gaps, and rely on the `HELLO` round trip.
- **S2.** The budget's examples are too low. A probe reads the whole `moov` (about 17 MB
  extrapolated for 2.5 h of 10 tracks), and every chunk run reads the index. A probe gets a floor of
  ~64 MiB plus a fraction of the size; a chunk run gets its span + the index size + a floor.
- **S3.** Pausing only by not answering drops the rule "using neither disk nor processor": FFmpeg
  goes on encoding from what it holds. Hold `READ`s and SIGSTOP too.
- **S4.** Restate that the parent closes its copies of the child's ends right after start, so its
  reader simply reaches end-of-file at exit.
- **S5.** Reserve (outstanding + 1) × longest `READ`, or say the storage read waits for a free buffer.
- **S6.** Separate stops from failures: kills for violations, storage failures and the budget are
  failures.
- **S7.** `run` ≥ 1, so 0 means none.
- **S8.** State the signals a ffstreamer run gets (a parent's stall stop may send SIGINT, then SIGTERM, then
  SIGKILL).
- **S9.** Test hardware runs under the new filter on real devices. NVIDIA's driver is reported to
  launch `nvidia-modprobe`.

## Solid

- The in-flight proof, FIFO, the frozen HELLO, exact lengths, checks that can't overflow.
- `pread`, and the disk place released before writing.
- Writers per channel, and premise 4.
- The joins before `ifile_close`.
- Close-on-exec, the 8 KiB fallback, `SyscallConn().Control`.
- The exit-status patch's places.
- Pausing by holding a `READ`.
- The whitelist, the exact URL, and one open per process.
