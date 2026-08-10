# EE Syscall Groups, and the Boot Tail

`docs/analysis/14-ee-kernel-syscalls.md` found the syscall table and its shape.
This asks what the 112 implemented slots are *for*, and follows the two that
matter most to the boot — the pair that ends up launching the OSD, which also
answers where `EELOAD` fits.

Two sources of evidence are used, both from the image itself: how the slots
cluster by target address, and the kernel's own diagnostic strings.

## Grouping by target

Sorting the table by target rather than by slot number makes the structure
visible — implementations of one subsystem sit together, in evenly sized
routines:

```sh
python3 tools/eeksys.py <outdir>/KERNEL --all
```

| Slots | Targets | Shape |
| --- | --- | --- |
| `0x0D`–`0x0F`, `0x14`–`0x1F` | `0x8C0`–`0xA80` | 0x40-byte leaf routines in the vector page |
| `0x21`–`0x44` (most) | `0x2FC0`–`0x35C0` | a dense run in 0x40/0x80 steps |
| `0x60`–`0x6A` | `0x28C0`–`0x2C40` | includes the three KSEG1 slots |
| `0x4A`–`0x4F`, `0x6E`, `0x6F`, `0x01` | `0xCD18`–`0xD9B8` | the far end of the image |

The first band matters most for the rebuild's layout: those handlers are inside
the `0x1000`-byte vector page, next to the exception vectors, because they must
stay reachable whatever else is mapped.

## The block-aliasing pattern

`14` recorded that sixteen targets are shared. Laid out by slot number, the
aliasing is not scattered — it comes in **consecutive blocks duplicated at a
fixed offset**:

| Block | Duplicate | Offset |
| --- | --- | --- |
| `0x14`–`0x19` | `0x1A`–`0x1F` | +6 |
| `0x63`–`0x66` | `0x67`–`0x6A` | +4 |

plus isolated pairs at `0x30`/`0x31`, `0x35`/`0x36`, `0x37`/`0x38`,
`0x45`/`0x46`, `0x47`/`0x48`.

A block duplicated wholesale is a strong hint that the two ranges are the same
operations published under two numbers — the convention `spec/02` IRX-6b
records on the IOP side. A rebuild must publish both ranges; collapsing them
would break callers that use the higher numbers.

## What the diagnostics name

The kernel reports its own failures, and the messages identify subsystems
better than any amount of disassembly:

```sh
strings -a -t x <outdir>/KERNEL | grep -E '^ *1(5|6)[0-9a-f]{3} #'
```

| Message | Tells us |
| --- | --- |
| `# INTC(%d) Handler does not exist.` | interrupt-controller handler registration |
| `# DMAC(%d) Handler does not exist.` | DMA-controller handler registration |
| `# <Thread> No active threads` | a thread scheduler |
| `# DisableDispatchThread is not supported in this version` | named, deliberately retired calls |
| `# SetHeap: HEAP_RELATIVE is not supported in this version` | a heap syscall with a mode argument |
| `# syscall (3): 'crt0.s' is old version` | slot `0x03` is retired, and the message blames the caller's startup code |
| `# syscall (63): 'crt0.s' is old version` | likewise slot `0x3F` — `63` decimal |

The last two are worth pausing on. Slots `0x03` and `0x3F` are two of the
thirteen bound to the "undefined" reporter (`14`), and the kernel carries a
*specific* message for each saying the caller is out of date. So those numbers
were once defined and were withdrawn — the table keeps them occupied precisely
so an old caller gets a diagnosis instead of a wild jump. That is the same
reasoning behind IRX-6a's occupied stubs, and it is an argument for reproducing
the retired slots rather than tidying them away.

The kernel also announces what it brings up, which is effectively a list of the
EE hardware a rebuild must initialise:

```
# Initialize Start.  /  DMAC  /  VU1  /  VIF1  /  GIF  /  VU0  /  VIF0  /  IPU
/  GS  /  INTC  /  TIMER  /  FPU  /  User Memory  /  Scratch Pad  /  Done.
```

## The boot tail: syscall 0x7B, EELOAD and the OSD

Three strings sit together near the end of the image — `EELOAD`,
`rom0:OSDSYS`, `BootBrowser` — and following their references closes the
question `13` left open about what loads `EELOAD`.

```sh
python3 tools/romdis.py <outdir>/KERNEL --cpu ee --vma 0x80000000 \
    --range 0x80005988 0x800059d4
```

**Slot `0x7B` is a four-instruction wrapper** that shifts its arguments along
and tail-calls slot `0x06`'s implementation with a fixed first argument:

```
80005988  move  $a2, $a1          # arg2 -> arg3
8000598c  move  $a1, $a0          # arg1 -> arg2
80005990  lui   $a0, 0x8001
80005994  j     0x80005598        # slot 0x06's implementation
80005998  addiu $a0, $a0, 0x5d30  # arg1 = "rom0:OSDSYS"
```

So **`0x7B` is `0x06` with the path pinned to `rom0:OSDSYS`**. And slot `0x06`
itself — the general program loader — holds `"EELOAD"` in a saved register
across its whole body:

```sh
python3 tools/romdis.py <outdir>/KERNEL --cpu ee --vma 0x80000000 \
    --range 0x800055c0 0x800055e0
# addiu $a1, $a1, 0x5d28   -> "EELOAD"
# move  $fp, $a1
```

`EELOAD` is therefore **the loader stub the program-loading syscall uses to
replace the running program** — not something the reset vector stages, which is
what `13` corrected, and not something loaded by name from the boot list.

Immediately after the wrapper is a routine that calls the same implementation
with the arguments already filled in:

```
800059a8  addiu $v0, $v0, 0x5d40   # "BootBrowser"
800059b4  addiu $a0, $a0, 0x5d30   # "rom0:OSDSYS"
800059bc  addiu $a1, $zero, 0x1    # argc = 1
800059c4  move  $a2, $sp           # argv = { "BootBrowser" }
800059c0  jal   0x80005598
```

That is the default boot: run `rom0:OSDSYS` with a single argument selecting the
browser. And `rom0:` is the device `ROMDRV` registers with `ioman` on the IOP
side (`docs/analysis/11-sif-and-rom-driver.md`) — so this one call crosses the
SIF, is served by the IOP's file manager, and reads back out of the same archive
the EE booted from.

## What this pins for the rebuild

- The syscall table's block aliasing must be reproduced, including the retired
  slots `0x03` and `0x3F`, which exist to diagnose out-of-date callers.
- The handlers for slots `0x0D`–`0x1F` live inside the vector page and must
  stay there.
- Slot `0x06` is the program loader and uses `EELOAD`; slot `0x7B` is the same
  call with the path fixed to `rom0:OSDSYS`; the default boot passes
  `argc = 1, argv = { "BootBrowser" }`.
- The EE kernel initialises DMAC, VU0/VU1, VIF0/VIF1, GIF, GS, IPU, INTC,
  TIMER, FPU, user memory and the scratchpad before any of that runs.

Next: `OSDSYS` itself, the last unexamined major component, and the first
`docs/spec/` document for the EE side (`docs/project-state.md` §5).
