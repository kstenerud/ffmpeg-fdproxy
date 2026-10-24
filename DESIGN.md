ffstreamer: Design Notes
========================

Why the [specification](ffstreamer-specification.md) is as it is, what it rests on in FFmpeg, how it
is tested and measured, and what is still open. The specification is normative; this document is
not.


### Contents

* [Status and History](#status-and-history)
* [Rationale](#rationale)
* [What It Rests On in FFmpeg](#what-it-rests-on-in-ffmpeg)
* [The Patches](#the-patches)
* [The Block Cache](#the-block-cache)
* [Testing](#testing)
* [Performance](#performance)
* [Open Questions](#open-questions)



Status and History
------------------

Version 1 of the protocol. The specification went through six drafts and four audits (in
[audits/](audits/), which cite the drafts' numbered sections) before it was implemented:

* **Draft 5** took in the third audit round
  ([audits/2026-10-22-draft-4-combined.md](audits/2026-10-22-draft-4-combined.md)).
* **Draft 6** took in the review of that change
  ([audits/2026-10-22-draft-5-review.md](audits/2026-10-22-draft-5-review.md)), which judged it ready
  to implement.
* **Since implementing it:** the file size is bounded at 2^62, after a long fuzz found an overflow
  near the top of int64; measuring the block cache found FFmpeg taking ffstreamer for a network
  protocol, now counted as local, and replaced a formula that was wrong; and the specification was
  restructured, its rationale moved here.

**What draft 5 changed from draft 4:**

* `DONE(TERMS)` is sent from the open, since FFmpeg never calls the close of an open that failed.
  Every other `DONE` comes from the close.
* An interrupt is `DONE`'s kind `INTERRUPTED`, and the fftools patch never lets one become exit 0.
  When the parent kills a run, its own reason decides whether that was a stop or a failure.
* The seccomp filter is written out, per architecture: `clone3` answers "not implemented" so glibc
  falls back to a `clone` the filter can read. Denying `clone3` outright broke FFmpeg's threads.
  `setpgid` is denied too.
* The parent kills the process itself, by pidfd, not its process group, which a hostile FFmpeg could
  leave.
* The block cache's size is an option the parent sets.
* Pausing holds `READ`s and stops the process (SIGSTOP), so a paused run uses neither disk nor
  processor.
* The inherited descriptors are contiguous.
* Stops are separated from failures; `run` is at least 1; the reserve is (outstanding + 1) × the
  longest `READ`.



Rationale
---------

**Pipes, not shared memory or `splice`.** Measured on a file in the page cache, a pipe with the
parent reading into its own memory carries about 940 MB/s, a sixth of what shared memory or `splice`
reach, and about 15% of one core at a NAS's 110 MB/s. `splice` was ruled out for safety: it holds
FFmpeg's pipe locked while the storage read waits, and FFmpeg's `read()` of that pipe then waits
uninterruptibly, so a dead share leaves FFmpeg unkillable (reproduced in the first audit). Shared
memory adds a second channel to confine and check. A staged `splice` (the file into a pipe private to
the parent, then pipe to pipe into FFmpeg's) would keep the safety if the cost ever matters.

**No messages to stop, pause or report errors.** The parent already has signals, and an FFmpeg taken
over by its input would ignore a polite message. Holding a `READ` trips no timeout inside FFmpeg: a
45-second hold mid-run finished normally (the third audit).

**Contiguous descriptors.** A gap in the inherited descriptors is filled by the first descriptor
FFmpeg opens or duplicates itself: measured, `dup(6)` returned 4, the gap.

**Counting a `READ` as answered when the parent takes it.** Counting down after the `DATA` header was
written wrongly judged 8 reads in 20,000 over the limit; counting down on taking the read, none.

**Signed fields.** The values on the wire are the quantities FFmpeg's I/O layer already holds as
signed (`int64_t` positions and sizes, `int` lengths, which share their return value with negative
`AVERROR`s), so signed fields go into them with no conversion. A negative value is then the one kind
of bad value. Signedness doesn't make arithmetic safe, which is why every check subtracts and the file
size is bounded.

**The file size bound, 2^62.** A four-hour fuzz of FFmpeg's side found `(size + block - 1) / block`
overflowing when a `REQUEST` gave a size near INT64_MAX. Rounding up without adding fixed that code;
the bound makes every such sum safe by construction.

**The 8 KiB fallback is tolerable.** No deadlock premise needs a bigger pipe: 127 `READ`s and a
`DONE` fitted in 8 KiB, since small writes merge into the pipe's pages. A read-through at 8 KiB ran at
half the speed of 1 MiB, still past a disk's.

**The budget's figures.** A probe reads the whole index: about 17 MB for 2.5 hours of a 10-track MP4
(the third audit), and a few percent of the file on the films in [bench/](bench/RESULTS.md). An
interleaved file is read about once through.



What It Rests On in FFmpeg
--------------------------

Checked against jellyfin-ffmpeg's source and its pinned binaries (see
[the internals audit](audits/2026-10-22-draft-3-ffmpeg-internals.md)).

* **ffmpeg reads an input from two threads, never at once.** The main thread opens it, probes it,
  runs `avformat_find_stream_info`, and makes `-ss` seeks, in `ifile_open`. Then the input's demux
  thread reads its packets, including the `-stream_loop` seek. The demux thread starts after
  `ifile_open`, and is joined (`sch_stop`) before the trailers are written and before inputs are
  closed.
* **ffprobe does all of it on one thread.**
* **Inputs close only at the end:** in ffmpeg's `ffmpeg_cleanup` → `ifile_close` →
  `avformat_close_input`, after the joins, and in ffprobe's `probe_file`. A failed open closes its
  context too. Every path on which `DONE` can be sent runs either the protocol's close or, for an open
  that fails after the `REQUEST`, the open's own failure path. FFmpeg never calls the close of an open
  that failed (`ffurl_connect` sets `is_connected` only on success).
* **No run shape a media server uses opens its input more than once:** counted for probe,
  read-through, subtitle extraction, chunk copy and encode with `-ss`/`-to` into the segment muxer,
  HLS, and `-stream_loop`.
* **Unpatched, ffmpeg and ffprobe exit 0 after an input failed mid-read** (reproduced): in the demux
  loop, in `avformat_find_stream_info`, and in ffprobe's packet loop. FFmpeg records every
  non-end-of-file read error on the input's I/O context (`pb->error`), and keeps it there.
* **ffmpeg's `main` turns `AVERROR_EXIT` into exit 0** (`ffmpeg.c`; reproduced: a SIGTERM during an
  input's open exited 0). ffprobe installs no interrupt callback and no handlers, so a signal simply
  ends it.
* **fftools merges every thread's error into one code** with no record of its source, so whether a
  failure came from demuxing, decoding or writing can't be told from inside FFmpeg. `DONE` doesn't
  try.
* **FFmpeg strips `AVSEEK_FORCE`** before calling a protocol's seek.
* **ffmpeg's signal handlers restart interrupted calls,** so a blocked read ignores the interrupt
  callback unless the protocol polls and checks it.
* **ffmpeg already ignores SIGPIPE;** ffprobe doesn't, which is why a frame's write guards it.



The Patches
-----------

Two patches, after jellyfin-ffmpeg's own series (`debian/patches/series`, applied by
`quilt push -a`), generated against the patched tree. jellyfin's 0028, 0077 and 0089 touch
`ffmpeg_demux.c`.

| File                                                  | What                                            |
| ----------------------------------------------------- | ----------------------------------------------- |
| `libavformat/ffstreamer.c` (new; from `src/`)         | the protocol                                    |
| `libavformat/protocols.c`, `Makefile`, `configure`    | one line each, registering it; `configure`'s `ffstreamer_protocol_deps` (`poll_h pthreads sigtimedwait`) drops it on builds without them: win64, macOS and OpenBSD |
| `libavformat/seek.c`                                  | one name added to the protocols FFmpeg treats as local ([The Block Cache](#the-block-cache)) |
| `fftools/ffmpeg_demux.c`, `ffmpeg.c`, `ffprobe.c`     | the exit status of a failed read, about 15 lines |

jellyfin's configure has no `--disable-everything`, so the protocol is built by default. There are
no public header changes and no new exported symbols. The fftools patch is generic, and could be
offered upstream on its own merits.



The Block Cache
---------------

FFmpeg's side keeps the last `cache_blocks` blocks it was sent, and evicts the least recently used.
Measured in [bench/RESULTS.md](bench/RESULTS.md) (`bench/interleave.py`), on five-minute films of 4 to
25 tracks, each track stored in one region of its own, and one interleaved.

**The first measurements found the cache thrashing, and the cache wasn't the cause.** An 11-track
film was served 25 times its size at 24 blocks of 1 MiB, and a prepare of a 25-track one thrashed at
every size measured. A trace of FFmpeg's reads showed why: through ffstreamer, FFmpeg asked for 16 MB
at a time, and read straight through the bytes between the tracks it wanted, where reading the same
file directly it touched 12 blocks. When FFmpeg opens an indexed file, `ff_configure_buffers_for_index`
(`libavformat/seek.c`) takes any protocol but `file`, `pipe` and `cache` for a network one, where a
seek costs a round trip: it enlarges the I/O buffer up to 16 MiB and has every seek of less than up to
8 MiB read forward instead. Through ffstreamer, whose seeks cost nothing, every hop between tracks
read megabytes, and swept them through the cache.

**Counting ffstreamer as local fixes it** (one line in the patch). Measured again, with the patched
release build:

* a probe, a read-through, a chunk or a subtitle reads a tenth of the film or less from 16 blocks,
  on every film, against a third or more before;
* a prepare mapping every track needs about a block for each track's region and a few more: 16
  blocks for 11 tracks, 24 for 17, 32 for 25, where it reads the film about 1.1 to 1.4 times;
* an interleaved film needs a few blocks, at any count of tracks.

**What remains** is least-recently-used eviction's own cliff: a prepare mapping more tracks than the
cache has blocks is served the film many times over, and a parent's budget stops it. Random eviction,
also measured, softens that cliff but costs up to five times as much where least-recently-used fits,
so it is no fix.

**How many tracks films have** (5,162 files of one household's library): a median of 3; at the 99th
percentile, 44, nearly all subtitles in Matroska remuxes, with up to 49 subtitle tracks and 44 font
attachments, but never more than 3 sound tracks. Matroska interleaves its tracks by design, and its
attachments aren't read as packets; the MP4 and MOV files, the containers whose tracks can be stored
one after another, have at most 10. So FFmpeg's default of 32 blocks, and a parent's of 48, cover any
film but a badly muxed MP4 with dozens of tracks, which the budget stops.



Testing
-------

* **A reference parent** (`test/refparent.py`) that speaks the protocol and can be slow, silent,
  wrong or short, and kill at any point; and **the suite** (`test/suite.py`), which every build must
  pass on amd64 and arm64:
    * every run shape that reads a file, read directly and through the protocol and compared byte for
      byte: probe, read-through, subtitle extraction, chunk copy, encode and sound;
    * every failure path, each with the `DONE` and exit status it must give.
* **The fuzz harness** (`fuzz/`), under AddressSanitizer and UndefinedBehaviorSanitizer; five
  minutes on every change and five hours a week in CI. Each input is a whole session: a header
  chooses the parent (answering each `READ` rightly, slowly, by dribs, wrongly or never, or sending
  the input's raw bytes), the terms, the cache, the pipe of requests (a page, full, unread, closed),
  when the interrupt fires and which system calls fail; a script of reads, seeks, second opens and
  the parent's own moves follows, through the protocol, FFmpeg's URL layer or avio. The harness
  takes the protocol's `poll`, `read`, `write` and `close`, plays the parent between its steps, and
  counts a silent slice instead of waiting it, so no input depends on the clock. It checks the
  specification's invariants as it goes, the bytes of every read against the file among them, and
  aborts on a breach ([README](README.md#building) lists them). Each of these faults injected into
  the protocol is found: the interrupt checked only on idle slices, a refused open closing its
  descriptors, only the read pipe claimed, LRU eviction made MRU, the sticky fault dropped, SIGPIPE
  left unblocked, end of file not mapped to `EIO`, `O_NONBLOCK` kept, the wait for room made
  uninterruptible, the sequence check removed, a prefetch past the `READ`s outstanding, and the
  `EAGAIN` map removed (README's Fuzzing gives the times).
* **Still to cover:**
    * two inputs, one choked by fftools' scheduler while the other is served;
    * a dead share: a storage read that never returns (a FUSE file) must leave FFmpeg killable at
      every point, and the run must end at the read's deadline;
    * repeated SIGSTOP and SIGCONT while the channel waits;
    * an interrupt during `read_header` and during probing, with a real demuxer above the protocol:
      the fuzzer fires it during the open and at any system call mid-run, but reads through avio
      alone;
    * hardware encoders under the seccomp filter: NVENC passes; VAAPI and QSV are untested.



Performance
-----------

Measured in [bench/](bench/RESULTS.md), on a file in the page cache, so the transport alone:

| Transport                                                                          | MB/s          | CPU-s per GB |
| ---------------------------------------------------------------------------------- | ------------- | ------------ |
| FFmpeg reads the file itself                                                       | ~4,300        | 0.23         |
| A pipe with a 1 MiB buffer, the parent reading into its memory and writing (chosen) | ~940         | 1.4          |
| The same with `splice` from the file (not used: see [Rationale](#rationale))       | ~1,500–2,000  | 0.6–0.7      |
| Shared memory (not chosen)                                                         | ~2,000        | 0.45         |

Prefetching when FFmpeg reads straight through keeps nearly all of always-prefetching's gain on
FFmpeg's own read traces, and never costs more than no prefetch.

**Still to measure:** a real NAS under real competing load; a chunk run's time to its first output;
one `READ` outstanding against two.



Open Questions
--------------

* **A cache sized for each run:** a parent that knows how many tracks a run maps could size its cache
  as that and a few more, so that no prepare thrashes. Nothing measured yet needs it.
