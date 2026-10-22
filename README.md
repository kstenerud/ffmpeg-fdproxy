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

They pass the suite (`test/suite.py`, 26 cases) against a development build. The release build with
jellyfin-ffmpeg's toolchain is next.

This repository lives in a sandbox's shared files for now. It moves to GitHub, with its own CI and
releases, once the protocol and patches are viable.

## Layout

| Path | What |
| --- | --- |
| `PROTOCOL.md` | The wire protocol between FFmpeg and the process that serves its reads. |
| `patches/` | The patches, appended to jellyfin-ffmpeg's own `debian/patches/series`. |
| `src/` | The protocol's source, from which `patches/9000` is made. |
| `tools/` | Scripts for the development tree (`build_dev.sh`, and the edits it applies). |
| `bench/` | The measurements behind the protocol's choices, and FFmpeg's read traces. |
| `audits/` | The audits of each draft. |
| `test/` | A reference server and the tests every build must pass. |
| `.github/workflows/` | Builds jellyfin-ffmpeg's portable Linux binaries with the patches, for amd64 and arm64. |

## Licence

FFmpeg and jellyfin-ffmpeg are GPL in the builds this patches, and so are these patches and the
binaries built with them. Every release publishes the patches and the jellyfin-ffmpeg tag it applies
to, so its source can always be had.
