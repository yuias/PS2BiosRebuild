#!/usr/bin/env python3
"""Turn a linked ELF into an IOP module, per `docs/spec/02-module-abi.md`.

The build links a module at virtual address 0 with `--emit-relocs`, which keeps
the fixups the linker would otherwise discard. This rewrites that ELF into the
form IRX-1 describes: two program headers, one carrying the `.iopmod` metadata
of IRX-2 and one the single relocatable load segment, with the `REL` sections of
IRX-3 preserved so a loader can rebase it.

    tools/mkirx.py build/sysmem.elf -o build/SYSMEM \\
        --name Memory_Manager --version 1.00

The name and version written here are the same ones the entry's `EXTINFO`
carries, because IRX-2a requires them to agree and one source should produce
both: `--extinfo` writes the matching directive file.
"""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys

ELF_HEADER_SIZE = 52
PHDR_SIZE = 32
SHDR_SIZE = 40

ET_IRX = 0xFF80                  # IRX-1: a processor-specific type
EM_MIPS = 8
PT_LOAD = 1
PT_IOPMOD = 0x70000080

SHT_PROGBITS, SHT_SYMTAB, SHT_STRTAB, SHT_NOBITS, SHT_REL = 1, 2, 3, 8, 9
SHF_ALLOC = 0x2

# IRX-3: exactly four relocation types occur in a module, and a loader that
# meets them all needs nothing else.
ALLOWED_RELOCATIONS = {2: "R_MIPS_32", 4: "R_MIPS_26", 5: "R_MIPS_HI16",
                       6: "R_MIPS_LO16"}
IOPMOD_FIXED = 26                # bytes before the NUL-terminated name
NO_MODULE_INFO = 0xFFFFFFFF


class Section:
    __slots__ = ("name", "type", "flags", "addr", "offset", "size", "entsize",
                 "info")

    def __init__(self, name: str, fields: tuple[int, ...]) -> None:
        self.name = name
        (_, self.type, self.flags, self.addr, self.offset, self.size,
         _link, self.info, _align, self.entsize) = fields


def readSections(data: bytes) -> list[Section]:
    shoff, = struct.unpack_from("<I", data, 32)
    entsize, count, strndx = struct.unpack_from("<HHH", data, 46)
    raw = [struct.unpack_from("<10I", data, shoff + i * entsize)
           for i in range(count)]
    names_off = raw[strndx][4]
    sections = []
    for fields in raw:
        end = data.index(b"\0", names_off + fields[0])
        sections.append(Section(
            data[names_off + fields[0]:end].decode("ascii"), fields))
    return sections


def loadImage(data: bytes, sections: list[Section]) -> tuple[bytes, int, int, int]:
    """The single load segment of IRX-1, and the three sizes of IRX-2.

    `text` is everything up to `.data` -- code and read-only data together --
    because the loader only needs the split to know how much to copy and how
    much to zero.
    """
    allocated = [s for s in sections if s.flags & SHF_ALLOC and s.size]
    if not allocated:
        sys.exit("mkirx: the module has no allocatable sections")
    allocated.sort(key=lambda s: s.addr)
    if allocated[0].addr != 0:
        sys.exit(f"mkirx: the load segment starts at {allocated[0].addr:#x}, "
                 f"not 0 -- IRX-1 wants it linked at zero")

    progbits = [s for s in allocated if s.type != SHT_NOBITS]
    nobits = [s for s in allocated if s.type == SHT_NOBITS]
    file_end = max(s.addr + s.size for s in progbits)

    image = bytearray(file_end)
    for section in progbits:
        image[section.addr:section.addr + section.size] = \
            data[section.offset:section.offset + section.size]

    data_sections = [s for s in progbits if s.name.startswith(".data")]
    data_start = min((s.addr for s in data_sections), default=file_end)
    # The bss runs from the end of the file's bytes to the end of the last
    # NOBITS section -- alignment padding between the two included, since a
    # loader zeroes and reserves `memsz - filesz` bytes and a module whose
    # memsz stopped short of its bss would have the next thing loaded on top
    # of its variables (which is how this line came to be written).
    bss = max((s.addr + s.size for s in nobits), default=file_end) - file_end
    return bytes(image), data_start, file_end - data_start, bss


