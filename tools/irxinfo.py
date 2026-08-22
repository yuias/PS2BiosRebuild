#!/usr/bin/env python3
"""Inspect an IOP module (IRX) extracted from the ROM archive.

Reports what the loader reads: the `.iopmod` metadata, the export and import
library tables, and the export ordinals. Export extent is derived from the
contiguous run of R_MIPS_32 relocations at the table's entry array, not by
scanning for a zero word -- in the stored file a module's slot 0 is a relocated
pointer to vaddr 0 and reads as zero, which a naive scan mistakes for the
terminator.

    tools/irxinfo.py <outdir>/SYSMEM
    tools/irxinfo.py <outdir>/INTRMANP --exports
    tools/irxinfo.py <outdir>/SYSMEM --dump-load <outdir>/SYSMEM.text

`--dump-load` writes the PT_LOAD segment so `tools/romdis.py --vma 0` can
disassemble it with addresses that read as module offsets.
"""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys

PT_LOAD = 1
PT_IOPMOD = 0x70000080
SHT_REL = 9
HI16_RUN_MAX = 8        # IRX-3a: the longest run of HI16 a loader holds
R_MIPS_32 = 2

EXPORT_MAGIC = 0x41C00000
IMPORT_MAGIC = 0x41E00000
TABLE_HEADER_SIZE = 0x14
STUB_SIZE = 8


