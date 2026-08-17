# EE Syscalls: The Main Thread's Setup

`docs/spec/05-ee-syscall-abi.md` SYS-1 gives slots `0x3C`, `0x3D` and `0x3E`
their signatures and nothing more, because until now nothing needed more. The
first program built with the PS2SDK toolchain and stored in our archive
(`docs/project-state.md` §6, M1) faults on `0x3C` before it reaches `main`:
its runtime calls `0x3C`, `0x3D` and `0x64` in that order, and takes its
argument list from what `0x3C` wrote. This document reads the three, and the
two things they lean on — the kernel's thread table and the argument list the
program loader stores for them.

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
python3 tools/eeksys.py <outdir>/KERNEL --slot 0x3c    # -> 0x80005190
python3 tools/eeksys.py <outdir>/KERNEL --slot 0x3d    # -> 0x80005298
python3 tools/eeksys.py <outdir>/KERNEL --slot 0x3e    # -> 0x800052d0
python3 tools/eeksys.py <outdir>/KERNEL --slot 0x07    # -> 0x80002f80
python3 tools/romdis.py <outdir>/KERNEL --cpu ee --vma 0x80000000 --range 0x80005190 0x800052f0
```

## Slot 0x3C: set the main thread up, and answer with its stack pointer

The handler takes **five** arguments. Four are in `$a0`–`$a3` as SYS-1 says;
the fifth is read from `$t0`:

```
80005190  addiu $sp, $sp, -0x70
80005194  lui   $v0, 0xffff
8000519c  move  $t1, $a1            # stack
800051a4  move  $s5, $a3            # args
800051ac  move  $s4, $t0            # root -- the fifth argument
800051b4  move  $s3, $a0            # gp
800051bc  move  $s2, $a2            # stack_size
800051c4  ori   $v0, $v0, 0xffff
800051c8  bne   $t1, $v0, 0x800051e0
```

SYS-1b's "no slot takes a fifth argument" was a liveness result over
`$a0`–`$a3` and the stack; `$t0` was not in its scope. The SDK's runtime sets
`$t0` before the `syscall`, so it is a caller-supplied argument, not a kernel
mode.

**A stack of `-1` means "the top of memory".** The comparison is 64-bit —
`lui`/`ori` builds `0xFFFFFFFFFFFFFFFF` — so a `-1` that is not sign-extended
is not `-1`:

```
800051d0  jal   0x80000c40          # returns the memory size (below)
800051d8  addiu $v0, $v0, -0x1000
800051dc  subu  $t1, $v0, $s2       # stack = size - 0x1000 - stack_size
800051e0  addu  $s0, $t1, $s2       # top = stack + stack_size
800051e8  addiu $v0, $s0, -0x20
800051f4  addiu $s0, $s0, -0x2a0    # the answer: top - 0x2a0
800051fc  sw    $s3, 0x1c0($s0)     # [sp + 0x1c0] = gp
80005200  sw    $s4, 0x1f0($s0)     # [sp + 0x1f0] = root
80005204  sw    $v0, 0x1e0($s0)     # [sp + 0x1e0] = top - 0x20
80005208  sw    $v0, 0x1d0($s0)     # [sp + 0x1d0] = top - 0x20
```

The `0x2a0` bytes above the returned pointer are a **saved thread context**,
primed: at 16 bytes a register, `0x1c0` is `$gp` (28), `0x1d0` `$sp` (29),
`0x1e0` `$fp` (30) and `0x1f0` `$ra` (31). A resume through this frame lands in
the thread with `$gp`, `$sp` and `$fp` set and `$ra` pointing at `root`, so a
`main` that returns goes to `root`. This is EE-7e's frame — `0x200` bytes of
registers and `0xa0` more — being used for what it is for.

Executed on the reference under `eesim`, which does not run `RDRAM` and so
leaves the memory size at zero:

```sh
python3 tools/eesim.py assets/SCPH-50000.bin --syscall 0x3c 0x1000 -1 0x20000 0x1218a8
# -> 0xffffed60           = 0 - 0x1000 - 0x2a0
python3 tools/eesim.py assets/SCPH-50000.bin --syscall 0x3c 0x1000 0x1000000 0x20000 0x1218a8
# -> 0x0101fd60           = 0x1000000 + 0x20000 - 0x2a0
python3 tools/eesim.py assets/SCPH-50000.bin --syscall 0x3c 0x1000 0xffffffff 0x20000 0x1218a8
# -> 0x0001fd5f           a zero-extended -1 is a stack at 0xffffffff, and wraps
```

The three answers are the arithmetic above; the third is the 64-bit
comparison seen from outside.

### The thread table

What the handler records goes into a per-thread record. Slot `0x3C` finds the
current one by index:

```
8000520c  lui   $v0, 0x8001
80005210  lw    $v0, 0x55ac($v0)    # current thread index, at 0x800155AC
80005214  mult  $v0, $v0, $a0       # x 0x4c -- three-operand, R5900 only
```

The word at `KERNEL+0x5214` is `0x00441018`: `SPECIAL`, `rs=$v0`, `rt=$a0`,
`rd=$v0`, function `0x18` — a `mult` with a destination, which is why
`tools/romdis.py` shows it as unknown. Records are `0x4C` bytes apart from
`0x8001A648` (built as `0x80020000 - 0x59B8`; slot `0x07` walks the table from
index 1 to `0x100`, so there are 256). Taking that base as `T + index * 0x4C`,
the fields the three slots and `0x07` touch:

| Offset | Written by | Meaning |
| --- | --- | --- |
| `+0x00` | — | state (`0x07` treats `0x10` as one it can delete outright) |
| `+0x04` | `0x07` | entry point |
| `+0x08` | `0x3C` | context: the stack pointer it answered with |
| `+0x0C` | `0x3C`, `0x07` | `$gp` |
| `+0x2C` | `0x07` | `argc` |
| `+0x30` | `0x3C`, `0x07` | argument list: `0x07` stores the packed strings, `0x3C` the caller's block |
| `+0x34` | `0x3C` | stack base |
| `+0x38` | `0x3C` | stack size |
| `+0x3C` | `0x3C` | root |
| `+0x40` | `0x3D` | end of heap |

### The argument block

Between recording the stack and returning, `0x3C` calls `0x80004FA8` with
`(strings = T[+0x30], block = args, count = T[+0x2C])` and then stores `args`
into `T[+0x30]`:

```
80004fa8  move  $t0, $a1
80004fac  move  $a3, $zero
80004fb0  sw    $a2, 0x0($t0)       # block[0] = argc
80004fb4  addiu $a1, $a1, 0x44      # strings start at block + 0x44
80004fb8  blez  $a2, 0x8000500c
80004fbc  addiu $t0, $t0, 0x4
80004fc0  sw    $a1, 0x0($t0)       # block[1 + i] = where string i lands
    ...   copy string i, NUL included, from `strings` to there
