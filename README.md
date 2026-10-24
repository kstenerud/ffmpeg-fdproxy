# FFmpeg + FFstreamer

FFstreamer is a new way to feed data into FFmpeg in a security-constrained environment. These
patches on top of [jellyfin-ffmpeg](https://github.com/jellyfin/jellyfin-ffmpeg) add `ffstreamer`,
a protocol over which a parent process serves every read FFmpeg makes of its input, by byte offset,
over two pipes it hands down.

A sandbox (Landlock, seccomp, or a container) that leaves FFmpeg no path to the file, no socket,
and no other way to reach the storage then still lets it read its input: the confinement is the
sandbox's, and the protocol is what makes it workable. The parent decides every read,
and can schedule, pace, share or refuse it. Storage that stops answering (such as a network share
that has gone away) holds a thread of the parent, which notices and can always end the run, rather
than FFmpeg waiting in the kernel where it cannot be killed. Each side checks every frame the other
sends, so an FFmpeg taken over by a crafted input can only ask for ranges of the one file it was
given.

## Documents

* [ffstreamer-specification.md](ffstreamer-specification.md)
* [ffstreamer.dogma](ffstreamer.dogma)
* [DESIGN.md](DESIGN.md)

## Code

* [Patch: ffstreamer protocol in libavformat](patches/9000-ffstreamer-protocol.patch)
* [Patch: ffmpeg and ffprobe exit non-zero when an input's reads failed](patches/9001-fftools-input-errors.patch)
* For FFmpeg's own tree: `make upstream` writes the same changes as a series of six commits on the
  FFmpeg commit `UPSTREAM_COMMIT` names, into `build/upstream/`: the protocol with its manual
  entry, Changelog line and version bump; a FATE test; then `seek.c`, `ffmpeg_demux.c`, `ffmpeg.c`
  and `ffprobe.c` one each. `tools/upstream.py` makes each change at an anchor in FFmpeg's file,
  from `src/` and `upstream/`, and fails when the fftools or `seek.c` lines differ from `patches/`.
  The commits carry no `Signed-off-by`; `git format-patch -s` adds the sender's.

## Verification

### Built with jellyfin-ffmpeg's own toolchain

(8.1.2-5, linux64-gpl and linuxarm64-gpl, on a 12-core machine)

* The unpatched build reproduces jellyfin's release: its build configuration, encoders and
  hardware accelerators are identical to the published binary.
* The patched build differs only by the `ffstreamer` protocol.
* Build times: amd64 74 minutes cold, 7 warm; arm64 64 minutes cold (its cross toolchain).

### Test suite

(`test/suite.py`, 27 cases, verified on amd64, and arm64 on an Apple M4 under OrbStack (Linux 7.0))

* Every run shape a media server uses, byte-identical read directly and through the protocol.
* Every failure path as the specification says.

### Hardware encoders

(confined, and reading through the protocol)

* NVENC on an NVIDIA RTX 3090, frame-identical to a direct run.
* VAAPI on an AMD Radeon RX 6600 XT, and on Intel Gen 8, 9.5 and 12 graphics, decoding to the frames
  of a direct run.
* Quick Sync on Intel Gen 12 (an i3-N305, in a Proxmox container), decoding to the frames of a
  direct run. The Quick Sync runtime jellyfin-ffmpeg uses supports nothing older than Gen 12; VAAPI
  covers those.

### Fuzzing

* FFmpeg's side, under AddressSanitizer and UndefinedBehaviorSanitizer: four hours on eight workers,
  2.1 billion inputs, after a first long run found (and kept as a seed) a file size near the top of
  int64 that overflowed rounding up to whole blocks.

### Also checked

* Claims, under AddressSanitizer and UndefinedBehaviorSanitizer (`test/claims_test.c`): a second
  session on a pipe that is open already is refused, and a hundred sessions in turn, each on a new
  pipe at the descriptor number the last one closed, all open.
* Byte-identical: a libx264 chunk encode, and an HLS fMP4 prepare (52 files).
* Under a parent's confinement (Landlock and the seccomp filter of the specification's Launching)
  on amd64 and arm64: a threaded libx264 encode and ffprobe run normally, and starting a process,
  leaving the session or the process group all fail.

## Building

```bash
make build-amd64          # or build-arm64; the tarball and its binaries land in dist/
make media                # the suite's two films, made from FFmpeg's test sources into media/
make test                 # the suite, against dist/bin-amd64, with the films in media/
make fuzz                 # FFmpeg's side fuzzed for 10 minutes under ASan and UBSan
make test-claims          # a pipe serves one session at a time; a closed one's number serves the next
make check-patch          # patches/9000 carries src/libavformat/ffstreamer.c as it stands
make check-spec           # the specification embeds ffstreamer.dogma as it stands
make patch                # rewrites patches/9000 from src/ after an edit there
make upstream             # the series for FFmpeg's own tree, into build/upstream/
make test-upstream        # the series built on FFmpeg master, its FATE test run, its manual built
```

`checkout.sh` checks out jellyfin-ffmpeg at the tag in `JELLYFIN_TAG`, adds `patches/` after
jellyfin's own `debian/patches/series`, and `build.sh` runs jellyfin's portable builder on it, which
needs Docker. The first build makes its toolchain image, an hour or more; later builds take minutes.

`make media` writes the suite's films with the patched FFmpeg's own test sources
(`test/make_media.py`): five minutes of 720p H.264, a 5.1 and a stereo track, subtitles and
chapters, as Matroska and as MP4 with its index at the end. They take a minute and 240 MB. The suite
takes any films with those four streams in that order (`MEDIA=<dir>`).

`make fuzz` builds `fuzz/ffstreamer_fuzzer.c`, a libFuzzer harness that feeds the protocol arbitrary
bytes as the parent's frames, with AddressSanitizer and UndefinedBehaviorSanitizer, against a minimal
FFmpeg built the same way, and runs it on eight workers from the seeds in `fuzz/corpus/`
(`fuzz/make_corpus.py` writes them), then makes seeks at the edges of the arithmetic.
`make test-claims` builds `test/claims_test.c` the same way and runs it.

`make test-upstream` builds FFmpeg at `UPSTREAM_COMMIT` with the series and nothing else enabled, in
Docker, fails on any warning in the series' files, builds the protocols manual, and runs
`fate-ffstreamer`: `src/libavformat/tests/ffstreamer.c`, which serves a file from a thread over two
pipes and reads it through the public avio API, against `src/tests/ref/fate/ffstreamer`. It reads
through, by seeks and at random through a cache of two blocks; meets a parent that leaves, stalls
(ended by the interrupt), misnumbers a `DATA` (the fault sticks) or stops reading (SIGPIPE at its
default) or sends a `DATA` a byte at a time (the interrupt is seen between bytes); waits for room in
a full request pipe until interrupted; and checks the descriptors: made blocking, closed after a
session and an open that fails once they are claimed, left open by an open refused for them (no
pipe, the wrong way round, one descriptor for both, stdin) and by an `EBUSY`, including one that
shares only a live session's write pipe, which reads on. Each of these faults injected into the
protocol fails it: LRU eviction made MRU, the sticky fault, the interrupt check, the sequence check
or the SIGPIPE mask removed, a refused open's descriptors kept, `O_NONBLOCK` kept; and the old
protocol fails the byte-at-a-time, full-pipe, refusal and write-pipe cases.

Media servers that burn subtitles in (`-vf subtitles=f=...`) or use `movie=` open those files by
name, outside any whitelist: such a run needs a path to the subtitle file, or the file extracted
first through this protocol.

## Layout

| Path | What |
| --- | --- |
| [`ffstreamer-specification.md`](ffstreamer-specification.md) | The protocol between FFmpeg and the process that serves its reads. |
| [`DESIGN.md`](DESIGN.md) | Why the protocol is as it is: rationale, FFmpeg's internals, testing, measurements, open questions. |
| [`ffstreamer.dogma`](ffstreamer.dogma) | The protocol's wire format, in [Dogma](https://github.com/kstenerud/dogma). |
| [`patches/`](patches/) | The patches, appended to jellyfin-ffmpeg's own `debian/patches/series`. |
| [`src/`](src/) | The protocol's source, from which `patches/9000` is made, and its FATE test and reference. |
| [`upstream/`](upstream/), [`UPSTREAM_COMMIT`](UPSTREAM_COMMIT) | The manual entry for FFmpeg's own tree, and the FFmpeg commit the series is made on. |
| [`checkout.sh`](checkout.sh), [`build.sh`](build.sh), [`Makefile`](Makefile), [`JELLYFIN_TAG`](JELLYFIN_TAG) | Building the patched FFmpeg (above). |
| [`fuzz/`](fuzz/) | The fuzz harness of FFmpeg's side, its build and its seeds. |
| [`tools/`](tools/) | `check_patch.py`, `check_spec.py` and `sync_patch.py`; `upstream.sh` and `upstream.py`, which make the series for FFmpeg's own tree; and scripts for the development tree (`build_dev.sh`, and the edits it applies). |
| [`.github/workflows/`](.github/workflows/) | CI and releases (below). |
| [`bench/`](bench/) | The measurements behind the protocol's choices, and the scripts that make them. |
| [`audits/`](audits/) | The audits of each draft. |
| [`test/`](test/) | A reference server and the tests every build must pass. |

## CI and releases

`.github/workflows/`:

* **ci**, on every pull request and push to main: the patch and grammar checks, the series on
  FFmpeg's own tree with its FATE test, the claims test, five minutes of fuzzing, then the build and the suite on amd64 and arm64, each on its own architecture. A build
  starts cold, about an hour, because jellyfin's toolchain cache is larger than an Actions cache.
* **fuzz-weekly**: five hours of fuzzing every Sunday.
* **release**, on a tag `v<jellyfin version>-<n>` (`v8.1.2-5-1` is the first release on
  jellyfin-ffmpeg v8.1.2-5): the same build and suite, then a release holding
  `ffmpeg-ffstreamer_<version>_linux64-gpl.tar.xz`, `…_linuxarm64-gpl.tar.xz` and `SHA256SUMS`. The
  tarballs are laid out as jellyfin's portable ones, `ffmpeg` and `ffprobe` at the root.

## Licence

The code is LGPL 2.1 or later ([LICENSE](LICENSE)), as FFmpeg's own source is: the patches, the
reference parent, the tests and the tools. The specification and its grammar
([ffstreamer-specification.md](ffstreamer-specification.md), [ffstreamer.dogma](ffstreamer.dogma))
are under the [Creative Commons Attribution License](https://creativecommons.org/licenses/by/4.0/),
so anyone may implement the protocol in any program. The binaries built with these patches are GPL, as
jellyfin-ffmpeg's builds are, since its configuration enables GPL libraries such as x264. Every
release publishes the patches and the jellyfin-ffmpeg tag it applies to, so its source can always
be had.
