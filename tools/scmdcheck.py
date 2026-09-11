#!/usr/bin/env python3
"""Judge CDVDMAN's mechacon ordinals against a modelled controller.

    python3 tools/scmdcheck.py build/rom.bin

`docs/spec/06-iop-kernel.md` IOP-8i to IOP-8k are the S-command register
sequence, NVM read and write, and the configuration session. Nothing in the
boot reaches any of them -- the EE-facing service that would has no ordinal
behind its configuration functions yet -- so without this they would sit in
the image unexecuted.

So: boot the image under `tools/iopsim.py`, find the export table LOADCORE
registered, and call the ordinals with a mechanism controller bolted onto the
bus. The model is deliberately strict about the one thing the sequence turns
on -- status bit 0x40 means "the result FIFO is empty", and it is the only
thing that ends both the drain and the read-back loop -- so a driver that
miscounts its result bytes hangs here rather than passing.

What is checked is what a rebuild gets wrong silently: the wire order of the
address bytes, `open`'s first two arguments arriving reversed, the sum byte
being stripped on the way in and appended on the way out, the verdict a bad
sum produces, and a read outside a session completing nothing.
"""

import pathlib
import sys

sys.path.insert(0, "tools")

import iopsim  # noqa: E402

S_COMMAND, S_STATUS, S_RESULT = 0x1F402016, 0x1F402017, 0x1F402018
S_BUSY, S_RESULT_EMPTY = 0x80, 0x40

# A recognisable NVM: word n reads back as 0xA000 | n.
NVM_WORDS = 512


class Mechacon(iopsim.Bus):
    """The S-command side of the mechanism controller, and nothing else."""

    def __init__(self, rom: bytes) -> None:
        super().__init__(rom)
        self.params: list[int] = []
        self.results: list[int] = []
        self.sent: list[tuple[int, list[int]]] = []
        self.config = [bytes(range(16 * b, 16 * b + 15)) for b in range(2)]
        self.config_index = 0

    # -- the command itself --------------------------------------------------

    def execute(self, command: int) -> None:
        self.sent.append((command, list(self.params)))
        if command == 0x0A:                              # ReadNVM
            address = (self.params[0] << 8) | self.params[1]
            word = 0xA000 | address
            self.results = [0, word >> 8, word & 0xFF]
        elif command == 0x0B:                            # WriteNVM
            self.results = [0]
        elif command == 0x40:                            # open the session
            self.config_index = 0
            self.results = [0]
        elif command == 0x41:                            # read one block
            block = self.config[self.config_index % len(self.config)]
            self.config_index += 1
            self.results = list(block) + [sum(block) & 0xFF]
        elif command == 0x42:                            # write one block
            self.results = [0]
        elif command == 0x43:                            # close the session
            self.results = [0]
        else:
            self.results = [0]
        self.params = []

    # -- the three registers -------------------------------------------------

    def read(self, addr: int, size: int) -> int:
        phys = addr & 0x1FFFFFFF
        if phys == S_STATUS:
            # Never busy: this model completes a command inside its write.
            return 0 if self.results else S_RESULT_EMPTY
        if phys == S_RESULT:
            return self.results.pop(0) if self.results else 0
        return super().read(addr, size)

    def write(self, addr: int, size: int, value: int, pc: int) -> None:
        phys = addr & 0x1FFFFFFF
        if phys == S_STATUS:
            self.params.append(value & 0xFF)
            return
        if phys == S_COMMAND:
            self.execute(value & 0xFF)
            return
        super().write(addr, size, value, pc)


def boot(image: pathlib.Path, steps: int = 20_000_000):
    bus = Mechacon(image.read_bytes())
    cpu = iopsim.Cpu(bus)
    while cpu.steps < steps and cpu.stop_reason is None:
        cpu.step()
    return bus, cpu


def findExports(bus, tag: str):
    for at, name, _version, _flags in iopsim.scanLibraries(bus)[0]:
        if name == tag:
            return at
    raise SystemExit(f"{tag}: not registered after the boot")


def call(cpu, entry: int, args, steps: int = 2_000_000) -> int:
    """Call one IOP routine and come back. `0` is the return sentinel: no
    module is linked there, so a `jr $ra` into it is unambiguous."""
    saved = (cpu.pc, cpu.next_pc, list(cpu.r))
    for index, value in enumerate(args):
        cpu.r[4 + index] = value & 0xFFFFFFFF
    cpu.r[31] = 0
    cpu.pc, cpu.next_pc = entry, entry + 4
    taken = 0
    while cpu.pc != 0 and taken < steps and cpu.stop_reason is None:
        cpu.step()
        taken += 1
    if cpu.pc != 0:
        raise SystemExit(f"call to {entry:#010x} did not return in {taken} steps"
                         f" (pc {cpu.pc:#010x}, {cpu.stop_reason})")
    result = cpu.r[2]
    cpu.pc, cpu.next_pc, cpu.r = saved[0], saved[1], saved[2]
    return result


def word(bus, address: int) -> int:
    return int.from_bytes(bus.ram[address:address + 2], "little")


JR_RA = 0x03E00008
THBASE_DELAY = 33


