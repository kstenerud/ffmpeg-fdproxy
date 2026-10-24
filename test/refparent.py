#!/usr/bin/env python3
"""The reference parent of fdproxy (fdproxy-specification.md, version 1): it serves a file to an
ffmpeg or ffprobe over two pipes, checks every frame the child sends, and can misbehave
on purpose, to test the child's side.

    refparent.py [options] --file FILM -- ffmpeg <arguments with {INPUT} where the input goes>

{INPUT} is replaced by the fdproxy input arguments. It prints a JSON summary: the
child's exit status, its DONE, what was served, and any violation the parent saw."""
import argparse, fcntl, json, os, select, struct, subprocess, sys, threading, time

HELLO, CAPABILITY, REQUEST, READ, DATA, DONE = 1, 2, 3, 4, 5, 6
KINDS = {1: "TERMS", 2: "CHANNEL", 3: "PROTOCOL", 4: "INTERRUPTED"}
F_SETPIPE_SZ = 1031


class Violation(Exception):
    pass


class Parent:
    def __init__(self, args):
        self.args = args
        self.file = open(args.file, "rb")
        self.size = os.fstat(self.file.fileno()).st_size
        self.violation = None
        self.done = None
        self.reads = 0
        self.served = 0
        self.lock = threading.Lock()
        self.queue = []                 # READs taken, not yet answered: (sequence, offset, length)
        self.queued = threading.Condition(self.lock)
        self.closing = False
        self.expected = 0               # the sequence number expected next
        self.terms = None
        self.after_done_frames = 0
        self.trace = open(args.trace, "w", encoding="utf-8") if args.trace else None  # each READ answered

    # --- channel ---------------------------------------------------------------
    def make_pipes(self):
        # The parent's ends close-on-exec from creation; the child's ends handed by pass_fds.
        self.child_read, self.parent_write = os.pipe2(os.O_CLOEXEC)
        self.parent_read, self.child_write = os.pipe2(os.O_CLOEXEC)
        try:
            fcntl.fcntl(self.parent_write, F_SETPIPE_SZ, self.args.pipe_bytes)
        except OSError as error:
            print(f"refparent: pipe size refused ({error}); going on at the default", file=sys.stderr)

    def input_arguments(self):
        # --old-names: the protocol and its options as named before 2026-10-24, which this
        # repository's build still takes.
        prefix = "ffstreamer_" if self.args.old_names else ""
        protocol = "ffstreamer" if self.args.old_names else "fdproxy"
        return ["-protocol_whitelist", protocol,
                f"-{prefix}read_fd", str(self.child_read),
                f"-{prefix}write_fd", str(self.child_write),
                *([f"-{prefix}cache_blocks", str(self.args.cache_blocks)] if self.args.cache_blocks else []),
                "-i", f"{protocol}:"]

    def write(self, data):
        view = memoryview(data)
        while view:
            n = os.write(self.parent_write, view)
            view = view[n:]

    def frame(self, kind, sequence, body=b""):
        return struct.pack("<bbi", kind, sequence, len(body)) + body

    # --- reading the child's frames --------------------------------------------
    def read_exact(self, n, blocking=True):
        data = b""
        while len(data) < n:
            if not blocking:
                ready, _, _ = select.select([self.parent_read], [], [], 0)
                if not ready:
                    return None
            chunk = os.read(self.parent_read, n - len(data))
            if not chunk:
                if data:
                    raise Violation(f"the child's frame ended after {len(data)} of {n} bytes")
                return None
            data += chunk
        return data

    def next_frame(self, blocking=True):
        header = self.read_exact(6, blocking)
        if header is None:
            return None
        kind, sequence, length = struct.unpack("<bbi", header)
        if sequence < 0 or length < 0:
            raise Violation(f"a negative field in a header: number {sequence}, length {length}")
        if sequence != self.expected:
            raise Violation(f"number {sequence}, expected {self.expected}")
        bounds = {HELLO: (4, 4), CAPABILITY: (9, 9), READ: (12, 12), DONE: (9, 1040)}
        if kind not in bounds:
            raise Violation(f"a frame of type {kind} from the child")
        low, high = bounds[kind]
        if not low <= length <= high:
            raise Violation(f"type {kind} with length {length}")
        body = self.read_exact(length) if length else b""
        if body is None:
            raise Violation("the child's frame ended before its body")
        self.expected = (self.expected + 1) % 128
        return kind, sequence, body

    def parse_done(self, body):
        run, result = struct.unpack("<qb", body[:9])
        if result == 0:
            if len(body) != 9:
                raise Violation("DONE OK with a body of %d bytes" % len(body))
            return {"run": run, "result": "OK"}
        if result != 1 or len(body) < 16:
            raise Violation("DONE with result %d" % result)
        kind, code, message_length = struct.unpack("<bih", body[9:16])
        if kind not in KINDS or code < 0 or not 0 <= message_length <= 1024 or len(body) != 16 + message_length:
            raise Violation("a malformed DONE ERR")
        message = body[16:].decode("utf-8", "replace")
        message = "".join(ch if ch.isprintable() else repr(ch)[1:-1] for ch in message)
        return {"run": run, "result": "ERR", "kind": KINDS[kind], "code": code, "message": message}

    # --- the opening exchange --------------------------------------------------
    def opening(self):
        got = self.next_frame()
        if got is None:
            return  # the child ended before its HELLO: no terms, no DONE
        kind, sequence, body = got
        if kind != HELLO:
            raise Violation(f"the first frame was type {kind}")
        lowest, highest = struct.unpack("<hh", body)
        mine = tuple(int(v) for v in self.args.versions.split(":"))
        if lowest < 1 or highest < lowest or min(highest, mine[1]) < max(lowest, mine[0]):
            raise Violation(f"no shared version: the child {lowest}-{highest}, the parent {mine[0]}-{mine[1]}")
        version = min(highest, mine[1])
        if self.args.signal_at_hello:
            # FFmpeg has installed its signal handlers before it opens an input: the signal reaches
            # them, not the default action, and the protocol waits for this HELLO, interruptibly.
            self.child.send_signal(self.args.signal)
            try:
                self.child.wait(timeout=10)
                return
            except subprocess.TimeoutExpired:
                pass
        self.write(self.frame(HELLO, 0, struct.pack("<hh", *mine)))
        got = self.next_frame()
        if got is None:
            return
        kind, sequence, body = got
        if kind != CAPABILITY:
            raise Violation(f"type {kind} where CAPABILITY was expected")
        preferred, largest, outstanding = struct.unpack("<iib", body)
        if preferred < 1 or largest < preferred or not 1 <= outstanding <= 16:
            raise Violation(f"a CAPABILITY outside its rules: {preferred}, {largest}, {outstanding}")
        block = self.args.block or preferred
        longest = self.args.longest or block
        allowed = min(outstanding, self.args.outstanding)
        run = self.args.run
        # --size lies about the file's size, to test FFmpeg's side against a REQUEST past the bound.
        self.terms = (version, run, self.args.size if self.args.size is not None else self.size, block, longest,
                      allowed)
        self.write(self.frame(REQUEST, sequence, struct.pack("<hqqiib", *self.terms)))

    # --- serving -----------------------------------------------------------------
    def reader(self):
        """Takes the child's READs into the queue, and its DONE. Ends at end-of-file."""
        try:
            while True:
                got = self.next_frame()
                if got is None:
                    return
                kind, sequence, body = got
                if self.done is not None:
                    self.after_done_frames += 1
                    continue
                if kind == DONE:
                    self.done = self.parse_done(body)
                    with self.lock:
                        self.queue.clear()
                        self.queued.notify_all()
                    continue
                if kind != READ:
                    raise Violation(f"type {kind} after the opening exchange")
                offset, length = struct.unpack("<qi", body)
                _, _, size, _, longest, allowed = self.terms
                if not (0 <= offset <= size and 0 < length <= longest and length <= size - offset):
                    raise Violation(f"a READ outside the file: offset {offset}, length {length}")
                with self.lock:
                    if len(self.queue) >= allowed:
                        raise Violation(f"{len(self.queue) + 1} READs outstanding, {allowed} allowed")
                    self.queue.append((sequence, offset, length))
                    self.queued.notify_all()
        except Violation as violation:
            self.fail(str(violation))
        except OSError:
            return

    def writer(self):
        """Answers each READ in order: reads the file into its own memory, then writes."""
        answered = 0
        while True:
            with self.lock:
                while not self.queue and not self.closing:
                    self.queued.wait()
                if self.closing and not self.queue:
                    return
                sequence, offset, length = self.queue.pop(0)  # stops counting on taking it
            answered += 1
            if self.args.stall_after is not None and answered > self.args.stall_after:
                time.sleep(3600)
            if self.args.die_after is not None and answered > self.args.die_after:
                os.close(self.parent_write)
                return
            data = os.pread(self.file.fileno(), length, offset)
            if self.trace:
                self.trace.write(f"{offset} {length}\n")
            if self.args.delay_ms:
                time.sleep(self.args.delay_ms / 1000)
            if self.args.wrong_number_at == answered:
                sequence = (sequence + 1) % 128
            if self.args.short_at == answered:
                data = data[:-1]
            try:
                self.write(self.frame(DATA, sequence, data))
            except OSError:
                return
            self.reads += 1
            self.served += len(data)
            if self.args.budget and self.served > self.args.budget:
                self.fail(f"over the budget: {self.served} bytes served")
                return

    def fail(self, reason):
        if self.violation is None:
            self.violation = reason
        try:
            self.child.kill()
        except Exception:
            pass

    def run(self, command):
        self.make_pipes()
        command = [part for item in command for part in (self.input_arguments() if item == "{INPUT}" else [item])]
        self.child = subprocess.Popen(command, pass_fds=(self.child_read, self.child_write),
                                      stdout=subprocess.PIPE if self.args.capture else subprocess.DEVNULL,
                                      stderr=subprocess.PIPE)
        os.close(self.child_read)
        os.close(self.child_write)
        if self.args.kill_after_ms:
            threading.Timer(self.args.kill_after_ms / 1000, self.child.kill).start()
        if self.args.signal_after_ms:
            # ffmpeg's interrupt fires on the second signal once its run has begun, so --signals 2.
            for count in range(self.args.signals):
                threading.Timer(self.args.signal_after_ms / 1000 + count * 0.3,
                                lambda: self.child.send_signal(self.args.signal)).start()
        try:
            self.opening()
        except Violation as violation:
            self.fail(str(violation))
        except OSError as error:
            self.fail(f"the opening exchange broke: {error}")
        reader = threading.Thread(target=self.reader, daemon=True)
        writer = threading.Thread(target=self.writer, daemon=True)
        if self.terms:
            reader.start()
            writer.start()
        stdout, stderr = self.child.communicate()
        # After the exit: close the DATA side, let the one reader finish what is in the pipe.
        with self.lock:
            self.closing = True
            self.queue.clear()
            self.queued.notify_all()
        try:
            os.close(self.parent_write)
        except OSError:
            pass
        if self.terms:
            reader.join(timeout=10)
        else:
            try:
                got = self.next_frame(blocking=False)
                if got and got[0] == DONE:
                    self.done = self.parse_done(got[2])
            except (Violation, OSError):
                pass
        os.close(self.parent_read)
        succeeded = (self.child.returncode == 0 and self.done is not None and self.done.get("result") == "OK"
                     and self.violation is None and (self.terms is None or self.done.get("run") == self.terms[1]))
        return {
            "exit": self.child.returncode,
            "done": self.done,
            "reads": self.reads,
            "served": self.served,
            "size": self.size,
            "violation": self.violation,
            "after_done_frames": self.after_done_frames,
            "succeeded": succeeded,
            "stdout_sha": __import__("hashlib").sha256(stdout).hexdigest()[:16] if stdout else None,
            "stderr_tail": stderr.decode("utf-8", "replace")[-600:],
        }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--file", required=True)
    parser.add_argument("--versions", default="1:1")
    parser.add_argument("--run", type=int, default=7)
    parser.add_argument("--block", type=int, default=0)
    parser.add_argument("--longest", type=int, default=0)
    parser.add_argument("--outstanding", type=int, default=2)
    parser.add_argument("--cache-blocks", type=int, default=0)
    parser.add_argument("--pipe-bytes", type=int, default=1 << 20)
    parser.add_argument("--delay-ms", type=float, default=0)
    parser.add_argument("--stall-after", type=int)
    parser.add_argument("--die-after", type=int)
    parser.add_argument("--wrong-number-at", type=int)
    parser.add_argument("--short-at", type=int)
    parser.add_argument("--budget", type=int)
    parser.add_argument("--size", type=int, help="the file size REQUEST gives, in place of the file's")
    parser.add_argument("--kill-after-ms", type=int)
    parser.add_argument("--signal-after-ms", type=int)
    parser.add_argument("--signal", type=int, default=15)
    parser.add_argument("--signals", type=int, default=1, help="how many signals --signal-after-ms sends")
    parser.add_argument("--old-names", action="store_true", help="name the options as before 2026-10-24")
    parser.add_argument("--signal-at-hello", action="store_true", help="signal once FFmpeg's HELLO arrives")
    parser.add_argument("--capture", action="store_true", help="hash the child's stdout")
    parser.add_argument("--trace", help="write each READ answered to this file: offset and length, a line each")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command and args.command[0] == "--" else args.command
    print(json.dumps(Parent(args).run(command), indent=1))


if __name__ == "__main__":
    main()
