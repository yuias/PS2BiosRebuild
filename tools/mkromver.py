#!/usr/bin/env python3
"""Write the archive's `ROMVER` file, per `docs/spec/01-rom-archive.md` ARC-7.

The file is sixteen bytes: fourteen ASCII characters, a newline and a NUL.

    VVVVRTYYYYMMDD\\n\\0

`VVVV` is a BCD version, `R` a region letter, `T` a console-type letter. Tools
identify an image by this file, so the layout is fixed -- but the values are
this project's own and must not impersonate a retail ROM (ARC-7a).

    tools/mkromver.py --version 01.00 --region X --type P --date 2026-08-10 \\
        -o build/romver.bin
"""

from __future__ import annotations

import argparse
import pathlib
import sys

FILE_SIZE = 16


def bcdVersion(text: str) -> str:
    """`01.00` -> `0100`, rejecting anything a BCD field cannot hold."""
    major, _, minor = text.partition(".")
    if not (major.isdigit() and minor.isdigit()):
        sys.exit(f"mkromver: version {text!r} wants MM.mm, both decimal")
    if len(major) > 2 or len(minor) > 2:
        sys.exit(f"mkromver: version {text!r} does not fit two digits each")
    return f"{int(major):02d}{int(minor):02d}"


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--version", required=True, help="e.g. 01.00")
    parser.add_argument("--region", required=True, help="one letter")
    parser.add_argument("--type", required=True, help="one letter")
    parser.add_argument("--date", required=True, help="YYYY-MM-DD")
    parser.add_argument("-o", "--output", type=pathlib.Path, required=True)
    arguments = parser.parse_args()

    for name, value in (("region", arguments.region), ("type", arguments.type)):
        if len(value) != 1 or not value.isascii() or not value.isalpha():
            sys.exit(f"mkromver: {name} {value!r} must be one ASCII letter")
    parts = arguments.date.split("-")
    if len(parts) != 3 or [len(p) for p in parts] != [4, 2, 2]:
        sys.exit(f"mkromver: date {arguments.date!r} wants YYYY-MM-DD")

    text = (bcdVersion(arguments.version) + arguments.region.upper()
            + arguments.type.upper() + "".join(parts))
    if len(text) != 14:
        sys.exit(f"mkromver: {text!r} is {len(text)} characters, want 14")

    arguments.output.write_bytes(text.encode("ascii") + b"\n\0")
    if arguments.output.stat().st_size != FILE_SIZE:
        sys.exit(f"mkromver: wrote {arguments.output.stat().st_size} bytes, "
                 f"want {FILE_SIZE}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
