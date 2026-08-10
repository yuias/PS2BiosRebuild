#!/usr/bin/env python3
"""Judge a built image against the specifications, at the depth it has reached.

The gates in `tools/iopsim.py`, `tools/eesim.py` and the rest ask a finished
image the questions a finished image can answer. This one is the build's own,
and it exists because a partial image needs a *stated* standard rather than a
lenient one: every milestone below is a claim about what the image now does,
and the run fails if a claim stops holding.

    tools/imgcheck.py build/rom.bin

What is deliberately not required yet is listed at the end of the run, so the
distance from a complete image is visible rather than implied.
"""

from __future__ import annotations

import argparse
import pathlib
import sys

import eesim
import iopsim
import irxinfo
import romdir

# spec/03 BOOT-5a: what a retail boot emits, now that the handoff to IOPBOOT
# succeeds.
EXPECTED_POST = [0xFC, 0x02, 0x03, 0x04, 0x05, 0x08, 0x09]

# Where IOPBOOT leaves what it parsed out of the boot list: a count, then the
# base load address the `@` token set (spec/03 BOOT-9).
BOOT_LIST = 0x2000
EXPECTED_BASE = 0x800
EXPECTED_MODULES = 1             # SYSMEM; the rest of the boot list follows

# Scaffolding inside the module: where its entry records that it ran, and what
# it writes there (src/iop/sysmem.S).
MODULE_MARKER = 0x3000
MODULE_MARK = 0x5359534D
# spec/02 IRX-5: the export table's entries are absolute pointers, so after
# loading every one of them must have moved by the load base.
EXPORT_TABLE = 0x40

RESET_COP0 = (("Config", 0x00073003), ("Status", 0x70400000),
              ("Count", 0), ("Compare", 1))
RESET_TLB = (0, 0x70000000, 0x80000007, 0x00000007)
KERNEL_BANNER = "PS2BiosRebuild EE kernel"

NOT_YET = (
    "binding imports between modules, and registering libraries (spec/02 "
    "IRX-9, IRX-10)",
    "the rest of the boot list: SYSMEM is the only module built",
    "111 of the 125 syscall slots: they resolve to the reporter of EE-8d "
    "rather than to their own handlers (spec/05 SYS-1)",
    "EE-7e's 128-bit context save, and the scheduler that needs it",
    "the EE handshake and everything past it (spec/03 BOOT-10)",
)

# The interface the kernel publishes, exercised through eesim's harness. Every
# one of these was a requirement read out of the reference first.
INTC_SOURCE = 3
INTC_ALIAS_SOURCE = 4
DMAC_CHANNEL = 2
INSTALLED_SLOT, INSTALLED_HANDLER = 0x40, 0xDEADBEEF
EXCEPTION_CODE, EXCEPTION_HANDLER = 2, 0x80005678
UNDEFINED_SLOT = 0x21


def checkArchive(image: pathlib.Path) -> tuple[list[str], list[str]]:
    """The archive parses, and the files this milestone needs are present."""
    problems: list[str] = []
    data = image.read_bytes()
    entries = romdir.parseEntries(data, romdir.findTable(data))
    names = [entry.name for entry in entries]
    for required in ("RESET", "ROMDIR", "EXTINFO", "ROMVER", "RDRAM", "KERNEL"):
        if required not in names:
            problems.append(f"ARC: the archive has no {required} entry")
    if names[:3] != ["RESET", "ROMDIR", "EXTINFO"]:
        problems.append(f"ARC-4: the archive opens {names[:3]}, want "
                        f"['RESET', 'ROMDIR', 'EXTINFO']")
    return problems, names


def moduleFromImage(image: pathlib.Path, name: str) -> irxinfo.Irx:
    """A module as the archive stores it, which is what the loader reads."""
    data = image.read_bytes()
    entries = romdir.parseEntries(data, romdir.findTable(data))
    entry = next(e for e in entries if e.name == name)
    return irxinfo.Irx.fromBytes(
        data[entry.offset:entry.offset + entry.size], name)


def checkModule(image: pathlib.Path) -> list[str]:
    """The built module against the conformance checker the reference passes."""
    return [f"IRX: {problem}"
            for problem in irxinfo.checkModule(moduleFromImage(image, "SYSMEM"))]


