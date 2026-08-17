#!/usr/bin/env python3
"""Build a ROM archive from a manifest, per `docs/spec/01-rom-archive.md`.

The manifest is a text file listing one entry per line, in storage order:

    # name        source                extinfo
    RESET         build/reset.bin       @extinfo/reset.txt
    ROMDIR        =table                -
    EXTINFO       =extinfo              -
    ROMVER        build/romver.bin      -
    -             =pad:0x400            -

`=table` and `=extinfo` are generated: the entry table itself (ARC-4a) and the
concatenated per-entry metadata (ARC-6). `=pad:N` is N zero bytes, the padding
entry of ARC-5. Everything else names a file to embed.

The third column is the entry's EXTINFO source: `-` for none, or `@path` for a
file of directives, one per line:

    date 2003-02-06
    version 1.01
    comment System_Memory_Manager
    null

Because `=table` and `=extinfo` are sized by the manifest rather than by their
contents, the layout is solved before anything is written -- sizes feed offsets
(ARC-3) and the table records sizes, so the two are computed together.

    tools/mkromdir.py manifest.txt -o build/rom.bin
    tools/mkromdir.py manifest.txt -o out.bin --size 0x400000
"""

from __future__ import annotations

import argparse
import pathlib
import re
import struct
import sys

ENTRY_SIZE = 16
ALIGN = 16
NAME_LEN = 10

EXT_DATE = 0x01
EXT_VERSION = 0x02
EXT_COMMENT = 0x03
EXT_NULL = 0x7F


def alignUp(n: int) -> int:
    return (n + ALIGN - 1) & ~(ALIGN - 1)


class Entry:
    def __init__(self, name: str, source: str, extinfo: str):
        if len(name) > NAME_LEN:
            sys.exit(f"mkromdir: name {name!r} exceeds {NAME_LEN} characters")
        self.name = name
        self.source = source
        self.extinfo_spec = extinfo
        self.data = b""
        self.extinfo = b""


def buildExtinfo(path: pathlib.Path) -> bytes:
    """Assemble one entry's EXTINFO slice from a directive file (ARC-6b)."""
    out = bytearray()
    for lineno, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        kind, _, rest = line.partition(" ")
        rest = rest.strip()
        if kind == "date":
            try:
                year, month, day = rest.split("-")
            except ValueError:
                sys.exit(f"{path}:{lineno}: date wants YYYY-MM-DD")
            payload = bytes(int(x, 16) for x in
                            (day, month, year[2:4], year[0:2]))
            out += struct.pack("<HBB", 0, 4, EXT_DATE) + payload
        elif kind == "version":
            major, _, minor = rest.partition(".")
            value = (int(major, 16) << 8) | int(minor, 16)
            out += struct.pack("<HBB", value, 0, EXT_VERSION)
        elif kind == "comment":
            text = rest.encode("ascii") + b"\0"
            text += b"\0" * (-len(text) % 4)
            out += struct.pack("<HBB", 0, len(text), EXT_COMMENT) + text
        elif kind == "null":
            out += struct.pack("<HBB", 0, 0, EXT_NULL)
        else:
            sys.exit(f"{path}:{lineno}: unknown directive {kind!r}")
    return bytes(out)


def readManifest(path: pathlib.Path) -> list[Entry]:
    entries: list[Entry] = []
    for lineno, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        fields = line.split()
        if len(fields) < 2:
            sys.exit(f"{path}:{lineno}: expected 'name source [extinfo]'")
        name, source = fields[0], fields[1]
        extinfo = fields[2] if len(fields) > 2 else "-"
        entries.append(Entry(name, source, extinfo))
    if not entries:
        sys.exit(f"{path}: manifest is empty")
    return entries


def loadSources(entries: list[Entry], root: pathlib.Path) -> None:
    for e in entries:
        if e.extinfo_spec.startswith("@"):
            e.extinfo = buildExtinfo(root / e.extinfo_spec[1:])
        elif e.extinfo_spec.startswith("%"):
            # Verbatim slice: used to round-trip a reference archive's metadata
            # without going through the directive encoder.
            e.extinfo = (root / e.extinfo_spec[1:]).read_bytes()
        elif e.extinfo_spec != "-":
            sys.exit(f"mkromdir: {e.name}: extinfo must be '-', '@path' "
                     f"or '%path'")
        if e.source.startswith("="):
            continue  # generated below
        e.data = (root / e.source).read_bytes()


def solveLayout(entries: list[Entry]) -> None:
    """Fill in the generated entries, whose sizes depend on the entry count."""
    table_size = (len(entries) + 1) * ENTRY_SIZE   # ARC-4a
    extinfo_size = sum(len(e.extinfo) for e in entries)  # ARC-6a
    for e in entries:
        if e.source == "=table":
            e.data = b"\0" * table_size
        elif e.source == "=extinfo":
            e.data = b"\0" * extinfo_size
        elif e.source.startswith("=pad:"):
            e.data = b"\0" * int(e.source[5:], 0)
        elif e.source.startswith("="):
            sys.exit(f"mkromdir: {e.name}: unknown generator {e.source!r}")


def renderTable(entries: list[Entry]) -> bytes:
    out = bytearray()
    for e in entries:
        out += e.name.encode("ascii").ljust(NAME_LEN, b"\0")
        out += struct.pack("<HI", len(e.extinfo), len(e.data))
    out += b"\0" * ENTRY_SIZE                      # ARC-2 terminator
    return bytes(out)


