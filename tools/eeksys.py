#!/usr/bin/env python3
"""Inspect the EE kernel's exception and syscall tables.

`docs/analysis/14-ee-kernel-syscalls.md` locates both tables inside the `KERNEL`
file, which runs at physical 0 -- so a file offset is its run-time address minus
`0x80000000`. This reports them, and groups the syscall slots by target so the
shared and aliased ones stand out.

    tools/romdir.py assets/SCPH-50000.bin --extract <outdir>
    tools/eeksys.py <outdir>/KERNEL
    tools/eeksys.py <outdir>/KERNEL --slot 0x64
"""

from __future__ import annotations

import argparse
import collections
import pathlib
import struct
import sys

KSEG0 = 0x80000000
EXCEPTION_TABLE = 0x15340        # indexed by Cause & 0x7C, so already scaled
SYSCALL_TABLE = 0x14F40
SYSCALL_COUNT = 0x7D             # slots 0x00 .. 0x7C

EXCEPTION_NAMES = {
    0: "Int", 1: "Mod", 2: "TLBL", 3: "TLBS", 4: "AdEL", 5: "AdES",
    6: "IBE", 7: "DBE", 8: "Sys", 9: "Bp", 10: "RI", 11: "CpU",
    12: "Ov", 13: "Tr",
}


