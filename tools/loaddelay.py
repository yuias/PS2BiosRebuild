#!/usr/bin/env python3
"""Find load-delay hazards in IOP code.

The R3000A has a load delay slot: the instruction after a load still sees the
register's old value. GNU as would pad for it, but our IOP assembly is written
under `.set noreorder`, so a load followed by a use of its register is a bug
that a lenient emulator forgives and hardware -- or an emulator that models
the pipeline -- does not (docs/implementation.md, "Four things that cost time").
A compiler targeting MIPS I schedules around it; hand-written assembly and any
`asm` it inlines does not.

    tools/loaddelay.py build/iopboot.elf build/sysmem.elf build/loadcore.elf
    tools/loaddelay.py build/reset.elf --range 0xBFC02000 0xBFC02130   # the IOP half

Reads an ELF's `.text` and reports every `lb/lbu/lh/lhu/lw/lwl/lwr` whose next
instruction reads the loaded register, with addresses and both instructions.
Branch delay slots count too: a load in a delay slot is followed by the branch
target, which this does not follow, so those are reported as `(delay slot)`
for a human to check. Exit status 1 when anything is found.
"""

import argparse
import pathlib
import struct
import subprocess
import sys

OBJDUMP = "llvm-objdump"

LOADS = {0x20: "lb", 0x21: "lh", 0x22: "lwl", 0x23: "lw", 0x24: "lbu",
         0x25: "lhu", 0x26: "lwr"}
BRANCH_OPS = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07}
STORE_OPS = {0x28, 0x29, 0x2A, 0x2B, 0x2E, 0x31, 0x39}


def readsOf(word: int) -> set[int]:
    """Registers an instruction reads, conservatively."""
    op = word >> 26
    rs = (word >> 21) & 31
    rt = (word >> 16) & 31
    if op == 0:                                        # SPECIAL
        funct = word & 0x3F
        if funct in (0x00, 0x02, 0x03):                # sll/srl/sra: rt
            return {rt}
        if funct in (0x08, 0x09):                      # jr/jalr: rs
            return {rs}
        if funct in (0x10, 0x12):                      # mfhi/mflo
            return set()
        if funct in (0x11, 0x13):                      # mthi/mtlo
            return {rs}
        if funct in (0x0C, 0x0D):                      # syscall/break
            return set()
        return {rs, rt}
    if op == 0x01:                                     # REGIMM: rs
        return {rs}
    if op in (0x02, 0x03):                             # j/jal
        return set()
    if op in (0x04, 0x05):                             # beq/bne
        return {rs, rt}
    if op in (0x06, 0x07):                             # blez/bgtz
        return {rs}
    if op == 0x10:                                     # COP0
        if rs in (0x04, 0x06):                         # mtc0/ctc0: rt
            return {rt}
        return set()
    if op in STORE_OPS:                                # stores: rs base, rt data
        return {rs, rt}
    if op in LOADS or op in (0x0F,):                   # loads: rs; lui: none
        return {rs} if op != 0x0F else set()
    return {rs}                                        # immediates: rs


def disassemble(elf: pathlib.Path) -> list[tuple[int, int, str]]:
    raw = subprocess.run(
        [OBJDUMP, "-d", "--triple=mipsel", str(elf)],
        capture_output=True, text=True, check=True).stdout
    lines = []
    for line in raw.splitlines():
        # "       0: 02 00 08 24  \taddiu\t$8, $zero, 0x2 <sym>"
        head, _, text = line.partition("\t")
        parts = head.split()
        if len(parts) != 5 or not parts[0].endswith(":"):
            continue
        try:
            address = int(parts[0][:-1], 16)
            word = int("".join(reversed(parts[1:5])), 16)
        except ValueError:
            continue
        lines.append((address, word, text.split(" <")[0].replace("\t", " ")))
    return lines


def main() -> int:
    global OBJDUMP
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("elf", type=pathlib.Path, nargs="+")
    parser.add_argument("--range", nargs=2, type=lambda v: int(v, 0),
                        metavar=("START", "END"),
                        help="only report addresses in [START, END): the boot "
                             "block holds both CPUs' code and only the IOP's "
                             "has a load delay slot")
    parser.add_argument("--objdump", default="llvm-objdump",
                        help="llvm-objdump to use (default: on PATH)")
    arguments = parser.parse_args()
    OBJDUMP = arguments.objdump
    found = 0
    for elf in arguments.elf:
        code = disassemble(elf)
        for k in range(len(code) - 1):
            address, word, text = code[k]
            if arguments.range and not arguments.range[0] <= address < arguments.range[1]:
                continue
            op = word >> 26
            if op not in LOADS:
                continue
            rt = (word >> 16) & 31
            if rt == 0:
                continue
            next_address, next_word, next_text = code[k + 1]
            if next_address != address + 4:
                continue
            note = ""
            if k > 0 and (code[k - 1][1] >> 26 in BRANCH_OPS
                          or (code[k - 1][1] >> 26 == 0
                              and code[k - 1][1] & 0x3F in (8, 9))):
                note = "  (delay slot: check the branch target too)"
            if rt in readsOf(next_word):
                found += 1
                print(f"{elf}: {address:#x}: {text}   ->   {next_text}{note}")
    if found:
        print(f"{found} load-delay hazard(s)")
        return 1
    print("no load-delay hazards")
    return 0


if __name__ == "__main__":
    sys.exit(main())
