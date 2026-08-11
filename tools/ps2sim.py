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
EE_CHAIN = 0x4                   # CHCR.MOD == 1, the chain modes
IOP_START = 0x01000000           # the IOP's equivalent busy bit

QUADWORD = 16

# The quadword that heads every SIF1 packet, read by the IOP's channel (BOOT-11a).
IOP_TAG_ADDRESS = 0x3FFFFFFF     # the rest of the first word is where it lands
IOP_TAG_IRQ = 0x40000000
IOP_TAG_END = 0x80000000

# The EE's source-chain tag ids the SIF path uses (BOOT-11b). `ref` points at
# data elsewhere and carries on; `refe` does the same and is the last one.
TAG_REFE, TAG_CNT, TAG_NEXT, TAG_REF, TAG_END = 0, 1, 2, 3, 7

# A list whose end marker never arrives is a hang, not a diagnosis. Both walks
# stop well past any length either side's driver builds, and say why.
MAX_TAGS = 64


def quadwords(count: int) -> int:
    """Bytes, rounded up to the quadword the SIF moves in."""
    return (count + QUADWORD - 1) // QUADWORD * QUADWORD


class SifDma:
    """The two directions of the SIF, as a queue of bytes each.

    Real hardware has a fixed FIFO and stalls a transfer that outruns it; a
    queue that grows models the same *result* for a driver that waits for its
    channel to finish, which is what both sides' drivers do.

    What the queue carries is *framed*, and the framing is the point of
    `docs/spec/03-boot-chain.md` BOOT-11: each side pushes packet headers the
    other side's channel parses to learn where the bytes belong. Neither
    direction is a bare copy -- a receiver is never told the destination by its
    own driver, only by the sender's header.
    """

    def __init__(self) -> None:
        self.to_ee = bytearray()         # SIF0
        self.to_iop = bytearray()        # SIF1
        # (what happened, bytes, address) -- both ends of one transfer are
        # recorded, because a send that nobody receives is the interesting case.
        self.transfers: list[tuple[str, int, int]] = []
        # Every framed packet, as the receiving end decoded it: which way it
        # went, where it landed, how big it was and what its header said.
        self.packets: list[tuple[str, int, int, int]] = []
        # Every chain tag walked, so the claim about what a tag list looks like
        # can be read off a run rather than taken on trust.
        self.tags: list[tuple[str, int, int, int]] = []

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

    def push(self, fifo: bytearray, values: bytes) -> None:
        fifo += values


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
        def __init__(self, image: bytes) -> None:
            super().__init__(image)
            # A started channel that cannot finish yet stays busy rather than
            # reporting itself idle with nothing moved: a receiver is armed
            # before the sender has pushed anything, and clearing STR there
            # would tell its driver a transfer had happened.
            self.armed: dict[int, int] = {}

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
                self.armed[offset] = value
                self.service()

        def word(self, address: int) -> int:
            return int.from_bytes(self.ram[address:address + 4], "little")

        def service(self) -> None:
            """Give every armed channel a chance to finish."""
            for channel, chcr in list(self.armed.items()):
                finished = (self.send(channel, chcr) if channel == EE_SIF1
                            else self.receive(channel, chcr))
                if finished:
                    del self.armed[channel]
                    self.io[channel] = chcr & ~EE_START

        def send(self, channel: int, chcr: int) -> bool:
            """SIF1, EE -> IOP. Everything the EE sends is already in its RAM,
            so a send always completes; the only question is what it moves."""
            if not chcr & EE_CHAIN:
                address = self.read(channel + 0x10, 4) & 0x1FFFFFFF
                dma.send(dma.to_iop, self.ram, address,
                         self.read(channel + 0x20, 4) * QUADWORD, "EE sent")
                return True
            # Source chain (BOOT-11b): a list of tags at TADR, each naming a
            # run of quadwords. TTE is clear in the reference's CHCR, so the
            # tag quadwords themselves are not sent -- what reaches the FIFO is
            # only the data they point at, and the IOP-facing header of
            # BOOT-11a is the first quadword of that data, not of the tag.
            tadr = self.read(channel + 0x30, 4) & 0x1FFFFFFF
            for _ in range(MAX_TAGS):
                tag = self.word(tadr)
                pointer = self.word(tadr + 4) & 0x1FFFFFFF
                count, identifier = tag & 0xFFFF, (tag >> 28) & 7
                elsewhere = identifier in (TAG_REFE, TAG_REF)
                source = pointer if elsewhere else tadr + QUADWORD
                dma.tags.append(("EE source", tadr, tag, pointer))
                dma.send(dma.to_iop, self.ram, source, count * QUADWORD,
                         "EE sent")
                inline_end = tadr + QUADWORD + count * QUADWORD
                tadr = tadr + QUADWORD if elsewhere else inline_end
                if identifier == TAG_NEXT:
                    tadr = pointer
                self.io[channel + 0x30] = tadr
                if identifier in (TAG_REFE, TAG_END):
                    return True
            raise RuntimeError(f"EE source chain at {channel:#010x} ran past "
                               f"{MAX_TAGS} tags without one ending it")

        def receive(self, channel: int, chcr: int) -> bool:
            """SIF0, IOP -> EE. In chain mode the destination comes out of the
            FIFO ahead of the data, so this can only proceed once the IOP has
            pushed something."""
            if not chcr & EE_CHAIN:
                length = self.read(channel + 0x20, 4) * QUADWORD
                if len(dma.to_ee) < length:
                    return False
                dma.receive(dma.to_ee, self.ram,
                            self.read(channel + 0x10, 4) & 0x1FFFFFFF,
                            length, "EE received")
                return True
            while len(dma.to_ee) >= QUADWORD:
                tag = int.from_bytes(dma.to_ee[0:4], "little")
                address = int.from_bytes(dma.to_ee[4:8], "little") & 0x1FFFFFFF
                count, identifier = tag & 0xFFFF, (tag >> 28) & 7
                if len(dma.to_ee) < QUADWORD + count * QUADWORD:
                    return False                          # the rest is coming
                del dma.to_ee[:QUADWORD]
                dma.receive(dma.to_ee, self.ram, address, count * QUADWORD,
                            "EE received")
                dma.packets.append(("IOP -> EE", address, count * 4, tag))
                self.io[channel + 0x10] = address + count * QUADWORD
                if identifier == TAG_END or tag & 0x80000000:
                    return True
            return False

    return SharedBus(rom)


