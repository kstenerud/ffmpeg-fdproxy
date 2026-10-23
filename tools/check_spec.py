#!/usr/bin/env python3
"""Check that the grammar embedded in ffstreamer-specification.md is ffstreamer.dogma, as it must be.

    check_spec.py

It needs nothing but Python."""
import os, sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SPEC = os.path.join(HERE, "ffstreamer-specification.md")
GRAMMAR = os.path.join(HERE, "ffstreamer.dogma")


def main():
    with open(SPEC, encoding="utf-8") as f:
        spec = f.read()
    with open(GRAMMAR, encoding="utf-8") as f:
        grammar = f.read().rstrip("\n")
    start = spec.find("```dogma\n")
    end = spec.find("\n```", start + 1)
    if start < 0 or end < 0:
        print(f"{SPEC}: no ```dogma block", file=sys.stderr)
        return 1
    if spec[start + len("```dogma\n"):end] != grammar:
        print("the specification's grammar differs from ffstreamer.dogma: copy the file into it", file=sys.stderr)
        return 1
    print("the specification embeds ffstreamer.dogma as it stands")
    return 0


if __name__ == "__main__":
    sys.exit(main())
