#!/usr/bin/env python3
"""Judge CDVDFSV's configuration and NVM entry points against a modelled mechacon.

    python3 tools/fsvcheck.py build/rom.bin

`docs/spec/06-iop-kernel.md` IOP-13 is the EE-facing CDVD service. Nothing in
the boot calls it -- our own OSD binds `0x80000592` and nothing else -- and the
six entry points that carry the configuration record and the NVM are exactly
the ones a rebuild gets silently wrong: two different reply shapes, and an
`open` whose request word holds the mechacon's own reversed argument order.

So: boot the image under `tools/iopsim.py` with `tools/scmdcheck.py`'s
mechanism controller on the bus, find the `0x80000593` server the boot
registered, and call its dispatcher the way the RPC layer would. What is
checked is what has no other witness -- that `open`'s word is unpacked in the
wire's byte order and not the obvious one, that the read's blocks land at
`+0x8` of the reply behind the return and the status, that the NVM pair
answers in its own shape instead, and that an `fno` outside the table is still
acknowledged rather than dropped.
"""

import pathlib
import sys

sys.path.insert(0, "tools")

import scmdcheck  # noqa: E402

MAIN_SID = 0x80000593

# `src/iop/sif.hpp`: the EE raises SIF_STAT_SIFINIT in MSFLG to say it is
# there, and `SIFMAN`'s entry spins until it does. Nothing in this harness is
# an EE, so the boot would stop four modules short of `CDVDFSV`.
MSFLG = 0x1D000020
SIF_STAT_SIFINIT = 0x00010000


class Machine(scmdcheck.Mechacon):
    """The mechanism controller, plus the one bit an EE would have set.

    Standing in for the EE this far and no further is deliberate: what is
    under test is a dispatcher that reads a request buffer and calls CDVDMAN,
    which no part of the SIF touches. A harness that modelled the handshake
    properly would be testing the handshake.
    """

    def read(self, addr: int, size: int) -> int:
        if (addr & 0x1FFFFFFF) == MSFLG:
            return super().read(addr, size) | SIF_STAT_SIFINIT
        return super().read(addr, size)


def boot(image: pathlib.Path, steps: int = 60_000_000):
    bus = Machine(image.read_bytes())
    cpu = scmdcheck.iopsim.Cpu(bus)
    while cpu.steps < steps and cpu.stop_reason is None:
        cpu.step()
    return bus, cpu

# spec/06 IOP-13e/13e1, and the ordinals behind them (IOP-8j, IOP-8k).
FNO_READ_NVM, FNO_WRITE_NVM = 8, 9
FNO_OPEN, FNO_CLOSE, FNO_READ, FNO_WRITE = 14, 15, 16, 17

CONFIG_BLOCK = 15


def findServer(bus, sid: int) -> int:
    """The registered server record for `sid`, found by its own first word.

    `src/iop/sifrpc.hpp` fixes the record: the id at `+0x00` and the
    dispatcher at `+0x04`. A scan is enough because the id is a distinctive
    constant and the record sits in CDVDFSV's `.bss`.
    """
    ram = bus.ram
    want = sid.to_bytes(4, "little")
    for at in range(0, len(ram) - 8, 4):
        if ram[at:at + 4] != want:
            continue
        function = int.from_bytes(ram[at + 4:at + 8], "little")
        if 0x1000 < function < 0x200000:
            return at
    raise SystemExit(f"{sid:#010x}: no registered server after the boot")


