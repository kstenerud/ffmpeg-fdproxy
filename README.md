# ffmpeg-ffstreamer

A new way to feed data into FFmpeg in a security-constrained environment. These patches on top of
[jellyfin-ffmpeg](https://github.com/jellyfin/jellyfin-ffmpeg) add `ffstreamer`, a protocol over
which a parent process serves every read FFmpeg makes of its input, by byte offset, over two pipes
it hands down.

FFmpeg confined by a sandbox (Landlock, seccomp, or a container) can then read its input with no
path to the file, no socket, and no other way to reach the storage: the parent decides every read,
and can schedule, pace, share or refuse it. Storage that stops answering, such as a network share
that has gone away, holds a thread of the parent, which notices and can always end the run, never
FFmpeg waiting in the kernel where it cannot be killed. Each side checks every frame the other
sends, so an FFmpeg taken over by a crafted input can only ask for ranges of the one file it was
given.

**Status:** implemented. The protocol is specified in
[ffstreamer-specification.md](ffstreamer-specification.md), version 1, after six drafts and four
audits ([audits/](audits/)); [DESIGN.md](DESIGN.md) says why it is as it is. The patches:
- `patches/9000-ffstreamer-protocol.patch`: the protocol in libavformat, plus its registration;
- `patches/9001-fftools-input-errors.patch`: ffmpeg and ffprobe exit non-zero when an input's reads
  failed.

**Built with jellyfin-ffmpeg's own toolchain** (8.1.2-5, linux64-gpl and linuxarm64-gpl), on a
12-core machine:
- **The unpatched build reproduces jellyfin's release:** its build configuration, encoders and
  hardware accelerators are identical to the published binary.
- **The patched build differs only by the `ffstreamer` protocol.**
- **Build times:** amd64 74 minutes cold, 7 warm; arm64 64 minutes cold (its cross toolchain).

**The suite** (`test/suite.py`, 27 cases) passes on both architectures: amd64, and arm64 on an
Apple M4 under OrbStack (Linux 7.0):
- every run shape a media server uses, byte-identical read directly and through the protocol;
- every failure path as the specification says.

Also checked:
- byte-identical: a libx264 chunk encode, and an HLS fMP4 prepare (52 files);
- NVENC on an RTX 3090 through the protocol, frame-identical to a direct run;
- under a parent's confinement (Landlock and the seccomp filter of the specification's Launching) on amd64 and
  arm64: a threaded libx264 encode and ffprobe run normally, and starting a process, leaving the
  session or the process group all fail.

**Still to do:**
- hardware encoders under confinement: NVENC passes on an RTX 3090; VAAPI and QSV are untested.

## Building

```
make build-amd64          # or build-arm64; the tarball and its binaries land in dist/
make media                # the suite's two films, made from FFmpeg's test sources into media/
make test                 # the suite, against dist/bin-amd64, with the films in media/
make fuzz                 # FFmpeg's side fuzzed for 10 minutes under ASan and UBSan
make check-patch          # patches/9000 carries src/libavformat/ffstreamer.c as it stands
make check-spec           # the specification embeds ffstreamer.dogma as it stands
make patch                # rewrites patches/9000 from src/ after an edit there
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
(`fuzz/make_corpus.py` writes them), then makes seeks at the edges of the arithmetic. Ten minutes,
17 million inputs, found nothing; a four-hour run then found that a file size near the top of int64
overflowed rounding up to whole blocks, fixed and kept as a seed.

## Layout

| Path | What |
| --- | --- |
| `ffstreamer-specification.md` | The protocol between FFmpeg and the process that serves its reads. |
| `DESIGN.md` | Why the protocol is as it is: rationale, FFmpeg's internals, testing, measurements, open questions. |
| `ffstreamer.dogma` | The protocol's wire format, in [Dogma](https://github.com/kstenerud/dogma). |
| `patches/` | The patches, appended to jellyfin-ffmpeg's own `debian/patches/series`. |
| `src/` | The protocol's source, from which `patches/9000` is made. |
| `checkout.sh`, `build.sh`, `Makefile`, `JELLYFIN_TAG` | Building the patched FFmpeg (above). |
| `fuzz/` | The fuzz harness of FFmpeg's side, its build and its seeds. |
| `tools/` | `check_patch.py`, `check_spec.py` and `sync_patch.py`, and scripts for the development tree (`build_dev.sh`, and the edits it applies). |
| `.github/workflows/` | CI and releases (below). |
| `bench/` | The measurements behind the protocol's choices, and the scripts that make them. |
| `audits/` | The audits of each draft. |
| `test/` | A reference server and the tests every build must pass. |

## CI and releases

`.github/workflows/`:
- **ci**, on every pull request and push to main: the patch check, five minutes of fuzzing, then
  the build and the suite on amd64 and arm64, each on its own architecture. A build starts cold,
  about an hour, because jellyfin's toolchain cache is larger than an Actions cache.
- **fuzz-weekly**: five hours of fuzzing every Sunday.
- **release**, on a tag `v<jellyfin version>-<n>` (`v8.1.2-5-1` is the first release on
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
