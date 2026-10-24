#!/usr/bin/env python3
"""Rewrite patches/9000's new-file hunk for libavformat/fdproxy.c from src/ as it stands.

    sync_patch.py

src/libavformat/fdproxy.c is where the protocol is edited; this carries it into the patch, which
check_patch.py then confirms. The patch's other hunks are left as they are."""
import os, sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PATCH = os.path.join(HERE, "patches", "9000-fdproxy-protocol.patch")
SOURCE = os.path.join(HERE, "src", "libavformat", "fdproxy.c")
NAME = "libavformat/fdproxy.c"


def main():
    with open(PATCH, encoding="utf-8") as f:
        lines = f.read().split("\n")
    with open(SOURCE, encoding="utf-8") as f:
        source = f.read().split("\n")
    if source and source[-1] == "":
        source.pop()
    for at, line in enumerate(lines):
        if line == f"+++ b/{NAME}" and lines[at + 1].startswith("@@ -0,0 +1,"):
            count = int(lines[at + 1].split(",")[2].split()[0])
            lines[at + 1:at + 2 + count] = [f"@@ -0,0 +1,{len(source)} @@"] + ["+" + text for text in source]
            with open(PATCH, "w", encoding="utf-8") as f:
                f.write("\n".join(lines))
            print(f"{PATCH}: {NAME} is now src/'s {len(source)} lines")
            return 0
    print(f"{PATCH}: no new-file hunk for {NAME}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
