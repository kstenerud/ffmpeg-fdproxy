# ffmpeg-ffstreamer

Patches on top of [jellyfin-ffmpeg](https://github.com/jellyfin/jellyfin-ffmpeg) that let a parent
process serve every read FFmpeg makes of its input, by byte offset, over descriptors it hands down:
no socket, no path to the media file, and every read the parent's to schedule, pace, share, or
refuse.

It exists for a media server's node. When a film sits on a
network share that goes away, an FFmpeg reading the file itself waits in the kernel and cannot be
killed. When FFmpeg reads through the node instead, the node knows, decides, and can always end the
run. The protocol is general, and nothing in the patches knows its parent.

**Status:** implementing. The protocol is [PROTOCOL.md](PROTOCOL.md), draft 6, ready to implement after
five audits ([audits/](audits/)). The patches are written:
- `patches/9000-ffstreamer-protocol.patch`: the protocol in libavformat, plus its registration;
- `patches/9001-fftools-input-errors.patch`: ffmpeg and ffprobe exit non-zero when an input's reads
  failed.

**Built with jellyfin-ffmpeg's own toolchain** (8.1.2-5, linux64-gpl and linuxarm64-gpl), on a
12-core machine:
- **The unpatched build reproduces jellyfin's release:** its build configuration, encoders and
  hardware accelerators are identical to the published binary.
- **The patched build differs only by the `ffstreamer` protocol.**
- **Build times:** amd64 74 minutes cold, 7 warm; arm64 64 minutes cold (its cross toolchain).

**The suite** (`test/suite.py`, 26 cases) passes on both architectures: amd64, and arm64 on an
Apple M4 under OrbStack (Linux 7.0):
- every run shape a media server uses, byte-identical read directly and through the protocol;
- every failure path as PROTOCOL.md specifies.

Also checked:
- byte-identical: a libx264 chunk encode, and an HLS fMP4 prepare (52 files);
- NVENC on an RTX 3090 through the protocol, frame-identical to a direct run;
- under a parent's confinement (Landlock and the seccomp filter of PROTOCOL.md §9.1) on amd64 and
  arm64: a threaded libx264 encode and ffprobe run normally, and starting a process, leaving the
  session or the process group all fail.

**Still to do:**
- CI, once the repository moves to GitHub;
- hardware encoders under confinement: NVENC passes on an RTX 3090; VAAPI and QSV are untested.

## Layout

| Path | What |
| --- | --- |
| `PROTOCOL.md` | The wire protocol between FFmpeg and the process that serves its reads. |
| `ffstreamer.dogma` | The protocol's wire format, in [Dogma](https://github.com/kstenerud/dogma). |
| `patches/` | The patches, appended to jellyfin-ffmpeg's own `debian/patches/series`. |
| `src/` | The protocol's source, from which `patches/9000` is made. |
| `tools/` | Scripts for the development tree (`build_dev.sh`, and the edits it applies). |
| `bench/` | The measurements behind the protocol's choices, and FFmpeg's read traces. |
| `audits/` | The audits of each draft. |
| `test/` | A reference server and the tests every build must pass. |

## Licence

The protocol's source (`src/libavformat/ffstreamer.c`, and so `patches/9000`) carries FFmpeg's own
header, LGPL 2.1 or later, as any file contributed to FFmpeg does. The binaries built with these
patches are GPL, as jellyfin-ffmpeg's builds are: its configuration enables GPL libraries such as
x264. Every release publishes the patches and the jellyfin-ffmpeg tag it applies to, so its source
can always be had.