80004ffc  addiu $a0, $a0, 0x1
80005004  bnez  $v0, 0x80004fc0     # while i < argc
```

So the block a program hands to `0x3C` comes back filled as **`argc`, then 16
words of `argv` pointers (`0x44 = 4 + 16 * 4`), then the strings**, copied out
of a packed list the kernel kept — the SDK's runtime reads `argc` and `argv`
from exactly that layout after the call. Where the packed list comes from is
slot `0x07`.

### Where the memory size comes from

`0x80000C40` returns the word at `0x80016E50`, and the kernel entry stores it
there first thing:

```
80001000  lui   $v0, 0x7000
80001004  lw    $v0, 0x3ff0($v0)    # EE-4a: the boot's result
8000100c  sw    $v0, 0x6e50($v1)    # -> 0x80016E50
```

And what the boot puts at `0x70003FF0` is **`RDRAM`'s return value**:

```sh
python3 tools/romdis.py assets/SCPH-50000.bin --cpu ee --vma 0xBFC00000 --range 0xBFC00874 0xBFC008A0
```

```
bfc00874  lui   $k0, 0x9fc4
bfc00878  ori   $k0, $k0, 0x1000    # RDRAM, EE-2
bfc00884  jalr  $k0
bfc0088c  bltz  $v0, 0xbfc0093c     # negative: failure
bfc00894  lui   $k0, 0x7000
bfc00898  ori   $k0, $k0, 0x3ff0
bfc0089c  sw    $v0, 0x0($k0)       # the result is the size RDRAM found
```

So `RDRAM` reports the size of main memory, the reset path passes it on, and
`0x3C` treats it as the end of memory when asked for a stack at the top.
`tools/eesim.py` skips `RDRAM` and returns 0 for it, which is why the first
`--syscall` above answers from zero; the emulators run the real `RDRAM`, which
finds 32 MiB.

## Slot 0x3D: the end of the heap

```
80005298  lui   $v1, 0x8001
8000529c  lw    $v1, 0x55ac($v1)    # current thread
800052a0  addiu $v0, $zero, 0x4c
800052a4  lui   $a2, 0x8002
800052a8  addiu $a2, $a2, -0x5978   # T + 0x40, the heap-end field
800052ac  mult  $v1, $v1, $v0       # 0x00621018
800052b0  bgez  $a1, 0x800052c4
800052b4  addu  $v1, $v0, $a2
800052b8  addu  $v0, $a2, $v0
800052bc  b     0x800052c8
800052c0  lw    $v0, -0xc($v0)      # size < 0: the end is the stack base (+0x34)
800052c4  addu  $v0, $a0, $a1       # size >= 0: start + size
800052c8  jr    $ra
800052cc  sw    $v0, 0x0($v1)       # store it, and answer with it
```

```sh
python3 tools/eesim.py assets/SCPH-50000.bin --syscall 0x3d 0x130000 0x10000
# -> 0x00140000
```

`(start, size)`, with a negative size meaning "up to the stack" — the SDK's
runtime passes `-1`, so its heap runs from the end of its image to the base of
the stack `0x3C` recorded. The value is stored and returned.

## Slot 0x3E: read it back

`0x800052D0` is the load of `T[+0x40]` for the current thread — the value the
last `0x3D` stored, and 0 before any.

## Slot 0x07: how a program's arguments reach it

The runtime's `argc`/`argv` come out of the block `0x3C` filled, and `0x3C`
copies them from the packed list in `T[+0x30]`. That list is written by
**slot `0x07`** — the scheduler-group handler of `docs/analysis/17` whose
operation is `0x800057E0`. After tearing down every other thread it packs the
caller's `argv` and records the program in the current thread's record:

```
800058bc  lui   $v0, 0x8001
800058c0  addiu $v0, $v0, 0x55c8    # the packed list lives at 0x800155C8
800058d0  lw    $a1, 0x0($s0)       # argv[i]
800058d8  move  $a0, $v0
800058e0  jal   0x80005558          # append it, NUL included; returns the new end
800058e8  bnezl $s1, 0x800058d8
    ...