def assemble(entries: list[Entry], image_size: int | None) -> bytes:
    table = renderTable(entries)
    extinfo = b"".join(e.extinfo for e in entries)
    for e in entries:
        if e.source == "=table":
            e.data = table
        elif e.source == "=extinfo":
            e.data = extinfo

    out = bytearray()
    for i, e in enumerate(entries):                # ARC-3
        out += e.data
        # ARC-8b: the final file's alignment padding is omitted, so an archive
        # ends at its last byte. With --size the tail is zero-filled anyway,
        # which is why this is invisible in a full-size image.
        if i != len(entries) - 1:
            out += b"\0" * (-len(e.data) % ALIGN)

    if image_size is not None:
        if len(out) > image_size:
            sys.exit(f"mkromdir: contents ({len(out):#x}) exceed the image "
                     f"size ({image_size:#x})")
        out += b"\0" * (image_size - len(out))     # ARC-1
    return bytes(out)


def entryOffsets(entries: list[Entry]) -> dict[str, int]:
    """Each entry's offset, implied by the aligned sizes of what precedes it
    (ARC-3) -- the same arithmetic `assemble()` uses to place them."""
    offsets: dict[str, int] = {}
    offset = 0
    for e in entries:
        offsets[e.name] = offset
        offset += alignUp(len(e.data))
    return offsets


def report(entries: list[Entry]) -> None:
    offsets = entryOffsets(entries)
    print(f"{'name':<10} {'offset':>9} {'size':>9} {'extinfo':>7}")
    for e in entries:
        print(f"{e.name:<10} {offsets[e.name]:#9x} {len(e.data):>9} "
              f"{len(e.extinfo):>7}")
    end = offsets[entries[-1].name] + alignUp(len(entries[-1].data)) \
        if entries else 0
    print(f"# {len(entries)} entries, contents end at {end:#x}")


def loadForOffset(entries: list[Entry], root: pathlib.Path, target: str) -> None:
    """Load what `entryOffsets` needs to place `target`, and nothing past it.

    An entry's own offset depends on the *data* sizes of every entry before
    it, but on the *extinfo* of every entry in the manifest -- `EXTINFO`
    concatenates all of them, wherever it sits (ARC-6a). So every entry's
    extinfo is read, but a non-generated entry's data only if it comes before
    `target`, which is what lets the offset be known before `target` itself,
    or anything after it, has been built.
    """
    try:
        target_index = next(i for i, e in enumerate(entries) if e.name == target)
    except StopIteration:
        sys.exit(f"mkromdir: no entry named {target!r} in the manifest")
    for i, e in enumerate(entries):
        if e.extinfo_spec.startswith("@"):
            e.extinfo = buildExtinfo(root / e.extinfo_spec[1:])
        elif e.extinfo_spec.startswith("%"):
            e.extinfo = (root / e.extinfo_spec[1:]).read_bytes()
        elif e.extinfo_spec != "-":
            sys.exit(f"mkromdir: {e.name}: extinfo must be '-', '@path' "
                     f"or '%path'")
        if i < target_index and not e.source.startswith("="):
            e.data = (root / e.source).read_bytes()


def parseDefsym(path: pathlib.Path) -> int:
    """The address a `--defsym=SYMBOL=0x...` response-file line recorded."""
    text = path.read_text().strip()
    match = re.search(r"=(0[xX][0-9a-fA-F]+)\s*$", text)
    if not match:
        sys.exit(f"mkromdir: {path}: no '--defsym=SYMBOL=0x...' line found "
                 f"in {text!r}")
    return int(match.group(1), 16)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("manifest", type=pathlib.Path)
    parser.add_argument("-o", "--output", type=pathlib.Path)
    parser.add_argument("--size", type=lambda s: int(s, 0),
                        help="pad the image to this size (e.g. 0x400000)")
    parser.add_argument("--list", action="store_true",
                        help="print the resulting layout")
    parser.add_argument("--offset-of", metavar="NAME",
                        help="print NAME's offset from the manifest and exit "
                             "-- needs neither NAME's own file nor any "
                             "entry's after it to exist")
    parser.add_argument("--base", type=lambda s: int(s, 0), default=0,
                        help="with --offset-of, the address the offset is "
                             "measured from")
    parser.add_argument("--defsym", metavar="SYMBOL",
                        help="with --offset-of, write "
                             "'--defsym=SYMBOL=<base>+<offset>' to --output "
                             "(or stdout) instead of the bare offset")
    parser.add_argument("--check", nargs=3, action="append", default=[],
                        metavar=("NAME", "BASE", "RSPFILE"),
                        help="after assembling, verify NAME's address (BASE "
                             "plus its offset in the finished image) equals "
                             "what RSPFILE's --defsym recorded, or fail")
    args = parser.parse_args()

    root = args.manifest.parent
    entries = readManifest(args.manifest)

    if args.offset_of:
        loadForOffset(entries, root, args.offset_of)
        solveLayout(entries)
        offset = entryOffsets(entries)[args.offset_of]
        if args.defsym:
            line = f"--defsym={args.defsym}={args.base + offset:#x}\n"
        else:
            line = f"{offset:#x}\n"
        if args.output:
            args.output.write_text(line)
        else:
            sys.stdout.write(line)
        return 0

    if not args.output:
        sys.exit("mkromdir: -o/--output is required unless --offset-of is")

    loadSources(entries, root)
    solveLayout(entries)
    image = assemble(entries, args.size)

    for name, base_text, rsp in args.check:
        base = int(base_text, 0)
        actual = base + entryOffsets(entries)[name]
        expected = parseDefsym(pathlib.Path(rsp))
        if actual != expected:
            sys.exit(f"mkromdir: {name} assembled at {actual:#x}, but "
                     f"{rsp} linked it at {expected:#x} -- the archive "
                     f"moved out from under its own link address")

    args.output.write_bytes(image)
    if args.list:
        report(entries)
    return 0


if __name__ == "__main__":
    sys.exit(main())
