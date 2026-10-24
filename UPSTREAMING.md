Upstreaming to FFmpeg
=====================

What FFmpeg's reviewers would want to know when they weigh these patches, gathered in one place,
and what is still to decide before anything is sent.

**Status: parked.** Nothing is sent, and no one has committed to sending the series or to
maintaining it in FFmpeg. These patches serve Orpheum, are easy to carry on top of jellyfin-ffmpeg,
and stay that way if FFmpeg declines them. Contributing them would be voluntary, and the decisions
under [Parked Decisions](#parked-decisions) wait until after Orpheum's beta (the owner, 2026-10-24).
Until then the series is kept ready: `make upstream` writes it against FFmpeg master, and
`make test-upstream` builds it there and runs its FATE test.


### Contents

* [Why It Is Needed](#why-it-is-needed)
* [Why Not What FFmpeg Has](#why-not-what-ffmpeg-has)
* [Design Choices a Reviewer Will Ask About](#design-choices-a-reviewer-will-ask-about)
* [The Series](#the-series)
* [Evidence](#evidence)
* [Parked Decisions](#parked-decisions)



Why It Is Needed
----------------

A program that runs FFmpeg on files it does not trust, as a media server does on every file in a
library, wants FFmpeg confined: no path to the file system beyond what the run needs, no network,
no way to reach other files if a crafted input takes the process over. Linux gives the tools
(Landlock, seccomp, namespaces), and the confinement is theirs, not this protocol's. What they
cannot fix is how FFmpeg reads its input:

* **Given the file by path,** FFmpeg needs the right to open it, and a confined process with a path
  to the library can be steered to read other files in it.
* **Given an open descriptor of the file** (`fd:`), it reads in the kernel. When the storage stops
  answering, as a network share does when its server goes away, the read waits in the kernel,
  uninterruptibly (reproduced in this protocol's first audit). The run cannot be killed, the program cannot tell a slow disk from a dead one, and its slot is lost until the
  storage returns.
* **Neither lets the parent decide the reads.** A server that runs several FFmpeg processes on one
  disk wants to schedule their reads, pace a background job behind a viewer's, and refuse a range
  that is not the file's.

`ffstreamer:` moves every read to the parent. FFmpeg inherits two pipes and asks for byte ranges of
its one input; the parent reads the file and answers. Storage that stops answering holds a thread
of the parent, which notices and kills the run, which always dies, since FFmpeg only ever waits in
`poll` on a pipe. The parent sees, paces and bounds every read. FFmpeg, confined with no path to
the file and no socket, can still seek anywhere in its input, so files with their index at the end
work, which no streaming input allows.

The draft paragraph for the protocol commit's message:

> A program that runs FFmpeg confined, with no path to the file system and no network, cannot hand
> it its input by path, and a descriptor of the file leaves FFmpeg reading in the kernel, where a
> network share that stops answering holds it in a state no signal ends. This protocol moves every
> read to the parent process: FFmpeg inherits two pipes, asks for byte ranges of its one input, and
> waits only in poll(), interruptibly, so a parent whose storage fails can always end the run. The
> parent can schedule, pace and refuse reads. Unlike pipe:, the input is seekable, so MP4 files with
> their index at the end can be read. The wire protocol is specified at <tagged URL>; a parent
> implementation is in production in a media server.



Why Not What FFmpeg Has
-----------------------

| Mechanism | Why it does not do the job |
| --- | --- |
| `file:` | Needs a path, so the confined process can open the library; reads in the kernel, so a dead share makes it unkillable. |
| `fd:` | Seekable, but reads in the kernel just the same: the dead-share hang, and no say for the parent over reads. |
| `pipe:` | Not seekable. MP4s with the index at the end, and every seek a player's chunk needs, fail. |
| `http:`, `tcp:`, `unix:` | Need a socket, a server and a listening address; the confinement then has to allow networking, and the parent becomes an HTTP server with range requests, its framing far heavier than one small header a block. |
| A custom `AVIOContext` | Only for programs that link libavformat. A server that runs the `ffmpeg` and `ffprobe` command-line tools, as most do for isolation and upgrades, cannot use one. |
| FUSE or NBD | Need a mount inside the sandbox, privileges to make one, and a kernel path that can hang the same way. |
| `async:` or `cache:` over any of these | Change buffering, not where the read waits. |



Design Choices a Reviewer Will Ask About
-----------------------------------------

**A new protocol, not an option of `fd:`.** The wire format, the block cache and the opening
exchange are their own code; folded into `fd:`, they would make it two protocols behind one name.

**No signals, no threads.** The protocol starts no thread and touches no signal. Writing to a pipe
whose reader has gone raises SIGPIPE, as for any pipe FFmpeg writes; like `pipe:`, it leaves that to
the program, which ignores it, as `ffmpeg` does, and as the series makes `ffprobe` do. It needs only
`poll()`, so it builds wherever `fd:` does, apart from Windows.

**Interruptible waits.** Every wait for the parent is `poll` in 100 ms slices, checking the interrupt
callback at every pass, so a run is never held by a parent.

**Descriptors, as `fd:` has them.** The protocol works on duplicates of the two descriptors, made
close-on-exec, and never closes or changes the caller's, whatever the outcome; a pipe handed over
non-blocking stays so, since every read and write polls first. Until 2026-10-24 it took them over,
closing them when the session ended so that no later open could reach the pipe and read bytes the
session left behind. Following `fd:` costs nothing an input file can reach: a nested open is given
no descriptors, and the default whitelist keeps out `pipe:` and `fd:`. A later session on a used
channel, which only a program passing the options itself could start, meets the leftover bytes where
its opening exchange expects `HELLO`, and fails, never taking them for its file (the owner's
decision, 2026-10-24).

**One session a pipe.** The protocol keeps a list of the pipes its open sessions use, by device and
inode, and refuses (`EBUSY`) an open whose pipe is in use, so two sessions cannot interleave their
frames. It is process-wide state, guarded by FFmpeg's static mutex; without it a second open given
a live session's descriptors would write into that session's stream.

**Restricting nested opens.** The protocol's default whitelist is itself, so a demuxer that opens
another resource while reading it (HLS, DASH, concat) cannot name `pipe:4` or `fd:` and read the
channel directly, even if the program sets no whitelist.

**Option names.** `read_fd`, `write_fd` and `cache_blocks`, plain, as `fd:` has `fd` and `shared:`
has `cache_dir`.

**Errors.** A malformed frame is `AVERROR_INVALIDDATA`; the channel ending is `AVERROR(EIO)`, never
end-of-file, so a parent that died is never taken for the end of the film; an interrupt is
`AVERROR_EXIT`. Each sticks: every later read returns it.

**Counting ffstreamer as local in `seek.c`.** FFmpeg's index seeking reads ahead generously on
network protocols, judged by a list of local protocol names. Every block here comes from a local
pipe, and taken for a network protocol an 11-track film was read 25 times over, where read directly
it touched 12 blocks (DESIGN.md, The Block Cache). The patch adds the name to the list; a protocol flag would be the
cleaner fix, and the series offers to make one if asked.

**An external specification.** The protocol is a contract between FFmpeg and a parent that is not
part of FFmpeg, so it is specified apart from FFmpeg's code, versioned, with its wire format in a
formal grammar (`ffstreamer.dogma`). The manual links a tagged version of it.



The Series
----------

Two series, sent apart, since they are separate decisions for separate maintainers:

1. **The fftools series** (to the ffmpeg CLI and ffprobe maintainers): ffmpeg fails when reading an
   input failed (an I/O error, not end-of-file), where it exited 0 before unless `-xerror` was
   given; `-stream_loop` stops looping an input whose reads fail; ffmpeg exits non-zero when a
   signal interrupts opening an input; ffprobe fails the same way, maps an interrupt to EINTR, and
   ignores SIGPIPE. These change behaviour for every input, HTTP among them: a dropped stream that
   exited 0 now fails. Offered as the default, with an opt-in form (a value of `-abort_on`) ready if
   the maintainers prefer it, since the ffstreamer parent can pass a flag as easily.
2. **The protocol series** (to libavformat): the protocol with its manual and Changelog; its FATE
   test; counting it as local in `seek.c`.

`make upstream` writes both into `build/upstream/fftools/` and `build/upstream/protocol/`.



Evidence
--------

* **FATE:** `fate-ffstreamer` serves a file from a thread and reads it through the public avio API:
  reads, seeks, random reads through a two-block cache, a parent that leaves, stalls, misnumbers,
  stops reading or trickles, a full request pipe, refused descriptors, a second session. Each fault
  injected into the protocol fails it (README, Verification).
* **Fuzzing** under ASan and UBSan, of every byte the parent sends and the sequence of reads and
  seeks (fuzz/).
* **A suite of real runs** (test/suite.py) against a release build: probes, read-throughs, subtitle
  extraction and chunk encodes of Matroska and MP4 films, byte-identical to reading the file
  directly, and every failure the parent can cause.
* **Production:** Orpheum's media node serves every FFmpeg and ffprobe run this way.



Parked Decisions
----------------

Each waits until after Orpheum's beta, and none commits anyone to anything until it is made.

| Decision | Where it stands |
| --- | --- |
| Send the series at all | Parked. Rejection is acceptable: the patches are carried on jellyfin-ffmpeg either way. |
| A MAINTAINERS entry | Parked (the owner, 2026-10-24). FFmpeg asks who adds a protocol to maintain it; no one takes that on during the beta. |
| The protocol's name | Researched (docs/research/ff-prefix-names.md); the choice is parked. In FFmpeg "ff" marks a format FFmpeg invented and owns (`ffv1`, `ffmetadata`, `ffconcat`) or an internal helper no one names (`ffrtmphttp`, renamed to it to set it apart from the protocols users name); an outside-defined protocol users name is neither, so the name will likely be questioned. Candidates no FFmpeg component uses: `fdproxy`, `piperange`. |
| `Signed-off-by` | Added by the sender, when sending. |
| Disclosing AI assistance | Parked; FFmpeg's policy discussion was unresolved when last checked (2026-10-23). |
| The fftools default | Proposed as the default, opt-in ready (see [The Series](#the-series)). |
