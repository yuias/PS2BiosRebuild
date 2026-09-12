#!/usr/bin/env python3
"""Judge SYSMEM's allocator against the three modes spec/02 IRX-15b names.

    python3 tools/memcheck.py build/rom.bin

The boot exercises modes 0 and 1 heavily -- every module image and every raw
file it is built from comes out of them -- but nothing in the image asks for
mode 2, and nothing asks ordinals 9 and 10 about an address that is not the
block just allocated. Those would otherwise sit in the image unexecuted.

So: boot the image under `tools/iopsim.py`, find the export table LOADCORE
registered, and call the ordinals on the heap the boot left behind. What is
checked is what a rebuild gets wrong silently -- that the two ends really do
grow towards each other (IRX-15c) rather than sharing one cursor, that a
release actually returns the range to the free space, that mode 2 refuses a
range it does not wholly own, and that the size query answers for an address
in the middle of a block and not only for its start.
"""

import pathlib
import sys

sys.path.insert(0, "tools")

import iopsim  # noqa: E402
import scmdcheck  # noqa: E402

UNIT = 0x100
FREE_MARK = 0x80000000

LOWEST, HIGHEST, AT_ADDRESS = 0, 1, 2


def main() -> int:
    image = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "build/rom.bin")
    bus, cpu = scmdcheck.boot(image)
    table = scmdcheck.findExports(bus, "sysmem")
    print(f"sysmem export table at {table:#010x}")

    def ordinal(n: int) -> int:
        return int.from_bytes(bus.ram[table + 0x14 + 4 * n:
                                      table + 0x18 + 4 * n], "little")

    allocate, release = ordinal(4), ordinal(5)
    block_address, block_size = ordinal(9), ordinal(10)

    def alloc(mode: int, size: int, address: int = 0) -> int:
        return scmdcheck.call(cpu, allocate, (mode, size, address))

    def free(address: int) -> int:
        return toSigned(scmdcheck.call(cpu, release, (address,)))

    def sizeOf(address: int) -> int:
        return scmdcheck.call(cpu, block_size, (address,))

    def startOf(address: int) -> int:
        return scmdcheck.call(cpu, block_address, (address,))

    failures: list[str] = []

    def expect(name: str, got, want) -> None:
        ok = got == want
        shown = f"{got:#x}" if isinstance(got, int) and got >= 0 else repr(got)
        wanted = f"{want:#x}" if isinstance(want, int) and want >= 0 else repr(want)
        print(f"  {'ok  ' if ok else 'FAIL'} {name}: {shown}"
              f"{'' if ok else f'  want {wanted}'}")
        if not ok:
            failures.append(name)

    # -- the two ends grow towards each other (IRX-15b, IRX-15c) -------------

    print("\nmodes 0 and 1, from opposite ends of the same free space:")
    low_one = alloc(LOWEST, UNIT)
    low_two = alloc(LOWEST, UNIT)
    high_one = alloc(HIGHEST, UNIT)
    high_two = alloc(HIGHEST, UNIT)
    expect("mode 0 twice climbs by one unit", low_two - low_one, UNIT)
    expect("mode 1 twice descends by one unit", high_one - high_two, UNIT)
    expect("mode 1 is above mode 0", high_two > low_two, True)

    print("\na size is rounded up to a whole unit (IRX-15a):")
    odd = alloc(LOWEST, 1)
    expect("one byte is one unit", sizeOf(odd), UNIT)
    expect("a zero size is refused", alloc(LOWEST, 0), 0)
    expect("the block starts on a unit boundary", odd % UNIT, 0)

    print("\nordinals 9 and 10 answer for an address inside a block:")
    expect("the size of a block, asked at its middle", sizeOf(odd + UNIT // 2),
           UNIT)
    expect("the start of a block, asked at its middle",
           startOf(odd + UNIT // 2), odd)

    # -- a release really gives the range back -------------------------------

    print("\na release returns the range to the free space:")
    expect("releasing the top block", free(high_two), 0)
    expect("mode 1 takes the same address again", alloc(HIGHEST, UNIT),
           high_two)
    expect("releasing a block that was never handed out", free(high_two - UNIT),
           -1)
    expect("releasing an unaligned address", free(low_one + 1), -1)
    expect("releasing the middle of a block", free(odd + UNIT // 2), -1)

    print("\nout of order, which is what a single cursor cannot do:")
    expect("releasing the lower of two low blocks", free(low_one), 0)
    expect("the freed range reads back as free",
           sizeOf(low_one), UNIT | FREE_MARK)
    expect("and mode 0 fills the hole rather than climbing past it",
           alloc(LOWEST, UNIT), low_one)

    # -- mode 2 (IRX-15b) ----------------------------------------------------

    print("\nmode 2 places at an address, or refuses:")
    expect("releasing the block to place into", free(low_two), 0)
    expect("an unaligned address is refused outright",
           alloc(AT_ADDRESS, UNIT, low_two + 1), 0)
    expect("a range overlapping a live block is refused",
           alloc(AT_ADDRESS, 2 * UNIT, low_two), 0)
    expect("the free range itself is granted",
           alloc(AT_ADDRESS, UNIT, low_two), low_two)
    expect("and granting it makes a second request fail",
           alloc(AT_ADDRESS, UNIT, low_two), 0)
    expect("an address below the heap is refused",
           alloc(AT_ADDRESS, UNIT, 0x1000), 0)
    expect("an address above the heap is refused",
           alloc(AT_ADDRESS, UNIT, 0x001F8000), 0)

    print("\nno mode falls back to another (IRX-15d):")
    expect("a request larger than the whole heap", alloc(LOWEST, 0x400000), 0)
    expect("the same request at the high end", alloc(HIGHEST, 0x400000), 0)
    expect("a fourth mode is not a mode", alloc(3, UNIT), 0)

    print("\nall ok" if not failures
          else f"\n{len(failures)} failure(s): {', '.join(failures)}")
    return 1 if failures else 0


def toSigned(value: int) -> int:
    return value - 0x100000000 if value >= 0x80000000 else value


if __name__ == "__main__":
    raise SystemExit(main())
