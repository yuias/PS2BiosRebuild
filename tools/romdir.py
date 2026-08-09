#!/usr/bin/env python3
"""Parse the ROMDIR file table of a PS2 BIOS image.

The 4 MiB ROM is a flat file archive: files are stored back to back, each
padded to a 16-byte boundary, and a table named ROMDIR lists them in storage
order. The table has no pointer to it; the boot code finds it by scanning for
the first entry, named "RESET", and so does this tool. Each entry is 16 bytes:

    name          char[10]   NUL-padded; "-" marks an unnamed padding region
    extinfo_size  u16 LE     bytes this file owns inside the EXTINFO file
    size          u32 LE     file size in bytes (storage rounds up to 16)

A file's offset is not stored: it is the running sum of the aligned sizes of
every entry before it, starting at offset 0 (the RESET entry itself describes
the boot block at the top of the ROM).

EXTINFO is itself one of the files: per-file metadata records, sliced by the
running sum of extinfo_size in entry order. Each record is a 4-byte header
`value:u16 size:u8 type:u8` followed by `size` payload bytes:

    type 0x01  date     payload 4 bytes, little-endian BCD: DD MM YY YY
    type 0x02  version  value is the version, BCD (0x0101 -> 1.1)
    type 0x03  comment  NUL-terminated ASCII, padded to a 4-byte boundary
    type 0x7f  null     no payload

Typical uses, from the repository root:

    tools/romdir.py assets/SCPH-50000.bin --list
    tools/romdir.py assets/SCPH-50000.bin --romver
    tools/romdir.py assets/SCPH-50000.bin --extinfo ROMDIR
    tools/romdir.py assets/SCPH-50000.bin --extract out/rom50
    tools/romdir.py assets/SCPH-50000.bin --compare assets/SCPH-70000.bin
"""

from __future__ import annotations

import argparse
import dataclasses
import pathlib
import struct
import sys

ENTRY_SIZE = 16
ALIGN = 16


@dataclasses.dataclass
class Entry:
    name: str
    extinfo_size: int
    size: int
    offset: int          # storage offset in the image
    extinfo_offset: int  # offset of this file's slice inside EXTINFO


def alignUp(n: int) -> int:
    return (n + ALIGN - 1) & ~(ALIGN - 1)


def findTable(data: bytes) -> int:
    """Return the image offset of the ROMDIR table.

    The table starts with the RESET entry. "RESET" can also appear as string
    data, so each candidate is verified by the table's own invariant: entry
    index 1 must be ROMDIR and its size must cover a whole number of entries.
    """
    at = 0
    while True:
        at = data.find(b"RESET\0", at)
        if at < 0:
            sys.exit("romdir: no ROMDIR table found (no valid RESET entry)")
        second = data[at + ENTRY_SIZE:at + ENTRY_SIZE + 10]
        if second.rstrip(b"\0") == b"ROMDIR":
            (size,) = struct.unpack_from("<I", data, at + ENTRY_SIZE + 12)
            if size % ENTRY_SIZE == 0 and size > 0:
                return at
        at += 1


def parseEntries(data: bytes, table: int) -> list[Entry]:
    entries: list[Entry] = []
    offset = 0
    extinfo_offset = 0
    at = table
    while True:
        raw_name = data[at:at + 10]
        extinfo_size, size = struct.unpack_from("<HI", data, at + 10)
        name = raw_name.rstrip(b"\0").decode("ascii", errors="replace")
        if not name:
            break
        entries.append(Entry(name, extinfo_size, size, offset, extinfo_offset))
        offset += alignUp(size)
        extinfo_offset += extinfo_size
        at += ENTRY_SIZE
    return entries


def entryMap(entries: list[Entry]) -> dict[str, Entry]:
    # "-" padding entries are not addressable by name; keep the first of any
    # duplicate name, which matches lookup-by-scan order.
    out: dict[str, Entry] = {}
    for e in entries:
        if e.name != "-" and e.name not in out:
            out[e.name] = e
    return out


def checkConsistency(data: bytes, entries: list[Entry]) -> None:
    by_name = entryMap(entries)
    table = findTable(data)
    romdir = by_name.get("ROMDIR")
    if romdir is None:
        sys.exit("romdir: table has no ROMDIR entry")
    if romdir.size != len(entries) * ENTRY_SIZE + ENTRY_SIZE:
        # +ENTRY_SIZE: the table's storage includes its all-zero terminator.
        print(
            f"romdir: warning: ROMDIR size {romdir.size} != "
            f"{len(entries)} entries + terminator",
            file=sys.stderr,
        )
    if romdir.offset != table:
        print(
            f"romdir: warning: ROMDIR content offset {romdir.offset:#x} != "
            f"table location {table:#x}",
            file=sys.stderr,
        )
    extinfo = by_name.get("EXTINFO")
    total_ext = sum(e.extinfo_size for e in entries)
    if extinfo is not None and extinfo.size != total_ext:
        print(
            f"romdir: warning: EXTINFO size {extinfo.size} != "
            f"sum of extinfo_size {total_ext}",
            file=sys.stderr,
        )
    end = entries[-1].offset + alignUp(entries[-1].size)
    if end > len(data):
        sys.exit(f"romdir: entries run past the image end ({end:#x})")


