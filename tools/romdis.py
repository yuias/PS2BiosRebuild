#!/usr/bin/env python3
"""Disassemble a raw PS2 image, or a slice of one, at a chosen VMA.

llvm-objdump has no equivalent of GNU objdump's `-b binary`, so the image is
wrapped into an ELF via an `.incbin` stub and placed at the requested address
by an explicit link script. Everything then reads as ordinary MIPS with
correct branch and jump targets.

The PS2 has two CPUs and this tool serves both:

    --cpu iop   MIPS-I decode (R3000 derivative) -- the default
    --cpu ee    MIPS-III decode for the R5900

LLVM has no R5900 target, so `--cpu ee` decodes the MIPS-III subset and the
R5900-specific instructions (lq/sq, the multimedia set, mult/div register
variants) come out as raw `.word`s or misdecodes; treat those regions with
care and cross-check the raw bytes.

The default VMA is 0xBFC00000, where the ROM is mapped on both CPUs. Files
extracted from the ROM archive run from wherever their loader places them, so
give --vma accordingly:

    # the boot block, at the reset vector
    tools/romdis.py assets/SCPH-50000.bin --range 0xbfc00000 0xbfc00100

    # a file extracted with romdir.py, at its load address
    tools/romdis.py IOPBOOT --cpu iop --vma 0x800

`--range` is always given in run-time addresses, never file offsets, so it
reads the same as the addresses in the output.
"""

from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import sys
import tempfile

DEFAULT_VMA = 0xBFC00000
LLVM_SUFFIX = "-22"

CPU_MCPU = {"iop": "mips1", "ee": "mips3"}


def toolPath(name: str) -> str:
    for candidate in (f"{name}{LLVM_SUFFIX}", name):
        found = shutil.which(candidate)
        if found:
            return found
    sys.exit(f"romdis: cannot find {name}{LLVM_SUFFIX} or {name} on PATH")


def buildWrapperElf(blob: pathlib.Path, vma: int,
                    workdir: pathlib.Path) -> pathlib.Path:
    stub = workdir / "wrap.S"
    # as_posix(): a Windows path's backslashes would be read as escapes inside
    # the assembler string literal. Forward slashes work on every host.
    stub.write_text(
        '.section .image, "ax"\n'
        f'.incbin "{blob.resolve().as_posix()}"\n'
    )

    # An explicit output-section address keeps lld from reserving space for
    # the ELF headers ahead of the contents, which would shift every address.
    # The assembler's own MIPS metadata sections are discarded: they default to
    # address 0 and would otherwise collide with a --vma 0 image, which is
    # exactly the VMA a relocatable IOP module is linked at.
    script = workdir / "wrap.ld"
    script.write_text(
        "SECTIONS {\n"
        "  /DISCARD/ : { *(.MIPS.abiflags) *(.reginfo) }\n"
        f"  .image {vma:#x} : {{ *(.image) }}\n"
        "}\n"
    )

    obj = workdir / "wrap.o"
    elf = workdir / "wrap.elf"

    subprocess.run(
        [toolPath("clang"), "--target=mipsel-none-elf", "-march=mips1",
         "-c", str(stub), "-o", str(obj)],
        check=True, stderr=subprocess.DEVNULL,
    )
    subprocess.run(
        [toolPath("ld.lld"), "-T", str(script), "-o", str(elf), str(obj),
         "-e", hex(vma)],
        check=True, stderr=subprocess.DEVNULL,
    )
    return elf


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("image", type=pathlib.Path)
    parser.add_argument("--cpu", choices=CPU_MCPU, default="iop",
                        help="decode as this CPU (default iop)")
    parser.add_argument("--vma", type=lambda s: int(s, 0), default=DEFAULT_VMA,
                        help=f"address the code runs from (default {DEFAULT_VMA:#x})")
    parser.add_argument("--slice", nargs=2, metavar=("OFFSET", "LENGTH"),
                        type=lambda s: int(s, 0),
                        help="carve a sub-region out of the file first")
    parser.add_argument("--range", nargs=2, metavar=("START", "END"),
                        type=lambda s: int(s, 0),
                        help="limit output to this run-time address range")
    args = parser.parse_args()

    data = args.image.read_bytes()
    if args.slice:
        offset, length = args.slice
        if offset + length > len(data):
            sys.exit(f"romdis: slice {offset:#x}+{length:#x} exceeds "
                     f"{args.image} ({len(data):#x} bytes)")
        data = data[offset:offset + length]

    cmd_tail = []
    if args.range:
        start, end = args.range
        cmd_tail = [f"--start-address={start:#x}", f"--stop-address={end:#x}"]

    with tempfile.TemporaryDirectory(prefix="romdis-") as tmp:
        tmpdir = pathlib.Path(tmp)
        blob = tmpdir / "image.bin"
        blob.write_bytes(data)
        elf = buildWrapperElf(blob, args.vma, tmpdir)
        subprocess.run(
            [toolPath("llvm-objdump"), "-d", f"--mcpu={CPU_MCPU[args.cpu]}",
             "--no-show-raw-insn", "--print-imm-hex", str(elf), *cmd_tail],
            check=True,
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
