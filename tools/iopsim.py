#!/usr/bin/env python3
"""Run a PS2 BIOS image's IOP boot on a minimal simulated IOP.

The requirements this project most needs to verify are all IOP-side and
structural -- the POST trace and step ordering of `docs/spec/03-boot-chain.md`,
and the run-time half of `docs/spec/02-module-abi.md` (stub patching, library
registration, supersession, residency). A game emulator runs that code but
surfaces almost none of it; this runs the same code and reports exactly those
things.

Scope is deliberately small. It models an R3000-class core (MIPS-I, no FPU, no
GTE) and only the hardware the boot path touches: RAM, the scratchpad, the ROM
window, and an I/O range whose interesting register is the POST byte at
0xBF802070. Nothing here aims to run a game.

    tools/iopsim.py assets/SCPH-50000.bin
    tools/iopsim.py assets/SCPH-50000.bin --trace 40
    tools/iopsim.py build/rom.bin --max-steps 50000000

The simulator is validated by the reference ROM: booting it must reproduce the
POST sequence `docs/spec/03-boot-chain.md` §BOOT-5a predicts for a retail
machine. A simulator that cannot do that is not trustworthy enough to gate our
own image.
"""

from __future__ import annotations

import argparse
import pathlib
import sys

RAM_SIZE = 2 * 1024 * 1024
SCRATCH_BASE, SCRATCH_SIZE = 0x1F800000, 0x400
# Wide enough to swallow the CDVD/DEV9 windows the kernel probes.
IO_BASE, IO_END = 0x1F400000, 0x1FA00000
ROM_BASE = 0x1FC00000
RESET_PC = 0xBFC00000

POST_REG = 0x1F802070
DISCRIMINATOR_REG = 0x1F801450   # BOOT-3: bit 3 selects the alternate machine
RAM_SIZE_REG = 0x1F801060

# BOOT-1 needs PRId >= 0x59 to mean "EE"; BOOT-3 needs PRId >= 0x10 to mean
# "retail". Any value in between satisfies both, and the boot path never uses
# it for anything else.
IOP_PRID = 0x1F

# CP0 Status bit 16: with the cache isolated, stores go to the data cache
# rather than to memory. The boot chain relies on it to invalidate lines.
STATUS_ISC = 0x00010000
STATUS_BEV = 0x00400000          # boot exception vectors, in ROM
STATUS_MODE_MASK = 0x3F          # the KU/IE three-deep stack

COP0_SR, COP0_CAUSE, COP0_EPC = 12, 13, 14
EXC_SYSCALL, EXC_BREAK = 8, 9
VECTOR_NORMAL, VECTOR_BEV = 0x80000080, 0xBFC00180

SIGN32 = 0xFFFFFFFF


def s32(x: int) -> int:
    x &= SIGN32
    return x - (1 << 32) if x & 0x80000000 else x


class Bus:
    def __init__(self, rom: bytes):
        self.ram = bytearray(RAM_SIZE)
        self.scratch = bytearray(SCRATCH_SIZE)
        self.rom = rom
        self.post: list[tuple[int, int]] = []   # (pc, value)
        self.io: dict[int, int] = {DISCRIMINATOR_REG: 0}
        self.unmapped: set[int] = set()

    def _region(self, addr: int):
        phys = addr & 0x1FFFFFFF
        if phys < RAM_SIZE:
            return self.ram, phys
        if SCRATCH_BASE <= phys < SCRATCH_BASE + SCRATCH_SIZE:
            return self.scratch, phys - SCRATCH_BASE
        if ROM_BASE <= phys < ROM_BASE + len(self.rom):
            return self.rom, phys - ROM_BASE
        return None, phys

    def read(self, addr: int, size: int) -> int:
        buf, off = self._region(addr)
        if buf is None:
            if IO_BASE <= off < IO_END:
                return self.io.get(off, 0)
            self.unmapped.add(off)
            return 0
        return int.from_bytes(buf[off:off + size], "little")

    def write(self, addr: int, size: int, value: int, pc: int) -> None:
        buf, off = self._region(addr)
        if buf is None:
            if off == POST_REG:
                self.post.append((pc, value & 0xFF))
            elif IO_BASE <= off < IO_END:
                self.io[off] = value
            else:
                self.unmapped.add(off)
            return
        if buf is self.rom:
            return                                  # writes to ROM are dropped
        buf[off:off + size] = (value & ((1 << (8 * size)) - 1)).to_bytes(
            size, "little")


