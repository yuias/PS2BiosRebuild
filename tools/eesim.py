#!/usr/bin/env python3
"""Run a PS2 BIOS image's EE boot on a minimal simulated R5900.

`tools/iopsim.py` executes the IOP half of the boot and judges it against
`docs/spec/03-boot-chain.md`. This is its counterpart for the other CPU: the
EE requirements of `docs/spec/04-ee-kernel.md` and `docs/spec/05-ee-syscall-abi.md`
were described and statically checked but never executed, which was the main
asymmetry left in the project.

Scope is deliberately small, and smaller than a game emulator's by a wide
margin. It models the R5900's integer core -- 128-bit registers, the 64-bit
instructions, `lq`/`sq`, `eret` -- plus RAM, the scratchpad, the ROM window and
a permissive I/O space. There is no GS, no VU, no timing and no cache. Nothing
here aims to run a game; it aims to run the reset path and the syscall
dispatcher and say whether they behaved as specified.

    tools/eesim.py assets/SCPH-50000.bin              # report the boot
    tools/eesim.py assets/SCPH-50000.bin --check      # judge it, exit 1 on failure
    tools/eesim.py assets/SCPH-50000.bin --trace 40   # first 40 instructions
    tools/eesim.py assets/SCPH-50000.bin --syscall 0x14 1    # call one syscall

It also runs an EE executable on its own, with no ROM and no kernel under it,
which is how a self-contained routine inside one can be executed rather than
read (`docs/analysis/27-osdsys-payload.md`):

    tools/eesim.py <outdir>/OSDSYS --call 0x100af8 0x100d80 0x200000 \
                   --dump 0x200000 result <outdir>/OSDSYS.expanded

Like `iopsim.py`, the simulator is validated by the reference image: booting it
must reproduce what the specifications predict. A simulator that cannot do that
is not trustworthy enough to gate our own image.
"""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys

RAM_SIZE = 32 * 1024 * 1024
SCRATCH_BASE, SCRATCH_SIZE = 0x70000000, 0x4000
ROM_BASE = 0x1FC00000
RESET_PC = 0xBFC00000
KERNEL_ENTRY = 0x80001000

MASK32 = 0xFFFFFFFF
MASK64 = 0xFFFFFFFFFFFFFFFF
MASK128 = (1 << 128) - 1

# CP0 registers used by the boot path and the exception entry.
COP0_INDEX, COP0_RANDOM, COP0_ENTRYLO0, COP0_ENTRYLO1 = 0, 1, 2, 3
COP0_PAGEMASK, COP0_ENTRYHI = 5, 10
COP0_COUNT, COP0_COMPARE, COP0_STATUS, COP0_CAUSE, COP0_EPC = 9, 11, 12, 13, 14
COP0_PRID, COP0_CONFIG = 15, 16
COP0_ERROREPC = 30

STATUS_EXL, STATUS_ERL, STATUS_BEV = 0x2, 0x4, 0x00400000

EXC_SYSCALL = 8

# PRId of an EE. `spec/03` BOOT-1's discriminator wants >= 0x59 to take the EE
# path, and `spec/04` EE-1 is only reached that way.
EE_PRID = 0x2E20

TLB_ENTRIES = 48

# T0..T3 COUNT. The only registers whose *value over time* the boot depends on.
TIMER_COUNTS = (0x10000000, 0x10000800, 0x10001000, 0x10001800)

SIO_TX = 0x1000F180              # the EE's serial console, where the ROM prints
INTC_MASK = 0x1000F010           # write-to-toggle (spec/04 EE-8g)
DMAC_STATUS = 0x1000E010         # upper half toggles, lower half clears
MEMORY_CONTROLLER_ID = 0x1000F520

# `RDRAM`'s identification of the memory controller: it reads the register
# above and looks the value up in its own table, hanging the machine if it
# misses. The accepted values are in the ROM at 0x9FC43C80 as 12-byte records
# terminated by -1; this is the first of them that a retail image accepts.
CONTROLLER_ID = 0x00002000


def s32(x: int) -> int:
    x &= MASK32
    return x - (1 << 32) if x & 0x80000000 else x


def s64(x: int) -> int:
    x &= MASK64
    return x - (1 << 64) if x & 0x8000000000000000 else x


def sign32to64(x: int) -> int:
    """A 32-bit result as the R5900 stores it: sign-extended into 64 bits."""
    return s32(x) & MASK64


class Bus:
    """RAM, scratchpad, ROM and an I/O space that answers rather than faults.

    The boot path probes hardware this simulator does not model. Returning
    zero and recording the address is more useful than refusing: the run gets
    further, and `--io` shows exactly what was touched.
    """

    def __init__(self, rom: bytes) -> None:
        self.ram = bytearray(RAM_SIZE)
        self.scratch = bytearray(SCRATCH_SIZE)
        self.rom = rom
        self.io: dict[int, int] = {}
        self.io_reads: dict[int, int] = {}
        self.io_writes: dict[int, int] = {}
        self.console = bytearray()
        self.cycles = 0
        self.io[MEMORY_CONTROLLER_ID] = CONTROLLER_ID

    def region(self, address: int):
        """Resolve an address to (buffer, offset), or (None, physical)."""
        if SCRATCH_BASE <= address < SCRATCH_BASE + SCRATCH_SIZE:
            return self.scratch, address - SCRATCH_BASE
        physical = address & 0x1FFFFFFF
        if physical < RAM_SIZE:
            return self.ram, physical
        if ROM_BASE <= physical < ROM_BASE + len(self.rom):
            return self.rom, physical - ROM_BASE
        return None, physical

    def read(self, address: int, size: int) -> int:
        buffer, offset = self.region(address & 0xFFFFFFFF)
        if buffer is None:
            self.io_reads[offset] = self.io_reads.get(offset, 0) + 1
            if offset in TIMER_COUNTS:
                # The one device that must actually move: the reset path waits
                # for a counter to advance before trusting RDRAM, and a
                # constant register would spin there forever.
                return (self.cycles >> 4) & 0xFFFF
            return self.io.get(offset, 0) & ((1 << (8 * size)) - 1)
        return int.from_bytes(buffer[offset:offset + size], "little")

    def write(self, address: int, size: int, value: int) -> None:
        buffer, offset = self.region(address & 0xFFFFFFFF)
        if buffer is None:
            self.io_writes[offset] = self.io_writes.get(offset, 0) + 1
            self.writeDevice(offset, value & ((1 << (8 * size)) - 1))
            return
        if buffer is self.rom:
            return                                  # writes to ROM are dropped
        buffer[offset:offset + size] = (
            value & ((1 << (8 * size)) - 1)).to_bytes(size, "little")

    def writeDevice(self, offset: int, value: int) -> None:
        """Three registers do not simply store what is written to them."""
        if offset == SIO_TX:
            self.console.append(value & 0xFF)       # the EE's serial console
            return
        if offset == INTC_MASK:
            # Write-to-toggle. `spec/04` EE-8g turns on this being modelled:
            # a register that merely stored would make the enable/disable pair
            # look correct when it is not.
            self.io[offset] = self.io.get(offset, 0) ^ value
            return
        if offset == DMAC_STATUS:
            # The upper half toggles; the lower half is write-one-to-clear.
            current = self.io.get(offset, 0)
            self.io[offset] = ((current & ~(value & 0xFFFF))
                               ^ (value & 0xFFFF0000)) & MASK32
            return
        self.io[offset] = value


