# The EE Kernel: Vectors, Exceptions and Syscalls

`docs/analysis/13-ee-boot-path.md` established that the `KERNEL` file is copied
to physical 0 and entered at `0x80001000`, its first `0x1000` bytes being the EE
exception vectors. This looks inside it. The find that matters most is the
**syscall table** — the EE's counterpart of the PS1 BIOS's A0/B0/C0 tables, and
the structure a reimplementation is ultimately judged against.

`KERNEL` runs at physical 0, so it is disassembled at `0x80000000` and every
address below is both a run-time address and a file offset plus `0x80000000`:

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
python3 tools/romdis.py <outdir>/KERNEL --cpu ee --vma 0x80000000 --range 0x80000280 0x80000300
```

## The vector page

The first `0x1000` bytes are sparse — mostly zero, with content at the R5900's
exception vectors and then a run of small routines:

```sh
python3 - <<'PY'
d = open('<outdir>/KERNEL', 'rb').read()
for r in range(0, 0x1000, 16):
    if any(d[r:r + 16]):
        print(f'{r:#06x}: {d[r:r + 16].hex(" ", 4)}')
PY
```

| Offset | R5900 vector |
| --- | --- |
| `0x000` | TLB refill |
| `0x080` | counter |
| `0x100` | debug |
| `0x180` | common exception |
| `0x200` | interrupt |

`0x000` and `0x180` hold the same seven instructions, and they are the entry to
everything: save `$t9` to a fixed slot, read `Cause`, mask it to the `ExcCode`
field, and use that as an index.

The rest of the page, from about `0x800`, is a set of leaf routines that
manipulate `Status` and the interrupt-controller registers — enable/disable
helpers reached through a small jump table at `0x80000AA0`. They live in the
vector page because they must be reachable regardless of what is mapped.

## Exception dispatch

The entry does not branch on the cause; it loads a handler pointer from a table:

```
80000000  lui   $k0, 0x8001
80000004  sd    $t9, 0x5378($k0)     # save $t9 in a fixed slot
80000008  mfc0  $t9, $13             # Cause
8000000c  lui   $k0, 0x8001
80000010  andi  $t9, $t9, 0x7c       # ExcCode, already scaled by 4
80000014  addu  $k0, $k0, $t9
80000018  lw    $k0, 0x5340($k0)     # table at 0x80015340
8000001c  lui   $t9, 0x8001
80000020  jr    $k0
80000024  ld    $t9, 0x5378($t9)     # restore in the delay slot
```

Masking `Cause` with `0x7C` yields the exception code *already multiplied by
four*, so it indexes the word table directly — there is no shift. The table:

```sh
python3 tools/eeksys.py <outdir>/KERNEL
```

| `ExcCode` | Handler |
| --- | --- |
| 8 (`Sys`) | `0x80000280` |
| every other code 0–13 | `0x800140C0` |

So **only the syscall exception has its own handler**; interrupts, TLB faults,
address errors, breakpoints and the rest all funnel into one common routine.
That is a deliberate shape rather than an accident of this build — a rebuild
must provide the table, and may not assume a handler per cause.

## The syscall entry

```sh
python3 tools/romdis.py <outdir>/KERNEL --cpu ee --vma 0x80000000 --range 0x80000280 0x80000300
```

```
80000280  bltzl $v1, ...           # negative numbers are legal
80000284  negu  $v1, $v1           # ... and are negated
80000288  addiu $k0, $zero, 0x7c
8000028c  bne   $k0, $v1, 0x29c    # 0x7c takes its own path
80000294  j     0x800141fc
8000029c  lui   $k0, 0x8000        # the general path
800002a0  sq    ...                # save context with 128-bit stores
800002ac  mfc0  $1, $12            # Status
800002b0  addiu $k0, $zero, -0x1c
800002b4  and   $1, $1, $k0        # clear the low mode bits
800002b8  mtc0  $1, $12
800002c0  move  $k0, $sp
800002c4  lui   $sp, 0x8002
800002c8  addiu $sp, $sp, -0x7180  # switch to a kernel stack
800002d8  mfc0  $k0, $14           # EPC
800002dc  addiu $k0, $k0, 0x4      # step past the syscall
800002e4  mtc0  $k0, $14           # so eret resumes after it
800002ec  sll   $v1, $v1, 0x2
800002f0  lui   $k0, 0x8001
800002f4  addu  $k0, $k0, $v1
800002f8  lw    $k0, 0x4f40($k0)   # the syscall table
800002fc  jalr  $k0
```

Four things a rebuild must match exactly:

- **The syscall number is in `$v1`**, not `$v0`, and **a negative number is
  negated** rather than rejected. Callers use the sign as a flag, so both forms
  reach the same slot.
- **`EPC` is advanced by 4 inside the handler**, so the eventual `eret` resumes
  after the `syscall` instruction rather than re-executing it.
- The handler **switches to its own stack** near `0x80018E80` before doing
  anything that can nest.
- Context is saved with `sq` — 128-bit stores — because the R5900's registers
  are 128 bits wide and the upper halves must survive a syscall.

## The syscall table

```
0x80014F40, indexed by the (absolute) syscall number, 125 slots: 0x00 .. 0x7C
```

```sh
python3 tools/eeksys.py <outdir>/KERNEL
# syscall table at 0x80014f40: 125 slots, 98 distinct targets, 0 null
```

- **125 slots, none null.** Every syscall number in range resolves to code.
- **98 distinct targets**, so slots are shared. `tools/eeksys.py` groups them:
  sixteen targets are reached from more than one slot, and the aliasing is
  systematic rather than incidental — **slots `0x14`–`0x19` duplicate
  `0x1A`–`0x1F` one for one**, six consecutive pairs, alongside isolated pairs
  at `0x30`/`0x31`, `0x35`/`0x36`, `0x37`/`0x38`, `0x45`/`0x46`, `0x47`/`0x48`
  and `0x63`/`0x67`. This is the same publishing convention `spec/02` IRX-6b
  records for the IOP libraries: two published numbers, one implementation.
- **Thirteen slots share `0x80001564`**, which is not a silent stub but a
  reporter: it recovers the number and prints a diagnostic naming it. Slots
  `0x00`, `0x03`, `0x08`, `0x3F`, `0x54`–`0x5B` and `0x7C` are undefined this
  way. (The message text itself is original expression; a rebuild supplies its
  own, per `docs/clean-room-policy.md` §3.)

That the "unimplemented" case *announces itself* is worth carrying into the
rebuild: it turns a whole class of mistake into a visible message instead of a
wrong return value, and it is the same instinct as the boot block's POST `0xFA`
(`spec/03` BOOT-5b).

### Three slots run uncached on purpose

Slots `0x60`, `0x61` and `0x62` point at `0xA0002C00`, `0xA00028C0` and
`0xA0002980` — addresses inside `KERNEL` (its span is `0x0..0x16E28`) but
reached through **KSEG1, the uncached window**, while the other 122 use the
cached KSEG0 alias.

```sh
python3 tools/eeksys.py <outdir>/KERNEL --slot 0x60
# syscall 0x60 -> 0xa0002c00 (kseg1), file offset 0x2c00
```

The choice of alias is part of each slot's contract, not a formatting detail: a
rebuild that publishes the KSEG0 address of those three handlers would run them
cached and change their behaviour around device memory.

## What this pins for the rebuild

- The vector page must supply all five R5900 vectors, with the common entry
  dispatching through a table at `0x80015340` indexed by `Cause & 0x7C`.
- Only `ExcCode 8` has a dedicated handler; the rest share one.
- Syscall number in `$v1`, negatives negated, `EPC` advanced by 4, kernel stack
  switch, context saved with `sq`.
- A 125-slot table at `0x80014F40` with no null entries, thirteen of them
  bound to a reporter that names the undefined number, and three published
  through KSEG1.

Next: what the defined syscalls actually do, group by group — the same
analysis → spec → implementation pattern the IOP side followed
(`docs/project-state.md` §5).
