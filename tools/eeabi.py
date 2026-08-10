#!/usr/bin/env python3
"""Infer the calling convention of each EE syscall from the KERNEL image.

`tools/eeksys.py` reports *where* every syscall slot points. This asks what
each one takes and gives back, by reading the handler's code:

  * which of `$a0`-`$a3` are live on entry -- an argument register that is read
    before it is written is an argument the caller must supply;
  * whether the handler produces a value in `$v0`;
  * how it leaves -- `jr $ra`, `eret`, or an indirect jump;
  * which string constants and hardware registers it names, which is usually
    what identifies the operation.

The analysis is a standard backward liveness pass over the handler's control
flow graph, with calls resolved: a `jal` uses whatever its callee reads, so an
argument forwarded straight to a subroutine is still seen as an argument.

    tools/romdir.py assets/SCPH-50000.bin --extract <outdir>
    tools/eeabi.py <outdir>/KERNEL                # every slot
    tools/eeabi.py <outdir>/KERNEL --slot 0x7b    # one, with detail

One correction is needed for the R5900 kernel to give sensible answers. The
scheduler syscalls call a routine that saves the whole user context with `sq`,
which reads every register -- so a naive pass calls every scheduling syscall a
four-argument call. Routines that save or restore a context wholesale are
recognised (many `sq`/`sd` of distinct registers, or the `lq`/`ld` mirror) and
contribute no argument reads.

Limits worth knowing when reading the output. Liveness is a may-analysis, so a
register read on only one path still counts as an argument. Recursion through
a call cycle is cut by assuming the callee reads nothing. Jump tables end an
analysis rather than being followed. And nothing here proves what an argument
*means* -- only that it is one.
"""

from __future__ import annotations

import argparse
import pathlib
import string
import struct
import sys

KSEG0 = 0x80000000
SYSCALL_TABLE = 0x14F40
SYSCALL_COUNT = 0x7D

REGISTERS = (
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra",
)
ARGUMENT_REGISTERS = (4, 5, 6, 7)          # $a0 .. $a3
RETURN_REGISTER = 2                        # $v0
RA = 31

# A call destroys the caller-saved registers. $k0/$k1 belong to the exception
# path rather than to the callee, so they are left alone.
CALL_CLOBBERS = frozenset({1, 2, 3, 4, 5, 6, 7, *range(8, 16), 24, 25, 31})

# EE hardware, for reporting which block a handler touches.
HARDWARE_RANGES = (
    (0x10000000, 0x10010000, "hw"),
    (0x11000000, 0x11010000, "vu"),
    (0x12000000, 0x12002000, "gs"),
    (0x1F800000, 0x1F810000, "iop"),
)

# The largest handler in the reference kernel reaches ~4300 instructions
# through its callees, so this bounds runaway walks without cutting real code.
MAX_INSTRUCTIONS = 12000

# A routine that moves this many distinct *caller-saved* registers to or from
# memory in one run is saving or restoring a context. Counting only volatile
# registers is what separates a context switch from an ordinary prologue: a
# compiled function saves `$s0`-`$s7` and `$ra`, never `$a0`-`$a3` or `$t0`.
CONTEXT_REGISTERS = frozenset({2, 3, 4, 5, 6, 7, *range(8, 16), 24, 25})
CONTEXT_REGISTER_THRESHOLD = 6
CONTEXT_SCAN_LIMIT = 128                   # instructions; these are straight-line


