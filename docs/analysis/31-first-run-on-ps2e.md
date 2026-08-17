# The First Run on PS2e

`docs/project-state.md` §6 names PS2e — the same author's emulator, not part
of this repository — as the debugging target: it exposes a gdb-remote stub per
CPU, and it runs the retail image to a drawing `OSDSYS`. This is what happened
the first time our image ran there, and it is `docs/analysis/25`'s lesson
again: a second target asks questions the first one did not.

```sh
# PS2e, headless; the image is passed as the BIOS. The EE's serial console is
# printed on the tty log target.
ps2-app --bios build/rom.bin --cycles 100000000 --log 'warn,ps2_core::tty=info'
```

```
# PS2BiosRebuild EE kernel: entered at 0x80001000.
```

One line, and no more. The EE boots to our kernel and waits for an IOP that
never answers, where PCSX2 prints all four lines (`25`).

## Where the IOP was

`--debug-iop <port> --wait-debugger` holds the machine at the reset vector
until a client attaches; a breakpoint at `IOPBOOT`'s ROM address confirmed the
IOP reset path reaches it (BOOT-1 to BOOT-5 hold on PS2e), and interrupting a
few seconds later found it in a two-instruction loop:

```
bfc17e64  b     0xbfc17e64          # _iopboot_stop: BOOT-5b's idiom
bfc17e68  nop
```

`IOPBOOT` had given up. Single-stepping from its entry — six thousand steps,
every PC recorded — showed it never leaving the archive scan of `_iopboot_find`
(ARC-4): it walked the ROM window sixteen bytes at a time, met the one entry
that begins `RESE` and has `T` in its fifth byte, and rejected it on the third
test:

```
bfc181d4  lbu   $t1, 0x4($t0)       # 'T'
bfc181e4  lw    $t1, 0xc($t0)       # the entry's size ...
bfc181e8  andi  $t2, $t1, 0xf       # ... must be a multiple of sixteen
bfc181ec  beqz  $t2, 0xbfc1820c
```

The size at that address is `0x2500`, a multiple of sixteen; the debugger read
it back as such (`m` packets read the ROM the IOP reads). The test still
failed, because on the R3000A **the instruction after a load sees the
register's old value** — the load delay slot. `andi` read `$t1` one
instruction after `lw` wrote it, and got `0x54`, the `T` the `lbu` before had
left there; `0x54 & 0xF` is `4`, and the table was skipped. Real hardware
behaves this way; PS2e models it (its notes call it the "SCP" bug); PCSX2 does
not, which is why five days of running there never showed it.

## How many more

The whole IOP side had been written under `.set noreorder`, where the assembler
inserts nothing, and never checked for this. `tools/loaddelay.py` now reads a
linked ELF and reports every `lb`/`lbu`/`lh`/`lhu`/`lw`/`lwl`/`lwr` whose next
instruction reads the loaded register:

```sh
python3 tools/loaddelay.py build/iopboot.elf build/sysmem.elf build/loadcore.elf build/eesync.elf
python3 tools/loaddelay.py build/reset.elf --between _iop_reset eeReset      # the IOP half
```

Before any fix: **64 hazards** — 40 in `IOPBOOT`, 4 in the boot block's IOP
assembly, 2 in `SYSMEM`, 5 in `LOADCORE`, and 13 false positives in the boot
block's EE half, which the R5900 does not have this slot for (hence
`--range`). `EESYNC`, the one IOP module compiled from C++, had **none**: a
compiler targeting MIPS I schedules around the slot itself.

That last number decided the fix. The four sites in the boot block are
assembly for a reason that stands and were fixed by hand; `IOPBOOT`, `SYSMEM`
and `LOADCORE` are rewritten in C++, which removes the hazards and the class of
bug with them (`docs/implementation.md`). The checker stays, gating what
assembly remains.

## What this pins

- The IOP's load delay slot is real, and every hand-written IOP instruction
  sequence must respect it: a loaded value is usable one instruction later.
  A simulator or emulator that resolves loads immediately hides this class of
  bug entirely; `tools/iopsim.py` did, and so did PCSX2.
- Where PS2e and PCSX2 disagree, the disagreement was a fact about the machine
  that one of them modelled and the other forgave — as §6 said to expect. The
  question is settled by reading the R3000A, not by choosing an emulator.