class Irx:
    def __init__(self, path: pathlib.Path):
        self.data = path.read_bytes()
        if self.data[:4] != b"\x7fELF":
            sys.exit(f"irxinfo: {path} is not an ELF file")
        self.load_off, self.load_size, self.iopmod_off = self._segments()

    @classmethod
    def fromBytes(cls, data: bytes, what: str = "<module>") -> "Irx":
        """A module read from inside an archive rather than from a file."""
        irx = cls.__new__(cls)
        irx.data = data
        if data[:4] != b"\x7fELF":
            sys.exit(f"irxinfo: {what} is not an ELF file")
        irx.load_off, irx.load_size, irx.iopmod_off = irx._segments()
        return irx

    def _segments(self) -> tuple[int, int, int | None]:
        d = self.data
        (phoff,) = struct.unpack_from("<I", d, 28)
        entsize, count = struct.unpack_from("<HH", d, 42)
        load = iopmod = None
        for i in range(count):
            typ, off, _va, _pa, filesz, _memsz = struct.unpack_from(
                "<6I", d, phoff + i * entsize)
            if typ == PT_LOAD:
                load = (off, filesz)
            elif typ == PT_IOPMOD:
                iopmod = off
        if load is None:
            sys.exit("irxinfo: no PT_LOAD segment")
        return load[0], load[1], iopmod

    def moduleInfo(self) -> dict[str, object] | None:
        if self.iopmod_off is None:
            return None
        d, o = self.data, self.iopmod_off
        mi, entry, gp, text, data, bss = struct.unpack_from("<6I", d, o)
        (version,) = struct.unpack_from("<H", d, o + 24)
        name = d[o + 26:].split(b"\0", 1)[0].decode("ascii", "replace")
        return {"moduleinfo": mi, "entry": entry, "gp": gp, "text": text,
                "data": data, "bss": bss, "version": version, "name": name}

    def relocatedWords(self) -> set[int]:
        """Virtual addresses carrying an R_MIPS_32 fixup."""
        d = self.data
        (shoff,) = struct.unpack_from("<I", d, 32)
        entsize, count, _ = struct.unpack_from("<HHH", d, 46)
        out: set[int] = set()
        for i in range(count):
            fields = struct.unpack_from("<10I", d, shoff + i * entsize)
            typ, off, size = fields[1], fields[4], fields[5]
            if typ != SHT_REL:
                continue
            for k in range(size // 8):
                r_off, r_info = struct.unpack_from("<II", d, off + k * 8)
                if r_info & 0xFF == R_MIPS_32:
                    out.add(r_off)
        return out

    def tables(self) -> list[dict[str, object]]:
        d = self.data
        found = []
        for kind, magic in (("export", EXPORT_MAGIC), ("import", IMPORT_MAGIC)):
            at = self.load_off
            end = self.load_off + self.load_size
            needle = struct.pack("<I", magic)
            while True:
                i = d.find(needle, at)
                if i < 0 or i >= end:
                    break
                # A table header is word-aligned within the segment; the same
                # byte pattern occurring at an odd offset is data, not a table.
                if (i - self.load_off) % 4 != 0:
                    at = i + 1
                    continue
                version, flags = struct.unpack_from("<HH", d, i + 8)
                tag = d[i + 12:i + 20].split(b"\0")[0].decode("ascii", "replace")
                found.append({"kind": kind, "vaddr": i - self.load_off,
                              "version": version, "flags": flags, "tag": tag})
                at = i + 4
        found.sort(key=lambda t: t["vaddr"])
        return found

    def exportEntries(self, table_vaddr: int) -> list[int]:
        """Entry values, delimited by the contiguous R_MIPS_32 run."""
        relocated = self.relocatedWords()
        base = table_vaddr + TABLE_HEADER_SIZE
        n = 0
        while base + 4 * n in relocated:
            n += 1
        return [struct.unpack_from("<I", self.data,
                                   self.load_off + base + 4 * i)[0]
                for i in range(n)]

    def importOrdinals(self, table_vaddr: int) -> list[int]:
        """Ordinals from each stub's `addiu $v0, $zero, N`, to the terminator."""
        d = self.data
        at = self.load_off + table_vaddr + TABLE_HEADER_SIZE
        out = []
        while True:
            first, second = struct.unpack_from("<II", d, at)
            if first == 0:
                break
            out.append(second & 0xFFFF)
            at += STUB_SIZE
        return out


def checkModule(irx: Irx) -> list[str]:
    """Assert the static requirements of docs/spec/02-module-abi.md."""
    d = irx.data
    problems: list[str] = []

    def require(ok: bool, requirement: str, detail: str) -> None:
        if not ok:
            problems.append(f"{requirement}: {detail}")

    (e_type,) = struct.unpack_from("<H", d, 16)
    (e_machine,) = struct.unpack_from("<H", d, 18)
    require(e_type == 0xFF80, "IRX-1", f"e_type is {e_type:#06x}, want 0xff80")
    require(e_machine == 8, "IRX-1", f"e_machine is {e_machine}, want 8 (MIPS)")
    require(irx.iopmod_off is not None, "IRX-1", "no PT_IOPMOD segment")

    # IRX-3a: every HI16 is followed by the LO16 it pairs with. The reference's
    # modules pair one to one; ours may carry further LO16s that share a paired
    # high half, and short runs of HI16 that share one LO16 (`tools/mkirx.py`
    # checks they name the same address), so what is required here is the
    # pairing, not equal counts.
    (shoff,) = struct.unpack_from("<I", d, 32)
    entsize, count, _ = struct.unpack_from("<HHH", d, 46)
    hi = lo = orphans = 0
    for i in range(count):
        fields = struct.unpack_from("<10I", d, shoff + i * entsize)
        typ, off, size = fields[1], fields[4], fields[5]
        if typ != SHT_REL:
            continue
        kinds = [struct.unpack_from("<II", d, off + k * 8)[1] & 0xFF
                 for k in range(size // 8)]
        hi += kinds.count(5)
        lo += kinds.count(6)
        run = 0
        for kind in kinds:
            if kind == 5:
                run += 1
                if run > HI16_RUN_MAX:
                    orphans += 1
            elif kind == 6:
                run = 0
            else:
                orphans += run
                run = 0
        orphans += run
    require(orphans == 0, "IRX-3a", f"{orphans} HI16 not followed by a LO16")
    require(hi <= lo, "IRX-3a", f"{hi} HI16 against {lo} LO16")

    relocated = irx.relocatedWords()
    for t in irx.tables():
        where = f"{t['kind']} {t['tag']!r} @{t['vaddr']:#x}"
        require(t["vaddr"] % 4 == 0, "IRX-4b", f"{where} is not word-aligned")
        if t["kind"] == "export":
            entries = irx.exportEntries(t["vaddr"])
            require(bool(entries), "IRX-5a", f"{where} has no relocated entries")
            if not entries:
                continue
            after = irx.load_off + t["vaddr"] + TABLE_HEADER_SIZE + 4 * len(entries)
            (term,) = struct.unpack_from("<I", d, after)
            require(term == 0, "IRX-5b", f"{where} terminator is {term:#x}")
            require(len(entries) >= 2, "IRX-7",
                    f"{where} has {len(entries)} slots, fewer than the two "
                    f"reserved ones")
        else:
            at = irx.load_off + t["vaddr"] + TABLE_HEADER_SIZE
            index = 0
            while True:
                first, second = struct.unpack_from("<II", d, at)
                if first == 0:
                    break
                require(first == 0x03E00008, "IRX-8",
                        f"{where} stub {index} starts with {first:#x}")
                require((second >> 26) == 9, "IRX-8",
                        f"{where} stub {index} second word {second:#x} "
                        f"is not addiu")
                require((second & 0xFFFF) >= 2, "IRX-7a",
                        f"{where} imports reserved ordinal {second & 0xFFFF}")
                at += STUB_SIZE
                index += 1

    info = irx.moduleInfo()
    if info and info["moduleinfo"] not in (0, 0xFFFFFFFF):
        # IRX-2: the module-info pair is {name pointer, version}.
        at = irx.load_off + info["moduleinfo"]
        if 0 <= at + 8 <= len(d):
            name_ptr, version = struct.unpack_from("<II", d, at)
            require(version & 0xFFFF == info["version"], "IRX-2",
                    f"module-info version {version & 0xFFFF:#06x} disagrees "
                    f"with .iopmod {info['version']:#06x}")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("module", type=pathlib.Path)
    parser.add_argument("--exports", action="store_true",
                        help="list export slot values")
    parser.add_argument("--imports", action="store_true",
                        help="list imported ordinals per library")
    parser.add_argument("--dump-load", metavar="PATH", type=pathlib.Path,
                        help="write the PT_LOAD segment here")
    parser.add_argument("--check", action="store_true",
                        help="assert docs/spec/02-module-abi.md; exit 1 on any "
                             "failure")
    parser.add_argument("--quiet", action="store_true",
                        help="with --check, print only failures")
    args = parser.parse_args()

    irx = Irx(args.module)

    if args.check:
        problems = checkModule(irx)
        for p in problems:
            print(f"{args.module}: {p}", file=sys.stderr)
        if not problems and not args.quiet:
            print(f"{args.module}: ok")
        return 1 if problems else 0

    info = irx.moduleInfo()
    if info:
        print(f"name    {info['name']}")
        print(f"version {info['version'] >> 8:x}.{info['version'] & 0xFF:02x}")
        print(f"entry   {info['entry']:#x}   gp {info['gp']:#x}")
        print(f"sizes   text {info['text']:#x}  data {info['data']:#x}  "
              f"bss {info['bss']:#x}")

    for t in irx.tables():
        line = (f"{t['kind']:6} @{t['vaddr']:#07x} tag {t['tag']:<10} "
                f"v{t['version'] >> 8:x}.{t['version'] & 0xFF:02x} "
                f"flags {t['flags']:#x}")
        if t["kind"] == "export":
            entries = irx.exportEntries(t["vaddr"])
            print(f"{line} entries {len(entries)}")
            if args.exports:
                for i, e in enumerate(entries):
                    print(f"    {i:3d}  {e:#x}")
        else:
            ordinals = irx.importOrdinals(t["vaddr"])
            print(f"{line} stubs {len(ordinals)}")
            if args.imports:
                print(f"    ordinals {ordinals}")

    if args.dump_load:
        args.dump_load.write_bytes(
            irx.data[irx.load_off:irx.load_off + irx.load_size])
        print(f"wrote {irx.load_size:#x} bytes to {args.dump_load}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
