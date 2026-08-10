# EE Syscalls: Exceptions and Interrupts

`docs/analysis/15-ee-syscall-groups.md` grouped the syscall table and picked out
the boot tail. This takes the first group of implementations in detail: the
slots that install exception and interrupt handlers, and the four that enable
and disable interrupt sources.

They are the natural first group because they are small leaf routines, because
the kernel's own diagnostics name them, and because they turn out to write
directly into the dispatch table `docs/spec/04-ee-kernel.md` EE-6a already
pins — which makes that requirement stricter than it first looked.

All of them live inside the vector page (EE-8f), so their addresses are module
offsets below `0x1000`:

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
python3 tools/romdis.py <outdir>/KERNEL --cpu ee --vma 0x80000000 --range 0x800009c0 0x800009f0
```

## There are two dispatch tables, not one

`14` found the exception table at `0x80015340`. There is a **second** table
immediately after it at `0x80015380`, and the two are read from different
places:

| Table | Read by | Indexed by | Entries |
| --- | --- | --- | --- |
| `0x80015340` | the common exception entry at `0x80000000`/`0x180` | `Cause & 0x7C` | 14 |
| `0x80015380` | the **interrupt** vector at `0x80000200` (`lw $k0, 0x5380($k0)` at `0x80000240`) | interrupt number × 4 | 8 |

```sh
python3 tools/romdis.py <outdir>/KERNEL --cpu ee --vma 0x80000000 --range 0x80000240 0x80000248
python3 - <<'PY'
import struct
d = open('<outdir>/KERNEL', 'rb').read()
for i in range(8):
    print(i, hex(struct.unpack_from('<I', d, 0x15380 + i * 4)[0]))
PY
```

Only three of its eight entries are populated:

| Index | Handler | Corroborating string |
| --- | --- | --- |
| 2 | `0x80000380` | `# INT: INTC (%d)` |
| 3 | `0x800004C0` | `# INT: DMAC (%d)` |
| 7 | `0x80000600` | `# INT: CPU Timer` |

The kernel carries exactly three `# INT:` diagnostics and the table has exactly
three live entries, at the indexes the R5900's `Cause.IP` assigns to the
interrupt controller, the DMA controller and the counter. Two independent
observations agreeing is what makes this identification solid rather than
plausible.

## Installing handlers: slots 0x0D, 0x0E, 0x0F

Three syscalls write into those tables, and their accepted index ranges are
disjoint:

```
800009c0  addiu $t0, $a0, -0x1     # slot 0x0D
800009c4  sltiu $t0, $t0, 0x3      #   accepts 1 <= code <= 3
800009c8  beqz  $t0, ...           #   out of range -> return 0
800009d0  sll   $a0, $a0, 0x2
800009dc  jr    $ra
800009e0  sw    $a1, 0x5340($1)    #   into the exception table

80000a00  addiu $t0, $a0, -0x4     # slot 0x0E
80000a04  sltiu $t0, $t0, 0xa      #   accepts 4 <= code <= 13
80000a20  sw    $a1, 0x5340($1)    #   into the exception table

80000a40  sltiu $t0, $a0, 0x8      # slot 0x0F
80000a5c  sw    $a1, 0x5380($1)    #   into the *interrupt* table
```

| Slot | Table | Accepted index |
| --- | --- | --- |
| `0x0D` | exception | `ExcCode` 1–3 — the TLB-related causes |
| `0x0E` | exception | `ExcCode` 4–13 |
| `0x0F` | interrupt | 0–7 |

Three consequences worth recording:

- **The dispatch tables are writable at run time.** `spec/04` EE-6a describes
  the exception table as a static structure; it is really an installable one,
  and a rebuild must place it in writable memory and publish these three
  syscalls to modify it.
- **`ExcCode 0` cannot be installed.** Slot `0x0D` starts at 1 and `0x0E` ends
  at 13, so the `Int` slot is not reachable through either — interrupts are
  routed by the second table instead.
- **`ExcCode 8` *can* be replaced**, since `0x0E` accepts 4–13 and 8 falls
  inside. A program can therefore take over the syscall handler itself.

Each returns the handler it was given on success and `0` when the index is out
of range; none returns the previous handler, so a caller that wants to chain
must have kept it.

## Enabling and disabling interrupt sources

Four slots, each duplicated by the `0x14`–`0x19` → `0x1A`–`0x1F` block of
`spec/04` EE-8c, manipulate one bit of a hardware mask:

| Slots | Register | Bit | Action |
| --- | --- | --- | --- |
| `0x14`, `0x1A` | `0x1000F010` (INTC) | `1 << n` | set if clear |
| `0x15`, `0x1B` | `0x1000F010` (INTC) | `1 << n` | clear if set |
| `0x16`, `0x1C` | `0x1000E010` (DMAC) | `0x10000 << n` | set if clear |
| `0x17`, `0x1D` | `0x1000E010` (DMAC) | `0x10000 << n` | clear if set |

```sh
python3 tools/romdis.py <outdir>/KERNEL --cpu ee --vma 0x80000000 --range 0x800008c0 0x800008ec
python3 tools/romdis.py <outdir>/KERNEL --cpu ee --vma 0x80000000 --range 0x80000940 0x8000096c
```

Two details of the encoding must be reproduced exactly:

- **The DMAC bit starts at 16**, not 0: the routine loads `lui $1, 0x1` before
  shifting, so source `n` is bit `16 + n`. That is the mask half of the DMAC
  status register, and using bit `n` instead would acknowledge interrupts
  rather than mask them.
- **The registers are write-to-toggle, so the code reads first.** Each routine
  tests the current bit and *only writes when the write will change it*,
  returning `1` when it acted and `0` when the bit was already in the wanted
  state. A rebuild that writes unconditionally would toggle the bit the wrong
  way on the second call.

That return value is the whole contract: callers use it to decide whether they
need to restore the previous state, which is why the "already in that state"
case must return `0` rather than succeeding silently.

## What this pins for the rebuild

- Two dispatch tables, adjacent: exceptions at `0x80015340` (14 entries) and
  interrupts at `0x80015380` (8 entries), the latter read by the interrupt
  vector and populated at indexes 2, 3 and 7.
- Slots `0x0D`/`0x0E`/`0x0F` install into them over disjoint index ranges,
  return the installed handler or `0`, and never return the previous one.
- The tables must be writable, and `ExcCode 0` must not be installable while
  `ExcCode 8` must be.
- The INTC/DMAC enable/disable quartet toggles `0x1000F010` bit `n` and
  `0x1000E010` bit `16 + n`, reads before writing, and returns whether it
  acted.

Next: the dense run of slots `0x21`–`0x44`, which the diagnostics of `15`
suggest is the thread scheduler (`docs/project-state.md` §5).
