#!/usr/bin/env python3
"""Check that patches/9000 adds src/libavformat/fdproxy.c exactly as it stands.

    check_patch.py

The source is edited in src/ and the patch regenerated from it; this fails when one changed without
the other. It needs nothing but Python."""
import os, sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PATCH = os.path.join(HERE, "patches", "9000-fdproxy-protocol.patch")
SOURCE = os.path.join(HERE, "src", "libavformat", "fdproxy.c")


def added_file(patch_lines, name):
    """The lines a new-file hunk for name adds, or None when the patch has no such hunk."""
    for at, line in enumerate(patch_lines):
        if line == f"+++ b/{name}" and patch_lines[at + 1].startswith("@@ -0,0 +1,"):
            count = int(patch_lines[at + 1].split(",")[2].split()[0])
            body = patch_lines[at + 2:at + 2 + count]
            if len(body) != count or not all(added.startswith("+") for added in body):
                return None
            return [added[1:] for added in body]
    return None


def main():
    with open(PATCH, encoding="utf-8") as f:
        patch_lines = f.read().split("\n")
    with open(SOURCE, encoding="utf-8") as f:
        source_lines = f.read().split("\n")
    if source_lines and source_lines[-1] == "":
        source_lines.pop()
    added = added_file(patch_lines, "libavformat/fdproxy.c")
    if added is None:
        print(f"{PATCH}: no new-file hunk for libavformat/fdproxy.c", file=sys.stderr)
        return 1
    if added != source_lines:
        first = next((n for n, (a, b) in enumerate(zip(added, source_lines)) if a != b),
                     min(len(added), len(source_lines)))
        print(f"the patch's fdproxy.c differs from src/ at line {first + 1}: regenerate the patch",
              file=sys.stderr)
        return 1
    print(f"the patch adds src/libavformat/fdproxy.c as it stands ({len(added)} lines)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
