# The "ff" prefix in FFmpeg names

Research for the reviewer's remark that `ffstreamer` will read as FFmpeg-blessed. Done 2026-10-24
against FFmpeg `master` at `9d2d5b1aa3` (full history, blobless clone in
`/yoloai/cache/ffmpeg-full`). Fetched list pages are cached in `/yoloai/cache/http/`.

## Answer in brief

- **"ff" does not mean "FFmpeg's implementation of a standard protocol".** Every user-visible "ff"
  name in FFmpeg falls into one of two groups:
  1. **a format FFmpeg itself invented and owns**: `ffm`, `ffv1`, `ffvhuff`, `ffmetadata`,
     `ffconcat`, `ffwavesynth`.
  2. **an internal helper protocol that is not for end users**: `ffrtmphttp`, `ffrtmpcrypt`.

  When FFmpeg implements a standard protocol natively, the name has no prefix (`rtmp`, `rtmpt`,
  `rtmpe`, `http`, `srtp`). The prefix that marks an implementation is `lib`, for wrappers of
  external libraries (`librtmp`, `libsrt`).
- **Did any start under another name? Two did, and both went *to* "ff":** `rtmphttp` was renamed
  `ffrtmphttp` a month after it landed, explicitly to mark it as internal. The concat script's file
  extensions were proposed as `cat`/`concat` and became `ffcat`/`ffconcat` because the bare words were
  too generic. None went the other way, and no "ff" name was ever argued over as a claim of
  endorsement. That is because all of them were written by core developers, for FFmpeg's own
  formats.
- **`ffstreamer` will probably draw the objection.** It claims both meanings at once: an FFmpeg-owned
  format, which a protocol defined by an outside project is not, and an internal helper, which a
  protocol meant to be named on the command line is not. The closest recent precedent, the IPFS
  rename (below), shows reviewers do push for names that say what the component really is.

## Inventory: every "ff" name FFmpeg has exposed

| Name | Kind | First commit | Origin of the name | Renamed? |
| --- | --- | --- | --- | --- |
| `ffm` | muxer/demuxer (ffserver feed) | `de6d9b6404` 2001-07-22 Fabrice Bellard (initial import) | FFmpeg's own feed format | Removed `c17f476144` 2017-10-21 |
| `ffv1` | codec | `5e20f836b3` 2003-06-09 Michael Niedermayer: "FFV1 codec (our very simple lossless intra only codec…)" | FFmpeg's own codec ("our") | No |
| `ffvhuff` | codec | `f37b9768f6` 2004-11-28 Loren Merritt: split ffhuffyuv into "huffyuv", compatible with the official version, and "ffvhuff", which "contains our improvements" | FFmpeg's incompatible extension of a third-party codec: "ff" marks *our variant*, the plain name stays with the standard | Split from `ffhuffyuv` |
| `ffmetadata` | muxer/demuxer | `a46515115c` / `fd5b124d74` 2010-12-27 Anton Khirnov; long name "FFmpeg metadata in text format" | FFmpeg's own text format | Named so from the first posting; only the source files moved, `meta*.c` → `ffmeta*.c` (`645439c3c3`) |
| `ffwavesynth` | decoder | `b33fd66f46` 2011-11-02 Nicolas George, "pseudo-codec"; its doc says "Internal wave synthetizer … Its use is purely internal and the format … is not publicly documented" | Internal, invented format (fed by the `sbg` demuxer) | No |
| `ffrtmphttp` | protocol | `8e50c57dcb` 2012-06-17 Samuel Pitoiset as **`rtmphttp`**, "an internal implementation detail just used as an abstraction layer" | Internal helper | **Yes**: `775c4d3625` 2012-07-16, "The prefix makes it easier to distinguish the proper end-user protocols from the internal ones." |
| `ffrtmpcrypt` | protocol | `acd554c103` 2012-07-19 Samuel Pitoiset; same "internal implementation detail" wording | Internal helper | Born with the prefix, three days after the rename |
| `ffconcat` / `ffcat` | concat script header and extensions | header `ffconcat version 1.0`: `b68dd8a161` 2013-02-03 Nicolas George; extensions `6cc12353a8` 2013-02-21 Stefano Sabatini | FFmpeg's own script format | Extensions **proposed as `cat`/`concat`**, renamed before they were pushed; later reverted for security (`3ab25e4574`), so only the header remains |

Not "ff" names, though they look it: the `fftdnoiz` and `fftfilt` filters (FFT), and the tools and
build directories (`ffmpeg`, `ffprobe`, `ffplay`, `fftools/`, `ffbuild/`). C symbols always carry
`ff_`, which is a linkage convention (below). Internal C helpers such as `ffjni.c` and `fflcms2.c`
are never seen by users.

As of `9d2d5b1aa3`, the registered user-visible "ff" names are `ffrtmpcrypt` and `ffrtmphttp`
(protocols, neither documented in `doc/protocols.texi`), `ffmetadata` (format), and `ffv1`,
`ffvhuff` and `ffwavesynth` (codecs).

## The discussions

### rtmphttp → ffrtmphttp (Libav, July 2012)

- The commit message gives the reason: "The prefix makes it easier to distinguish the proper end-user
  protocols from the internal ones" (`775c4d3625`; FFmpeg's copy:
  https://ffmpeg.org/pipermail/ffmpeg-cvslog/2012-July/052622.html).