class NoDelay:
    """Neutralise `DelayThread` for the duration of a call.

    Ordinal 31 delays before it sends, which on the machine is a thread
    sleeping while the scheduler runs something else. This harness calls a
    routine synchronously with no scheduler behind it, so the sleep never
    ends and the call never returns -- a limitation of the harness, not of
    the driver. Swapping the import stub for `jr $ra` removes the sleep and
    leaves the register sequence, which is what is under test.

    IRX-9's stubs are eight bytes with the ordinal in the second word's low
    half, so the stub to patch is found by that rather than by position. The
    patch is global -- every module's `thbase` stub for that ordinal -- which
    is harmless only because nothing else runs while a call is in flight.
    """

    def __init__(self, bus) -> None:
        self.bus = bus
        self.saved: list[tuple[int, bytes]] = []

    def __enter__(self):
        ram = self.bus.ram
        for at, tag, _version, _flags, _bound in iopsim.scanLibraries(self.bus)[1]:
            if tag != "thbase":
                continue
            stub = at + 0x14
            while True:
                first = int.from_bytes(ram[stub:stub + 4], "little")
                second = int.from_bytes(ram[stub + 4:stub + 8], "little")
                if first == 0 and second == 0:
                    break
                if (second >> 26) == 9 and (second & 0xFFFF) == THBASE_DELAY:
                    self.saved.append((stub, bytes(ram[stub:stub + 4])))
                    ram[stub:stub + 4] = JR_RA.to_bytes(4, "little")
                stub += 8
        return self

    def __exit__(self, *_):
        for stub, original in self.saved:
            self.bus.ram[stub:stub + 4] = original
        return False


def main() -> int:
    image = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "build/rom.bin")
    bus, cpu = boot(image)
    table = findExports(bus, "cdvdman")
    print(f"cdvdman export table at {table:#010x}")

    def ordinal(n: int) -> int:
        return int.from_bytes(bus.ram[table + 0x14 + 4 * n:
                                      table + 0x18 + 4 * n], "little")

    scratch = 0x00080000                          # well past the resident image
    failures = []

    def expect(name: str, got, want) -> None:
        ok = got == want
        print(f"  {'ok  ' if ok else 'FAIL'} {name}: {got!r}"
              f"{'' if ok else f'  want {want!r}'}")
        if not ok:
            failures.append(name)

    print("\nordinal 26, sceCdReadNVM(0x0102):")
    bus.ram[scratch:scratch + 8] = bytes(8)
    answer = call(cpu, ordinal(26), (0x0102, scratch, scratch + 4))
    expect("returned", answer, 1)
    expect("data", hex(word(bus, scratch)), hex(0xA102))
    expect("status", bus.ram[scratch + 4], 0)
    expect("wire parameters", bus.sent[-1], (0x0A, [0x01, 0x02]))

    print("\nordinal 27, sceCdWriteNVM(0x0304, 0xBEEF):")
    answer = call(cpu, ordinal(27), (0x0304, 0xBEEF, scratch + 4))
    expect("returned", answer, 1)
    expect("wire parameters", bus.sent[-1], (0x0B, [0x03, 0x04, 0xBE, 0xEF]))

    print("\nordinal 31, open(1, 0, 2) -- the OSD's own call:")
    with NoDelay(bus):
        answer = call(cpu, ordinal(31), (1, 0, 2, scratch + 4))
    expect("returned", answer, 1)
    expect("wire parameters, a and b reversed", bus.sent[-1], (0x40, [0, 1, 2]))

    print("\nordinal 33, read two blocks:")
    bus.ram[scratch:scratch + 64] = bytes(64)
    answer = call(cpu, ordinal(33), (scratch, scratch + 60))
    expect("blocks completed", answer, 2)
    expect("block 0, sum byte stripped",
           bytes(bus.ram[scratch:scratch + 15]), bus.config[0])
    expect("block 1 follows at +15",
           bytes(bus.ram[scratch + 15:scratch + 30]), bus.config[1])
    expect("checksum verdict", int.from_bytes(
        bus.ram[scratch + 60:scratch + 64], "little"), 0)

    print("\nordinal 33 again, with one block's sum corrupted:")
    bad = bytearray(bus.config[0])
    bus.config[0] = bytes(bad)
    original = Mechacon.execute

    def corrupt(self, command):
        original(self, command)
        if command == 0x41:
            self.results[-1] ^= 0xFF
    Mechacon.execute = corrupt
    with NoDelay(bus):
        call(cpu, ordinal(31), (1, 0, 1, scratch + 4))
    answer = call(cpu, ordinal(33), (scratch, scratch + 60))
    expect("blocks completed", answer, 1)
    expect("checksum verdict", int.from_bytes(
        bus.ram[scratch + 60:scratch + 64], "little"), 1)
    Mechacon.execute = original

    print("\nordinal 34, write one block, and 32 to close:")
    with NoDelay(bus):
        call(cpu, ordinal(31), (1, 0, 1, scratch + 4))
    bus.ram[scratch:scratch + 15] = bytes(range(15))
    answer = call(cpu, ordinal(34), (scratch, scratch + 60))
    expect("blocks completed", answer, 1)
    command, params = bus.sent[-1]
    expect("command", hex(command), hex(0x42))
    expect("fifteen bytes plus their sum", params,
           list(range(15)) + [sum(range(15)) & 0xFF])
    answer = call(cpu, ordinal(32), (scratch + 4,))
    expect("returned", answer, 1)
    expect("read after close completes nothing",
           call(cpu, ordinal(33), (scratch, scratch + 60)), 0)

    print(f"\n{len(failures)} failure(s)" if failures else "\nall ok")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
