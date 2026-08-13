# SYSMEM and LOADCORE

`SYSMEM` and `LOADCORE` are the first two modules in the boot list
(`docs/analysis/03-iopboot-and-boot-list.md`) and the base every later module
depends on: `SYSMEM` owns the memory the kernel allocates from, and `LOADCORE`
owns the registry that binds modules to one another. This document covers the
library-table layout that `04-iop-module-format.md` deferred, the binding
algorithm `LOADCORE` implements, and the surface each module exports.

## Disassembling a module

A module's load segment is linked at vaddr 0 (`04`), so disassembling it at VMA
0 makes the listing's addresses read directly as module offsets — the form used
throughout this document. Carve the segment out first:

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
python3 - <<'PY'
import struct
d = open('<outdir>/SYSMEM', 'rb').read()
phoff, = struct.unpack_from('<I', d, 28)
es, n = struct.unpack_from('<HH', d, 42)
for i in range(n):
    t, off, va, pa, fsz, msz = struct.unpack_from('<6I', d, phoff + i * es)
    if t == 1:  # PT_LOAD
        open('<outdir>/SYSMEM.text', 'wb').write(d[off:off + fsz])
PY
python3 tools/romdis.py <outdir>/SYSMEM.text --cpu iop --vma 0 --range 0x2ec 0x33c
```

Two caveats apply to any such listing, both consequences of reading the file
*before* the loader relocates it: `lui`/`addiu` address literals show their
unrelocated halves, and `R_MIPS_32` pointer words show their vaddr-0-relative
values. Branches, and `jal`/`j` targets, read correctly as module offsets.

## Library tables

Both table kinds share a 20-byte header, distinguished by a magic word:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | magic — `0x41C00000` export, `0x41E00000` import |
| 4 | 4 | link pointer (0 in the stored file; used at run time) |
| 8 | 2 | version, BCD |
| 10 | 2 | flags |
| 12 | 8 | library tag, NUL-padded (`sysmem`, `loadcore`, …) |
| 20 | … | entries |

The tag is the lowercase library name from `04`, and it is a fixed 8-byte
field: `loadcore` fills it exactly, with no NUL terminator.

### Export tables: relocated pointer arrays

Export entries are absolute function pointers, terminated by a zero word.
Their extent is confirmed independently of the terminator by the relocations —
every entry is an `R_MIPS_32`, and the relocation offsets are contiguous:

```sh
python3 - <<'PY'
import struct
d = open('<outdir>/SYSMEM', 'rb').read()
shoff, = struct.unpack_from('<I', d, 32)
es, n, strndx = struct.unpack_from('<HHH', d, 46)
for i in range(n):
    nm, typ, fl, addr, off, size, link, info, al, entsz = struct.unpack_from('<10I', d, shoff + i * es)
    if typ == 9:  # SHT_REL
        r32 = sorted(o for o, inf in
                     (struct.unpack_from('<II', d, off + k * 8) for k in range(size // 8))
                     if inf & 0xFF == 2)
        print(len(r32), hex(r32[0]), hex(r32[-1]))
PY
# SYSMEM .rel.text: 16 R_MIPS_32, 0x14 .. 0x50
```

`SYSMEM` exports 16 functions, `LOADCORE` 25, and in both the terminating word
that follows is zero.

In both modules here, slot 0 holds the `.iopmod` entry address — `0x60` for
`SYSMEM`, `0x0` for `LOADCORE`. `LOADCORE`'s stored slot 0 therefore reads as
the word `0`, which is *not* a terminator: it is a relocated pointer to vaddr 0
that becomes the load base at run time. A parser that scans the stored file for
the first zero word will get this wrong; a scan is only valid after relocation.
The relocation table is the reliable source.

> **Corrected by a later sweep.** "Slot 0 is the module entry" holds for these
> two but is *not* a general rule — it fails for 2 of the 41 single-library
> modules, and for every secondary library of a multi-library module, where
> slot 0 is a `jr $ra` stub instead. Slots 0 and 1 are reserved hooks that
> nothing ever imports; the lowest ordinal any module binds is 2. See
> `docs/spec/02-module-abi.md` §IRX-7 for the rule that actually holds.

### Reserved slots share a return stub

Several export slots point at one tiny function rather than distinct code:
`SYSMEM` slots 2, 11, 12 and 13 all hold `0x58`, and `LOADCORE` slots 2, 18 and
19 all hold `0x1BDC`. At `SYSMEM` offset `0x58` is simply:

```
  58:  jr   $ra
  5c:  nop
```

So these are reserved slots kept present so that ordinals stay stable, filled
with a shared do-nothing return. A rebuild must keep them occupied: the ordinal
of every later slot depends on it.

### Import tables: patchable call stubs

Import entries are 8-byte stubs, two instructions each, terminated by a zero
word. `LOADCORE`'s own import of `sysmem` (at module offset `0x1BF0`) holds
three:

```
  jr    $ra
  addiu $v0, $zero, 4      # ordinal 4
  jr    $ra
  addiu $v0, $zero, 5
  jr    $ra
  addiu $v0, $zero, 6
  00000000                 # terminator
```

The second instruction carries the ordinal into the export table of the named
library. Unbound, the stub simply returns — which is what makes an unresolved
import survivable rather than a crash.

## LOADCORE: registration and binding

### Registering an export library

`LOADCORE` export slot 6 (offset `0x898`) registers a library. It rejects
anything whose first word is not `0x41C00000` (returning −1), then walks the
global registry list (head at module offset `0x1C70`), comparing the candidate's
tag, version and flags against each already-registered library before linking it
in.

```sh
python3 tools/romdis.py <outdir>/LOADCORE.text --cpu iop --vma 0 --range 0x898 0x930
```

### Binding a module's imports

Export slot 8 (offset `0xC00`) walks a module's words looking for the
`0x41E00000` magic, validates each import table it finds, skips tables whose
flags (halfword at +10) have any of the low three bits set, and calls the
resolver at `0xFC4` for the rest; a failure unwinds through `0xCBC`.

The resolver searches the registry for a library whose tag and version match the
import table's, then patches the stubs (`0x1064`):

```sh
python3 tools/romdis.py <outdir>/LOADCORE.text --cpu iop --vma 0 --range 0x1064 0x1110
```

The loop is worth stating precisely, because a rebuild must reproduce its
encodings exactly:

1. Count the exporter's entries by scanning its pointer array to the zero
   terminator.
2. For each 8-byte stub in the importer's table, check that its **second** word
   decodes as `addiu` (top six bits equal `9`), and take the ordinal from that
   word's low 16 bits.
3. If the ordinal is within the exporter's entry count, read `export[ordinal]`
   and overwrite the stub's **first** word with a `j` to it —
   `0x08000000 | ((target >> 2) & 0x03FFFFFF)`.
4. If the ordinal is out of range, write `0x03E00008` (`jr $ra`) instead, so an
   over-range import degrades to a return rather than a wild jump.
5. Advance 8 bytes; stop at the zero terminator.

Note what is *not* rewritten: the `addiu $v0, $zero, N` stays in place and
becomes the delay slot of the new `j`. It is harmless — `$v0` is the return-value
register, which the callee overwrites — and it means a bound stub still records
the ordinal it was bound to.

## SYSMEM: the kernel's memory

`SYSMEM`'s entry (offset `0x60`) takes the RAM size the boot chain passes down
(`02` §"IOP reset path" step 6, threaded through `IOPBOOT`), clamps it to a
ceiling of `0x007FFF00`, rounds the region bounds down to `0x100` and records
them in a pair of globals that everything else tests for "initialised".

```sh
python3 tools/romdis.py <outdir>/SYSMEM.text --cpu iop --vma 0 --range 0x60 0xd8
```

The allocator entry (slot 4, offset `0x2EC`) shows the shape of the public API:
it returns 0 immediately if the globals are unset, validates that its first
argument — an allocation mode — is less than 3, and otherwise takes a lock
before delegating to the worker at `0x9E8`. The query entry (slot 6, offset
`0x204`) likewise returns 0 when uninitialised.

The allocation-mode argument having exactly three legal values, and the
behaviour of each, is the level of detail the specification will need; it is
left to the spec pass rather than settled here.

## What this pins for the rebuild

- Library tables: 20-byte header, magic `0x41C00000` / `0x41E00000`, 8-byte
  fixed tag field, entries at +20, zero-terminated.
- Export slot 0 must equal the module entry; reserved slots must stay occupied
  by a shared return stub so ordinals do not shift.
- Import stubs are `jr $ra` + `addiu $v0, $zero, ordinal`, and the binder
  rewrites only the first word, to `j target` or `jr $ra` when out of range.
- Export-table extent is derivable from relocations, not from scanning the
  stored file for a zero word.

Next: the modules the rest of the boot list depends on in turn — the exception
and interrupt managers (`EXCEPMAN`, `INTRMANP`/`INTRMANI`) — following the boot
order (`docs/project-state.md` §5).