8000590c  sw    $s5, 0x0($v1)       # T + 0x04 = entry
80005924  sw    $fp, 0x8($a1)       # T + 0x0C = gp
8000592c  sw    $s4, 0x28($v0)      # T + 0x2C = argc
80005934  sw    $a3, 0x2c($a0)      # T + 0x30 = the packed list
80005950  move  $v0, $s5            # answer: the entry point
```

The skeleton around it (`0x80002F80`) then does what `17` describes for a
context switch, with one detail that is the entry contract: it writes the
answer into `EPC` and into the saved `$v0`, restores the rest of the frame and
`eret`s:

```
80002f88  jal   0x800057e0
80002f90  mtc0  $v0, $14            # EPC = entry
80002f98  lui   $sp, 0x8000
80002f9c  lw    $sp, 0x10c0($sp)    # the saved frame
80002fa0  jal   0x80003800          # restore it ...
80002fa4  sd    $v0, 0x20($sp)      # ... with $v0 = entry
80002fbc  eret
```

So a program is entered with **the registers its launcher had at the
`syscall`** — `$a0` = entry, `$a1` = gp, `$a2` = argc, `$a3` = argv, since
those were `0x07`'s own arguments — and `$v0` = its entry point. Its
arguments proper are not in registers at all: they are the packed list in the
thread record, which the program collects by calling `0x3C` with a block to
fill. The SDK's runtime does exactly that, and consults `$a0` only if the
block comes back with `argc = 0`.

## What this pins for the rebuild

- `0x3C` takes five arguments, the fifth in `$t0`; answers `top - 0x2a0` where
  `top = stack + stack_size`, or `memory_size - 0x1000` when `stack` is a
  sign-extended `-1`; primes the frame above the answer as a saved context with
  `$gp`, `$sp`/`$fp` = `top - 0x20`, `$ra` = root; and fills the caller's block
  as `argc`, sixteen `argv` words, strings.
- The memory size is `RDRAM`'s return, passed through `0x70003FF0` and kept.
- `0x3D` stores and answers `start + size`, or the stack base when `size < 0`;
  `0x3E` answers what was stored.
- A launcher (`0x07`, and the ROM's own boot through it) packs the argument
  strings and their count into the thread record and enters the program with
  the launcher's registers and `$v0` = entry, not with `argc`/`argv` in
  `$a0`/`$a1`.