def iopBus(sif: Sif, dma: SifDma, rom: bytes) -> iopsim.Bus:
    class SharedBus(iopsim.Bus):
        def __init__(self, image: bytes) -> None:
            super().__init__(image)
            self.armed: dict[int, int] = {}

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
                self.armed[offset - 8] = value
                self.service()

        def word(self, address: int) -> int:
            return int.from_bytes(self.ram[address:address + 4], "little")

        def service(self) -> None:
            for channel, chcr in list(self.armed.items()):
                finished = (self.send(channel) if channel == IOP_SIF0
                            else self.receive(channel))
                if finished:
                    del self.armed[channel]
                    self.io[channel + 8] = chcr & ~IOP_START

        def send(self, channel: int) -> bool:
            """SIF0, IOP -> EE. TADR walks a list of 16-byte send blocks; each
            names IOP memory to read and carries, ready-made, the tag the EE's
            destination chain will pop to learn where it goes (BOOT-11c)."""
            tadr = self.read(channel + 0xC, 4) & 0x1FFFFFFF
            for _ in range(MAX_TAGS):
                header = self.word(tadr)
                source = header & IOP_TAG_ADDRESS
                words = self.word(tadr + 4)
                dma.push(dma.to_ee, self.ram[tadr + 8:tadr + 16] + bytes(8))
                dma.send(dma.to_ee, self.ram, source, quadwords(words * 4),
                         "IOP sent")
                tadr += QUADWORD
                self.io[channel + 0xC] = tadr
                if header & IOP_TAG_END:
                    return True
            raise RuntimeError(f"IOP send blocks at {channel:#010x} ran past "
                               f"{MAX_TAGS} without one ending the run")

        def receive(self, channel: int) -> bool:
            """SIF1, EE -> IOP. The header quadword at the front of the FIFO is
            what says where the data lands; this end supplies no address at all
            (BOOT-11a)."""
            while len(dma.to_iop) >= QUADWORD:
                header = int.from_bytes(dma.to_iop[0:4], "little")
                words = int.from_bytes(dma.to_iop[4:8], "little")
                length = quadwords(words * 4)
                if len(dma.to_iop) < QUADWORD + length:
                    return False                          # the rest is coming
                del dma.to_iop[:QUADWORD]
                address = header & IOP_TAG_ADDRESS
                dma.receive(dma.to_iop, self.ram, address, length,
                            "IOP received")
                dma.packets.append(("EE -> IOP", address, words, header))
                self.io[channel] = address + length
                if header & IOP_TAG_END:
                    return True
            return False

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
            # A channel armed before its data existed finishes here, once the
            # other CPU's turn has pushed what it was waiting for.
            self.ee.bus.service()
            self.iop_bus.service()
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

    # BOOT-11a is the claim that the *sender* frames the transfer. The check
    # that bites is not that a packet crossed but that its header, and nothing
    # on the receiving side, chose where it landed: the address in it has to be
    # the one the IOP published in SMCOM during the handshake.
    inbound = [packet for packet in console.dma.packets
               if packet[0] == "EE -> IOP"]
    require(bool(inbound), "BOOT-11a",
            "nothing crossed to the IOP with a header in front of it")
    astray = [f"{address:#x}" for _, address, _, _ in inbound
              if address != smcom]
    require(not astray, "BOOT-11a",
            f"headers sent the data to {astray}, not to the address the IOP "
            f"published in SMCOM ({smcom:#010x})")
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

    if console.dma.packets:
        print("\nframed packets (BOOT-11), as the receiving channel read them:")
        for direction, address, words, header in console.dma.packets:
            print(f"   {direction}  {words:5d} words to {address:#010x}"
                  f"   header {header:#010x}")

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
        if console.dma.tags:
            print("\nchain tags walked (BOOT-11b):")
            for what, at, tag, pointer in console.dma.tags:
                print(f"   {what}  at {at:#010x}: {tag:#010x} {pointer:#010x}"
                      f"   id {(tag >> 28) & 7}, {tag & 0xFFFF} quadwords")
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
