#!/usr/bin/env python3
"""Make the series for FFmpeg's own tree from this repository, one commit per change.

    upstream.py FFMPEG_TREE [fftools|protocol|all]

There are two series, to be sent apart (UPSTREAMING.md, The Series): fftools, how ffmpeg and ffprobe
end when an input's reads fail, for every input; and protocol, ffstreamer itself. Each applies to
FFmpeg alone; all makes both, fftools first.

FFMPEG_TREE is a clean checkout of FFmpeg (tools/upstream.sh checks out the commit UPSTREAM_COMMIT
names). Each change is made by inserting lines at an anchor in FFmpeg's file, so the series follows
FFmpeg as it moves, and fails, naming the anchor, when one is gone. The code comes from src/ and the
documentation from upstream/, the same files the jellyfin patches carry, so the two cannot drift.
The commits carry no Signed-off-by: `git format-patch -s` adds the sender's."""
import os, re, shutil, subprocess, sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def insert(tree, path, anchor, lines, before=False):
    """Inserts lines after (or before) the one place anchor appears in tree's file path."""
    file = os.path.join(tree, path)
    with open(file, encoding="utf-8") as f:
        text = f.read()
    if text.count(anchor) != 1:
        raise SystemExit(f"{path}: the anchor appears {text.count(anchor)} times, not once:\n{anchor}")
    at = text.index(anchor) + (0 if before else len(anchor))
    with open(file, "w", encoding="utf-8") as f:
        f.write(text[:at] + lines + text[at:])


def replace(tree, path, old, new):
    """Replaces the one place old appears in tree's file path with new."""
    file = os.path.join(tree, path)
    with open(file, encoding="utf-8") as f:
        text = f.read()
    if text.count(old) != 1:
        raise SystemExit(f"{path}: the anchor appears {text.count(old)} times, not once:\n{old}")
    with open(file, "w", encoding="utf-8") as f:
        f.write(text.replace(old, new))


def copy(tree, source, path):
    os.makedirs(os.path.dirname(os.path.join(tree, path)), exist_ok=True)
    shutil.copyfile(os.path.join(HERE, source), os.path.join(tree, path))


def read(path):
    with open(os.path.join(HERE, path), encoding="utf-8") as f:
        return f.read()


def protocol(tree):
    copy(tree, "src/libavformat/ffstreamer.c", "libavformat/ffstreamer.c")
    insert(tree, "libavformat/protocols.c", "extern const URLProtocol ff_ffrtmphttp_protocol;\n",
           "extern const URLProtocol ff_ffstreamer_protocol;\n")
    insert(tree, "libavformat/Makefile", "OBJS-$(CONFIG_FFRTMPHTTP_PROTOCOL)       += rtmphttp.o\n",
           "OBJS-$(CONFIG_FFSTREAMER_PROTOCOL)       += ffstreamer.o\n")
    insert(tree, "configure", "\n    siglongjmp\n", "    sigtimedwait\n")
    insert(tree, "configure", 'ffrtmphttp_protocol_select="http_protocol"\n',
           'ffstreamer_protocol_deps="poll_h pthreads sigtimedwait"\n')
    insert(tree, "configure", "check_func  setrlimit\n", "check_func_headers signal.h sigtimedwait\n")
    insert(tree, "doc/protocols.texi", "@section file\n", read("upstream/protocols.texi"), before=True)
    insert(tree, "Changelog", "version <next>:\n", "- ffstreamer protocol\n")
    version = os.path.join(tree, "libavformat/version.h")
    with open(version, encoding="utf-8") as f:
        text = f.read()
    minor = re.search(r"#define LIBAVFORMAT_VERSION_MINOR +(\d+)", text)
    micro = re.search(r"#define LIBAVFORMAT_VERSION_MICRO +(\d+)", text)
    if not minor or not micro:
        raise SystemExit("libavformat/version.h: no LIBAVFORMAT_VERSION_MINOR or _MICRO")
    text = text.replace(minor.group(0), minor.group(0)[:-len(minor.group(1))] + str(int(minor.group(1)) + 1))
    text = text.replace(micro.group(0), micro.group(0)[:-len(micro.group(1))] + "100")
    with open(version, "w", encoding="utf-8") as f:
        f.write(text)


def fate(tree):
    copy(tree, "src/libavformat/tests/ffstreamer.c", "libavformat/tests/ffstreamer.c")
    copy(tree, "src/tests/ref/fate/ffstreamer", "tests/ref/fate/ffstreamer")
    insert(tree, "libavformat/Makefile", "TESTPROGS-$(CONFIG_FFRTMPCRYPT_PROTOCOL) += rtmpdh\n",
           "TESTPROGS-$(CONFIG_FFSTREAMER_PROTOCOL)  += ffstreamer\n")
    insert(tree, "tests/fate/libavformat.mak", "FATE_LIBAVFORMAT-$(CONFIG_FFRTMPCRYPT_PROTOCOL) += fate-rtmpdh\n",
           "FATE_LIBAVFORMAT-$(CONFIG_FFSTREAMER_PROTOCOL) += fate-ffstreamer\n"
           "fate-ffstreamer: libavformat/tests/ffstreamer$(EXESUF)\n"
           "fate-ffstreamer: CMD = run libavformat/tests/ffstreamer$(EXESUF)\n\n", before=True)