def word(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def segment(address: int) -> str:
    return {0x8: "kseg0", 0x9: "kseg0", 0xA: "kseg1", 0xB: "kseg1"}.get(
        address >> 28, "?")


def readSyscalls(data: bytes) -> list[int]:
    return [word(data, SYSCALL_TABLE + i * 4) for i in range(SYSCALL_COUNT)]


def reportExceptions(data: bytes) -> None:
    print("exception handlers (table at "
          f"{KSEG0 + EXCEPTION_TABLE:#010x}):")
    for code in range(14):
        target = word(data, EXCEPTION_TABLE + code * 4)
        print(f"   {code:2d} {EXCEPTION_NAMES.get(code, ''):<5} {target:#010x}")


def reportSyscalls(data: bytes, verbose: bool) -> None:
    slots = readSyscalls(data)
    by_target = collections.defaultdict(list)
    for index, target in enumerate(slots):
        by_target[target].append(index)

    span = len(data)
    outside = [(i, t) for i, t in enumerate(slots)
               if (t & 0x1FFFFFFF) >= span]
    print(f"\nsyscall table at {KSEG0 + SYSCALL_TABLE:#010x}: "
          f"{len(slots)} slots, {len(by_target)} distinct targets, "
          f"{slots.count(0)} null")
    if outside:
        print(f"   targets outside the image: {[(hex(i), hex(t)) for i, t in outside]}")

    uncached = [(i, t) for i, t in enumerate(slots) if segment(t) == "kseg1"]
    if uncached:
        print("   published uncached (kseg1): "
              + ", ".join(f"{i:#04x}->{t:#010x}" for i, t in uncached))

    shared = {t: idx for t, idx in by_target.items() if len(idx) > 1}
    print(f"   shared targets: {len(shared)}")
    for target, indexes in sorted(shared.items(), key=lambda kv: -len(kv[1])):
        print(f"     {target:#010x}  <- {len(indexes)} slots: "
              + ", ".join(f"{i:#04x}" for i in indexes))

    if verbose:
        print("\n   all slots:")
        for index, target in enumerate(slots):
            note = "  shared" if len(by_target[target]) > 1 else ""
            print(f"     {index:#04x}  {target:#010x}  {segment(target)}{note}")


# What docs/spec/04-ee-kernel.md requires of a KERNEL image.
VECTOR_OFFSETS = (0x000, 0x080, 0x100, 0x180, 0x200)
UNDEFINED_SLOTS = {0x00, 0x03, 0x08, 0x3F, 0x7C} | set(range(0x54, 0x5C))
KSEG1_SLOTS = {0x60, 0x61, 0x62}
ALIAS_BLOCKS = ((range(0x14, 0x1A), 6), (range(0x63, 0x67), 4))
ALIAS_PAIRS = ((0x30, 0x31), (0x35, 0x36), (0x37, 0x38),
               (0x45, 0x46), (0x47, 0x48))


def checkKernel(data: bytes) -> list[str]:
    """Judge a KERNEL image against docs/spec/04-ee-kernel.md."""
    problems: list[str] = []

    def require(ok: bool, requirement: str, detail: str) -> None:
        if not ok:
            problems.append(f"{requirement}: {detail}")

    # EE-5: every vector offset carries code.
    for off in VECTOR_OFFSETS:
        require(word(data, off) != 0, "EE-5",
                f"vector {off:#05x} is empty")
    require(data[0x000:0x028] == data[0x180:0x1A8], "EE-5a",
            "vectors 0x000 and 0x180 differ")

    # EE-6: only ExcCode 8 has its own handler.
    handlers = [word(data, EXCEPTION_TABLE + c * 4) for c in range(14)]
    others = {h for c, h in enumerate(handlers) if c != 8}
    require(len(others) == 1, "EE-6b",
            f"exception codes other than 8 use {len(others)} handlers, want 1")
    require(handlers[8] not in others, "EE-6b",
            "ExcCode 8 shares the common handler")

    slots = readSyscalls(data)
    require(len(slots) == SYSCALL_COUNT, "EE-8a",
            f"{len(slots)} slots, want {SYSCALL_COUNT}")
    null = [i for i, t in enumerate(slots) if t == 0]
    require(not null, "EE-8b",
            f"null slots: {[hex(i) for i in null]}")

    # EE-8c: the alias blocks and pairs.
    for block, offset in ALIAS_BLOCKS:
        for i in block:
            require(slots[i] == slots[i + offset], "EE-8c",
                    f"slot {i:#04x} and {i + offset:#04x} differ "
                    f"({slots[i]:#010x} vs {slots[i + offset]:#010x})")
    for a, b in ALIAS_PAIRS:
        require(slots[a] == slots[b], "EE-8c",
                f"slot {a:#04x} and {b:#04x} differ")

    # EE-8d: the undefined slots all share one reporter, and nothing else does.
    reporters = {slots[i] for i in UNDEFINED_SLOTS}
    require(len(reporters) == 1, "EE-8d",
            f"the undefined slots use {len(reporters)} handlers, want 1")
    if len(reporters) == 1:
        reporter = reporters.pop()
        extra = {i for i, t in enumerate(slots)
                 if t == reporter} - UNDEFINED_SLOTS
        require(not extra, "EE-8d",
                f"slots {[hex(i) for i in sorted(extra)]} also use the "
                f"undefined reporter")

    # EE-8e: exactly three slots are published uncached, and they are these.
    uncached = {i for i, t in enumerate(slots) if segment(t) == "kseg1"}
    require(uncached == KSEG1_SLOTS, "EE-8e",
            f"kseg1 slots {sorted(hex(i) for i in uncached)} != "
            f"{sorted(hex(i) for i in KSEG1_SLOTS)}")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("kernel", type=pathlib.Path)
    parser.add_argument("--all", action="store_true",
                        help="list every syscall slot, not just shared ones")
    parser.add_argument("--slot", type=lambda s: int(s, 0),
                        help="report one slot's target and exit")
    parser.add_argument("--check", action="store_true",
                        help="judge against docs/spec/04-ee-kernel.md; "
                             "exit 1 on any failure")
    args = parser.parse_args()

    data = args.kernel.read_bytes()
    if len(data) < SYSCALL_TABLE + SYSCALL_COUNT * 4:
        sys.exit(f"eeksys: {args.kernel} is too small to be an EE kernel")

    if args.check:
        problems = checkKernel(data)
        for problem in problems:
            print(f"{args.kernel}: {problem}", file=sys.stderr)
        if problems:
            print(f"\n{args.kernel}: {len(problems)} failure(s)", file=sys.stderr)
            return 1
        print(f"{args.kernel}: ok -- {SYSCALL_COUNT} syscall slots, "
              f"vectors and exception dispatch as specified")
        return 0

    if args.slot is not None:
        if not 0 <= args.slot < SYSCALL_COUNT:
            sys.exit(f"eeksys: slot {args.slot:#x} is outside 0..{SYSCALL_COUNT - 1:#x}")
        target = readSyscalls(data)[args.slot]
        print(f"syscall {args.slot:#04x} -> {target:#010x} ({segment(target)}), "
              f"file offset {target & 0x1FFFFFFF:#x}")
        return 0

    reportExceptions(data)
    reportSyscalls(data, args.all)
    return 0


if __name__ == "__main__":
    sys.exit(main())