def readRelocations(data: bytes, sections: list[Section], image: bytes) -> bytes:
    """The fixups, as IRX-3 stores them: `offset` then the bare type.

    The symbol index is dropped: a loader rebases in place and has no symbol
    table to consult, and leaving indices behind would point at a table this
    file does not carry.

    IRX-3a pairs each `HI16` with the `LO16` that follows it, and the
    reference loader takes "follows" literally: it resolves a `HI16` by
    reading the *next* entry's target word, without checking that entry's
    type. So the only portable stream is strictly alternating -- every `HI16`
    immediately followed by a `LO16` of its address -- and a run of two
    `HI16` misrelocates the first, silently, at load addresses that differ
    from the link address.

    Our object writer does not emit that stream. It groups a symbol's `HI16`
    together and leaves the `LO16` that pairs with them elsewhere: the same
    address materialised twice comes out as two adjacent `HI16` and two
    `LO16` further along, and an address kept live in a register across
    several accesses comes out as one `HI16` and several `LO16`. Neither is a
    codegen choice -- the instructions are already in the right order in the
    text -- so the fixups are re-ordered here rather than the code being
    written around the object writer.

    The re-ordering is sound because only `HI16` reads a neighbour: `LO16`,
    `R_MIPS_32` and `R_MIPS_26` each correct their own word from the load
    delta alone, so moving one changes nothing as long as it is still applied
    exactly once. Each `HI16` therefore takes its own `LO16`, chosen to have
    the symbol and low half of the one the object writer paired its run with,
    so the address every `HI16` resolves to is the address it resolved to
    before. A `HI16` with no `LO16` left to take is refused: nothing can make
    that one portable.
    """
    out = bytearray()
    paired: set[tuple[int, int]] = set()
    unpaired: list[tuple[int, int, str]] = []
    for section in sections:
        if section.type != SHT_REL:
            continue
        # Fixups into a section that is not loaded (debug records, say) are
        # not the module's business and would point into it after loading.
        if not sections[section.info].flags & SHF_ALLOC:
            continue
        entries = [struct.unpack_from("<II", data, section.offset + k * 8)
                   for k in range(section.size // 8)]
        parsed: list[tuple[int, int, int, int]] = []
        for offset, info in entries:
            kind = info & 0xFF
            if kind not in ALLOWED_RELOCATIONS:
                sys.exit(f"mkirx: {section.name} carries relocation type "
                         f"{kind}, which IRX-3 does not allow")
            low = struct.unpack_from("<H", image, offset)[0] \
                if kind in (5, 6) else 0
            parsed.append((offset, kind, info >> 8, low))

        # Which address each HI16 resolves to today: the low half of the LO16
        # that ends the run it belongs to.
        wanted: dict[int, tuple[int, int]] = {}
        run: list[int] = []
        for index, (offset, kind, symbol, low) in enumerate(parsed):
            if kind == 5:
                run.append(index)
                continue
            if kind == 6 and run:
                for hi in run:
                    if parsed[hi][2] != symbol:
                        sys.exit(f"mkirx: HI16 at {parsed[hi][0]:#x} in "
                                 f"{section.name} is not followed by a LO16 "
                                 "of its symbol (IRX-3a)")
                    wanted[hi] = (symbol, low)
            elif run:
                sys.exit(f"mkirx: HI16 at {parsed[run[0]][0]:#x} in "
                         f"{section.name} is not followed by a LO16 of its "
                         "symbol (IRX-3a)")
            run = []
        if run:
            sys.exit(f"mkirx: HI16 at {parsed[run[0]][0]:#x} in "
                     f"{section.name} is not followed by a LO16 of its "
                     "symbol (IRX-3a)")

        # Give each HI16 a LO16 of that address, one apiece.
        free: dict[tuple[int, int], list[int]] = {}
        for index, (offset, kind, symbol, low) in enumerate(parsed):
            if kind == 6:
                free.setdefault((symbol, low), []).append(index)
        partner: dict[int, int] = {}
        taken: set[int] = set()
        for index, (offset, kind, symbol, low) in enumerate(parsed):
            if kind != 5:
                continue
            candidates = free.get(wanted[index], [])
            if not candidates:
                sys.exit(f"mkirx: HI16 at {offset:#x} in {section.name} has "
                         "no LO16 of its own to pair with (IRX-3a)")
            chosen = candidates.pop(0)
            partner[index] = chosen
            taken.add(chosen)
            paired.add(wanted[index])

        for index, (offset, kind, symbol, low) in enumerate(parsed):
            if index in taken:
                continue                     # emitted after its HI16 instead
            out += struct.pack("<II", offset, kind)
            if kind == 5:
                mate = partner[index]
                out += struct.pack("<II", parsed[mate][0], 6)
            elif kind == 6:
                unpaired.append((symbol, low, f"{offset:#x} in {section.name}"))
    for symbol, low, where in unpaired:
        if (symbol, low) not in paired:
            sys.exit(f"mkirx: LO16 at {where} shares no HI16 with an address "
                     "it names, so its high half cannot be rebased (IRX-3a)")
    return bytes(out)


def symbolValue(data: bytes, sections: list[Section], wanted: str) -> int | None:
    for section in sections:
        if section.type != SHT_SYMTAB:
            continue
        strings = next(s for s in sections
                       if s.type == SHT_STRTAB and s.name == ".strtab")
        for k in range(section.size // 16):
            name_off, value, _size, _info, _other, _shndx = struct.unpack_from(
                "<IIIBBH", data, section.offset + k * 16)
            end = data.index(b"\0", strings.offset + name_off)
            if data[strings.offset + name_off:end].decode("ascii") == wanted:
                return value
    return None


def bcdVersion(text: str) -> int:
    major, _, minor = text.partition(".")
    if not (major.isdigit() and minor.isdigit()):
        sys.exit(f"mkirx: version {text!r} wants MM.mm, both decimal")
    return (int(major) << 8) | int(f"{int(minor):02d}", 16)


def build(elf: bytes, name: str, version: str) -> bytes:
    sections = readSections(elf)
    image, text_size, data_size, bss_size = loadImage(elf, sections)
    relocations = readRelocations(elf, sections, image)

    entry, = struct.unpack_from("<I", elf, 24)
    gp = symbolValue(elf, sections, "_gp") or 0
    module_info = symbolValue(elf, sections, "_module_info")

    iopmod = struct.pack("<6IH", NO_MODULE_INFO if module_info is None
                         else module_info, entry, gp, text_size, data_size,
                         bss_size, bcdVersion(version))
    iopmod += name.encode("ascii") + b"\0"
    iopmod += b"\0" * (-len(iopmod) % 4)

    # Lay the file out: headers, then .iopmod, the load segment, the fixups and
    # the section table that lets a reader find them.
    phoff = ELF_HEADER_SIZE
    iopmod_off = phoff + 2 * PHDR_SIZE
    load_off = iopmod_off + len(iopmod)
    rel_off = load_off + len(image)
    shoff = rel_off + len(relocations)

    out = bytearray()
    out += b"\x7fELF\x01\x01\x01" + b"\0" * 9
    out += struct.pack("<HHIIIIIHHHHHH", ET_IRX, EM_MIPS, 1, entry, phoff,
                       shoff, 0, ELF_HEADER_SIZE, PHDR_SIZE, 2, SHDR_SIZE,
                       2, 0)
    out += struct.pack("<8I", PT_IOPMOD, iopmod_off, 0, 0, len(iopmod),
                       len(iopmod), 0x4, 4)
    out += struct.pack("<8I", PT_LOAD, load_off, 0, 0, len(image),
                       len(image) + bss_size, 0x7, 16)
    out += iopmod
    out += image
    out += relocations
    out += b"\0" * SHDR_SIZE                       # the null section
    out += struct.pack("<10I", 0, SHT_REL, 0, 0, rel_off, len(relocations),
                       0, 0, 4, 8)
    return bytes(out)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("elf", type=pathlib.Path)
    parser.add_argument("-o", "--output", type=pathlib.Path, required=True)
    parser.add_argument("--name", required=True,
                        help="the module name, as .iopmod and EXTINFO carry it")
    parser.add_argument("--version", required=True, help="e.g. 1.00")
    parser.add_argument("--date", help="written to --extinfo, if given")
    parser.add_argument("--extinfo", type=pathlib.Path,
                        help="also write the matching EXTINFO directives "
                             "(ARC-6c, IRX-2a: one source for both)")
    arguments = parser.parse_args()

    elf = arguments.elf.read_bytes()
    if elf[:4] != b"\x7fELF":
        sys.exit(f"mkirx: {arguments.elf} is not an ELF file")
    arguments.output.write_bytes(
        build(elf, arguments.name, arguments.version))

    if arguments.extinfo:
        lines = [f"version {arguments.version}", f"comment {arguments.name}"]
        if arguments.date:
            lines.insert(0, f"date {arguments.date}")
        arguments.extinfo.write_text("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
