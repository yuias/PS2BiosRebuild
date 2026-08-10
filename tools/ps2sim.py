#!/usr/bin/env python3
"""Boot both of a PS2 BIOS image's CPUs at once, across a modelled SIF.

`tools/iopsim.py` boots the IOP and stops where it waits for the EE.
`tools/eesim.py` boots the EE and stops where it waits for the IOP. Each was
blocked on the other. This runs the two together, with the SIF registers both
sides see shared between them, and the deadlock resolves.

What that buys, concretely:

  * the IOP boot **completes** -- it runs off the end of its boot list and
    reaches its idle loop, instead of spinning on a flag;
  * `SIFINIT` is torn down, which `docs/analysis/12-ee-facing-services.md`
    named as the one-shot module no IOP-only simulator could reach. That is
    `docs/spec/02-module-abi.md` IRX-12 observed at the place it was hardest
    to observe.

    tools/ps2sim.py assets/SCPH-50000.bin            # report the joint boot
    tools/ps2sim.py assets/SCPH-50000.bin --check    # judge it, exit 1 on failure
    tools/ps2sim.py assets/SCPH-50000.bin --traffic  # every SIF register access

Scope, again deliberately small. The registers are shared and the two SIF DMA
channels move data in **normal mode** -- a channel transfers the quadwords it
was given. The EE's *chain* mode, where a tag list at TADR describes the
transfer, is not modelled: the reference's driver uses it, so the reference
still stops after the handshake with its channel idle and nothing sent. Our own
image uses normal mode and gets its data across.
"""

from __future__ import annotations

import argparse
import pathlib
import sys

import eesim
import iopsim

# The six registers both CPUs see, at the two addresses each has for them.
MSCOM, SMCOM, MSFLG, SMFLG, CTRL, BD6 = range(6)
REGISTER_NAMES = ("MSCOM", "SMCOM", "MSFLG", "SMFLG", "CTRL", "BD6")
EE_REGISTERS = {0x1000F200: MSCOM, 0x1000F210: SMCOM, 0x1000F220: MSFLG,
                0x1000F230: SMFLG, 0x1000F240: CTRL, 0x1000F260: BD6}
IOP_REGISTERS = {0x1D000000: MSCOM, 0x1D000010: SMCOM, 0x1D000020: MSFLG,
                 0x1D000030: SMFLG, 0x1D000040: CTRL, 0x1D000060: BD6}

# `docs/analysis/11-sif-and-rom-driver.md`: SIFMAN accepts the bus only if the
# register at 0xBD000060 reads back its own address, or reads as near zero.
BD6_IDENTITY = 0x1D000060

STEPS_PER_TURN = 2000            # small enough that a poll loop makes progress
MAX_TURNS = 4000

# The two SIF DMA channels, as each side addresses them. Which IOP channels
# they are was derived by running the reference and watching which registers
# its driver touches after the handshake, not assumed:
#
#   python3 tools/ps2sim.py assets/SCPH-50000.bin --traffic
#
# SIF0 carries IOP -> EE and SIF1 EE -> IOP, and each side drives its own end.
EE_SIF0, EE_SIF1 = 0x1000C000, 0x1000C400        # CHCR +0, MADR +0x10, QWC +0x20
IOP_SIF0, IOP_SIF1 = 0x1F801520, 0x1F801530      # MADR +0, BCR +4, CHCR +8

EE_START = 0x100                 # CHCR.STR, which the hardware clears when done
IOP_START = 0x01000000           # the IOP's equivalent busy bit


class SifDma:
    """The two directions of the SIF, as a queue of bytes each.

    Real hardware has a fixed FIFO and stalls a transfer that outruns it; a
    queue that grows models the same *result* for a driver that waits for its
    channel to finish, which is what both sides' drivers do. Nothing here
    models the tag chains of the EE's chain mode: a transfer moves the
    quadwords its channel was given, and a chain-mode caller would find its
    channel idle with nothing sent.
    """

    def __init__(self) -> None:
        self.to_ee = bytearray()         # SIF0
        self.to_iop = bytearray()        # SIF1
        # (what happened, bytes, address) -- both ends of one transfer are
        # recorded, because a send that nobody receives is the interesting case.
        self.transfers: list[tuple[str, int, int]] = []

    def send(self, fifo: bytearray, memory, address: int, length: int,
             name: str) -> None:
        chunk = bytes(memory[address:address + length])
        fifo += chunk + bytes(length - len(chunk))
        self.transfers.append((name, length, address))

    def receive(self, fifo: bytearray, memory, address: int, length: int,
                name: str) -> None:
        taken = bytes(fifo[:length])
        del fifo[:len(taken)]
        memory[address:address + len(taken)] = taken
        self.transfers.append((name, len(taken), address))