- The thread was on libav-devel as "[PATCH] rtmpt: Rename rtmphttp to ffrtmphttp", with replies from
  Martin Storsjö, Diego Biurrun and Derek Buitenhuis (thread index:
  https://lists.libav.org/pipermail/libav-devel/2012-July/thread.html, messages 031126–031184;
  read through the Wayback Machine, copy cached as `libav-devel-2012-July.html`). The bodies could
  not be fetched: lists.libav.org and patchwork.libav.org are offline, and Wayback refused the
  individual messages. The Libav patchwork summary (patch 24363, 2012-07-15, seen only in a search
  snippet) describes the aim as making "the concept of internal protocols as abstraction layer
  nicer".
- Context: five months earlier Diego Biurrun had given the librtmp wrappers a `lib` prefix
  (`c3b57d6e76`, "allows easily differentiating between both implementations"). That left the bare
  names `rtmp`, `rtmpt` and so on for the native implementations, `lib*` for external ones, and
  `ff*` for internal plumbing.

### ffmetadata (ffmpeg-devel, December 2010)

The format was "ffmetadata" from the first posting: "[PATCH 2/3] Metadata muxer"
(https://ffmpeg.org/pipermail/ffmpeg-devel/2010-December/084062.html) added `doc/ffmetadata.txt`
describing "The FFmpeg metadata format". The reviews (084076, 084079, 084180, 084238) were about
encoding and where the documentation goes. Nobody questioned the name.

### ffconcat (ffmpeg-devel, February 2013)

"[PATCH] lavf/concatdec: define "cat" and "concat" extensions"
(https://ffmpeg.org/pipermail/ffmpeg-devel/2013-February/139411.html). Nicolas George replied:
"I used "ffconcat" for my test files; I am afraid cat, and even concat, would be a little too
generic" (139414). Stefano Sabatini: ""ffcat/concat" seems also consistent with the "ffmeta"
format" (139609). It was pushed as `ffcat`/`ffconcat` (139787) and reverted the next week over
safety (139809, `3ab25e4574`). The prefix here meant *a format of FFmpeg's own*, to tell it apart
from generic words.

### IPFS → ipfs_gateway (ffmpeg-devel, December 2022): the closest precedent

This rename did not involve "ff", but it is the clearest case of reviewers objecting that a name
claims more than the code delivers. Derek Buitenhuis: "It is a URL rewriter for IPFS gateways, not an
actual implementation of IPFS, and naming it as such was both incorrect and misleading"
(https://ffmpeg.org/pipermail/ffmpeg-devel/2022-December/304733.html; commit `94aa70d757`).
Tomas Härdin: "I agree verbosity is better here" (304775). The URL scheme stayed `ipfs:` so that
existing URLs would not break (304809). Only the component name (`configure`, symbols) changed, to
`ipfs_gateway`. Recent protocols follow the same split: a descriptive component name with a short
scheme. Examples are `windows_handle` with scheme `handle:` (`1ae485ba98`, 2026-10-10) and
`android_content` with scheme `content:` (`6567516a5e`).

## Written conventions

`doc/developer.texi`, "Naming conventions", covers only C symbols: `ff_` for symbols visible outside
a file but internal to a library (e.g. `ff_w64_demuxer`, to avoid clashes in static builds),
`avpriv_` for symbols shared across libraries, and `av_`/`AV` for public API. No written rule covers
the user-visible names of protocols, formats or codecs. The convention above is unwritten and comes
only from precedent. Note also that a URL scheme may contain only `[A-Za-z0-9+-.]`
(`URL_SCHEME_CHARS` in `libavformat/avio.c`), so a scheme cannot contain an underscore.

## Counterexamples

I found **none**: no third-party or externally defined protocol or format has been merged under an
"ff" name. Protocols contributed recently for outside use all have plain descriptive names: `fd`
(`6e7c006e40`, 2022), `concatf` (`8c2c0135e5`, 2021), `ipfs_gateway` (2022), `android_content`
(2024), `shared` (`56de70a2e6`, 2026-01-26), `windows_handle` (2026-10-10). `fd` and
`windows_handle` are the nearest relatives of this protocol: an input handed down by the parent
process.

## Conclusion and alternative names

"ff" conventionally means *FFmpeg's own invented format* (`ffv1`, `ffmetadata`, `ffconcat`) or *an
internal helper protocol that users don't name* (`ffrtmphttp`, `ffrtmpcrypt`). `ffstreamer` is
neither: an outside project defines it, and users name it on the command line. Expect the objection.
The IPFS thread shows the remedy reviewers ask for: a name that says what the component does.

None of these candidates appears anywhere in the FFmpeg tree (`git grep -wi` at `9d2d5b1aa3`
finds no protocol, format, codec, filter, bitstream filter, device or option of that name). Each is
a legal URL scheme, and each fits beside `pipe`, `fd` and `handle`:

1. **`fdproxy`**: reads are proxied by the process that owns the descriptors. Short, and close to
   `fd`.
2. **`piperange`**: byte-range reads served over pipes. Says the mechanism.
3. **`fdrange`**: the same idea, aligned with the `fd` protocol.
4. **`pipeproxy`**: like `fdproxy`, keyed to the existing `pipe` protocol.
5. **`parentio`**: I/O served by the parent process. Says who serves the reads rather than how.

My preference is `fdproxy` or `piperange`. Like `ipfs_gateway`, either says what it really does: the
parent process serves FFmpeg's reads, and FFmpeg opens no file itself.
