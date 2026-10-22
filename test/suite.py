#!/usr/bin/env python3
"""The ffstreamer suite: every run shape read directly and through the protocol, compared,
then the failure paths through the reference parent.

    suite.py --bin DIR --media DIR

DIR holds ffmpeg and ffprobe built with the patches. The media are film.mkv and
film-moov-at-end.mp4, at least 5 minutes long and over 50 MiB, with video, a 5.1 track, a stereo
track and subtitles as streams 0 to 3: make_media.py makes them. It prints a table and exits non-zero when any case fails."""
import argparse, hashlib, json, os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def sha(data):
    return hashlib.sha256(data).hexdigest()[:16]


class Suite:
    def __init__(self, bindir, media):
        self.ffmpeg = os.path.join(bindir, "ffmpeg")
        self.ffprobe = os.path.join(bindir, "ffprobe")
        self.media = media
        self.results = []

    def parent(self, film, command, *options):
        argv = [sys.executable, os.path.join(HERE, "refparent.py"), "--file", film, *options, "--", *command]
        out = subprocess.run(argv, capture_output=True, text=True, timeout=600)
        try:
            return json.loads(out.stdout)
        except json.JSONDecodeError:
            return {"exit": None, "error": out.stderr[-800:]}

    def record(self, name, ok, detail=""):
        self.results.append((name, ok, detail))
        print(f"{'PASS' if ok else 'FAIL'}  {name}  {detail}", flush=True)

    # --- equivalence: the same output read directly and through the protocol ---------
    def same_output(self, name, film, tool, before, after):
        direct = subprocess.run([tool, "-v", "error", "-nostdin", *before, "-i", film, *after],
                                capture_output=True, timeout=600) if tool == self.ffmpeg else \
            subprocess.run([tool, "-v", "error", *before, "-i", film, *after], capture_output=True, timeout=600)
        command = [tool, "-v", "error", *(["-nostdin"] if tool == self.ffmpeg else []), *before, "{INPUT}", *after]
        through = self.parent(film, command, "--capture")
        ok = (direct.returncode == 0 and through.get("exit") == 0 and through.get("succeeded")
              and sha(direct.stdout) == through.get("stdout_sha"))
        self.record(name, ok, f"direct {sha(direct.stdout)} through {through.get('stdout_sha')} "
                              f"exit {through.get('exit')} done {through.get('done')} "
                              f"served {through.get('served')}/{through.get('size')}"
                              + ("" if ok else f" stderr {through.get('stderr_tail', '')[-200:]!r}"))

    def equivalence(self):
        mkv = os.path.join(self.media, "film.mkv")
        mp4 = os.path.join(self.media, "film-moov-at-end.mp4")
        probe = ["-print_format", "json", "-show_format", "-show_streams", "-show_chapters"]
        for film, label in ((mkv, "mkv"), (mp4, "mp4, index at the end")):
            # ffprobe names the file in its JSON; compare streams and chapters only.
            self.same_output(f"probe ({label})", film, self.ffprobe,
                             ["-print_format", "json", "-show_streams", "-show_chapters"], [])
            self.same_output(f"read-through: timeline ({label})", film, self.ffmpeg, [],
                             ["-map", "0:0", "-c", "copy", "-f", "framecrc", "-"])
            self.same_output(f"read-through: loudness frames ({label})", film, self.ffmpeg, [],
                             ["-map", "0:2", "-af", "ebur128=framelog=quiet,ashowinfo", "-f", "framecrc", "-"])
            self.same_output(f"subtitle extraction ({label})", film, self.ffmpeg, [],
                             ["-map", "0:3", "-c:s", "ass", "-f", "ass", "-"])
            self.same_output(f"chunk, copy from 2:30 ({label})", film, self.ffmpeg, ["-ss", "150", "-to", "156"],
                             ["-map", "0:0", "-c:v", "copy", "-f", "framemd5", "-"])
            self.same_output(f"chunk, sound from 2:29 ({label})", film, self.ffmpeg, ["-ss", "149", "-to", "156"],
                             ["-map", "0:2", "-c:a", "aac", "-b:a", "128k", "-f", "framemd5", "-"])
            self.same_output(f"chunk, encode from 2:30 ({label})", film, self.ffmpeg, ["-ss", "150", "-to", "153"],
                             ["-map", "0:0", "-c:v", "mpeg4", "-threads", "1", "-f", "framemd5", "-"])

    # --- failure paths --------------------------------------------------------------
    def failures(self):
        mkv = os.path.join(self.media, "film.mkv")
        read_through = [self.ffmpeg, "-v", "error", "-nostdin", "{INPUT}", "-map", "0:2", "-f", "null", "-"]

        def case(name, expect, *options, command=read_through):
            result = self.parent(mkv, command, *options)
            ok, why = expect(result)
            self.record(name, ok, why if not ok else
                        f"exit {result.get('exit')} done {result.get('done')} violation {result.get('violation')}")

        def ends(kind, exit_nonzero=True):
            def check(r):
                done = r.get("done") or {}
                ok = ((r.get("exit") != 0) == exit_nonzero and done.get("kind") == kind and not r.get("succeeded"))
                return ok, json.dumps(r)[:600]
            return check

        def killed_without_done(r):
            return (r.get("exit") not in (0, None) and r.get("done") is None and not r.get("succeeded")), json.dumps(r)[:600]

        def succeeds(r):
            return (r.get("succeeded") is True), json.dumps(r)[:600]

        case("a whole film read through succeeds", succeeds)
        case("one READ outstanding at a time also succeeds", succeeds, "--outstanding", "1")
        case("small blocks (64 KiB) succeed", succeeds, "--block", "65536")
        case("the parent's channel ends mid-film: CHANNEL, non-zero exit", ends("CHANNEL"), "--die-after", "20")
        case("a wrong sequence number: PROTOCOL, non-zero exit", ends("PROTOCOL"), "--wrong-number-at", "10")
        case("a short DATA: PROTOCOL, non-zero exit", ends("PROTOCOL"), "--short-at", "10")
        case("terms outside the capability: TERMS, non-zero exit", ends("TERMS"), "--block", str(64 << 20))
        case("no shared version: killed, no DONE", killed_without_done, "--versions", "2:3")
        case("the parent stalls and kills: no DONE, non-zero exit", killed_without_done,
             "--stall-after", "5", "--kill-after-ms", "3000")
        case("over its budget: killed", killed_without_done, "--budget", str(50 << 20))
        case("a signal while the parent holds a read: non-zero exit", lambda r: (
            r.get("exit") not in (0, None) and not r.get("succeeded"), json.dumps(r)[:600]),
             "--stall-after", "5", "--signal-after-ms", "1500", "--kill-after-ms", "8000")
        case("a signal during the opening exchange: non-zero exit", lambda r: (
            r.get("exit") not in (0, None) and not r.get("succeeded"), json.dumps(r)[:600]),
             "--signal-after-ms", "1", "--kill-after-ms", "8000")

    def report(self):
        failed = [r for r in self.results if not r[1]]
        print(f"\n{len(self.results) - len(failed)} of {len(self.results)} passed")
        return 1 if failed else 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", required=True)
    parser.add_argument("--media", required=True)
    parser.add_argument("--only", choices=["equivalence", "failures"])
    args = parser.parse_args()
    suite = Suite(args.bin, args.media)
    if args.only != "failures":
        suite.equivalence()
    if args.only != "equivalence":
        suite.failures()
    sys.exit(suite.report())


if __name__ == "__main__":
    main()
