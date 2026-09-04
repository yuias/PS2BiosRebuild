#!/usr/bin/env python3
"""Offline model of the IOP module merge a title's UDNL performs on a reboot.

`docs/analysis/45-iop-reboot.md` §"UDNL: the merge core" reads the rule from
the bytes: `UDNL` always opens rom0 as an unnamed extra source alongside
whatever `argv` names, resolves an `IOPBTCONF` by scanning every open source
newest to oldest and taking the first one found (rom0 is the oldest source
and so is the fallback, which is what the retail case actually exercises,
since the disc's `IOPRP310.IMG` carries none), and then, name by name, walks
the same newest-to-oldest order keeping whichever candidate's version is
**strictly greater** -- so the newest candidate wins ties. This module gets
that arithmetic right in Python, over `tools/romdir.py`, before it is written
in C++ for the IOP (`src/iop/udnl.cpp`, not yet built).

**Which version field.** `docs/spec/01-rom-archive.md` ARC-6c ties an entry's
`EXTINFO` `0x02` version record to its `.iopmod` version, and
`docs/spec/02-module-abi.md` IRX-2a requires the same of an honestly-built
archive; checked directly against both `assets/SCPH-50000.bin` and the disc's
`IOPRP310.IMG`, `ROMDRV`'s `EXTINFO`/`.iopmod` pair agrees in both (1.03 on
rom0, 2.01 on the disc). The trap IRX-2b actually names is a *different* pair:
a module's own version and any library it **exports** are independent fields,
and `ROMDRV` is the concrete case -- rom0's `.iopmod`/`EXTINFO` read 1.03 while
its exported `romdrv` table reads 2.01. `tools/irxinfo.py` prints both, so a
reader who takes the export table's version for "the module's version" gets a
silently wrong answer for rom0's `ROMDRV` (the disc's does not expose the
mistake, since there both fields happen to agree). This tool reads the
archive's own `EXTINFO` record and never opens the ELF, which is right for
both reasons: it is the field the merge actually compares, and it sidesteps
the export-table trap entirely.

Sources are given **oldest-first / newest-last** on the command line, which
reads the same way the underlying command line does: rom0 is the several
-years-old reference kernel, always the oldest, so it is appended after
every named source rather than passed as one. A source token is a path to a
standalone `ROMDIR` archive, `rom0:NAME` to pull a nested archive (`ARC-8`)
out of `--rom0` itself, or `iso:NAME` to pull `MODULES/NAME` out of `--iso`
(a small embedded ISO9660 reader, so a source can be a disc image with
nothing else needed outside the repository's own tools).

    tools/iopmerge.py --rom0 assets/SCPH-50000.bin iso:IOPRP310.IMG \\
        --iso assets/SLPS-25918.iso
    tools/iopmerge.py --rom0 assets/SCPH-50000.bin rom0:EELOADCNF
    tools/iopmerge.py --check

`--check` asserts the one answer already known (`IOPRP310.IMG` merged over
rom0: 16 names from the disc, 13 from rom0) and separately *reports*, without
asserting, the `EELOADCNF` case -- that merge's numbers are not a settled
answer yet. It defaults `--rom0`/`--iso` to `assets/SCPH-50000.bin` and
`assets/SLPS-25918.iso` (overridable), neither committed
(`docs/clean-room-policy.md`), so the build only wires it into `ninja check`
when a checkout has both.
"""

from __future__ import annotations

import argparse
import dataclasses
import pathlib
import struct
import sys

import romdir  # reuse ARC-1..9 parsing rather than re-deriving it

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_ROM0 = REPO_ROOT / "assets" / "SCPH-50000.bin"
DEFAULT_ISO = REPO_ROOT / "assets" / "SLPS-25918.iso"

ISO_SECTOR = 2048

# BOOT-9 / analysis/45's richer UDNL grammar: '@' sets the base load address,
# '#' is an unused directive (BOOT-9b), and UDNL additionally recognises
# "!addr " / "!include " lines. None of a plain name, so none is a module.
BOOTCONFIG_SKIP_PREFIXES = ("@", "#", "!")


@dataclasses.dataclass
class Source:
    label: str
    data: bytes
    entries: list[romdir.Entry]


@dataclasses.dataclass
class Resolution:
    name: str
    source: str
    version: int


def loadArchive(label: str, data: bytes) -> Source:
    entries = romdir.parseEntries(data, romdir.findTable(data))
    return Source(label, data, entries)


def sliceNested(parent: Source, name: str) -> bytes:
    """Bytes of a nested archive (ARC-8): a named entry that is itself a
    complete ROMDIR table, such as rom0:EELOADCNF."""
    entry = romdir.entryMap(parent.entries).get(name)
    if entry is None:
        sys.exit(f"iopmerge: {parent.label} has no entry named {name}")
    return parent.data[entry.offset:entry.offset + entry.size]