class Sif:
    """The shared registers, with the asymmetry each side's writes have.

    `MSFLG` is the EE's to raise and the IOP's to acknowledge; `SMFLG` is the
    reverse. Modelling both as plain storage deadlocks the handshake, which is
    how the asymmetry was found: with it, both sides proceed.
    """

    def __init__(self) -> None:
        self.registers = [0] * 6
        self.registers[BD6] = BD6_IDENTITY
        self.traffic: list[tuple[str, int, int, bool]] = []
        # The handshake is a sequence, not a final state: MSCOM is cleared
        # again once the IOP has read it, so what matters is what was ever
        # published, not what is left behind.
        self.published: dict[int, int] = {}

    def read(self, which: int, from_ee: bool) -> int:
        value = self.registers[which]
        self.traffic.append(("read", which, value, from_ee))
        return value

    def write(self, which: int, value: int, from_ee: bool) -> None:
        value &= 0xFFFFFFFF
        if which == MSFLG:
            self.registers[which] = (self.registers[which] | value if from_ee
                                     else self.registers[which] & ~value)
        elif which == SMFLG:
            self.registers[which] = (self.registers[which] & ~value if from_ee
                                     else self.registers[which] | value)
        else:
            self.registers[which] = value
        self.registers[which] &= 0xFFFFFFFF
        if value:
            self.published.setdefault(which, value)
        self.traffic.append(("write", which, value, from_ee))


class Split(Sif):
    """The same registers, *not* shared -- one set per side.

    This is what the two simulators had before they were joined, and it is
    kept so the gate can be run in the failing direction: with it, neither
    side's flags reach the other and both boots stall where they used to.
    """

    def __init__(self) -> None:
        super().__init__()
        self.iop_registers = [0] * 6
        self.iop_registers[BD6] = BD6_IDENTITY

    def read(self, which: int, from_ee: bool) -> int:
        return super().read(which, from_ee) if from_ee \
            else self.iop_registers[which]

    def write(self, which: int, value: int, from_ee: bool) -> None:
        if from_ee:
            super().write(which, value, from_ee)
        else:
            self.iop_registers[which] = value & 0xFFFFFFFF


def eeBus(sif: Sif, dma: SifDma, rom: bytes) -> eesim.Bus:
    class SharedBus(eesim.Bus):
        def read(self, address: int, size: int) -> int:
            which = EE_REGISTERS.get(address & 0x1FFFFFFF)
            if which is not None:
                return sif.read(which, True)
            return super().read(address, size)

        def write(self, address: int, size: int, value: int) -> None:
            offset = address & 0x1FFFFFFF
            which = EE_REGISTERS.get(offset)
            if which is not None:
                sif.write(which, value, True)
                return
            super().write(address, size, value)
            if offset in (EE_SIF0, EE_SIF1) and value & EE_START:
                self.runChannel(offset, value)

        def runChannel(self, channel: int, chcr: int) -> None:
            """A channel started: move its quadwords, then report itself idle."""
            address = self.read(channel + 0x10, 4) & 0x1FFFFFFF
            length = self.read(channel + 0x20, 4) * 16
            if channel == EE_SIF1:
                dma.send(dma.to_iop, self.ram, address, length, "EE sent")
            else:
                dma.receive(dma.to_ee, self.ram, address, length, "EE received")
            self.io[channel] = chcr & ~EE_START

    return SharedBus(rom)


