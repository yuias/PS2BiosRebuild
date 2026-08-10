# Exceptions and Interrupts

`EXCEPMAN`, `INTRMANP` and `INTRMANI` are boot-list entries three to five
(`docs/analysis/03-iopboot-and-boot-list.md`), loaded straight after `SYSMEM`
and `LOADCORE`. Together they own the IOP's exception entry and its interrupt
dispatch. This document covers what each does at init, and — the structurally
interesting part — why two modules exporting the *same* library can both sit in
the boot list.

Module offsets and disassembly follow the conventions of
`docs/analysis/05-sysmem-and-loadcore.md` §"Disassembling a module": carve the
`PT_LOAD` segment, disassemble at VMA 0.

## The P/I variant pair

`INTRMANP` and `INTRMANI` are distinct files with distinct contents, but both
declare the same module name (`Interrupt_Manager`) and, more importantly, export
the **same library tag at the same version** — `intrman` 1.02:

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
python3 - <<'PY'
import struct
o = '<outdir>'
for m in ('INTRMANP', 'INTRMANI', 'TIMEMANP', 'TIMEMANI'):
    d = open(f'{o}/{m}', 'rb').read()
    i = d.find(struct.pack('<I', 0x41C00000))
    ver, fl = struct.unpack_from('<HH', d, i + 8)
    print(m, d[i + 12:i + 20].split(b'\0')[0].decode(), hex(ver))