def isoReadFile(iso_path: pathlib.Path, want: str) -> bytes:
    """`MODULES/<want>` out of a retail disc image.

    A minimal ISO9660 reader: single-session, non-interleaved files, the only
    shape a PS2 disc uses. Kept local so `--check` and `iso:NAME` sources need
    nothing outside the repository's own tools.
    """
    data = iso_path.read_bytes()

    def sector(lba: int, count: int = 1) -> bytes:
        return data[lba * ISO_SECTOR:(lba + count) * ISO_SECTOR]

    def recordFields(rec: bytes) -> tuple[int, int, int, str]:
        (lba,) = struct.unpack_from("<I", rec, 2)
        (size,) = struct.unpack_from("<I", rec, 10)
        flags = rec[25]
        namelen = rec[32]
        name = rec[33:33 + namelen].decode("latin1").split(";")[0]
        return lba, size, flags, name

    def listDir(lba: int, size: int) -> list[tuple[int, int, int, str]]:
        raw = sector(lba, (size + ISO_SECTOR - 1) // ISO_SECTOR)
        out: list[tuple[int, int, int, str]] = []
        i = 0
        while i < size:
            length = raw[i]
            if length == 0:
                i = (i // ISO_SECTOR + 1) * ISO_SECTOR
                continue
            out.append(recordFields(raw[i:i + length]))
            i += length
        return out

    pvd = sector(16)
    root_lba, root_size, _, _ = recordFields(pvd[156:156 + 34])
    for lba, size, flags, name in listDir(root_lba, root_size):
        if flags & 2 and name.upper() == "MODULES":
            for lba2, size2, flags2, name2 in listDir(lba, size):
                if not (flags2 & 2) and name2.upper() == want.upper():
                    return sector(lba2, (size2 + ISO_SECTOR - 1) // ISO_SECTOR)[:size2]
    sys.exit(f"iopmerge: {want} not found under MODULES/ in {iso_path}")


def resolveSource(token: str, rom0: Source, iso_path: pathlib.Path | None) -> Source:
    if token.startswith("rom0:"):
        name = token[len("rom0:"):]
        return loadArchive(f"rom0:{name}", sliceNested(rom0, name))
    if token.startswith("iso:"):
        if iso_path is None:
            sys.exit("iopmerge: an 'iso:NAME' source needs --iso")
        name = token[len("iso:"):]
        return loadArchive(f"{iso_path.name}:{name}", isoReadFile(iso_path, name))
    path = pathlib.Path(token)
    return loadArchive(path.name, path.read_bytes())


def entryVersion(source: Source, name: str) -> int | None:
    """The EXTINFO 0x02 version record for `name` in `source`, or None if
    `source` carries no entry of that name at all -- not a candidate.

    A few `IOPBTCONF` names are resources rather than modules (`IGREETING` and
    `SIFINIT` carry only a date record, ARC-6b) and so have no version record;
    those still resolve as version 0 rather than dropping out, which only
    matters when they are the sole candidate, as in the retail case -- the
    rule as read from the bytes says nothing about this, since every real
    module satisfies ARC-6c and always has one.
    """
    by_name = romdir.entryMap(source.entries)
    entry = by_name.get(name)
    if entry is None:
        return None
    extinfo = by_name.get("EXTINFO")
    if extinfo is None or entry.extinfo_size == 0:
        return 0
    at = extinfo.offset + entry.extinfo_offset
    end = at + entry.extinfo_size
    while at < end:
        value, size, rtype = struct.unpack_from("<HBB", source.data, at)
        if rtype == 0x02:
            return value
        at += 4 + size
    return 0


def parseBootConfig(text: bytes) -> list[str]:
    names = []
    for token in text.decode("ascii", "replace").split():
        if token.startswith(BOOTCONFIG_SKIP_PREFIXES):
            continue
        names.append(token)
    return names


def findBootConfig(newest_first: list[Source]) -> tuple[Source, list[str]]:
    """The order-defining IOPBTCONF: the first one found scanning sources
    newest to oldest. rom0 is always the oldest and so the fallback -- the
    retail case, since IOPRP310.IMG carries no IOPBTCONF of its own."""
    for source in newest_first:
        entry = romdir.entryMap(source.entries).get("IOPBTCONF")
        if entry is None:
            continue
        return source, parseBootConfig(
            source.data[entry.offset:entry.offset + entry.size])
    sys.exit("iopmerge: no source (including rom0) carries an IOPBTCONF")


def resolveVersions(names: list[str], newest_first: list[Source]) -> list[Resolution]:
    """Per name, walk newest to oldest; a candidate replaces the champion only
    if strictly greater, so the newest candidate wins a tie."""
    out = []
    for name in names:
        champion: tuple[str, int] | None = None
        for source in newest_first:
            version = entryVersion(source, name)
            if version is None:
                continue
            if champion is None or version > champion[1]:
                champion = (source.label, version)
        if champion is None:
            sys.exit(f"iopmerge: {name} resolves against no source "
                      f"(BOOT-9a would abort the real boot here)")
        out.append(Resolution(name, champion[0], champion[1]))
    return out


def merge(sources: list[Source], rom0: Source) -> tuple[Source, list[Resolution]]:
    """`sources` is oldest-first / newest-last; rom0 is always the oldest and
    is appended, unnamed, to complete the pool -- the merge rule itself."""
    newest_first = list(reversed(sources)) + [rom0]
    order_source, names = findBootConfig(newest_first)
    resolutions = resolveVersions(names, newest_first)
    return order_source, resolutions


def formatVersion(version: int) -> str:
    return f"{version >> 8:x}.{version & 0xFF:02x}"


def printMerge(order_source: Source, resolutions: list[Resolution]) -> None:
    print(f"order from {order_source.label} ({len(resolutions)} names)")
    for r in resolutions:
        print(f"  {r.name:<10} {formatVersion(r.version):>6}  <- {r.source}")


def runCheck(rom0_path: pathlib.Path, iso_path: pathlib.Path) -> int:
    rom0 = loadArchive("rom0", rom0_path.read_bytes())

    ioprp = loadArchive("IOPRP310.IMG", isoReadFile(iso_path, "IOPRP310.IMG"))
    order_source, resolutions = merge([ioprp], rom0)

    problems: list[str] = []
    if order_source.label != "rom0":
        problems.append(f"order came from {order_source.label}, want rom0 "
                         f"(IOPRP310.IMG carries no IOPBTCONF)")
    from_disc = [r for r in resolutions if r.source == "IOPRP310.IMG"]
    from_rom0 = [r for r in resolutions if r.source == "rom0"]
    if len(resolutions) != 29:
        problems.append(f"{len(resolutions)} names total, want 29")
    if len(from_disc) != 16:
        problems.append(f"{len(from_disc)} names from the disc, want 16")
    if len(from_rom0) != 13:
        problems.append(f"{len(from_rom0)} names from rom0, want 13")

    by_name = {r.name: r for r in resolutions}
    spot_checks = {
        "SYSMEM": 0x0203, "LOADCORE": 0x0206,
        "THREADMAN": 0x0203, "CDVDFSV": 0x0226,
    }
    for name, version in spot_checks.items():
        r = by_name.get(name)
        if r is None or r.source != "IOPRP310.IMG" or r.version != version:
            got = f"{r.source} {formatVersion(r.version)}" if r else "missing"
            problems.append(f"{name}: got {got}, want IOPRP310.IMG "
                             f"{formatVersion(version)}")

    print(f"IOPRP310.IMG over rom0: {len(from_disc)} disc, {len(from_rom0)} "
          f"rom0, {len(resolutions)} total")
    for p in problems:
        print(f"iopmerge: FAIL {p}", file=sys.stderr)

    # EELOADCNF's answer is not known yet (see the module docstring): report
    # it, do not assert it.
    eeloadcnf = loadArchive("rom0:EELOADCNF", sliceNested(rom0, "EELOADCNF"))
    ee_order_source, ee_resolutions = merge([eeloadcnf], rom0)
    by_source: dict[str, int] = {}
    for r in ee_resolutions:
        by_source[r.source] = by_source.get(r.source, 0) + 1
    print(f"EELOADCNF over rom0 (unasserted): order from "
          f"{ee_order_source.label}, {len(ee_resolutions)} total, "
          f"by source {by_source}")

    if problems:
        print(f"iopmerge: {len(problems)} check(s) failed", file=sys.stderr)
        return 1
    print("iopmerge: ok")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--rom0", type=pathlib.Path,
                         help="the reference ROM archive; always an implicit, "
                              "unnamed source")
    parser.add_argument("--iso", type=pathlib.Path,
                         help="a disc image, to resolve 'iso:NAME' source "
                              "tokens against its MODULES/ directory")
    parser.add_argument("sources", nargs="*",
                         help="named source archives, oldest-first / "
                              "newest-last: a file path, or 'rom0:NAME' / "
                              "'iso:NAME'")
    parser.add_argument("--check", action="store_true",
                         help="assert the known IOPRP310.IMG-over-rom0 "
                              "answer, report EELOADCNF unasserted; defaults "
                              f"--rom0/--iso to {DEFAULT_ROM0.relative_to(REPO_ROOT)} "
                              f"and {DEFAULT_ISO.relative_to(REPO_ROOT)}")
    args = parser.parse_args()

    if args.check:
        rom0_path = args.rom0 or DEFAULT_ROM0
        iso_path = args.iso or DEFAULT_ISO
        if not rom0_path.exists() or not iso_path.exists():
            sys.exit(f"iopmerge: --check needs {rom0_path} and {iso_path}, "
                      f"neither committed (docs/clean-room-policy.md)")
        return runCheck(rom0_path, iso_path)

    if args.rom0 is None:
        sys.exit("iopmerge: --rom0 is required")
    if not args.sources:
        sys.exit("iopmerge: at least one source is required")

    rom0 = loadArchive("rom0", args.rom0.read_bytes())
    sources = [resolveSource(token, rom0, args.iso) for token in args.sources]
    order_source, resolutions = merge(sources, rom0)
    printMerge(order_source, resolutions)
    return 0


if __name__ == "__main__":
    sys.exit(main())