def iopBus(sif: Sif, dma: SifDma, rom: bytes) -> iopsim.Bus:
    class SharedBus(iopsim.Bus):
        def read(self, addr: int, size: int) -> int:
            which = IOP_REGISTERS.get(addr & 0x1FFFFFFF)
            if which is not None:
                return sif.read(which, False)
            return super().read(addr, size)

        def write(self, addr: int, size: int, value: int, pc: int) -> None:
            offset = addr & 0x1FFFFFFF
            which = IOP_REGISTERS.get(offset)
            if which is not None:
                sif.write(which, value, False)
                return
            super().write(addr, size, value, pc)
            if offset in (IOP_SIF0 + 8, IOP_SIF1 + 8) and value & IOP_START:
                self.runChannel(offset - 8, value)

        def runChannel(self, channel: int, chcr: int) -> None:
            address = self.read(channel, 4) & 0x1FFFFFFF
            blocks = self.read(channel + 4, 4)
            length = (blocks & 0xFFFF) * ((blocks >> 16) or 1) * 4
            if channel == IOP_SIF0:
                dma.send(dma.to_ee, self.ram, address, length, "IOP sent")
            else:
                dma.receive(dma.to_iop, self.ram, address, length, "IOP received")
            self.io[channel + 8] = chcr & ~IOP_START

    return SharedBus(rom)


class Console:
    """Both machines, run against each other."""

    def __init__(self, rom: bytes, bridge: bool = True) -> None:
        self.sif = Sif() if bridge else Split()
        self.dma = SifDma()
        self.ee = eesim.Machine(rom)
        self.ee.bus = eeBus(self.sif, self.dma, rom)
        self.ee.cpu = eesim.Cpu(self.ee.bus)
        self.iop_bus = iopBus(self.sif, self.dma, rom)
        self.iop = iopsim.Cpu(self.iop_bus)
        self.watcher = iopsim.FreeWatcher(self.iop_bus)
        self.frees_before_handshake = 0
        self.turns = 0

    def idle(self) -> bool:
        """True when the IOP has run out of work: a jump to itself.

        BOOT-10d says a jump, not one encoding of one: `j` and the `beq
        $zero, $zero` a `b` assembles to both spin in place, and a rebuild is
        free to use either. A turn can end on either half of the loop, so the
        delay slot counts as being in it too.
        """
        for address in (self.iop.pc, self.iop.pc - 4):
            instruction = self.iop_bus.read(address, 4)
            opcode = instruction >> 26
            if opcode == 2:                        # j
                target = (address & 0xF0000000) | ((instruction & 0x3FFFFFF) << 2)
            elif opcode == 4 and (instruction >> 16) & 0x3FF == 0:   # b
                offset = instruction & 0xFFFF
                if offset & 0x8000:
                    offset -= 0x10000
                target = address + 4 + offset * 4
            else:
                continue
            if target == address:
                return True
        return False

    def run(self) -> None:
        # The EE reaches its kernel without needing the IOP at all, so run that
        # part first and interleave only where the two actually interact.
        recorded: int | None = None
        self.ee.boot()
        for self.turns in range(MAX_TURNS):
            for _ in range(STEPS_PER_TURN):
                if self.iop.stop_reason:
                    break
                self.watcher.observe(self.iop)
                self.iop.step()
            for _ in range(STEPS_PER_TURN):
                if self.ee.cpu.stop_reason:
                    break
                self.ee.cpu.step()
            if recorded is None and self.handshakeDone():
                recorded = len(self.watcher.frees)
                self.frees_before_handshake = recorded
            if (recorded is not None and self.idle()
                    and len(self.watcher.frees) > recorded):
                return

    @property
    def handshake(self) -> tuple[int, int, int, int]:
        published = self.sif.published
        return (published.get(MSCOM, 0), published.get(MSFLG, 0),
                published.get(SMCOM, 0), published.get(SMFLG, 0))

    def handshakeDone(self) -> bool:
        return bool(self.sif.published.get(SMCOM)
                    and self.sif.published.get(SMFLG))


# What a completed handshake looks like, and what it must produce.
EXPECTED_IOP_FREES = 5           # four before the handshake, SIFINIT after it