def word(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def offsetOf(address: int) -> int:
    """File offset of a run-time address; KERNEL runs from physical 0."""
    return address & 0x1FFFFFFF


def signed16(value: int) -> int:
    return value - 0x10000 if value & 0x8000 else value


class Instruction:
    """One decoded instruction, reduced to what liveness needs.

    `kind` drives the control flow graph: "branch" (conditional, both edges),
    "jump" (unconditional, one edge), "call", "return", "indirect", "eret" and
    plain "op". `uses` and `defs` are register numbers.
    """

    __slots__ = ("address", "raw", "kind", "uses", "defs", "target")

    def __init__(self, address: int, raw: int, kind: str,
                 uses: frozenset[int], defs: frozenset[int],
                 target: int | None = None) -> None:
        self.address = address
        self.raw = raw
        self.kind = kind
        self.uses = uses
        self.defs = defs
        self.target = target

    @property
    def has_delay_slot(self) -> bool:
        return self.kind in ("branch", "jump", "call", "return", "indirect")


def decode(address: int, raw: int) -> Instruction:
    """Decode enough of one MIPS/R5900 word to know its registers and flow."""
    op = raw >> 26
    rs, rt, rd = (raw >> 21) & 31, (raw >> 16) & 31, (raw >> 11) & 31
    immediate = raw & 0xFFFF

    def make(kind: str, uses: tuple[int, ...] = (), defs: tuple[int, ...] = (),
             target: int | None = None) -> Instruction:
        # $zero is never a real definition and never carries a value.
        return Instruction(address, raw, kind,
                           frozenset(r for r in uses if r),
                           frozenset(r for r in defs if r), target)

    if op == 0x00:                                     # SPECIAL
        funct = raw & 0x3F
        if funct == 0x08:                              # jr
            return make("return" if rs == RA else "indirect", uses=(rs,))
        if funct == 0x09:                              # jalr
            return make("indirect", uses=(rs,), defs=(rd or RA,))
        if funct in (0x0C, 0x0D, 0x0F):                # syscall, break, sync
            return make("op")
        if funct in (0x00, 0x02, 0x03, 0x38, 0x3A, 0x3B,
                     0x3C, 0x3E, 0x3F):                # shifts by constant
            return make("op", uses=(rt,), defs=(rd,))
        if funct in (0x10, 0x12, 0x28):                # mfhi, mflo, mfsa
            return make("op", defs=(rd,))
        if funct in (0x11, 0x13, 0x29):                # mthi, mtlo, mtsa
            return make("op", uses=(rs,))
        if funct in (0x18, 0x19, 0x1A, 0x1B):          # mult, div -- rd on R5900
            return make("op", uses=(rs, rt), defs=(rd,))
        return make("op", uses=(rs, rt), defs=(rd,))   # the arithmetic bulk

    if op == 0x01:                                     # REGIMM: bltz, bgez, ...
        target = address + 4 + signed16(immediate) * 4
        defs = (RA,) if rt in (0x10, 0x11) else ()     # the ...al forms
        return make("branch", uses=(rs,), defs=defs, target=target)

    if op in (0x02, 0x03):                             # j, jal
        target = (address & 0xF0000000) | ((raw & 0x03FFFFFF) << 2)
        if op == 0x02:
            return make("jump", target=target)
        return make("call", defs=(RA,), target=target)

    if op in (0x04, 0x05, 0x14, 0x15):                 # beq, bne, and likely
        target = address + 4 + signed16(immediate) * 4
        return make("branch", uses=(rs, rt), target=target)

    if op in (0x06, 0x07, 0x16, 0x17):                 # blez, bgtz, and likely
        target = address + 4 + signed16(immediate) * 4
        return make("branch", uses=(rs,), target=target)

    if op == 0x0F:                                     # lui
        return make("op", defs=(rt,))

    if op in (0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x18, 0x19):
        return make("op", uses=(rs,), defs=(rt,))      # immediate arithmetic

    if op == 0x10:                                     # COP0
        if rs == 0x00:                                 # mfc0
            return make("op", defs=(rt,))
        if rs == 0x04:                                 # mtc0
            return make("op", uses=(rt,))
        if rs == 0x10 and (raw & 0x3F) == 0x18:        # eret
            return make("eret")
        return make("op")

    if op in (0x11, 0x12):                             # COP1/COP2 moves
        if rs in (0x00, 0x01, 0x02):                   # mfc, dmfc, cfc
            return make("op", defs=(rt,))
        if rs in (0x04, 0x05, 0x06):                   # mtc, dmtc, ctc
            return make("op", uses=(rt,))
        if rs == 0x08:                                 # bc1/bc2
            target = address + 4 + signed16(immediate) * 4
            return make("branch", target=target)
        return make("op")

    if op == 0x1C:                                     # MMI (R5900)
        return make("op", uses=(rs, rt), defs=(rd,))

    if op in (0x1A, 0x1B, 0x1E, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,
              0x27, 0x30, 0x37):                       # loads, including lq
        return make("op", uses=(rs,), defs=(rt,))

    if op in (0x1F, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x38, 0x3F):
        return make("op", uses=(rs, rt))               # stores, including sq

    if op in (0x2F, 0x33):                             # cache, pref -- rt is a field
        return make("op", uses=(rs,))

    if op in (0x31, 0x36, 0x39, 0x3E):                 # coprocessor loads/stores
        return make("op", uses=(rs,))

    return make("op")                                  # unknown: assume harmless


class Analysis:
    """Everything the pass learned about one entry point."""

    def __init__(self, entry: int) -> None:
        self.entry = entry
        self.arguments: list[int] = []
        self.returns_v0 = False
        self.returns_via_call = False
        self.terminators: set[str] = set()
        self.strings: list[str] = []
        self.hardware: list[tuple[int, str]] = []
        self.instruction_count = 0
        self.truncated = False


class KernelAbi:
    """Reads handlers out of a KERNEL image and infers their signatures."""

    def __init__(self, data: bytes) -> None:
        self.data = data
        self.cache: dict[int, Analysis] = {}
        self.in_progress: set[int] = set()
        self.context_routines: dict[int, bool] = {}

    # -- reading -----------------------------------------------------------

    def inImage(self, address: int) -> bool:
        offset = offsetOf(address)
        return 0 <= offset < len(self.data) - 3 and address >= KSEG0

    def instructionAt(self, address: int) -> Instruction:
        return decode(address, word(self.data, offsetOf(address)))

    def stringAt(self, address: int) -> str | None:
        """The printable string at `address`, if there plausibly is one."""
        offset = offsetOf(address)
        if not 0 <= offset < len(self.data):
            return None
        end = self.data.find(b"\0", offset, offset + 128)
        if end <= offset:
            return None
        try:
            text = self.data[offset:end].decode("ascii")
        except UnicodeDecodeError:
            return None
        printable = set(string.printable) - {"\x0b", "\x0c"}
        if len(text) < 3 or not all(c in printable for c in text):
            return None
        return text

    def isContextRoutine(self, entry: int) -> bool:
        """True for a wholesale context save or restore.

        Such a routine touches every register because that is its job, so its
        reads say nothing about what the caller passed. The scheduler group
        would otherwise look like four-argument calls throughout.
        """
        if entry in self.context_routines:
            return self.context_routines[entry]
        self.context_routines[entry] = False        # cuts recursion cheaply

        saved: set[int] = set()
        restored: set[int] = set()
        address = entry
        for _ in range(CONTEXT_SCAN_LIMIT):
            if not self.inImage(address):
                break
            raw = word(self.data, offsetOf(address))
            op, rt = raw >> 26, (raw >> 16) & 31
            if rt in CONTEXT_REGISTERS:
                if op in (0x1F, 0x3F, 0x2B):        # sq, sd, sw
                    saved.add(rt)
                elif op in (0x1E, 0x37, 0x23):      # lq, ld, lw
                    restored.add(rt)
            instruction = decode(address, raw)
            if instruction.kind in ("return", "indirect", "jump", "eret"):
                break
            address += 4

        verdict = (len(saved) >= CONTEXT_REGISTER_THRESHOLD
                   or len(restored) >= CONTEXT_REGISTER_THRESHOLD)
        self.context_routines[entry] = verdict
        return verdict

    # -- the pass ----------------------------------------------------------

    def analyze(self, entry: int) -> Analysis:
        if entry in self.cache:
            return self.cache[entry]
        if entry in self.in_progress or not self.inImage(entry):
            return Analysis(entry)              # a cycle, or off the edge

        self.in_progress.add(entry)
        try:
            result = self.run(entry)
        finally:
            self.in_progress.discard(entry)
        self.cache[entry] = result
        return result

    def run(self, entry: int) -> Analysis:
        result = Analysis(entry)
        instructions: dict[int, Instruction] = {}
        successors: dict[int, list[int]] = {}
        calls: dict[int, int | None] = {}       # delay-slot address -> callee

        # Graph nodes are addresses; a call gets a second node so that its
        # register effects land after the delay slot, where they belong.
        def callNode(address: int) -> int:
            return address | 1                  # instructions are word-aligned

        pending = [entry]
        seen: set[int] = set()
        while pending:
            address = pending.pop()
            if address in seen:
                continue
            if len(seen) >= MAX_INSTRUCTIONS:
                result.truncated = True
                break
            if not self.inImage(address):
                continue
            seen.add(address)

            instruction = self.instructionAt(address)
            instructions[address] = instruction
            edges: list[int] = []

            if instruction.has_delay_slot:
                delay = address + 4
                edges = [delay]
                # The delay slot carries the transfer's outgoing edges.
                if instruction.kind == "branch":
                    onward = [instruction.target, address + 8]
                elif instruction.kind == "jump":
                    if self.isContextRoutine(instruction.target):
                        onward = []             # a tail context switch, not code to read
                        result.terminators.add("context switch")
                    else:
                        onward = [instruction.target]
                elif instruction.kind == "call":
                    onward = [callNode(delay)]
                    calls[callNode(delay)] = instruction.target
                    successors[callNode(delay)] = [address + 8]
                    pending.append(address + 8)
                else:                            # return, indirect
                    onward = []
                    result.terminators.add(
                        "jr $ra" if instruction.kind == "return" else "indirect")
                if self.inImage(delay):
                    seen.add(delay)
                    instructions[delay] = self.instructionAt(delay)
                    successors[delay] = [t for t in onward if t is not None]
                    pending.extend(t for t in onward
                                   if t is not None and t & 1 == 0)
            elif instruction.kind == "eret":
                result.terminators.add("eret")
            else:
                edges = [address + 4]
                pending.append(address + 4)

            successors.setdefault(address, edges)

        result.instruction_count = len(instructions)

        # Register effects per node, with calls resolved to their callee.
        uses: dict[int, frozenset[int]] = {}
        defs: dict[int, frozenset[int]] = {}
        for address, instruction in instructions.items():
            uses[address] = instruction.uses
            defs[address] = instruction.defs
        for node, callee in calls.items():
            if callee is None:
                uses[node] = frozenset()
                defs[node] = CALL_CLOBBERS
            elif self.isContextRoutine(callee):
                # A context routine reads every register because it is saving
                # them, and *preserves* them rather than clobbering them --
                # which is what lets the scheduler skeleton hand the caller's
                # own arguments to the operation it wraps.
                uses[node] = frozenset()
                defs[node] = frozenset({RA})
            else:
                uses[node] = frozenset(self.analyze(callee).arguments)
                defs[node] = CALL_CLOBBERS

        # Backward liveness to a fixed point.
        live_in: dict[int, frozenset[int]] = {n: frozenset() for n in successors}
        changed = True
        while changed:
            changed = False
            for node in successors:
                live_out: set[int] = set()
                for target in successors[node]:
                    live_out |= live_in.get(target, frozenset())
                new = uses.get(node, frozenset()) | (
                    live_out - defs.get(node, frozenset()))
                if new != live_in[node]:
                    live_in[node] = frozenset(new)
                    changed = True

        entry_live = live_in.get(entry, frozenset())
        result.arguments = [r for r in ARGUMENT_REGISTERS if r in entry_live]

        # A value comes back if some instruction really writes $v0. A tail call
        # in the last position returns whatever its callee returned.
        result.returns_v0 = any(
            RETURN_REGISTER in instruction.defs
            for instruction in instructions.values())
        if not result.returns_v0:
            for node, callee in calls.items():
                if callee is not None and self.analyze(callee).returns_v0:
                    result.returns_via_call = True
                    break
            for address, instruction in instructions.items():
                if instruction.kind == "jump" and self.inImage(instruction.target):
                    if instruction.target not in instructions:
                        if self.analyze(instruction.target).returns_v0:
                            result.returns_via_call = True

        self.collectConstants(instructions, result)
        return result

    def collectConstants(self, instructions: dict[int, Instruction],
                         result: Analysis) -> None:
        """Recover `lui`/`addiu` address pairs and say what they point at."""
        upper: dict[int, int] = {}
        strings: list[str] = []
        hardware: list[tuple[int, str]] = []
        for address in sorted(instructions):
            raw = instructions[address].raw
            op, rs, rt = raw >> 26, (raw >> 21) & 31, (raw >> 16) & 31
            immediate = raw & 0xFFFF
            if op == 0x0F:                                    # lui
                upper[rt] = immediate << 16
                continue
            if op in (0x08, 0x09, 0x0D, 0x23, 0x24) and rs in upper:
                delta = immediate if op == 0x0D else signed16(immediate)
                value = (upper[rs] + delta) & 0xFFFFFFFF
                if op in (0x09, 0x0D, 0x08):
                    upper[rt] = value                          # still an address
                for low, high, name in HARDWARE_RANGES:
                    if low <= (value & 0x1FFFFFFF) < high:
                        pair = (value, name)
                        if pair not in hardware:
                            hardware.append(pair)
                        break
                else:
                    text = self.stringAt(value)
                    if text and text not in strings:
                        strings.append(text)
            elif rt in upper and op not in (0x09, 0x0D, 0x08):
                upper.pop(rt, None)                            # overwritten
        result.strings = strings
        result.hardware = hardware


def describe(analysis: Analysis) -> str:
    """One-line signature, in the form the specification quotes."""
    arguments = ", ".join(f"${REGISTERS[r]}" for r in analysis.arguments) or "-"
    if analysis.returns_v0:
        returns = "$v0"
    elif analysis.returns_via_call:
        returns = "$v0?"
    elif "indirect" in analysis.terminators:
        returns = "?"                          # left through a jump table
    else:
        returns = "-"
    return f"({arguments}) -> {returns}"


# What docs/spec/05-ee-syscall-abi.md requires of each slot: the argument
# registers the handler reads, and whether it produces a value. Recorded from
# the reference kernel and address-independent, so it judges a rebuild too.
# "v0?" is a value that reaches $v0 only through a callee, "?" a slot whose
# exit is a jump table the pass cannot follow.
SIGNATURES = """
00 -        -     01 a0       v0    02 a0a1a2   v0    03 -        -
04 -        v0    05 -        -     06 a0a1a2   v0    07 a0a1a2a3 v0?
08 -        -     09 a0a1a2a3 v0    0a a0a1     v0    0b a0       v0
0c a0       v0    0d a0a1     v0    0e a0a1     v0    0f a0a1     v0
10 a0a1a2a3 v0?   11 a0a1a2a3 v0    12 a0a1a2a3 v0?   13 a0a1a2a3 v0
14 a0       v0    15 a0       v0    16 a0       v0    17 a0       v0
18 a0a1a2   v0    19 a0       v0    1a a0       v0    1b a0       v0
1c a0       v0    1d a0       v0    1e a0a1a2   v0    1f a0       v0
20 a0       v0    21 a0       v0    22 a0a1     v0    23 -        v0?
24 -        v0?   25 a0       v0    26 a0       v0    27 -        v0
28 -        v0    29 a0a1a3   v0?   2a a0a1a3   v0    2b a0       v0
2c a0       v0    2d a0       v0    2e a0       v0    2f -        v0
30 a0a1     v0    31 a0a1     v0    32 -        v0    33 a0       v0
34 a0       v0    35 a0       v0    36 a0       v0    37 a0       v0
38 a0       v0    39 a0       v0    3a a0       v0    3b -        v0?
3c a0a1a2a3 v0    3d a0a1     v0    3e -        v0    3f -        -
40 a0       v0    41 a0       v0    42 a0       v0    43 a0       v0
44 a0       v0    45 a0       v0    46 a0       v0    47 a0a1     v0
48 a0a1     v0    49 a0       v0    4a a0       v0    4b a0       v0
4c a0a1a2a3 v0    4d -        v0    4e a0a1a2a3 v0    4f a0       v0
50 -        v0    51 -        v0    52 -        v0    53 -        v0
54 -        -     55 -        -     56 -        -     57 -        -
58 -        -     59 -        -     5a -        -     5b -        -
5c a0       v0    5d a0       v0    5e a0       v0    5f a0       v0
60 a0       -     61 a0       -     62 a0       -     63 a0       ?
64 a0       v0    65 a0a1     v0    66 a0       v0    67 a0       ?
68 a0       v0    69 a0a1     v0    6a a0       v0    6b -        v0
6c a0       -     6d a0       v0    6e a0a1a2   v0    6f a0a1a2   v0
70 -        v0    71 a0       v0    72 a0       -     73 a0a1     -
74 a0a1     -     75 -        -     76 a0       v0    77 a0a1     v0
78 -        v0    79 a0a1     v0    7a a0       v0    7b a0a1     v0
7c -        -
"""

# Eight operations published at two slot numbers, one rescheduling and one
# returning locally (docs/spec/04-ee-kernel.md EE-8h). The two paths are
# different code, so agreeing on arguments is a real check, not a tautology.
SCHEDULER_PAIRS = ((0x25, 0x26), (0x29, 0x2A), (0x2B, 0x2C), (0x2D, 0x2E),
                   (0x33, 0x34), (0x39, 0x3A), (0x41, 0x49), (0x42, 0x43))
UNDEFINED_SLOTS = {0x00, 0x03, 0x08, 0x3F, 0x7C} | set(range(0x54, 0x5C))
KSEG1_SLOTS = (0x60, 0x61, 0x62)


def expectedSignatures() -> dict[int, str]:
    """Parse SIGNATURES into `slot -> "(args) -> return"`."""
    fields = SIGNATURES.split()
    table: dict[int, str] = {}
    for slot, arguments, returns in zip(*[iter(fields)] * 3):
        registers = "-" if arguments == "-" else ", ".join(
            f"${arguments[i:i + 2]}" for i in range(0, len(arguments), 2))
        table[int(slot, 16)] = f"({registers}) -> " + (
            returns if returns == "?" else returns.replace("v0", "$v0"))
    return table


def checkAbi(abi: KernelAbi, slots: list[int]) -> list[str]:
    """Judge a KERNEL image against docs/spec/05-ee-syscall-abi.md."""
    problems: list[str] = []
    expected = expectedSignatures()
    signatures = {slot: describe(abi.analyze(target))
                  for slot, target in enumerate(slots)}

    for slot in range(SYSCALL_COUNT):
        if signatures[slot] != expected[slot]:
            problems.append(f"SYS-1: slot {slot:#04x} is "
                            f"{signatures[slot]}, want {expected[slot]}")

    for rescheduling, direct in SCHEDULER_PAIRS:
        left = signatures[rescheduling].split(" -> ")[0]
        right = signatures[direct].split(" -> ")[0]
        if left != right:
            problems.append(
                f"SYS-2: slot {rescheduling:#04x} takes {left} but its direct "
                f"form {direct:#04x} takes {right}")

    for slot in sorted(UNDEFINED_SLOTS):
        if signatures[slot] != "(-) -> -":
            problems.append(f"SYS-3: undefined slot {slot:#04x} is "
                            f"{signatures[slot]}, want (-) -> -")

    for slot in KSEG1_SLOTS:
        if signatures[slot] != "($a0) -> -":
            problems.append(f"SYS-4: cache slot {slot:#04x} is "
                            f"{signatures[slot]}, want ($a0) -> -")
    return problems


def reportSlot(abi: KernelAbi, slot: int, target: int) -> None:
    analysis = abi.analyze(target)
    print(f"syscall {slot:#04x} -> {target:#010x}  {describe(analysis)}")
    print(f"   {analysis.instruction_count} instructions reachable, "
          f"leaves via {', '.join(sorted(analysis.terminators)) or 'fall-through'}"
          + ("  [truncated]" if analysis.truncated else ""))
    if analysis.hardware:
        print("   hardware: " + ", ".join(
            f"{address:#010x} ({name})" for address, name in analysis.hardware))
    for text in analysis.strings:
        print(f"   string: {text!r}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("kernel", type=pathlib.Path)
    parser.add_argument("--slot", type=lambda s: int(s, 0),
                        help="report one slot in detail")
    parser.add_argument("--strings", action="store_true",
                        help="show each slot's string and hardware references")
    parser.add_argument("--check", action="store_true",
                        help="judge against docs/spec/05-ee-syscall-abi.md; "
                             "exit 1 on any failure")
    args = parser.parse_args()

    data = args.kernel.read_bytes()
    if len(data) < SYSCALL_TABLE + SYSCALL_COUNT * 4:
        sys.exit(f"eeabi: {args.kernel} is too small to be an EE kernel")

    abi = KernelAbi(data)
    slots = [word(data, SYSCALL_TABLE + i * 4) for i in range(SYSCALL_COUNT)]

    if args.check:
        problems = checkAbi(abi, slots)
        for problem in problems:
            print(f"{args.kernel}: {problem}", file=sys.stderr)
        if problems:
            print(f"\n{args.kernel}: {len(problems)} failure(s)", file=sys.stderr)
            return 1
        print(f"{args.kernel}: ok -- {SYSCALL_COUNT} syscall signatures as "
              f"specified")
        return 0

    if args.slot is not None:
        if not 0 <= args.slot < SYSCALL_COUNT:
            sys.exit(f"eeabi: slot {args.slot:#x} is outside "
                     f"0..{SYSCALL_COUNT - 1:#x}")
        reportSlot(abi, args.slot, slots[args.slot])
        return 0

    for slot, target in enumerate(slots):
        analysis = abi.analyze(target)
        print(f"{slot:#04x}  {target:#010x}  {describe(analysis):<28}"
              f"{analysis.instruction_count:>5} insn"
              + ("  [truncated]" if analysis.truncated else ""))
        if args.strings:
            for address, name in analysis.hardware:
                print(f"        hw {address:#010x} ({name})")
            for text in analysis.strings:
                print(f"        {text!r}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
