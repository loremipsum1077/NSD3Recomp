#!/usr/bin/env python3
"""Write a changed copy of one decomp source file into the build directory.

The decomp is never edited: it has to stay identical to the original game. A
deliberate change to the game ("mod") is made by compiling a patched copy of one
file in its place. Each replacement must match the source exactly once, so a mod
fails loudly when the decomp's text has moved on instead of silently doing nothing.

    patch_source.py <source> <output> --replace OLD NEW [--replace OLD NEW ...]
"""
import argparse
import pathlib
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source", type=pathlib.Path)
    ap.add_argument("output", type=pathlib.Path)
    ap.add_argument("--replace", nargs=2, action="append", default=[], metavar=("OLD", "NEW"))
    args = ap.parse_args()
    text = args.source.read_text()
    for old, new in args.replace:
        if text.count(old) != 1:
            sys.exit(f"{args.source}: expected exactly one '{old}', found {text.count(old)}")
        text = text.replace(old, new)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(f"// A patched copy of {args.source.name}, made by tools/patch_source.py. Do not edit.\n" + text)


if __name__ == "__main__":
    main()