def main() -> int:
    image = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "build/rom.bin")
    bus, cpu = boot(image)

    server = findServer(bus, MAIN_SID)
    dispatch = int.from_bytes(bus.ram[server + 4:server + 8], "little")
    request = int.from_bytes(bus.ram[server + 8:server + 12], "little")
    print(f"0x80000593 server at {server:#010x}, dispatcher {dispatch:#010x}, "
          f"request buffer {request:#010x}")

    failures: list[str] = []

    def expect(name: str, got, want) -> None:
        ok = got == want
        print(f"  {'ok  ' if ok else 'FAIL'} {name}: {got!r}"
              f"{'' if ok else f'  want {want!r}'}")
        if not ok:
            failures.append(name)

    def call(fno: int, payload: bytes = b"", size: int | None = None) -> int:
        """One RPC call, and the reply address the dispatcher answers with."""
        bus.ram[request:request + max(len(payload), 16)] = \
            payload.ljust(max(len(payload), 16), b"\0")
        with scmdcheck.NoDelay(bus):
            return scmdcheck.call(
                cpu, dispatch,
                (fno, request, len(payload) if size is None else size))

    def word(at: int, index: int = 0) -> int:
        return int.from_bytes(bus.ram[at + 4 * index:at + 4 * index + 4],
                              "little")

    # -- the session, in the order the OSD drives it ------------------------

    print("\nopen: the request word carries the wire's own byte order:")
    # IOP-13e: the OSD's open(1, 0, 2). byte 0 = second argument, byte 1 =
    # first, byte 2 = count. Unpacked the obvious way this sends (0, 1).
    reply = call(FNO_OPEN, (0x00020100).to_bytes(4, "little"))
    expect("the call returned a reply address", reply != 0, True)
    expect("open succeeded", word(reply), 1)
    expect("the command the mechacon saw", bus.sent[-1][0], 0x40)
    expect("its parameters, unreversed on the wire", bus.sent[-1][1], [0, 1, 2])
    expect("the status word at +0x4", word(reply, 1), 0)

    print("\nread: the blocks land behind the return and the status:")
    reply = call(FNO_READ)
    expect("blocks completed", word(reply), 2)
    first = bytes(bus.ram[reply + 8:reply + 8 + CONFIG_BLOCK])
    second = bytes(bus.ram[reply + 8 + CONFIG_BLOCK:
                           reply + 8 + 2 * CONFIG_BLOCK])
    expect("block 0, its sum byte stripped", first, bytes(range(15)))
    expect("block 1 follows it immediately", second, bytes(range(16, 31)))
    expect("the checksum verdict at +0x4", word(reply, 1), 0)

    print("\nwrite: the blocks come from the request:")
    reply = call(FNO_WRITE, bytes(range(15)) + bytes(range(16, 31)))
    expect("blocks completed", word(reply), 2)
    expect("the command the mechacon saw", bus.sent[-1][0], 0x42)
    expect("fifteen bytes plus their sum",
           bus.sent[-1][1], list(range(16, 31)) + [sum(range(16, 31)) & 0xFF])

    print("\nclose:")
    reply = call(FNO_CLOSE)
    expect("close succeeded", word(reply), 1)
    expect("the command the mechacon saw", bus.sent[-1][0], 0x43)
    reply = call(FNO_READ)
    expect("a read after close completes nothing", word(reply), 0)

    # -- the NVM pair, which answers in the other shape ---------------------

    print("\nthe NVM pair answers two words further in (IOP-13e1):")
    reply = call(FNO_READ_NVM, (0x0102).to_bytes(4, "little") + b"\0" * 4,
                 size=8)
    expect("the return at +0x0", word(reply), 1)
    expect("the address echoed at +0x4", word(reply, 1), 0x0102)
    expect("the data at +0x8, status in its bit 16", word(reply, 2), 0xA102)

    reply = call(FNO_WRITE_NVM,
                 (0x0304).to_bytes(4, "little") + (0xBEEF).to_bytes(4, "little"),
                 size=8)
    expect("the return at +0x0", word(reply), 1)
    expect("the command the mechacon saw", bus.sent[-1][0], 0x0B)
    expect("its parameters", bus.sent[-1][1], [3, 4, 0xBE, 0xEF])

    # -- the table's edges (IOP-13b) ----------------------------------------

    print("\nan fno outside the table is acknowledged, not dropped:")
    for fno in (0, 26, 0xFFFFFFFF):
        expect(f"fno {fno:#x} still answers a reply address", call(fno) != 0,
               True)

    print("\nall ok" if not failures
          else f"\n{len(failures)} failure(s): {', '.join(failures)}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