PY
# INTRMANP intrman 0x102 / INTRMANI intrman 0x102
# TIMEMANP timrman 0x101 / TIMEMANI timrman 0x101
```

That should be impossible to register twice: `LOADCORE`'s registration walks the
registry and rejects a library already present (`05` §"Registering an export
library"). The resolution is that **only one of the pair ever registers**. Each
module's entry begins with the same test and takes the opposite branch:

```sh
python3 tools/romdis.py <outdir>/INTRMANP.text --cpu iop --vma 0 --range 0x0 0x40
python3 tools/romdis.py <outdir>/INTRMANI.text --cpu iop --vma 0 --range 0x0 0x40
```

Both read `PRId` and the word at `0xBF801450`, forming the predicate

```
PRId < 0x10  ||  (*(u32*)0xBF801450 & 8)
```

`INTRMANP` proceeds with initialisation when that predicate holds and otherwise
returns 1 immediately; `INTRMANI` does exactly the reverse. So on any given
machine one initialises and registers `intrman`, and the other returns without
touching anything.

This is the *same* predicate the boot block uses to choose between its two
bus-configuration tables (`docs/analysis/02-boot-block.md` §"IOP reset path",
step 2) — one discriminator, applied consistently from the reset vector up
through the kernel modules.

`TIMEMANP`/`TIMEMANI` are built the same way (identical opening sequence, same
predicate, same `timrman` 1.01 on both), so the `P`/`I` suffix is a general
convention in this ROM: **a variant pair, selected at run time, of which exactly
one becomes resident.**

What the loader does with the `1` return — the convention that keeps the
non-selected module from staying resident — is not traced here; it belongs with
`IOPBOOT`'s per-module load step and is left open.

### Ordinal parity between variants

Because importers bind by ordinal into whichever variant registered (`05`
§"Binding a module's imports"), the two variants must expose identical export
counts and identical slot meanings. They do: both export exactly 32 entries.

```sh
# counts derived from the contiguous R_MIPS_32 run at table+0x14, per 05
python3 - <<'PY'
import struct
o = '<outdir>'
for mod, tbl in (('EXCEPMAN', 0x640), ('INTRMANP', 0x1110), ('INTRMANI', 0x1480)):
    d = open(f'{o}/{mod}', 'rb').read()
    shoff, = struct.unpack_from('<I', d, 32)
    es, n, _ = struct.unpack_from('<HHH', d, 46)
    r32 = set()
    for i in range(n):
        nm, typ, fl, addr, off, size, *_ = struct.unpack_from('<10I', d, shoff + i * es)
        if typ == 9:
            r32 |= {a for a, inf in (struct.unpack_from('<II', d, off + k * 8)
                                     for k in range(size // 8)) if inf & 0xFF == 2}
    c = 0
    while tbl + 0x14 + 4 * c in r32:
        c += 1
    print(mod, c)
PY
# EXCEPMAN 9 / INTRMANP 32 / INTRMANI 32
```

Note the contiguous-run rule from `05` matters here: taking every `R_MIPS_32` in
the module instead would run past the table's terminator into the import tables
that follow it and report 51.

Within each `intrman` table, slots 19 and 20 repeat the addresses of slots 17
and 18 — deliberate aliases, distinct from the shared do-nothing stub pattern
(`05` §"Reserved slots share a return stub"), since the targets here are real
code.

### What actually differs

The variants are not cosmetic. `INTRMANI` carries code for a second DMA
controller register bank at `0xBF8015xx` that `INTRMANP` never touches, while
both handle the primary interrupt mask and DMA interrupt registers identically:

```sh
for m in INTRMANP INTRMANI; do
  python3 tools/romdis.py <outdir>/$m.text --cpu iop --vma 0 > /tmp/$m.dis
  echo "$m 0x157x:$(grep -c 0x157 /tmp/$m.dis) 0x1074:$(grep -c 0x1074 /tmp/$m.dis)"
done
# INTRMANP 0x157x:0 0x1074:7   INTRMANI 0x157x:11 0x1074:7
```

`INTRMANI` is correspondingly larger (text `0x1570` against `0x1200`). Both clear
a 128-entry handler table at init and zero `0xBF801074` and `0xBF8010F4` before
enabling anything.

## EXCEPMAN

`EXCEPMAN` (`Exception_Manager` 1.01, 9 exports, imports `sysmem` and
`loadcore`) installs the first-level exception entry. Its init runs in four
steps:

```sh
python3 tools/romdis.py <outdir>/EXCEPMAN.text --cpu iop --vma 0 --range 0x0 0xe0
```

1. **Clear the dispatch table.** A 16-word table is zeroed — one slot per MIPS
   exception cause, each the head of a handler chain.
2. **Allocate working memory.** The helper at `0x4F0` requests `0x100` bytes
   through the `sysmem` import and stores the result in a global.
3. **Install the vector stub.** The words from module offset `0x560` to `0x630`
   are copied to **address 0** — the IOP's exception entry — one word at a time.
   The copy is not verbatim: any source word equal to `0x8F5A0000`
   (`lw $26, 0($26)`) or `0x8F7B0000` (`lw $27, 0($27)`) has a 16-bit immediate
   OR-ed in as it is written, filling in the run-time address of the dispatch
   table. The stub therefore ships with placeholder immediates that only become
   correct once installed — a rebuild must reproduce the placeholder encodings
   and the patch, not just the stub's behaviour.
4. **Register.** A default handler descriptor at `0x630` is registered through
   the module's own registration path, then the export table at `0x640` is
   handed to `loadcore` so `intrman` and everything later can bind to it.

The registration entry points themselves (slots 4 and 5, offsets `0x110` and
`0x134`) take a cause code and a descriptor and link it onto the corresponding
chain; slot 2 (`0xE0`) is a bare `jr $ra; move $v0, $zero` — a stub in the sense
of `05`, not a real function.

## What this pins for the rebuild

- `P`/`I` module pairs export one library tag between them; both must be
  present in the archive and the boot list, both must expose identical export
  ordinals, and each must self-select on
  `PRId < 0x10 || (*(u32*)0xBF801450 & 8)`, the same discriminator the boot
  block uses.
- `EXCEPMAN` must install its exception stub at address 0 by copying with
  immediate patching, and must own a 16-entry cause-indexed chain table.
- Export-table extents in these modules are only correct when read as the
  contiguous relocation run — the naive read gives 51 instead of 32.

Open: the loader-side convention for a module entry returning 1 (non-residency),
which belongs with `IOPBOOT`'s per-module load step.

Next in boot order: `SSBUSC` and `DMACMAN` (bus and DMA controller managers),
then `SYSCLIB`/`HEAPLIB` (`docs/project-state.md` §5).
