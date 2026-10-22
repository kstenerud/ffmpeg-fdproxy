# Audit of PROTOCOL.md draft 3, from FFmpeg's internals (2026-10-22)

An independent audit by an FFmpeg internals expert, against jellyfin-ffmpeg 8.1.3's source (patches
not applied) and the pinned 8.1.2-5 binaries, with `fd:` standing in for `ffstreamer:`. Scratch
scripts, including `rst.py` (a mid-run I/O error by a TCP reset on an inherited fd), are in the
auditor's scratch directory `ffaudit/`.

**Verdict:** buildable. Patch 1, the protocol, fits libavformat cleanly. Patch 2 assumes things
fftools doesn't do.

## Must fix

| # | Finding | Fix |
| --- | --- | --- |
| M1 | `DONE`'s kinds `OPTIONS`, `MEDIA` and `OUTPUT` can't be produced: fftools carries one merged `AVERROR` (`ffmpeg_sched.c:2735-2760`, `ffmpeg.c:921-926, 1043-1048`), with no record of where it came from | Keep only the kinds the protocol knows itself (`TERMS`, `CHANNEL`, `PROTOCOL`, a fault on this channel). The exit status is the run's verdict |
| M2 | There's no single exit point: ffprobe calls `exit(1)` after opening the input (`ffprobe.c:2483…2528`); ffmpeg's signal handler calls `exit(123)`. A demuxer closing a channel early never happens in ffmpeg 7/8: inputs close only in `ffmpeg_cleanup` → `ifile_close` → `avformat_close_input`. A failed open closes the pb too (`demux.c:370-371`). An exported function would need a public `av*` symbol upstream won't take | **Send `DONE` from `url_close`.** Every path that skips it exits non-zero or is killed, and a missing `DONE` already counts as failure. Optionally, fftools passes its status in with `av_opt_set_int(ic, "ffstreamer_status", …, AV_OPT_SEARCH_CHILDREN)` before `ifile_close` (precedent: hls reads http's cookies this way). No registry and no new symbol |
| M3 | "Exit non-zero when an input failed" has three holes: `input_thread` turns a read error into success without `-xerror` (`ffmpeg_demux.c:776-778`; reproduced: exit 0); `avformat_find_stream_info` treats a read error as end-of-file (`demux.c:2814-2817`; ffprobe cut at 300 KB exits 0); ffprobe's packet loop does the same (reproduced) | Promote `pb->error`, which `fill_buffer`/`avio_read` set on every non-EOF read error and which persists: after `input_thread`'s loop, after `find_stream_info` in `ifile_open`, and in ffprobe's `probe_file`. In the protocol, a fault is sticky: every later read returns it |

## Should fix

- **S1. Threads.** Open, probe, `find_stream_info` and `-ss` seeks run on the main thread. Packet reads and the `-stream_loop` seek run on the demux thread. They never overlap, since `sch_stop` joins first. ffprobe is one thread. `async:` and `cache:` are kept out by the whitelist. A mutex is harmless, but the "`DONE` racing `READ`s" test describes a state fftools never creates.
- **S2.** A blocked `read()` ignores FFmpeg's interrupt callback (`avio.c:515`; `SA_RESTART`). So SIGTERM or `q` can't end ffmpeg while the parent is silent, and `sch_stop`'s join hangs. Wait with `poll()` in ~100 ms slices, and return `AVERROR_EXIT` on `ff_check_interrupt`. That is still no timeout. Upstream will expect it.
- **S3. Return conventions.**
  - End of file returns `AVERROR_EOF`, never 0: 0 spins `retry_transfer_wrapper`.
  - Never `AVERROR(EAGAIN)`, which busy-waits.
  - Short reads are fine.
  - `url_seek` handles `SEEK_SET`, `SEEK_CUR`, `SEEK_END` and `AVSEEK_SIZE` (`AVSEEK_FORCE` is stripped first).
- **S4.** Reads aren't always 32 KiB: a packet larger than the buffer is read at full size, and probing grows the buffer. Answer any size from the block cache. Don't set `max_packet_size`. `-blocksize`, `url_get_file_handle`, `url_check` and `url_get_short_seek` aren't needed.
- **S5. Opening.**
  - Default both descriptors to -1, and fail if they're left so.
  - Accept only the exact URL `ffstreamer:`.
  - Refuse a second open for the life of the process.
  - Mark the options `AV_OPT_FLAG_DECODING_PARAM`.
- **S6.** The FIFO and direction check proves less than claimed. `-progress pipe:N` opens during option parsing, before the inputs, and duplicates its descriptor to the lowest free number, so a missed write descriptor could be its duplicate. Call the check a sanity check, rely on the `HELLO` round trip, and number the channel descriptors below the progress descriptor.
- **S7. SIGPIPE.**
  - ffmpeg already ignores it before inputs open.
  - Drop the ffprobe change: an EPIPE means the parent is dead anyway.
  - The protocol's guard: check `sigpending`, block with `pthread_sigmask`, write, and on EPIPE consume the signal with `sigtimedwait` (zero timeout) if it wasn't pending before.
- **S8.** Artwork and subtitle conversion stay on stdin: they feed bytes the node holds in memory, not the film. Only `FileInput` callers move (probe, read-through, extraction, chunks, prepare).
- **S9.** The budget against poor interleaving: a badly interleaved MP4 can thrash a 4-block cache. Size the cache above the number of streams read, and test k against a non-interleaved MP4.
- **S10. Build.**
  - Append the patches after jellyfin's `0101`, generated against the patched tree: `0028`, `0077` and `0089` touch `ffmpeg_demux.c`.
  - No `--disable-everything`.
  - Registration needs an `extern` line in `protocols.c`, an `OBJS` line in the Makefile, and `ffstreamer_protocol_deps=…` in `configure`, so win64 drops a POSIX-only protocol.

**Checked, needing no change:**
- Per-input options reach the protocol's private data before connect. Plain `url_open` suffices (`-fd 7 -i fd:` reproduced for ffmpeg and ffprobe).
- The whitelist reaches nested opens (an ffconcat naming `pipe:7` was refused) and breaks no run shape.
- No run shape reopens its input: exactly one open each, counted for every shape.
- `AVFMT_FLAG_NONBLOCK` never reaches the protocol.

## The patch's shape

| File | Change | Lines |
| --- | --- | --- |
| `libavformat/ffstreamer.c` (new) | Options; a process-wide claimed-descriptor table; open with checks and the opening exchange; frame writing (one `write`, the SIGPIPE pattern) and frame carving; read (block cache, `READ` and prefetch, `poll` with the interrupt check, `DATA` checks, sticky fault); seek (local, every whence); close (`DONE`, close the descriptors, keep the claim) | 600–800 |
| `libavformat/protocols.c`, `Makefile`, `configure` | Registration | 3 |
| `doc/protocols.texi` | The options and contract | ~30 |
| `fftools/ffmpeg_demux.c` | Promote `pb->error` after the demux loop and after `find_stream_info` | ~10 |
| `fftools/ffprobe.c` | Promote `pb->error` in `probe_file` | ~4 |
| `fftools/ffmpeg.c` (optional) | `ffstreamer_status` before `ifile_close` | ~4 |

No public header changes, and no new exported symbol. The fftools part, which exits non-zero on an
input I/O error, is generic and could go upstream on its own merits.