class Cpu:
    def __init__(self, bus: Bus):
        self.bus = bus
        self.r = [0] * 32
        self.hi = self.lo = 0
        self.pc = RESET_PC
        self.next_pc = RESET_PC + 4
        self.cop0 = [0] * 32
        self.cop0[15] = IOP_PRID
        self.steps = 0
        self.stop_reason: str | None = None
        self.branched = False        # did the last instruction take a branch?
        self.in_delay = False        # is the current one a delay slot?
        self.exceptions = 0

    def _set(self, i: int, v: int) -> None:
        if i:
            self.r[i] = v & SIGN32

    def _branch(self, target: int) -> None:
        self.next_pc = target & SIGN32
        self.branched = True

    def step(self) -> None:
        pc = self.pc
        instr = self.bus.read(pc, 4)
        self.pc = self.next_pc
        self.next_pc = (self.pc + 4) & SIGN32
        self.in_delay, self.branched = self.branched, False
        self.steps += 1
        self._execute(instr, pc)

    def _raise(self, code: int, pc: int) -> None:
        """Enter the R3000 exception path.

        EPC points at the branch, not the delay slot, when the faulting
        instruction is in one -- the handler restarts the branch so the delay
        slot is not executed twice.
        """
        sr = self.cop0[COP0_SR]
        cause = self.cop0[COP0_CAUSE] & ~0x7C
        cause |= (code << 2)
        if self.in_delay:
            cause |= 0x80000000
        self.cop0[COP0_CAUSE] = cause & SIGN32
        self.cop0[COP0_EPC] = (pc - 4 if self.in_delay else pc) & SIGN32
        # Push the KU/IE stack two places, entering kernel mode with
        # interrupts disabled.
        self.cop0[COP0_SR] = (sr & ~STATUS_MODE_MASK) | ((sr << 2) & STATUS_MODE_MASK)
        target = VECTOR_BEV if sr & STATUS_BEV else VECTOR_NORMAL
        self.pc = target
        self.next_pc = (target + 4) & SIGN32
        self.branched = False
        self.exceptions += 1

    def _execute(self, instr: int, pc: int) -> None:
        op = instr >> 26
        rs, rt = (instr >> 21) & 31, (instr >> 16) & 31
        rd, sa = (instr >> 11) & 31, (instr >> 6) & 31
        fn = instr & 63
        imm = instr & 0xFFFF
        simm = imm - 0x10000 if imm & 0x8000 else imm
        r = self.r

        if op == 0:
            if fn == 0x00: self._set(rd, r[rt] << sa)
            elif fn == 0x02: self._set(rd, r[rt] >> sa)
            elif fn == 0x03: self._set(rd, s32(r[rt]) >> sa)
            elif fn == 0x04: self._set(rd, r[rt] << (r[rs] & 31))
            elif fn == 0x06: self._set(rd, r[rt] >> (r[rs] & 31))
            elif fn == 0x07: self._set(rd, s32(r[rt]) >> (r[rs] & 31))
            elif fn == 0x08: self._branch(r[rs])
            elif fn == 0x09:
                target = r[rs]
                self._set(rd if rd else 31, (pc + 8) & SIGN32)
                self._branch(target)
            elif fn == 0x0C: self._raise(EXC_SYSCALL, pc)
            elif fn == 0x0D: self._raise(EXC_BREAK, pc)
            elif fn == 0x10: self._set(rd, self.hi)
            elif fn == 0x11: self.hi = r[rs]
            elif fn == 0x12: self._set(rd, self.lo)
            elif fn == 0x13: self.lo = r[rs]
            elif fn == 0x18:
                p = s32(r[rs]) * s32(r[rt])
                self.lo, self.hi = p & SIGN32, (p >> 32) & SIGN32
            elif fn == 0x19:
                p = r[rs] * r[rt]
                self.lo, self.hi = p & SIGN32, (p >> 32) & SIGN32
            elif fn == 0x1A:
                n, d = s32(r[rs]), s32(r[rt])
                if d == 0:
                    self.lo, self.hi = (0xFFFFFFFF if n >= 0 else 1), n & SIGN32
                else:
                    q = abs(n) // abs(d)
                    if (n < 0) != (d < 0):
                        q = -q
                    self.lo, self.hi = q & SIGN32, (n - q * d) & SIGN32
            elif fn == 0x1B:
                n, d = r[rs], r[rt]
                if d == 0:
                    self.lo, self.hi = SIGN32, n
                else:
                    self.lo, self.hi = (n // d) & SIGN32, (n % d) & SIGN32
            elif fn == 0x20 or fn == 0x21: self._set(rd, r[rs] + r[rt])
            elif fn == 0x22 or fn == 0x23: self._set(rd, r[rs] - r[rt])
            elif fn == 0x24: self._set(rd, r[rs] & r[rt])
            elif fn == 0x25: self._set(rd, r[rs] | r[rt])
            elif fn == 0x26: self._set(rd, r[rs] ^ r[rt])
            elif fn == 0x27: self._set(rd, ~(r[rs] | r[rt]))
            elif fn == 0x2A: self._set(rd, 1 if s32(r[rs]) < s32(r[rt]) else 0)
            elif fn == 0x2B: self._set(rd, 1 if r[rs] < r[rt] else 0)
            else: self.stop_reason = f"unknown SPECIAL fn {fn:#04x} at {pc:#x}"
        elif op == 1:
            take = s32(r[rs]) < 0 if rt & 1 == 0 else s32(r[rs]) >= 0
            if rt & 0x1E == 0x10:
                self._set(31, (pc + 8) & SIGN32)
            if take:
                self._branch((self.pc + (simm << 2)) & SIGN32)
        elif op == 2 or op == 3:
            if op == 3:
                self._set(31, (pc + 8) & SIGN32)
            self._branch((self.pc & 0xF0000000) | ((instr & 0x3FFFFFF) << 2))
        elif op == 4:
            if r[rs] == r[rt]: self._branch((self.pc + (simm << 2)) & SIGN32)
        elif op == 5:
            if r[rs] != r[rt]: self._branch((self.pc + (simm << 2)) & SIGN32)
        elif op == 6:
            if s32(r[rs]) <= 0: self._branch((self.pc + (simm << 2)) & SIGN32)
        elif op == 7:
            if s32(r[rs]) > 0: self._branch((self.pc + (simm << 2)) & SIGN32)
        elif op == 8 or op == 9: self._set(rt, r[rs] + simm)
        elif op == 0x0A: self._set(rt, 1 if s32(r[rs]) < simm else 0)
        elif op == 0x0B: self._set(rt, 1 if r[rs] < (simm & SIGN32) else 0)
        elif op == 0x0C: self._set(rt, r[rs] & imm)
        elif op == 0x0D: self._set(rt, r[rs] | imm)
        elif op == 0x0E: self._set(rt, r[rs] ^ imm)
        elif op == 0x0F: self._set(rt, imm << 16)
        elif op == 0x10:
            if rs == 0: self._set(rt, self.cop0[rd])
            elif rs == 4: self.cop0[rd] = r[rt]
            elif rs == 0x10:                         # rfe: pop the KU/IE stack
                sr = self.cop0[COP0_SR]
                self.cop0[COP0_SR] = (sr & ~0xF) | ((sr >> 2) & 0xF)
            else: self.stop_reason = f"unknown COP0 rs {rs:#x} at {pc:#x}"
        elif op in (0x11, 0x12, 0x13):
            pass                                     # no coprocessors on this path
        elif op == 0x20: self._set(rt, self._lb(r[rs] + simm))
        elif op == 0x21: self._set(rt, self._lh(r[rs] + simm, signed=True))
        elif op == 0x23: self._set(rt, self.bus.read((r[rs] + simm) & SIGN32, 4))
        elif op == 0x24: self._set(rt, self._lb(r[rs] + simm) & 0xFF)
        elif op == 0x25: self._set(rt, self._lh(r[rs] + simm, signed=False))
        elif op == 0x22 or op == 0x26:
            self._set(rt, self._lwlr(op, r[rs] + simm, r[rt]))
        elif op == 0x28: self._store((r[rs] + simm), 1, r[rt], pc)
        elif op == 0x29: self._store((r[rs] + simm), 2, r[rt], pc)
        elif op == 0x2B: self._store((r[rs] + simm), 4, r[rt], pc)
        elif op == 0x2A or op == 0x2E: self._swlr(op, r[rs] + simm, r[rt], pc)
        else:
            self.stop_reason = f"unknown opcode {op:#04x} at {pc:#x}"

    def _store(self, addr: int, size: int, value: int, pc: int) -> None:
        """A store, honouring the R3000's isolate-cache mode.

        With `Status.IsC` set, stores land in the data cache instead of memory.
        The boot chain uses that as its cache-invalidate idiom: isolate, store
        zeroes over a range of line addresses, un-isolate. We model no cache, so
        the correct behaviour is to drop the store -- passing it through would
        wipe whatever really lives at those addresses, which is exactly what a
        freshly loaded module does.
        """
        addr &= SIGN32
        # Isolation only affects the cached segments. KSEG1 is uncached, which
        # is why the boot chain can still write its POST code while isolated.
        if self.cop0[12] & STATUS_ISC and not (0xA0000000 <= addr < 0xC0000000):
            return
        self.bus.write(addr, size, value, pc)

    def _lb(self, addr: int) -> int:
        v = self.bus.read(addr & SIGN32, 1)
        return v - 256 if v & 0x80 else v

    def _lh(self, addr: int, signed: bool) -> int:
        v = self.bus.read(addr & SIGN32, 2)
        if signed and v & 0x8000:
            v -= 0x10000
        return v & SIGN32

    def _lwlr(self, op: int, addr: int, old: int) -> int:
        """Unaligned loads, little-endian: shift = byte offset within the word."""
        addr &= SIGN32
        word = self.bus.read(addr & ~3, 4)
        shift = (addr & 3) * 8
        if op == 0x22:                               # lwl: high part of the word
            keep = (1 << (24 - shift)) - 1
            return ((word << (24 - shift)) | (old & keep)) & SIGN32
        keep = (0xFFFFFFFF << (32 - shift)) & SIGN32 if shift else 0
        return ((word >> shift) | (old & keep)) & SIGN32

    def _swlr(self, op: int, addr: int, value: int, pc: int) -> None:
        addr &= SIGN32
        base = addr & ~3
        word = self.bus.read(base, 4)
        shift = (addr & 3) * 8
        if op == 0x2A:                               # swl
            keep = ~(0xFFFFFFFF >> (24 - shift)) & SIGN32
            merged = (word & keep) | (value >> (24 - shift))
        else:                                        # swr
            keep = ~((0xFFFFFFFF << shift) & SIGN32) & SIGN32
            merged = (word & keep) | ((value << shift) & SIGN32)
        self._store(base, 4, merged & SIGN32, pc)


def scanLibraries(bus: "Bus") -> tuple[list, list]:
    """Find the library tables the boot left in RAM.

    Registration links a table into LOADCORE's registry in place, so every
    resident library is an export header sitting in RAM. Import headers are
    there too, and their stubs record whether the binder reached them: an
    unbound stub still starts with `jr $ra`, a bound one with a `j`
    (`docs/spec/02-module-abi.md` IRX-9).
    """
    ram = bus.ram
    exports, imports = [], []
    for at in range(0, len(ram) - 24, 4):
        magic = int.from_bytes(ram[at:at + 4], "little")
        version = int.from_bytes(ram[at + 8:at + 10], "little")
        flags = int.from_bytes(ram[at + 10:at + 12], "little")
        raw = ram[at + 12:at + 20]
        tag = raw.split(b"\0")[0].decode("ascii", "replace")
        # Library tags are short lowercase identifiers; insisting on that keeps
        # ordinary string data from matching the header shape below.
        if len(tag) < 4 or not all(
                c.islower() or c.isdigit() or c == "_" for c in tag):
            continue
        if magic == 0x41E00000:
            first = int.from_bytes(ram[at + 0x14:at + 0x18], "little")
            bound = first != 0 and (first >> 26) == 2      # a `j`
            imports.append((at, tag, version, flags, bound))
            continue
        # A registered export table has had its magic overwritten by the
        # registry link, so recognise it by shape: a plausible link, a BCD
        # version, and a first entry pointing into RAM.
        entry = int.from_bytes(ram[at + 0x14:at + 0x18], "little")
        plausible_link = magic == 0 or magic < len(ram)
        # Every library in the reference is 1.xx or 2.xx, BCD.
        plausible_ver = (0 < version >> 8 <= 9
                         and (version & 0xF) <= 9 and (version >> 4 & 0xF) <= 9)
        if (plausible_link and plausible_ver and flags < 0x10
                and 0 < entry < len(ram)):
            exports.append((at, tag, version, flags))
    return exports, imports


class FreeWatcher:
    """Records the module teardowns of `docs/spec/02-module-abi.md` IRX-12b.

    A module whose entry asks not to stay is released through `sysmem`
    ordinal 5. That ordinal's address is not known until `SYSMEM` has been
    loaded *and relocated*, so it is resolved lazily: before relocation the
    table holds small file offsets, after it they are RAM addresses above the
    table itself, which is the test used here.
    """

    SYSMEM_TABLE = 0x830             # where the boot leaves it (analysis 05)
    FREE_ORDINAL = 5

    def __init__(self, bus: "Bus") -> None:
        self.bus = bus
        self.free_entry: int | None = None
        self.frees: list[tuple[int, int]] = []      # (address, step)

    def resolve(self) -> None:
        table = self.SYSMEM_TABLE
        first = self.bus.read(table + 0x14, 4)
        if first > table:                            # relocated, so usable
            self.free_entry = self.bus.read(
                table + 0x14 + self.FREE_ORDINAL * 4, 4)

    def observe(self, cpu: "Cpu") -> None:
        if self.free_entry is None:
            if cpu.steps % 4096 == 0:
                self.resolve()
            return
        if cpu.pc == self.free_entry:
            self.frees.append((cpu.r[4], cpu.steps))


def reportLibraries(exports: list, imports: list) -> None:
    print(f"\nlibraries registered in RAM: {len(exports)}")
    for at, tag, version, flags in exports:
        print(f"   {at:#08x}  {tag:<10} v{version >> 8:x}.{version & 0xFF:02x}"
              f"  flags {flags:#x}{'  (pinned)' if flags & 1 else ''}")
    if imports:
        bound = sum(1 for i in imports if i[4])
        print(f"import tables in RAM: {len(imports)}, of which bound: {bound}")
        for at, tag, version, flags, ok in imports:
            if not ok:
                print(f"   UNBOUND {at:#08x}  {tag} "
                      f"v{version >> 8:x}.{version & 0xFF:02x}")


# What a correct IOP boot must achieve, per docs/spec/02 and docs/spec/03.
EXPECTED_POST = [0xFC, 0x02, 0x03, 0x04, 0x05, 0x08, 0x09]
EXPECTED_LIBRARIES = [
    "sysmem", "loadcore", "excepman", "intrman", "ssbusc", "dmacman",
    "timrman", "sysclib", "stdio", "heaplib", "thbase", "thevent", "thsemap",
    "thmsgbx", "thfpool", "thvpool", "thrdman", "vblank", "ioman", "modload",
    "romdrv", "stdio", "sifman",
]
PINNED_LIBRARIES = {"thrdman"}
EXPECTED_FREES = 4               # IRX-12a, observed: two variant halves and two more
SINGLE_VARIANT_LIBRARIES = {"intrman", "timrman"}


def checkBoot(bus: "Bus", exports: list, imports: list,
              watcher: "FreeWatcher") -> list[str]:
    """Judge a boot against the specifications. Returns the failures."""
    problems: list[str] = []

    def require(ok: bool, requirement: str, detail: str) -> None:
        if not ok:
            problems.append(f"{requirement}: {detail}")

    post = [v for _, v in bus.post]
    require(post == EXPECTED_POST, "BOOT-5a",
            f"POST sequence {[hex(v) for v in post]} != "
            f"{[hex(v) for v in EXPECTED_POST]}")

    tags = [tag for _, tag, _, _ in exports]
    require(tags == EXPECTED_LIBRARIES, "IRX-10a",
            f"registered libraries {tags} != {EXPECTED_LIBRARIES}")

    # IRX-11a: SYSCLIB publishes stdio 1.01 and STDIO supersedes it with 1.02.
    stdio = [v for _, tag, v, _ in exports if tag == "stdio"]
    require(stdio == [0x0101, 0x0102], "IRX-11a",
            f"stdio versions {[hex(v) for v in stdio]} != ['0x101', '0x102'] "
            f"-- the provisional stdio was not superseded")

    # IRX-10b: the pin is set at run time, and only where it belongs.
    pinned = {tag for _, tag, _, flags in exports if flags & 1}
    require(pinned == PINNED_LIBRARIES, "IRX-10b",
            f"pinned libraries {sorted(pinned)} != {sorted(PINNED_LIBRARIES)}")

    # A P/I pair must resolve to exactly one resident variant (analysis 06).
    for tag in SINGLE_VARIANT_LIBRARIES:
        require(tags.count(tag) == 1, "variant selection",
                f"{tag} registered {tags.count(tag)} times, want 1")

    unbound = [(at, tag) for at, tag, _, _, ok in imports if not ok]
    require(not unbound, "IRX-9",
            f"{len(unbound)} unbound import tables, e.g. "
            f"{[(hex(a), t) for a, t in unbound[:3]]}")
    require(bool(imports), "IRX-9", "no import tables found at all")

    # IRX-12: modules that ask not to stay really are torn down, and the
    # release is on a 0x100 boundary. The reference frees four during the boot
    # -- the rejected halves of the two P/I variant pairs among them.
    require(len(watcher.frees) >= EXPECTED_FREES, "IRX-12a",
            f"{len(watcher.frees)} module images were released, want at least "
            f"{EXPECTED_FREES}: no module was observed asking to be freed")
    misaligned = [address for address, _ in watcher.frees if address % 0x100]
    require(not misaligned, "IRX-12b",
            f"released bases {[hex(a) for a in misaligned]} are not rounded "
            f"down to a 0x100 boundary")

    require(not bus.unmapped - {0x1D000020, 0x1D000060}, "hardware",
            f"unexpected unmapped access: "
            f"{[hex(a) for a in sorted(bus.unmapped)][:6]}")
    return problems


def run(image: pathlib.Path, max_steps: int, trace: int, check: bool) -> int:
    bus = Bus(image.read_bytes())
    cpu = Cpu(bus)

    watcher = FreeWatcher(bus)
    seen_post = 0
    while cpu.steps < max_steps and cpu.stop_reason is None:
        if trace and cpu.steps < trace:
            print(f"  {cpu.steps:6d} pc={cpu.pc:#010x}")
        watcher.observe(cpu)
        cpu.step()
        if len(bus.post) > seen_post:
            pc, value = bus.post[-1]
            seen_post = len(bus.post)
            if not check:
                print(f"POST {value:#04x}   at {pc:#010x}   step {cpu.steps}")
            if value == 0xFA:
                cpu.stop_reason = "POST 0xfa: boot block could not find its module"

    exports, imports = scanLibraries(bus)

    if check:
        problems = checkBoot(bus, exports, imports, watcher)
        for p in problems:
            print(f"{image}: {p}", file=sys.stderr)
        if problems:
            print(f"\n{image}: {len(problems)} failure(s)", file=sys.stderr)
            return 1
        print(f"{image}: ok -- POST {[hex(v) for _, v in bus.post]}, "
              f"{len(exports)} libraries, {len(imports)} import tables all bound")
        return 0

    print(f"\nsteps executed: {cpu.steps}")
    print(f"POST sequence:  {[hex(v) for _, v in bus.post]}")
    print(f"final pc:       {cpu.pc:#010x}")
    if cpu.stop_reason:
        print(f"stopped:        {cpu.stop_reason}")
    elif cpu.steps >= max_steps:
        print("stopped:        step limit reached")
    if watcher.frees:
        print(f"module images released (IRX-12): {len(watcher.frees)}")
        for address, step in watcher.frees:
            print(f"   {address:#08x}  at step {step}")
    if bus.unmapped:
        addrs = sorted(bus.unmapped)[:8]
        print(f"unmapped access at: {[hex(a) for a in addrs]}"
              f"{' ...' if len(bus.unmapped) > 8 else ''}")
    reportLibraries(exports, imports)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("image", type=pathlib.Path)
    parser.add_argument("--max-steps", type=lambda s: int(s, 0), default=20_000_000)
    parser.add_argument("--trace", type=int, default=0,
                        help="print the pc of the first N steps")
    parser.add_argument("--check", action="store_true",
                        help="judge the boot against docs/spec; exit 1 on any "
                             "failure")
    args = parser.parse_args()
    return run(args.image, args.max_steps, args.trace, args.check)


if __name__ == "__main__":
    sys.exit(main())