def seek(tree):
    replace(tree, "libavformat/seek.c",
            '    if (proto && !(strcmp(proto, "file") && strcmp(proto, "pipe") && strcmp(proto, "cache")))\n',
            "    /* ffstreamer is local too: its parent serves any offset, so a seek\n"
            "     * costs nothing, and reading forward in place of one would read\n"
            "     * megabytes it never needed. */\n"
            '    if (proto && !(strcmp(proto, "file") && strcmp(proto, "pipe") &&\n'
            '                   strcmp(proto, "cache") && strcmp(proto, "ffstreamer")))\n')


def demux(tree):
    replace(tree, "fftools/ffmpeg_demux.c",
            "            if (d->loop) {\n",
            "            /* An input whose reads failed is not looped: it would fail\n"
            "             * again at once, for ever. */\n"
            "            if (d->loop && !(f->ctx->pb && f->ctx->pb->error < 0 &&\n"
            "                             f->ctx->pb->error != AVERROR(EAGAIN))) {\n")
    insert(tree, "fftools/ffmpeg_demux.c",
           "    // EOF/EXIT is normal termination\n    if (ret == AVERROR_EOF || ret == AVERROR_EXIT)\n",
           "    /* An input whose reads failed fails the run, where it would otherwise\n"
           "     * end as if the input had simply ended. */\n"
           "    if ((ret >= 0 || ret == AVERROR_EOF || ret == AVERROR_EXIT) &&\n"
           "        f->ctx->pb && f->ctx->pb->error < 0 &&\n"
           "        f->ctx->pb->error != AVERROR(EAGAIN)) {\n"
           "        ret = f->ctx->pb->error == AVERROR_EXIT ? AVERROR(EINTR)\n"
           "                                                : f->ctx->pb->error;\n"
           '        av_log(d, AV_LOG_ERROR, "Reading the input failed: %s\\n",\n'
           "               av_err2str(ret));\n"
           "    }\n\n", before=True)
    insert(tree, "fftools/ffmpeg_demux.c",
           "            if (ic->nb_streams == 0)\n                return ret;\n        }\n",
           "        /* A read that failed while probing is not the input ending. */\n"
           "        if (ic->pb && ic->pb->error < 0 &&\n"
           "            ic->pb->error != AVERROR(EAGAIN)) {\n"
           "            ret = ic->pb->error == AVERROR_EXIT ? AVERROR(EINTR)\n"
           "                                                : ic->pb->error;\n"
           "            av_log(d, AV_LOG_FATAL,\n"
           '                   "Reading the input failed while probing it: %s\\n",\n'
           "                   av_err2str(ret));\n"
           "            return ret;\n"
           "        }\n")


def signal_exit(tree):
    replace(tree, "fftools/ffmpeg.c",
            "finish:\n    if (ret == AVERROR_EXIT)\n        ret = 0;\n",
            "finish:\n"
            "    /* An interrupt during the opening of an input never reaches\n"
            "     * transcode(); a signal must still exit non-zero. */\n"
            "    if (ret == AVERROR_EXIT)\n"
            "        ret = received_nb_signals ? 255 : 0;\n")


def probe(tree):
    replace(tree, "fftools/ffprobe.c",
            "end:\n    if (ifile.fmt_ctx)\n        close_input_file(&ifile);\n",
            "end:\n"
            "    /* A read that failed is not the input ending. */\n"
            "    if (ret >= 0 && ifile.fmt_ctx && ifile.fmt_ctx->pb &&\n"
            "        ifile.fmt_ctx->pb->error < 0 &&\n"
            "        ifile.fmt_ctx->pb->error != AVERROR(EAGAIN)) {\n"
            "        ret = ifile.fmt_ctx->pb->error;\n"
            "        if (ret == AVERROR_EXIT)\n"
            "            ret = AVERROR(EINTR);\n"
            '        av_log(NULL, AV_LOG_ERROR, "Reading the input failed: %s\\n",\n'
            "               av_err2str(ret));\n"
            "    }\n"
            "    if (ifile.fmt_ctx)\n        close_input_file(&ifile);\n")