def checkIop(image: pathlib.Path) -> list[str]:
    """The boot block's IOP path, to where it runs out of image."""
    problems: list[str] = []
    bus = iopsim.Bus(image.read_bytes())
    cpu = iopsim.Cpu(bus)
    while cpu.steps < 200_000 and cpu.stop_reason is None:
        cpu.step()
    post = [value for _, value in bus.post]
    if post != EXPECTED_POST:
        problems.append(f"BOOT-5a: POST {[hex(v) for v in post]} != "
                        f"{[hex(v) for v in EXPECTED_POST]}")

    # BOOT-7 and BOOT-9: IOPBOOT resolved IOPBTCONF by name -- which exercises
    # the whole ARC-4 scan -- and parsed its tokens.
    count = bus.read(BOOT_LIST, 4)
    base = bus.read(BOOT_LIST + 4, 4)
    if base != EXPECTED_BASE:
        problems.append(f"BOOT-9: the boot list's base address parsed as "
                        f"{base:#x}, want {EXPECTED_BASE:#x} -- IOPBOOT did "
                        f"not reach or read IOPBTCONF")
    if count != EXPECTED_MODULES:
        problems.append(f"BOOT-9: {count} module names resolved, want "
                        f"{EXPECTED_MODULES}")
        return problems

    # IRX-12: the module was copied, relocated and entered.
    if bus.read(MODULE_MARKER, 4) != MODULE_MARK:
        problems.append(f"IRX-12: the module's entry left "
                        f"{bus.read(MODULE_MARKER, 4):#010x} at "
                        f"{MODULE_MARKER:#x}, not {MODULE_MARK:#010x} -- it "
                        f"was not entered")
    record = bus.read(MODULE_MARKER + 4, 4)
    if not base <= record < len(bus.ram):
        problems.append(f"IRX-12c: the entry was given {record:#x} as its "
                        f"module record, which is not in RAM")
    elif bus.read(record + 0x10, 4) == 0 or bus.read(record + 0x14, 4) is None:
        problems.append("IRX-12c: the module record's +0x10 entry is empty")

    # IRX-3: every export entry is an R_MIPS_32, so each must have moved by the
    # base. Comparing against the stored file is what makes this a test of the
    # relocation rather than of the table.
    entries = moduleFromImage(image, "SYSMEM").exportEntries(EXPORT_TABLE)
    for index, value in enumerate(entries):
        at = base + EXPORT_TABLE + 0x14 + index * 4
        loaded = bus.read(at, 4)
        if loaded != value + base:
            problems.append(f"IRX-3: export {index} loaded as {loaded:#x}, "
                            f"want {value + base:#x} -- the R_MIPS_32 fixup "
                            f"was not applied")
            break
    return problems


def checkEe(image: pathlib.Path) -> list[str]:
    """The boot block's EE path, and the kernel it hands control to."""
    problems: list[str] = []
    machine = eesim.Machine(image.read_bytes())
    machine.boot()

    written: dict[str, int] = {}
    for name, value, _ in machine.cpu.cop0_writes:
        written.setdefault(name, value)
    for name, expected in RESET_COP0:
        if written.get(name) != expected:
            problems.append(f"EE-1: {name} was written "
                            f"{written.get(name, 0):#010x}, want {expected:#010x}")
    if not machine.cpu.tlb_writes or machine.cpu.tlb_writes[0] != (0, RESET_TLB):
        problems.append(f"EE-1a: first TLB write {machine.cpu.tlb_writes[:1]}, "
                        f"want index 0 = {RESET_TLB}")
    if not (eesim.SCRATCH_BASE <= machine.stack_at_rdram
            < eesim.SCRATCH_BASE + eesim.SCRATCH_SIZE):
        problems.append(f"EE-1b: the stack was {machine.stack_at_rdram:#010x}, "
                        f"not in the scratchpad")
    if not machine.rdram_called:
        problems.append("EE-2: RDRAM was never called")
    if not machine.reached_kernel:
        problems.append(f"EE-3d: control never reached "
                        f"{eesim.KERNEL_ENTRY:#010x}"
                        + (f" ({machine.cpu.stop_reason})"
                           if machine.cpu.stop_reason else ""))
    elif KERNEL_BANNER not in machine.kernel_console:
        problems.append(f"EE-4: the kernel did not announce itself; its "
                        f"console held {machine.kernel_console!r}")
    else:
        problems += checkSyscalls(machine)
    return problems


