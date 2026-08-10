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
import romdir

# The IOP path stops here for now: `IOPBOOT` is not in the image, so the boot
# block's name lookup fails and reports it (spec/03 BOOT-6d, BOOT-5b). That is
# the specified behaviour for a missing module, and the POST prefix before it
# is BOOT-5a's retail sequence.
EXPECTED_POST = [0xFC, 0x02, 0x03, 0x04, 0x05, 0x08, 0xFA]

RESET_COP0 = (("Config", 0x00073003), ("Status", 0x70400000),
              ("Count", 0), ("Compare", 1))
RESET_TLB = (0, 0x70000000, 0x80000007, 0x00000007)
KERNEL_BANNER = "PS2BiosRebuild EE kernel"

NOT_YET = (
    "IOPBOOT and the IOP kernel modules (spec/03 BOOT-7, spec/02)",
    "the EE vector page and exception dispatch (spec/04 EE-5, EE-6)",
    "the 125 syscall slots (spec/04 EE-8, spec/05)",
    "the EE handshake and everything past it (spec/03 BOOT-10)",
)


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


def checkIop(image: pathlib.Path) -> list[str]:
    """The boot block's IOP path, to where it runs out of image."""
    problems: list[str] = []
    bus = iopsim.Bus(image.read_bytes())
    cpu = iopsim.Cpu(bus)
    while cpu.steps < 200_000 and cpu.stop_reason is None:
        cpu.step()
    post = [value for _, value in bus.post]
    if post != EXPECTED_POST:
        problems.append(f"BOOT-5: POST {[hex(v) for v in post]} != "
                        f"{[hex(v) for v in EXPECTED_POST]}")
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
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("image", type=pathlib.Path)
    arguments = parser.parse_args()

    problems, names = checkArchive(arguments.image)
    problems += checkIop(arguments.image)
    problems += checkEe(arguments.image)

    for problem in problems:
        print(f"{arguments.image}: {problem}", file=sys.stderr)
    if problems:
        print(f"\n{arguments.image}: {len(problems)} failure(s)", file=sys.stderr)
        return 1

    print(f"{arguments.image}: ok -- {len(names)} archive entries; the IOP path "
          f"reaches its handoff; the EE path reaches its kernel and the kernel "
          f"announces itself")
    print("\nnot required of the image yet:")
    for item in NOT_YET:
        print(f"   {item}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
