#!/usr/bin/env python3
"""Judge LOADCORE's versioned registration and the supersession behind it.

    python3 tools/libcheck.py build/rom.bin

`docs/spec/02-module-abi.md` IRX-10a is exercised by every module the boot
loads, and IRX-11 is not: taking a superseded library's clients needs a second
generation of a library that something has already bound to, and this image
has none -- its `SYSCLIB` publishes no provisional `stdio`, so the one case
the reference shows never arises here.

So: boot the image under `tools/iopsim.py`, build a second generation of a
library that already has clients, and hand it to `loadcore` ordinal 6. What is
checked is what a rebuild gets wrong silently -- that the clients are actually
re-pointed at the new table rather than merely moved between lists, that a
client with the pin of IRX-11 stays where it is, and that a registration the
version test refuses leaves every client exactly where it was.
"""

import pathlib
import sys

sys.path.insert(0, "tools")

import iopsim  # noqa: E402
import scmdcheck  # noqa: E402

EXPORT_MAGIC = 0x41C00000
REGISTER_VERSIONED = 6

CLIENT_LINK = 4
CLIENT_FLAGS = 0xA
CLIENT_PINNED = 1

ERROR_LIBRARY_FOUND = -212

# Somewhere no module was placed, for the tables this builds by hand.
SCRATCH = 0x00100000
# A recognisable body for every ordinal of the second generation: a bound stub
# becomes `j ENTRY_BASE + 4 * ordinal`, which no real module's code occupies.
ENTRY_BASE = 0x00110000


class Ram:
    def __init__(self, bus) -> None:
        self.ram = bus.ram

    def word(self, at: int) -> int:
        return int.from_bytes(self.ram[at:at + 4], "little")

    def setWord(self, at: int, value: int) -> None:
        self.ram[at:at + 4] = (value & 0xFFFFFFFF).to_bytes(4, "little")

    def half(self, at: int) -> int:
        return int.from_bytes(self.ram[at:at + 2], "little")

    def setHalf(self, at: int, value: int) -> None:
        self.ram[at:at + 2] = (value & 0xFFFF).to_bytes(2, "little")

    def clients(self, table: int) -> list[int]:
        """The chain an exporter's `+0x4` heads (IRX-11)."""
        chain, at, guard = [], self.word(table + CLIENT_LINK), 0
        while at != 0 and guard < 256:
            chain.append(at)
            at = self.word(at + CLIENT_LINK)
            guard += 1
        return chain


def buildTable(ram: Ram, at: int, tag: bytes, version: int, count: int) -> int:
    """A second generation of `tag`, laid out as IRX-4 describes."""
    ram.setWord(at, EXPORT_MAGIC)
    ram.setWord(at + 4, 0)                      # no clients of its own yet
    ram.setHalf(at + 8, version)
    ram.setHalf(at + 0xA, 0)
    ram.ram[at + 12:at + 20] = tag.ljust(8, b"\0")
    for ordinal in range(count):
        ram.setWord(at + 0x14 + 4 * ordinal, ENTRY_BASE + 4 * ordinal)
    ram.setWord(at + 0x14 + 4 * count, 0)       # IRX-5b's terminator
    return at


def main() -> int:
    image = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "build/rom.bin")
    bus, cpu = scmdcheck.boot(image)
    ram = Ram(bus)

    loadcore = scmdcheck.findExports(bus, "loadcore")
    register = ram.word(loadcore + 0x14 + 4 * REGISTER_VERSIONED)

    # The library to supersede: whichever registered one the boot left with the
    # most clients, so the walk has something to walk.
    exports, _imports = iopsim.scanLibraries(bus)
    victim_at, victim_tag, victim_version, _flags = max(
        exports, key=lambda e: len(ram.clients(e[0])))
    original = ram.clients(victim_at)
    print(f"superseding {victim_tag} v{victim_version:#06x} at "
          f"{victim_at:#010x}, with {len(original)} clients")
    if len(original) < 2:
        print("FAIL: the boot left no library with two clients to move")
        return 1

    tag = bytes(bus.ram[victim_at + 12:victim_at + 20])
    entries = 0
    while ram.word(victim_at + 0x14 + 4 * entries) != 0:
        entries += 1

    failures: list[str] = []

    def expect(name: str, got, want) -> None:
        ok = got == want
        print(f"  {'ok  ' if ok else 'FAIL'} {name}: {got!r}"
              f"{'' if ok else f'  want {want!r}'}")
        if not ok:
            failures.append(name)

    def firstStub(client: int) -> int:
        return ram.word(client + 0x14)

    # -- a registration the version test refuses moves nothing --------------

    print("\nan equal minor version is refused, and nothing moves:")
    same = buildTable(ram, SCRATCH, tag, victim_version, entries)
    before = [(c, firstStub(c)) for c in original]
    expect("the return", toSigned(scmdcheck.call(cpu, register, (same,))),
           ERROR_LIBRARY_FOUND)
    expect("the old table keeps its clients",
           ram.clients(victim_at) == original, True)
    expect("and none of their stubs were rewritten",
           [(c, firstStub(c)) for c in original] == before, True)

    # -- the pin exempts one client (IRX-11) --------------------------------

    pinned = original[0]
    ram.setHalf(pinned + CLIENT_FLAGS,
                ram.half(pinned + CLIENT_FLAGS) | CLIENT_PINNED)
    pinned_stub = firstStub(pinned)

    print("\na higher minor version takes the unpinned clients:")
    newer = buildTable(ram, SCRATCH + 0x400, tag, victim_version + 1, entries)
    expect("the return", scmdcheck.call(cpu, register, (newer,)), 0)

    moved = ram.clients(newer)
    expect("every unpinned client is now the new table's",
           sorted(moved) == sorted(c for c in original if c != pinned), True)
    expect("the count that moved", len(moved), len(original) - 1)
    expect("the pinned one stayed on the old table",
           ram.clients(victim_at) == [pinned], True)
    expect("and its stub was not rewritten",
           firstStub(pinned) == pinned_stub, True)

    print("\nthe moved clients really call the new table:")
    for client in moved[:3]:
        stub = firstStub(client)
        ordinal = ram.word(client + 0x18) & 0xFFFF        # IRX-9a leaves it
        target = (stub & 0x03FFFFFF) << 2
        expect(f"client {client:#x} ordinal {ordinal} jumps to the new body",
               (stub >> 26, target), (2, ENTRY_BASE + 4 * ordinal))

    print("\nthe new table is the one an importer now finds:")
    chain = registryChain(ram)
    expect("it is at the registry head", chain[0] == newer, True)
    expect("and the superseded one is still on the registry",
           victim_at in chain, True)

    print("\nall ok" if not failures
          else f"\n{len(failures)} failure(s): {', '.join(failures)}")
    return 1 if failures else 0


def registryChain(ram: Ram) -> list[int]:
    chain, at, guard = [], ram.word(0x001F8010), 0
    while at != 0 and guard < 256:
        chain.append(at)
        at = ram.word(at)
        guard += 1
    return chain


def toSigned(value: int) -> int:
    return value - 0x100000000 if value >= 0x80000000 else value


if __name__ == "__main__":
    raise SystemExit(main())
