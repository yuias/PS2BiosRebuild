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

    def _set(self, i: int, v: int) -> None:
        if i:
            self.r[i] = v & SIGN32

    def _branch(self, target: int) -> None:
        self.next_pc = target & SIGN32

    def step(self) -> None:
        pc = self.pc
        instr = self.bus.read(pc, 4)
        self.pc = self.next_pc
        self.next_pc = (self.pc + 4) & SIGN32
        self.steps += 1
        self._execute(instr, pc)

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
            elif fn == 0x0C: self.stop_reason = f"syscall at {pc:#x}"
            elif fn == 0x0D: self.stop_reason = f"break at {pc:#x}"
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
            elif rs == 0x10: pass                    # rfe: no exceptions modelled
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


def run(image: pathlib.Path, max_steps: int, trace: int) -> int:
    bus = Bus(image.read_bytes())
    cpu = Cpu(bus)

    seen_post = 0
    while cpu.steps < max_steps and cpu.stop_reason is None:
        if trace and cpu.steps < trace:
            print(f"  {cpu.steps:6d} pc={cpu.pc:#010x}")
        cpu.step()
        if len(bus.post) > seen_post:
            pc, value = bus.post[-1]
            seen_post = len(bus.post)
            print(f"POST {value:#04x}   at {pc:#010x}   step {cpu.steps}")
            if value == 0xFA:
                cpu.stop_reason = "POST 0xfa: boot block could not find its module"

    print(f"\nsteps executed: {cpu.steps}")
    print(f"POST sequence:  {[hex(v) for _, v in bus.post]}")
    print(f"final pc:       {cpu.pc:#010x}")
    if cpu.stop_reason:
        print(f"stopped:        {cpu.stop_reason}")
    elif cpu.steps >= max_steps:
        print("stopped:        step limit reached")
    if bus.unmapped:
        addrs = sorted(bus.unmapped)[:8]
        print(f"unmapped access at: {[hex(a) for a in addrs]}"
              f"{' ...' if len(bus.unmapped) > 8 else ''}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("image", type=pathlib.Path)
    parser.add_argument("--max-steps", type=lambda s: int(s, 0), default=20_000_000)
    parser.add_argument("--trace", type=int, default=0,
                        help="print the pc of the first N steps")
    args = parser.parse_args()
    return run(args.image, args.max_steps, args.trace)


if __name__ == "__main__":
    sys.exit(main())