def check(console: Console) -> list[str]:
    problems: list[str] = []

    def require(ok: bool, requirement: str, detail: str) -> None:
        if not ok:
            problems.append(f"{requirement}: {detail}")

    mscom, msflg, smcom, smflg = console.handshake
    require(msflg != 0, "BOOT-10a",
            "the EE never raised a bit in MSFLG, so the IOP had nothing to see")
    require(mscom != 0, "BOOT-10a",
            "the EE never published an address in MSCOM")
    require(smcom != 0, "BOOT-10b",
            "the IOP never answered with an address in SMCOM")
    require(smflg != 0, "BOOT-10b",
            "the IOP never raised a bit in SMFLG")
    require(console.idle(), "BOOT-10c",
            f"the IOP did not reach its idle loop; it stopped at "
            f"{console.iop.pc:#010x}"
            + (f" ({console.iop.stop_reason})" if console.iop.stop_reason else ""))
    require(len(console.watcher.frees) >= EXPECTED_IOP_FREES, "IRX-12a",
            f"{len(console.watcher.frees)} module images were released, want "
            f"{EXPECTED_IOP_FREES}: the one past the handshake was not reached")
    require(len(console.watcher.frees) > console.frees_before_handshake,
            "IRX-12a", "no module was torn down after the handshake")
    misaligned = [address for address, _ in console.watcher.frees
                  if address % 0x100]
    require(not misaligned, "IRX-12b",
            f"released bases {[hex(a) for a in misaligned]} are not on a "
            f"0x100 boundary")
    return problems


def report(console: Console) -> None:
    mscom, msflg, smcom, smflg = console.handshake
    print(f"IOP: {console.iop.steps} instructions, "
          f"POST {[hex(v) for _, v in console.iop_bus.post]}, "
          f"pc {console.iop.pc:#010x}"
          + ("  (idle loop)" if console.idle() else ""))
    print(f"EE:  {console.ee.cpu.steps} instructions, "
          f"pc {console.ee.cpu.pc:#010x}")
    print(f"\nSIF after the handshake:")
    print(f"   MSCOM {mscom:#010x}   MSFLG {msflg:#010x}   (the EE's to write)")
    print(f"   SMCOM {smcom:#010x}   SMFLG {smflg:#010x}   (the IOP's to write)")

    if console.dma.transfers:
        print("\nSIF transfers:")
        for what, length, address in console.dma.transfers:
            print(f"   {what:<13} {length:5d} bytes at {address:#010x}")

    print(f"\nmodule images released (IRX-12): {len(console.watcher.frees)}")
    for index, (address, step) in enumerate(console.watcher.frees):
        past = index >= console.frees_before_handshake
        print(f"   {address:#08x}  at IOP step {step}"
              + ("   <- past the handshake" if past else ""))

    console_text = console.ee.bus.console.decode("ascii", "replace")
    tail = [line for line in console_text.splitlines() if line.strip()][-3:]
    if tail:
        print("\nthe EE's last words:")
        for line in tail:
            print(f"   {line}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("image", type=pathlib.Path)
    parser.add_argument("--check", action="store_true",
                        help="judge the joint boot; exit 1 on any failure")
    parser.add_argument("--traffic", action="store_true",
                        help="list every SIF register access in order")
    parser.add_argument("--no-bridge", action="store_true",
                        help="give each CPU its own copy of the SIF registers, "
                             "which is what they had before being joined")
    arguments = parser.parse_args()

    console = Console(arguments.image.read_bytes(), bridge=not arguments.no_bridge)
    console.run()

    if arguments.check:
        problems = check(console)
        for problem in problems:
            print(f"{arguments.image}: {problem}", file=sys.stderr)
        if problems:
            print(f"\n{arguments.image}: {len(problems)} failure(s)",
                  file=sys.stderr)
            return 1
        print(f"{arguments.image}: ok -- both CPUs booted, the SIF handshake "
              f"completed, {len(console.watcher.frees)} module images released")
        return 0

    report(console)
    if arguments.traffic:
        print("\nSIF register accesses:")
        seen = set()
        for kind, which, value, from_ee in console.sif.traffic:
            key = (kind, which, value, from_ee)
            if kind == "read" and key in seen:
                continue                     # polls repeat; show each once
            seen.add(key)
            print(f"   {'EE ' if from_ee else 'IOP'} {kind:<5} "
                  f"{REGISTER_NAMES[which]:<6} {value:#010x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