def listEntries(entries: list[Entry]) -> None:
    print(f"{'name':<10} {'offset':>9} {'size':>9} {'extinfo':>7}")
    for e in entries:
        print(f"{e.name:<10} {e.offset:#9x} {e.size:>9} {e.extinfo_size:>7}")
    end = entries[-1].offset + alignUp(entries[-1].size)
    print(f"# {len(entries)} entries, contents end at {end:#x}")


def decodeRomver(data: bytes, entries: list[Entry]) -> None:
    e = entryMap(entries).get("ROMVER")
    if e is None:
        sys.exit("romdir: image has no ROMVER")
    text = data[e.offset:e.offset + e.size].rstrip(b"\n\0").decode("ascii")
    # VVVVRTYYYYMMDD: version BCD, region letter, console type letter, date.
    ver, region, ctype, date = text[0:4], text[4], text[5], text[6:14]
    print(f"raw     {text}")
    print(f"version {ver[0:2].lstrip('0') or '0'}.{ver[2:4]}")
    print(f"region  {region}")
    print(f"type    {ctype}")
    print(f"date    {date[0:4]}-{date[4:6]}-{date[6:8]}")


def decodeBcdDate(payload: bytes) -> str:
    day, month, y_lo, y_hi = payload
    return f"{y_hi:02x}{y_lo:02x}-{month:02x}-{day:02x}"


def dumpExtinfo(data: bytes, entries: list[Entry], which: str) -> None:
    extinfo = entryMap(entries).get("EXTINFO")
    if extinfo is None:
        sys.exit("romdir: image has no EXTINFO")
    for e in entries:
        if which != "all" and e.name != which:
            continue
        if e.extinfo_size == 0:
            if which != "all":
                print(f"{e.name}: no EXTINFO data")
            continue
        print(f"{e.name}:")
        at = extinfo.offset + e.extinfo_offset
        end = at + e.extinfo_size
        while at < end:
            value, size, rtype = struct.unpack_from("<HBB", data, at)
            payload = data[at + 4:at + 4 + size]
            if rtype == 0x01:
                print(f"  date    {decodeBcdDate(payload)}")
            elif rtype == 0x02:
                print(f"  version {value >> 8:x}.{value & 0xFF:02x}")
            elif rtype == 0x03:
                text = payload.split(b"\0", 1)[0].decode("ascii", "replace")
                print(f"  comment {text}")
            elif rtype == 0x7F:
                print("  null")
            else:
                print(f"  type={rtype:#04x} value={value:#06x} data={payload.hex()}")
            at += 4 + size


def extractFiles(
    data: bytes, entries: list[Entry], outdir: pathlib.Path, only: list[str]
) -> None:
    wanted = set(only)
    known = {e.name for e in entries}
    missing = wanted - known
    if missing:
        sys.exit(f"romdir: no such entry: {', '.join(sorted(missing))}")
    outdir.mkdir(parents=True, exist_ok=True)
    count = 0
    for e in entries:
        if e.name == "-" or (wanted and e.name not in wanted):
            continue
        (outdir / e.name).write_bytes(data[e.offset:e.offset + e.size])
        count += 1
    print(f"extracted {count} files to {outdir}")


def compareImages(data: bytes, other_path: pathlib.Path) -> None:
    other = other_path.read_bytes()
    a = parseEntries(data, findTable(data))
    b = parseEntries(other, findTable(other))
    map_a, map_b = entryMap(a), entryMap(b)
    for name in map_a:
        if name not in map_b:
            print(f"only in first:  {name}")
    for name in map_b:
        if name not in map_a:
            print(f"only in second: {name}")
    for name, ea in map_a.items():
        eb = map_b.get(name)
        if eb is None:
            continue
        same = data[ea.offset:ea.offset + ea.size] == \
            other[eb.offset:eb.offset + eb.size]
        if ea.size != eb.size:
            print(f"size differs:   {name} {ea.size} -> {eb.size}")
        elif not same:
            print(f"bytes differ:   {name} ({ea.size} bytes)")


def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("image", type=pathlib.Path, help="BIOS ROM image")
    parser.add_argument("--list", action="store_true",
                        help="list table entries with computed offsets (default)")
    parser.add_argument("--romver", action="store_true",
                        help="decode the ROMVER file")
    parser.add_argument("--extinfo", metavar="NAME",
                        help="decode EXTINFO records for NAME, or 'all'")
    parser.add_argument("--extract", metavar="DIR", type=pathlib.Path,
                        help="extract file contents into DIR")
    parser.add_argument("--only", metavar="NAME", action="append", default=[],
                        help="with --extract: limit to named entries")
    parser.add_argument("--compare", metavar="IMAGE", type=pathlib.Path,
                        help="diff the file table against another image")
    args = parser.parse_args()

    data = args.image.read_bytes()
    entries = parseEntries(data, findTable(data))
    checkConsistency(data, entries)

    acted = False
    if args.romver:
        decodeRomver(data, entries)
        acted = True
    if args.extinfo:
        dumpExtinfo(data, entries, args.extinfo)
        acted = True
    if args.extract:
        extractFiles(data, entries, args.extract, args.only)
        acted = True
    if args.compare:
        compareImages(data, args.compare)
        acted = True
    if args.list or not acted:
        listEntries(entries)


if __name__ == "__main__":
    main()