class Cpu:
    """The R5900's integer core, to the depth the boot and syscall paths need."""

    def __init__(self, bus: Bus) -> None:
        self.bus = bus
        self.r = [0] * 32                    # 128 bits each
        self.hi = self.lo = 0
        self.hi1 = self.lo1 = 0              # the R5900's second pipeline
        self.sa = 0
        self.fpr = [0] * 32
        self.fcr = [0] * 32
        self.cop0 = [0] * 32
        self.cop0[COP0_PRID] = EE_PRID
        self.cop0[COP0_RANDOM] = TLB_ENTRIES - 1
        self.tlb: list[tuple[int, int, int, int]] = [(0, 0, 0, 0)] * TLB_ENTRIES
        self.pc = RESET_PC
        self.next_pc = RESET_PC + 4
        self.branched = False
        self.in_delay = False
        self.steps = 0
        self.stop_reason: str | None = None
        self.exceptions = 0
        # Observations the specifications ask about.
        self.cop0_writes: list[tuple[str, int, int]] = []   # (name, value, pc)
        self.tlb_writes: list[tuple[int, tuple[int, int, int, int]]] = []
        self.calls: set[int] = set()
        self.syscalls: list[tuple[int, int]] = []           # (number, pc)

    # -- register access ---------------------------------------------------

    def get(self, index: int) -> int:
        """The low 64 bits, which is what every ordinary instruction uses."""
        return self.r[index] & MASK64

    def set(self, index: int, value: int) -> None:
        if index:
            self.r[index] = value & MASK64        # the upper half is discarded

    def setq(self, index: int, value: int) -> None:
        if index:
            self.r[index] = value & MASK128

    def branch(self, target: int) -> None:
        self.next_pc = target & MASK32
        self.branched = True

    # -- memory ------------------------------------------------------------

    def load(self, address: int, size: int, signed: bool) -> int:
        value = self.bus.read(address, size)
        if signed:
            bits = 8 * size
            if value & (1 << (bits - 1)):
                value -= 1 << bits
        return value & MASK64

    def loadq(self, address: int) -> int:
        address &= ~0xF
        low = self.bus.read(address, 8)
        high = self.bus.read(address + 8, 8)
        return low | (high << 64)

    def storeq(self, address: int, value: int) -> None:
        address &= ~0xF
        self.bus.write(address, 8, value & MASK64)
        self.bus.write(address + 8, 8, (value >> 64) & MASK64)

    # -- execution ---------------------------------------------------------

    def step(self) -> None:
        pc = self.pc
        instruction = self.bus.read(pc, 4)
        self.pc = self.next_pc
        self.next_pc = (self.pc + 4) & MASK32
        self.in_delay, self.branched = self.branched, False
        self.steps += 1
        self.bus.cycles = self.steps
        self.cop0[COP0_COUNT] = (self.cop0[COP0_COUNT] + 1) & MASK32
        self.cop0[COP0_RANDOM] = (self.cop0[COP0_RANDOM] - 1) or (TLB_ENTRIES - 1)
        self.execute(instruction, pc)

    def raiseException(self, code: int, pc: int) -> None:
        status = self.cop0[COP0_STATUS]
        cause = self.cop0[COP0_CAUSE] & ~0x7C
        cause |= code << 2
        if self.in_delay:
            cause |= 0x80000000
        self.cop0[COP0_CAUSE] = cause & MASK32
        if not status & STATUS_EXL:
            self.cop0[COP0_EPC] = (pc - 4 if self.in_delay else pc) & MASK32
            self.cop0[COP0_STATUS] = status | STATUS_EXL
        base = 0xBFC00200 if status & STATUS_BEV else 0x80000000
        self.pc = base + 0x180
        self.next_pc = self.pc + 4
        self.branched = False
        self.exceptions += 1

    def execute(self, instruction: int, pc: int) -> None:
        op = instruction >> 26
        rs, rt = (instruction >> 21) & 31, (instruction >> 16) & 31
        rd, sa = (instruction >> 11) & 31, (instruction >> 6) & 31
        funct = instruction & 0x3F
        immediate = instruction & 0xFFFF
        simm = immediate - 0x10000 if immediate & 0x8000 else immediate
        get, put = self.get, self.set

        if op == 0x00:
            self.special(instruction, pc, rs, rt, rd, sa, funct)
        elif op == 0x01:                              # REGIMM
            value = s64(get(rs))
            take = value < 0 if rt & 1 == 0 else value >= 0
            if rt & 0x1E == 0x10:
                put(31, (pc + 8) & MASK32)
            if take:
                self.branch(self.pc + (simm << 2))
            elif rt & 0x02:                           # the ...l forms nullify
                self.nullifyDelaySlot()
        elif op in (0x02, 0x03):                      # j, jal
            if op == 0x03:
                put(31, (pc + 8) & MASK32)
            target = (self.pc & 0xF0000000) | ((instruction & 0x3FFFFFF) << 2)
            self.calls.add(target)
            self.branch(target)
        elif op in (0x04, 0x05, 0x14, 0x15):          # beq, bne, beql, bnel
            equal = get(rs) == get(rt)
            take = equal if op in (0x04, 0x14) else not equal
            if take:
                self.branch(self.pc + (simm << 2))
            elif op in (0x14, 0x15):
                self.nullifyDelaySlot()
        elif op in (0x06, 0x07, 0x16, 0x17):          # blez, bgtz, and likely
            value = s64(get(rs))
            take = value <= 0 if op in (0x06, 0x16) else value > 0
            if take:
                self.branch(self.pc + (simm << 2))
            elif op in (0x16, 0x17):
                self.nullifyDelaySlot()
        elif op in (0x08, 0x09):                      # addi, addiu
            put(rt, sign32to64(get(rs) + simm))
        elif op in (0x18, 0x19):                      # daddi, daddiu
            put(rt, get(rs) + simm)
        elif op == 0x0A:
            put(rt, 1 if s64(get(rs)) < simm else 0)
        elif op == 0x0B:
            put(rt, 1 if get(rs) < (simm & MASK64) else 0)
        elif op == 0x0C:
            put(rt, get(rs) & immediate)
        elif op == 0x0D:
            put(rt, get(rs) | immediate)
        elif op == 0x0E:
            put(rt, get(rs) ^ immediate)
        elif op == 0x0F:
            put(rt, sign32to64(immediate << 16))
        elif op == 0x10:
            self.cop0Instruction(instruction, pc, rs, rt, rd)
        elif op in (0x11, 0x12):
            self.coprocessor(instruction, op, rs, rt, rd)
        elif op == 0x1C:
            self.multimedia(instruction, pc, rs, rt, rd, funct)
        elif op == 0x1E:                              # lq
            self.setq(rt, self.loadq(get(rs) + simm))
        elif op == 0x1F:                              # sq
            self.storeq(get(rs) + simm, self.r[rt])
        elif op == 0x20:
            put(rt, self.load(get(rs) + simm, 1, True))
        elif op == 0x21:
            put(rt, self.load(get(rs) + simm, 2, True))
        elif op == 0x23:
            put(rt, self.load(get(rs) + simm, 4, True))
        elif op == 0x24:
            put(rt, self.load(get(rs) + simm, 1, False))
        elif op == 0x25:
            put(rt, self.load(get(rs) + simm, 2, False))
        elif op == 0x27:
            put(rt, self.load(get(rs) + simm, 4, False))
        elif op == 0x37:
            put(rt, self.load(get(rs) + simm, 8, False))
        elif op in (0x22, 0x26):                      # lwl, lwr
            put(rt, self.unalignedLoad(op == 0x22, 4, get(rs) + simm, get(rt)))
        elif op in (0x1A, 0x1B):                      # ldl, ldr
            put(rt, self.unalignedLoad(op == 0x1A, 8, get(rs) + simm, get(rt)))
        elif op == 0x28:
            self.bus.write(get(rs) + simm, 1, get(rt))
        elif op == 0x29:
            self.bus.write(get(rs) + simm, 2, get(rt))
        elif op == 0x2B:
            self.bus.write(get(rs) + simm, 4, get(rt))
        elif op == 0x3F:
            self.bus.write(get(rs) + simm, 8, get(rt))
        elif op in (0x2A, 0x2E):                      # swl, swr
            self.unalignedStore(op == 0x2A, 4, get(rs) + simm, get(rt))
        elif op in (0x2C, 0x2D):                      # sdl, sdr
            self.unalignedStore(op == 0x2C, 8, get(rs) + simm, get(rt))
        elif op in (0x2F, 0x33):                      # cache, pref
            pass                                      # no cache is modelled
        elif op == 0x31:                              # lwc1
            self.fpr[rt] = self.bus.read(get(rs) + simm, 4)
        elif op == 0x39:                              # swc1
            self.bus.write(get(rs) + simm, 4, self.fpr[rt])
        elif op == 0x36:                              # lqc2
            pass                                      # VU registers are not modelled
        elif op == 0x3E:                              # sqc2
            pass
        else:
            self.stop_reason = f"unimplemented opcode {op:#04x} at {pc:#010x}"

    def nullifyDelaySlot(self) -> None:
        """A not-taken `...l` branch skips its delay slot."""
        self.pc = self.next_pc
        self.next_pc = (self.pc + 4) & MASK32

    def special(self, instruction: int, pc: int, rs: int, rt: int, rd: int,
                sa: int, funct: int) -> None:
        get, put = self.get, self.set
        if funct == 0x00:
            put(rd, sign32to64(get(rt) << sa))
        elif funct == 0x02:
            put(rd, sign32to64((get(rt) & MASK32) >> sa))
        elif funct == 0x03:
            put(rd, sign32to64(s32(get(rt)) >> sa))
        elif funct == 0x04:
            put(rd, sign32to64(get(rt) << (get(rs) & 31)))
        elif funct == 0x06:
            put(rd, sign32to64((get(rt) & MASK32) >> (get(rs) & 31)))
        elif funct == 0x07:
            put(rd, sign32to64(s32(get(rt)) >> (get(rs) & 31)))
        elif funct == 0x08:                            # jr
            self.branch(get(rs))
        elif funct == 0x09:                            # jalr
            target = get(rs)
            put(rd or 31, (pc + 8) & MASK32)
            self.calls.add(target & MASK32)
            self.branch(target)
        elif funct == 0x0A:                            # movz
            if get(rt) == 0:
                put(rd, get(rs))
        elif funct == 0x0B:                            # movn
            if get(rt) != 0:
                put(rd, get(rs))
        elif funct == 0x0C:                            # syscall
            self.syscalls.append((s32(self.get(3)), pc))
            self.raiseException(EXC_SYSCALL, pc)
        elif funct == 0x0D:
            self.stop_reason = f"break at {pc:#010x}"
        elif funct == 0x0F:                            # sync / sync.p
            pass
        elif funct == 0x10:
            put(rd, self.hi)
        elif funct == 0x11:
            self.hi = get(rs)
        elif funct == 0x12:
            put(rd, self.lo)
        elif funct == 0x13:
            self.lo = get(rs)
        elif funct == 0x14:
            put(rd, get(rt) << (get(rs) & 63))
        elif funct == 0x16:
            put(rd, get(rt) >> (get(rs) & 63))
        elif funct == 0x17:
            put(rd, s64(get(rt)) >> (get(rs) & 63))
        elif funct in (0x18, 0x19):                    # mult, multu
            left = s32(get(rs)) if funct == 0x18 else get(rs) & MASK32
            right = s32(get(rt)) if funct == 0x18 else get(rt) & MASK32
            product = left * right
            self.lo = sign32to64(product & MASK32)
            self.hi = sign32to64((product >> 32) & MASK32)
            put(rd, self.lo)                           # the R5900's third operand
        elif funct in (0x1A, 0x1B):                    # div, divu
            self.lo, self.hi = self.divide(get(rs), get(rt),
                                           signed=funct == 0x1A)
        elif funct in (0x20, 0x21):                    # add, addu
            put(rd, sign32to64(get(rs) + get(rt)))
        elif funct in (0x22, 0x23):                    # sub, subu
            put(rd, sign32to64(get(rs) - get(rt)))
        elif funct == 0x24:
            put(rd, get(rs) & get(rt))
        elif funct == 0x25:
            put(rd, get(rs) | get(rt))
        elif funct == 0x26:
            put(rd, get(rs) ^ get(rt))
        elif funct == 0x27:
            put(rd, ~(get(rs) | get(rt)) & MASK64)
        elif funct == 0x28:                            # mfsa
            put(rd, self.sa)
        elif funct == 0x29:                            # mtsa
            self.sa = get(rs)
        elif funct == 0x2A:
            put(rd, 1 if s64(get(rs)) < s64(get(rt)) else 0)
        elif funct == 0x2B:
            put(rd, 1 if get(rs) < get(rt) else 0)
        elif funct in (0x2C, 0x2D):                    # dadd, daddu
            put(rd, get(rs) + get(rt))
        elif funct in (0x2E, 0x2F):                    # dsub, dsubu
            put(rd, get(rs) - get(rt))
        elif funct == 0x38:
            put(rd, get(rt) << sa)
        elif funct == 0x3A:
            put(rd, get(rt) >> sa)
        elif funct == 0x3B:
            put(rd, s64(get(rt)) >> sa)
        elif funct == 0x3C:
            put(rd, get(rt) << (sa + 32))
        elif funct == 0x3E:
            put(rd, get(rt) >> (sa + 32))
        elif funct == 0x3F:
            put(rd, s64(get(rt)) >> (sa + 32))
        else:
            self.stop_reason = (f"unimplemented SPECIAL {funct:#04x} "
                                f"at {pc:#010x}")

    def divide(self, left: int, right: int, signed: bool) -> tuple[int, int]:
        """Returns (quotient, remainder) as the pair a `LO`/`HI` set wants."""
        if signed:
            numerator, denominator = s32(left), s32(right)
            if denominator == 0:
                return sign32to64(-1 if numerator >= 0 else 1), sign32to64(numerator)
            quotient = abs(numerator) // abs(denominator)
            if (numerator < 0) != (denominator < 0):
                quotient = -quotient
            return (sign32to64(quotient),
                    sign32to64(numerator - quotient * denominator))
        numerator, denominator = left & MASK32, right & MASK32
        if denominator == 0:
            return sign32to64(-1), sign32to64(numerator)
        return (sign32to64(numerator // denominator),
                sign32to64(numerator % denominator))

    def cop0Instruction(self, instruction: int, pc: int, rs: int, rt: int,
                        rd: int) -> None:
        names = {COP0_STATUS: "Status", COP0_CONFIG: "Config",
                 COP0_COUNT: "Count", COP0_COMPARE: "Compare",
                 COP0_ENTRYHI: "EntryHi", COP0_ENTRYLO0: "EntryLo0",
                 COP0_ENTRYLO1: "EntryLo1", COP0_PAGEMASK: "PageMask",
                 COP0_INDEX: "Index", COP0_EPC: "EPC"}
        if rs == 0x00:                                 # mfc0
            self.set(rt, sign32to64(self.cop0[rd]))
        elif rs == 0x04:                               # mtc0
            value = self.get(rt) & MASK32
            self.cop0[rd] = value
            if rd in names:
                self.cop0_writes.append((names[rd], value, pc))
        elif rs == 0x10:                               # CO
            funct = instruction & 0x3F
            if funct == 0x02:                          # tlbwi
                self.writeTlb(self.cop0[COP0_INDEX] % TLB_ENTRIES)
            elif funct == 0x06:                        # tlbwr
                self.writeTlb(self.cop0[COP0_RANDOM] % TLB_ENTRIES)
            elif funct == 0x18:                        # eret
                status = self.cop0[COP0_STATUS]
                if status & STATUS_ERL:
                    self.pc = self.cop0[COP0_ERROREPC]
                    self.cop0[COP0_STATUS] = status & ~STATUS_ERL
                else:
                    self.pc = self.cop0[COP0_EPC]
                    self.cop0[COP0_STATUS] = status & ~STATUS_EXL
                self.next_pc = (self.pc + 4) & MASK32
                self.branched = False
            elif funct == 0x38:                        # ei
                self.cop0[COP0_STATUS] |= 0x10000
            elif funct == 0x39:                        # di
                self.cop0[COP0_STATUS] &= ~0x10000
            else:
                self.stop_reason = (f"unimplemented COP0 op {funct:#04x} "
                                    f"at {pc:#010x}")
        else:
            self.stop_reason = f"unimplemented COP0 rs {rs:#x} at {pc:#010x}"

    def writeTlb(self, index: int) -> None:
        entry = (self.cop0[COP0_PAGEMASK], self.cop0[COP0_ENTRYHI],
                 self.cop0[COP0_ENTRYLO0], self.cop0[COP0_ENTRYLO1])
        self.tlb[index] = entry
        self.tlb_writes.append((index, entry))

    def coprocessor(self, instruction: int, op: int, rs: int, rt: int,
                    rd: int) -> None:
        """COP1 and COP2 moves. No arithmetic: the boot path only configures."""
        if op == 0x11:
            if rs in (0x00, 0x01):                     # mfc1, dmfc1
                self.set(rt, sign32to64(self.fpr[rd]))
            elif rs == 0x02:                           # cfc1
                self.set(rt, sign32to64(self.fcr[rd]))
            elif rs in (0x04, 0x05):                   # mtc1, dmtc1
                self.fpr[rd] = self.get(rt) & MASK32
            elif rs == 0x06:                           # ctc1
                self.fcr[rd] = self.get(rt) & MASK32
        # COP2 is the VU macro mode; nothing here models it.

    def multimedia(self, instruction: int, pc: int, rs: int, rt: int, rd: int,
                   funct: int) -> None:
        """The MMI group.

        The R5900 has a second multiplier pipeline with its own `HI1`/`LO1`,
        and the `...1` instructions here are that pipeline -- not variants of
        the ordinary ones. The RDRAM initialisation uses `div1`, so getting the
        pair wrong is not academic.
        """
        get = self.get
        if funct == 0x10:                              # mfhi1
            self.set(rd, self.hi1)
        elif funct == 0x11:                            # mthi1
            self.hi1 = get(rs)
        elif funct == 0x12:                            # mflo1
            self.set(rd, self.lo1)
        elif funct == 0x13:                            # mtlo1
            self.lo1 = get(rs)
        elif funct in (0x18, 0x19):                    # mult1, multu1
            left = s32(get(rs)) if funct == 0x18 else get(rs) & MASK32
            right = s32(get(rt)) if funct == 0x18 else get(rt) & MASK32
            product = left * right
            self.lo1 = sign32to64(product & MASK32)
            self.hi1 = sign32to64((product >> 32) & MASK32)
            self.set(rd, self.lo1)
        elif funct in (0x1A, 0x1B):                    # div1, divu1
            quotient, remainder = self.divide(get(rs), get(rt),
                                              signed=funct == 0x1A)
            self.lo1, self.hi1 = quotient, remainder
        elif funct == 0x29 and (instruction >> 6) & 0x1F == 0x12:   # por
            self.setq(rd, self.r[rs] | self.r[rt])
        elif funct == 0x29 and (instruction >> 6) & 0x1F == 0x13:   # pnor
            self.setq(rd, ~(self.r[rs] | self.r[rt]) & MASK128)
        elif funct == 0x09 and (instruction >> 6) & 0x1F == 0x12:   # pand
            self.setq(rd, self.r[rs] & self.r[rt])
        elif funct == 0x28 and (instruction >> 6) & 0x1F == 0x10:   # padduw
            # v2.00's reset path uses `padduw rd, $zero, $zero` to clear a
            # register's full 128 bits, which is why this is here at all.
            result = 0
            for lane in range(4):
                left = (self.r[rs] >> (32 * lane)) & MASK32
                right = (self.r[rt] >> (32 * lane)) & MASK32
                result |= min(left + right, MASK32) << (32 * lane)
            self.setq(rd, result)
        elif funct == 0x08 and (instruction >> 6) & 0x1F == 0x0A:   # pcpyld
            self.setq(rd, (self.r[rs] & MASK64) << 64 | (self.r[rt] & MASK64))
        elif funct == 0x28 and (instruction >> 6) & 0x1F == 0x1B:   # pcpyud
            self.setq(rd, ((self.r[rs] >> 64) & MASK64)
                      | (((self.r[rt] >> 64) & MASK64) << 64))
        else:
            self.stop_reason = (f"unimplemented MMI {funct:#04x} "
                                f"at {pc:#010x}")

    def unalignedLoad(self, left: bool, size: int, address: int,
                      previous: int) -> int:
        """`lwl`/`lwr` and their doubleword twins, little-endian."""
        address &= MASK32
        mask = (1 << (8 * size)) - 1
        top = 8 * (size - 1)
        chunk = self.bus.read(address & ~(size - 1), size)
        shift = (address & (size - 1)) * 8
        if left:
            keep = (1 << (top - shift)) - 1
            merged = ((chunk << (top - shift)) | (previous & keep)) & mask
        else:
            keep = (mask << (8 * size - shift)) & mask if shift else 0
            merged = ((chunk >> shift) | (previous & keep)) & mask
        return sign32to64(merged) if size == 4 else merged

    def unalignedStore(self, left: bool, size: int, address: int,
                       value: int) -> None:
        address &= MASK32
        mask = (1 << (8 * size)) - 1
        top = 8 * (size - 1)
        base = address & ~(size - 1)
        chunk = self.bus.read(base, size)
        shift = (address & (size - 1)) * 8
        if left:
            keep = ~(mask >> (top - shift)) & mask
            merged = (chunk & keep) | ((value & mask) >> (top - shift))
        else:
            keep = ~((mask << shift) & mask) & mask
            merged = (chunk & keep) | ((value << shift) & mask)
        self.bus.write(base, size, merged & mask)


# `spec/04` EE-2: the reset path calls `RDRAM`. The reference reaches it by the
# hard-coded address below -- ROM offset 0x41000 -- but EE-2a allows a build to
# place the file elsewhere, so the address is taken from the image's own archive
# when it can be, and this is only the fallback.
RDRAM_ENTRY = 0x9FC41000
ROM_WINDOW = 0x9FC00000

# Where the syscall harness puts its two-instruction caller. Far above the
# kernel's own data, and inside the RAM the kernel has already accounted for.
STUB_ADDRESS = 0x80700000
STUB_RETURN = 0x80700100

BOOT_STEPS = 3_000_000           # reset vector to kernel entry costs ~236k
KERNEL_STEPS = 20_000_000        # kernel entry to its wait for the IOP: it clears
                                 # user memory to the top of RAM on the way
SYSCALL_STEPS = 400_000

# `spec/04` EE-7e is a claim about *width*: the registers are 128 bits and the
# upper halves have to survive a syscall. A marker with something in both
# halves is what makes a 64-bit save look like the failure it is.
CONTEXT_MARKER = 0xFACE0000_00000000_C0DE0000_00000000


def locateRdram(rom: bytes) -> int:
    """Where this image keeps `RDRAM`, which is where EE-2's call must land.

    A rebuild may resolve the file by name rather than carry the reference's
    constant, so the observation point is the file's actual address.
    """
    try:
        import romdir
        entries = romdir.parseEntries(rom, romdir.findTable(rom))
        for entry in entries:
            if entry.name == "RDRAM":
                return ROM_WINDOW + entry.offset
    except Exception:
        pass                                 # not our archive; use the reference's
    return RDRAM_ENTRY


class Machine:
    """A booted EE, and the operations the specifications want performed on it."""

    def __init__(self, rom: bytes) -> None:
        self.bus = Bus(rom)
        self.cpu = Cpu(self.bus)
        self.rdram_entry = locateRdram(rom)
        self.rdram_called = False
        self.stack_at_rdram = 0
        self.reached_kernel = False
        self.kernel_console = ""

    def boot(self, trace: int = 0) -> None:
        """Reset vector to the kernel's wait for the IOP.

        The one thing not executed is `RDRAM` itself. It talks a serial
        protocol to the memory controller that no specification requirement
        concerns, and this simulator has its RAM already; the call is observed
        (EE-2) and then returned from with what it would have found -- the
        size of that RAM (EE-2b), which the kernel keeps as the top of memory
        (SYS-8a). Everything downstream -- EE-3's search and copy, the kernel,
        the syscalls -- is really executed.
        """
        cpu = self.cpu
        while cpu.steps < BOOT_STEPS and cpu.stop_reason is None:
            if trace and cpu.steps < trace:
                print(f"  {cpu.steps:7d}  {cpu.pc:#010x}  "
                      f"{self.bus.read(cpu.pc, 4):08x}")
            if cpu.pc == self.rdram_entry:
                self.rdram_called = True
                self.stack_at_rdram = cpu.get(29) & MASK32
                cpu.set(2, RAM_SIZE)                  # EE-2b
                cpu.pc = cpu.get(31) & MASK32
                cpu.next_pc = (cpu.pc + 4) & MASK32
                cpu.branched = False
                continue
            if cpu.pc == KERNEL_ENTRY:
                self.reached_kernel = True
                break
            cpu.step()
        if not self.reached_kernel:
            cpu.stop_reason = cpu.stop_reason or (
                f"did not reach {KERNEL_ENTRY:#010x} in {BOOT_STEPS} steps")
            return

        start = cpu.steps
        while cpu.steps - start < KERNEL_STEPS and cpu.stop_reason is None:
            cpu.step()
        self.kernel_console = self.bus.console.decode("ascii", "replace")

    def syscall(self, number: int, *arguments: int) -> int | None:
        """Call one syscall on the booted kernel and return `$v0`.

        The caller is two instructions in RAM: `syscall`, then a return to a
        sentinel address. Reaching the sentinel is itself a test of `spec/04`
        EE-7c -- the handler must have advanced `EPC` past the `syscall`, or
        control would come back to the same instruction forever.
        """
        cpu = self.cpu
        self.bus.write(STUB_ADDRESS, 4, 0x0000000C)          # syscall
        self.bus.write(STUB_ADDRESS + 4, 4, 0x03E00008)      # jr $ra
        self.bus.write(STUB_ADDRESS + 8, 4, 0x00000000)      # nop
        cpu.cop0[COP0_STATUS] &= ~(STATUS_EXL | STATUS_ERL)
        cpu.set(3, number & MASK64)                          # $v1: EE-7a
        for index, argument in enumerate(arguments):
            cpu.set(4 + index, argument & MASK64)
        cpu.set(31, STUB_RETURN)
        cpu.pc, cpu.next_pc, cpu.branched = STUB_ADDRESS, STUB_ADDRESS + 4, False
        start = cpu.steps
        while (cpu.pc != STUB_RETURN and cpu.steps - start < SYSCALL_STEPS
               and cpu.stop_reason is None):
            cpu.step()
        if cpu.pc != STUB_RETURN:
            return None
        return s32(cpu.get(2))

    def contextProbe(self, number: int) -> tuple[list[int], int] | None:
        """Call a syscall with a distinct 128-bit marker in every register it
        must preserve, and report which came back changed, along with `$v1`.

        `$k0`/`$k1` are the kernel's own, `$v0` carries the result and `$v1`
        the dispatcher's index, so none of them is planted. `$ra` is not
        either: the harness needs it to hold the sentinel it returns to, which
        makes reaching the sentinel at all the proof that `$ra` survived.
        """
        cpu = self.cpu
        checked = [1] + list(range(4, 26)) + [28, 29, 30]
        for index in checked:
            cpu.setq(index, CONTEXT_MARKER + index)
        if self.syscall(number) is None:
            return None
        lost = [index for index in checked
                if cpu.r[index] != CONTEXT_MARKER + index]
        return lost, cpu.get(3)

    def word(self, address: int) -> int:
        return self.bus.read(address, 4)


PROGRAM_STACK = 0x01F00000       # above any ROM executable's image and its BSS
PROGRAM_RETURN = 0x01FF0000      # the sentinel a called routine returns to
PROGRAM_STEPS = 200_000_000


def loadExecutable(bus: Bus, image: bytes) -> int:
    """Place an `ET_EXEC`'s loadable segments in RAM and return its entry.

    `spec/02` records four of these in the archive. They are ordinary programs
    rather than kernel components, so running one needs no ROM under it: the
    segments go where their headers say, and execution starts wherever the
    caller asks rather than necessarily at the entry.
    """
    if image[:4] != b"\x7fELF":
        raise ValueError("not an ELF")
    entry, phoff = struct.unpack_from("<II", image, 0x18)
    phentsize, phnum = struct.unpack_from("<HH", image, 0x2A)
    loaded = 0
    for index in range(phnum):
        kind, offset, vaddr, _, filesz, memsz, _, _ = struct.unpack_from(
            "<8I", image, phoff + index * phentsize)
        if kind != 1:                                       # PT_LOAD
            continue
        buffer, start = bus.region(vaddr)
        if buffer is not bus.ram:
            raise ValueError(f"segment at {vaddr:#010x} is not in RAM")
        buffer[start:start + memsz] = (
            image[offset:offset + filesz].ljust(memsz, b"\0"))
        loaded += 1
    if not loaded:
        raise ValueError("no loadable segment")
    return entry


def callProgram(bus: Bus, address: int, arguments: tuple[int, ...],
                steps: int = PROGRAM_STEPS) -> tuple[Cpu, int | None]:
    """Call one routine in a loaded program; return the CPU and its `$v0`.

    The routine returns to a sentinel that holds no code, so arriving there is
    the run's termination condition. Anything else means the step budget ran
    out or the CPU stopped, and both are reported rather than hidden.
    """
    cpu = Cpu(bus)
    cpu.set(29, PROGRAM_STACK)
    cpu.set(31, PROGRAM_RETURN)
    for index, argument in enumerate(arguments[:4]):
        cpu.set(4 + index, argument)
    cpu.pc, cpu.next_pc, cpu.branched = address, address + 4, False
    while (cpu.pc != PROGRAM_RETURN and cpu.steps < steps
           and cpu.stop_reason is None):
        cpu.step()
    if cpu.pc != PROGRAM_RETURN:
        return cpu, None
    return cpu, s32(cpu.get(2))


# `spec/04` EE-1: what the reset path must have written before anything else.
RESET_COP0 = (("Config", 0x00073003), ("Status", 0x70400000),
              ("Count", 0), ("Compare", 1))
# EE-1a: the scratchpad mapping, at index 0.
RESET_TLB = (0, 0x70000000, 0x80000007, 0x00000007)

INTC_SOURCE = 3                  # an arbitrary source, for the toggle test
DMAC_CHANNEL = 2
ALARM_ARGUMENTS = (100, 0x80700200, 1)
TLB_GOOD = (0, 0x04000000, 0x80000007, 0x00000007)
TLB_BAD = (0, 0x00000000, 0x00000007, 0x00000007)
GS_MASK = 0xFF00


def checkMachine(machine: Machine) -> list[str]:
    """Judge a booted EE against the executable requirements of the specs."""
    problems: list[str] = []
    cpu = machine.cpu

    def require(ok: bool, requirement: str, detail: str) -> None:
        if not ok:
            problems.append(f"{requirement}: {detail}")

    # EE-1 is about the reset path's *first* writes; the kernel rewrites
    # Status many times afterwards.
    written: dict[str, int] = {}
    for name, value, _ in cpu.cop0_writes:
        written.setdefault(name, value)
    for name, expected in RESET_COP0:
        require(written.get(name) == expected, "EE-1",
                f"{name} was written {written.get(name, 0):#010x}, "
                f"want {expected:#010x}")
    require(bool(cpu.tlb_writes) and cpu.tlb_writes[0] == (0, RESET_TLB),
            "EE-1a", f"first TLB write was {cpu.tlb_writes[:1]}, "
                     f"want index 0 = {RESET_TLB}")
    require(SCRATCH_BASE <= machine.stack_at_rdram
            < SCRATCH_BASE + SCRATCH_SIZE, "EE-1b",
            f"the stack was {machine.stack_at_rdram:#010x}, "
            f"not in the scratchpad")
    require(machine.rdram_called, "EE-2",
            f"{machine.rdram_entry:#010x} -- where RDRAM lies -- was never "
            f"called")
    require(machine.reached_kernel, "EE-3d",
            f"control never reached {KERNEL_ENTRY:#010x}")
    if not machine.reached_kernel:
        return problems                          # nothing below can be judged

    vectors = bytes(machine.bus.ram[0:0x28])
    require(vectors != bytes(0x28) and vectors == bytes(
        machine.bus.ram[0x180:0x1A8]), "EE-3b",
        "the vector page in RAM is empty or its two entries differ, so "
        "KERNEL was not copied to physical 0 intact")
    require("Initialize Done." in machine.kernel_console, "EE-10",
            "the kernel never announced its initialisation as complete")

    # EE-7: the ABI, exercised rather than described. Reaching the harness's
    # return address at all requires EE-7c's advance past the `syscall`.
    acted = machine.syscall(0x14, INTC_SOURCE)
    require(acted == 1, "EE-8g",
            f"enabling INTC source {INTC_SOURCE} returned {acted}, want 1")
    require(machine.bus.io.get(INTC_MASK, 0) & (1 << INTC_SOURCE), "EE-8g",
            f"{INTC_MASK:#010x} bit {INTC_SOURCE} is not set after enabling")
    again = machine.syscall(0x14, INTC_SOURCE)
    require(again == 0, "EE-8g",
            f"enabling it twice returned {again}, want 0")
    negated = machine.syscall(-0x14 & MASK64, INTC_SOURCE)
    require(negated == 0, "EE-7b",
            f"syscall -0x14 returned {negated}; a negative number must be "
            f"negated and reach the same slot")
    require(machine.syscall(0x15, INTC_SOURCE) == 1
            and machine.syscall(0x15, INTC_SOURCE) == 0, "EE-8g",
            "disabling an INTC source did not report acting exactly once")

    machine.syscall(0x16, DMAC_CHANNEL)
    require(machine.bus.io.get(DMAC_STATUS, 0) & (1 << (16 + DMAC_CHANNEL)),
            "EE-8g", f"{DMAC_STATUS:#010x} bit {16 + DMAC_CHANNEL} is not set: "
                     f"the DMAC bit must start at 16, not 0")

    slot, handler = 0x40, 0xDEADBEEF
    machine.syscall(0x74, slot, handler)
    require(machine.word(0x80014F40 + slot * 4) == handler, "SYS-5a",
            f"slot {slot:#x} was not installed into the syscall table")

    first = machine.syscall(0x18, *ALARM_ARGUMENTS)
    second = machine.syscall(0x18, *ALARM_ARGUMENTS)
    require(first == 0 and second == 1, "SYS-7a",
            f"the first two alarms were {first} and {second}, want 0 and 1")
    machine.syscall(0x19, first)
    machine.syscall(0x19, second)
    require(machine.syscall(0x19, second) == -1, "SYS-7a",
            "releasing an alarm that is not allocated did not return -1")

    require(machine.syscall(0x09, *TLB_BAD) == -1, "SYS-7b",
            "a TLB write with a rejected EntryHi did not return -1")
    index = machine.syscall(0x09, *TLB_GOOD)
    require(index is not None and 0 <= index < TLB_ENTRIES, "SYS-7b",
            f"an accepted TLB write returned {index}, want an entry index")

    require(machine.syscall(0x71, GS_MASK) == GS_MASK, "SYS-7c",
            "setting the GS interrupt mask did not return the value set")
    require(machine.syscall(0x70) == GS_MASK, "SYS-7c",
            "reading the GS interrupt mask back did not return the shadow")

    require(machine.syscall(0x75) is not None, "SYS-3b",
            "the empty syscall 0x75 did not return")
    return problems


def report(machine: Machine) -> None:
    cpu = machine.cpu
    print(f"reset: {cpu.steps} instructions to the kernel's wait for the IOP"
          + (f", stopped: {cpu.stop_reason}" if cpu.stop_reason else ""))
    print(f"   CP0 at reset: " + ", ".join(
        f"{name}={value:#x}" for name, value, _ in cpu.cop0_writes[:4]))
    if cpu.tlb_writes:
        index, entry = cpu.tlb_writes[0]
        print(f"   first TLB entry: index {index}, EntryHi={entry[1]:#010x}, "
              f"EntryLo0={entry[2]:#010x}, EntryLo1={entry[3]:#010x}")
    print(f"   RDRAM called at {machine.rdram_entry:#010x}: "
          f"{machine.rdram_called}")
    print(f"   reached {KERNEL_ENTRY:#010x}: {machine.reached_kernel}")
    if machine.kernel_console:
        print("\nthe kernel's own console:")
        for line in machine.kernel_console.splitlines():
            if line.strip():
                print(f"   {line}")


def callMode(image: bytes, arguments) -> int:
    """`--call`: run a routine in an EE executable, with nothing under it."""
    bus = Bus(b"")
    entry = loadExecutable(bus, image)
    values = [int(value, 0) for value in arguments.call]
    address = values[0] if values else entry
    cpu, result = callProgram(bus, address, tuple(values[1:]))
    print(f"call {address:#010x}({', '.join(hex(v) for v in values[1:])}): "
          f"{cpu.steps} instructions, "
          + (f"returned {result} ({result & MASK32:#010x})"
             if result is not None else
             f"did not return -- {cpu.stop_reason or 'step budget exhausted'}"))
    if result is None:
        return 1
    if arguments.dump:
        where = int(arguments.dump[0], 0)
        length = (result if arguments.dump[1] == "result"
                  else int(arguments.dump[1], 0))
        if length <= 0:
            print(f"nothing to dump: length is {length}", file=sys.stderr)
            return 1
        buffer, start = bus.region(where)
        pathlib.Path(arguments.dump[2]).write_bytes(
            bytes(buffer[start:start + length]))
        print(f"   wrote {length} bytes from {where:#010x} "
              f"to {arguments.dump[2]}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("image", type=pathlib.Path)
    parser.add_argument("--check", action="store_true",
                        help="judge the boot against docs/spec/; exit 1 on failure")
    parser.add_argument("--trace", type=int, default=0, metavar="N",
                        help="print the first N instructions of the reset path")
    parser.add_argument("--syscall", nargs="+", metavar="N",
                        help="call one syscall on the booted kernel: "
                             "number, then up to four arguments")
    parser.add_argument("--io", action="store_true",
                        help="list the I/O addresses the run touched")
    parser.add_argument("--call", nargs="*", metavar="V",
                        help="load the image as an EE executable instead of "
                             "booting it as a ROM, and call an address with up "
                             "to four arguments; no address calls its entry")
    parser.add_argument("--dump", nargs=3, metavar=("ADDRESS", "LENGTH", "PATH"),
                        help="after --call, write memory out; a LENGTH of "
                             "'result' uses the value the call returned")
    arguments = parser.parse_args()

    rom = arguments.image.read_bytes()

    if arguments.call is not None:
        return callMode(rom, arguments)

    machine = Machine(rom)
    machine.boot(trace=arguments.trace)

    if arguments.syscall:
        values = [int(value, 0) for value in arguments.syscall]
        result = machine.syscall(values[0], *values[1:])
        if result is None:
            print(f"syscall {values[0]:#04x} did not return", file=sys.stderr)
            return 1
        print(f"syscall {values[0]:#04x}"
              f"({', '.join(hex(v) for v in values[1:])}) -> {result} "
              f"({result & MASK32:#010x})")
        return 0

    if arguments.check:
        problems = checkMachine(machine)
        for problem in problems:
            print(f"{arguments.image}: {problem}", file=sys.stderr)
        if problems:
            print(f"\n{arguments.image}: {len(problems)} failure(s)",
                  file=sys.stderr)
            return 1
        print(f"{arguments.image}: ok -- EE booted to its kernel in "
              f"{machine.cpu.steps} instructions, syscall ABI as specified")
        return 0

    report(machine)
    if arguments.io:
        print("\nI/O touched:")
        for offset in sorted(set(machine.bus.io_reads) | set(machine.bus.io_writes)):
            print(f"   {offset:#010x}  "
                  f"r{machine.bus.io_reads.get(offset, 0):<6d} "
                  f"w{machine.bus.io_writes.get(offset, 0)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