def checkSyscalls(machine: eesim.Machine) -> list[str]:
    """The syscall interface, called rather than read.

    Reaching the harness's return address at all requires EE-7c: the handler
    must have advanced EPC past the `syscall`, or control would come back to
    the same instruction forever.
    """
    problems: list[str] = []

    def require(ok: bool, requirement: str, detail: str) -> None:
        if not ok:
            problems.append(f"{requirement}: {detail}")

    acted = machine.syscall(0x14, INTC_SOURCE)
    again = machine.syscall(0x14, INTC_SOURCE)
    require((acted, again) == (1, 0), "EE-8g",
            f"enabling an INTC source twice returned {acted} then {again}, "
            f"want 1 then 0")
    require(bool(machine.bus.io.get(eesim.INTC_MASK, 0) & (1 << INTC_SOURCE)),
            "EE-8g", "the INTC mask bit is not set after enabling")
    require((machine.syscall(0x15, INTC_SOURCE),
             machine.syscall(0x15, INTC_SOURCE)) == (1, 0), "EE-8g",
            "disabling an INTC source did not report acting exactly once")

    machine.syscall(0x16, DMAC_CHANNEL)
    require(bool(machine.bus.io.get(eesim.DMAC_STATUS, 0)
                 & (1 << (16 + DMAC_CHANNEL))), "EE-8g",
            "the DMAC bit must start at 16, not 0")

    require(machine.syscall(0x1A, INTC_ALIAS_SOURCE) == 1, "EE-8c",
            "slot 0x1a does not reach the same handler as 0x14")
    require(machine.syscall(-0x14 & eesim.MASK64, INTC_ALIAS_SOURCE + 1) == 1,
            "EE-7b", "a negative syscall number did not reach the same slot")

    machine.syscall(0x74, INSTALLED_SLOT, INSTALLED_HANDLER)
    require(machine.word(0x80014F40 + INSTALLED_SLOT * 4) == INSTALLED_HANDLER,
            "SYS-5a", "slot 0x74 did not install into the syscall table")

    installed = machine.syscall(0x0D, EXCEPTION_CODE, EXCEPTION_HANDLER)
    require((installed & eesim.MASK32) == EXCEPTION_HANDLER, "EE-6e",
            f"installing an exception handler returned {installed}")
    require(machine.word(0x80015340 + EXCEPTION_CODE * 4) == EXCEPTION_HANDLER,
            "EE-6e", "the exception table was not written")
    require(machine.syscall(0x0D, 9, EXCEPTION_HANDLER) == 0, "EE-6e",
            "an out-of-range exception code was accepted")

    require(machine.syscall(0x75) is not None, "SYS-3b",
            "the empty syscall 0x75 did not return")

    before = len(machine.bus.console)
    machine.syscall(UNDEFINED_SLOT)
    reported = machine.bus.console[before:].decode("ascii", "replace")
    require(f"{UNDEFINED_SLOT:02x}" in reported, "EE-8d",
            f"an unimplemented slot reported {reported!r}, which does not name "
            f"the number")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("image", type=pathlib.Path)
    arguments = parser.parse_args()

    problems, names = checkArchive(arguments.image)
    problems += checkModule(arguments.image)
    problems += checkIop(arguments.image)
    problems += checkEe(arguments.image)

    for problem in problems:
        print(f"{arguments.image}: {problem}", file=sys.stderr)
    if problems:
        print(f"\n{arguments.image}: {len(problems)} failure(s)", file=sys.stderr)
        return 1

    print(f"{arguments.image}: ok -- {len(names)} archive entries; the IOP path "
          f"loads and enters its first module; the EE path reaches "
          f"its kernel, which "
          f"announces itself and serves its syscalls")
    print("\nnot required of the image yet:")
    for item in NOT_YET:
        print(f"   {item}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