# Each commit: what it changes, and its message in FFmpeg's form.
PROTOCOL = [
    (protocol, """avformat/ffstreamer: add the ffstreamer protocol

ffstreamer reads an input whose every byte a parent process serves, by
offset, over two pipes the parent hands down. FFmpeg confined by a
sandbox (Landlock, seccomp, a container) can then read its input with
no path to the file, no socket and no other way to reach the storage.
The parent decides every read, and can schedule, pace or refuse it.
Storage that stops answering, such as a network share that has gone
away, holds a thread of the parent, which can always end the run,
rather than FFmpeg waiting in the kernel where it cannot be killed.

The protocol has six messages of fixed form, each bounded and checked
before use; FFmpeg's side trusts the parent, and the parent trusts
nothing FFmpeg sends. Its specification:
https://github.com/kstenerud/ffmpeg-ffstreamer

It needs poll(), pthreads and sigtimedwait(), so it is built on Linux
and the BSDs that have it, not on macOS or OpenBSD, which lack
sigtimedwait().
"""),
    (fate, """fate/libavformat: add a test of the ffstreamer protocol

A thread serves as the parent over two pipes: a file read through and
by seeks, a parent that leaves mid-session, terms the protocol refuses,
and a second session on a pipe that is open already.
"""),
    (seek, """avformat/seek: treat ffstreamer as a local protocol

Its parent serves any offset, so a seek costs no more than reading on,
and reading forward in place of a seek read megabytes the demuxer never
needed.
"""),
]
FFTOOLS = [
    (demux, """fftools/ffmpeg_demux: fail when reading an input failed

Without -xerror, an error while demuxing is logged and the input ends
with status 0, so a run whose input could not be read through exits 0
with a truncated output. That suits a stream that may break; it does
not suit a file read for a measurement or an encode, which the caller
must not take as whole. -xerror is no way out, since it also ends the
run on a decode error that would have been concealed.

A read error left in the input's AVIOContext, an I/O failure rather
than a damaged stream, now fails the run, after the input ends and
while its streams are probed. An input whose reads failed is not
looped by -stream_loop either: it seeked back and failed again at
once, for ever, with -xerror or without.
"""),
    (signal_exit, """fftools/ffmpeg: exit non-zero when a signal interrupts opening an input

A signal while an input was opening ended ffmpeg with AVERROR_EXIT,
which never reached transcode() and was turned into exit status 0, so a
killed run looked like a successful one.
"""),
    (probe, """fftools/ffprobe: fail when reading the input failed

As in ffmpeg: a read error left in the input's AVIOContext was taken as
the input ending, and ffprobe printed partial results and exited 0.
"""),
]


def git(tree, *args):
    subprocess.run(["git", "-C", tree, *args], check=True)


def added_lines(diff, paths):
    """The lines a diff adds to the files in paths, in order, per file."""
    added, current = {}, None
    for line in diff.split("\n"):
        if line.startswith("+++ "):
            current = line[len("+++ b/"):] if line[len("+++ b/"):] in paths else None
        elif current and line.startswith("+"):
            added.setdefault(current, []).append(line[1:])
    return added


SERIES = {"fftools": FFTOOLS, "protocol": PROTOCOL, "all": FFTOOLS + PROTOCOL}
# The files each series changes that patches/ changes too, for jellyfin.
CHECKED = {"fftools": ["fftools/ffmpeg_demux.c", "fftools/ffmpeg.c", "fftools/ffprobe.c"],
           "protocol": ["libavformat/seek.c"]}
CHECKED["all"] = CHECKED["fftools"] + CHECKED["protocol"]


def check_against_patches(tree, base, paths):
    """The fftools and seek.c changes are written twice, in patches/ for jellyfin and here for
    FFmpeg; fails when what they add differs."""
    ours = added_lines(subprocess.run(["git", "-C", tree, "diff", base, "HEAD", "--", *paths],
                                      capture_output=True, text=True, check=True).stdout, paths)
    jellyfin = {}
    for name in ("9000-ffstreamer-protocol.patch", "9001-fftools-input-errors.patch"):
        jellyfin.update(added_lines(read(os.path.join("patches", name)), paths))
    for path in paths:
        if ours.get(path) != jellyfin.get(path):
            raise SystemExit(f"{path}: the series adds other lines than patches/ do; change both")


def main(argv):
    if len(argv) not in (1, 2) or (len(argv) == 2 and argv[1] not in SERIES):
        print(__doc__, file=sys.stderr)
        return 2
    tree = argv[0]
    name = argv[1] if len(argv) == 2 else "all"
    base = subprocess.run(["git", "-C", tree, "rev-parse", "HEAD"], capture_output=True, text=True,
                          check=True).stdout.strip()
    for change, message in SERIES[name]:
        change(tree)
        git(tree, "add", "-A")
        git(tree, "commit", "--quiet", "-m", message)
    check_against_patches(tree, base, CHECKED[name])
    print(f"the {name} series: {len(SERIES[name])} commits in {tree}, adding what patches/ add")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
