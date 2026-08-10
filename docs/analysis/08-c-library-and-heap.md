# The C Library and Heap: SYSCLIB, STDIO and HEAPLIB

`SYSCLIB` and `HEAPLIB` are boot-list entries ten and eleven, and `STDIO` comes
much later at eighteen (`docs/analysis/03-iopboot-and-boot-list.md`). They are
grouped here because between them they expose a mechanism the earlier modules
only hinted at: **how one library replaces another at run time**, and why
`LOADCORE`'s registration rule is shaped the way it is.

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
for m in SYSCLIB STDIO HEAPLIB; do python3 tools/irxinfo.py <outdir>/$m --imports; done
```

| Module | Exports | Imports |
| --- | --- | --- |
| `SYSCLIB` | `sysclib` 1.01 (42), **`stdio` 1.02 (14)** | `loadcore` 6 |
| `STDIO` | **`stdio` 1.02 (14)** | `loadcore` 6; `sysclib` 8, 18; `ioman` 6, 7 |
| `HEAPLIB` | `heaplib` 1.01 (18) | `sysmem` 4, 5, 10; `loadcore` 6 |

`SYSCLIB` is the first module seen to export **two** libraries from one file.
And the second of them collides head-on with `STDIO`: the same tag, the same
version, the same entry count. Both modules are in the boot list, and both
register unconditionally. Under a naive "reject duplicates" rule one of them
must fail — so the rule is not that.

## What registration actually compares

`LOADCORE`'s registration path (`docs/analysis/05-sysmem-and-loadcore.md`
§"Registering an export library") calls three tiny helpers per registry entry.
Disassembled, they are unambiguous:

```sh
python3 tools/irxinfo.py <outdir>/LOADCORE --dump-load <outdir>/LOADCORE.text
python3 tools/romdis.py <outdir>/LOADCORE.text --cpu iop --vma 0 --range 0xe3c 0xe94
```

| Offset | Computes |
| --- | --- |
| `0xE3C` | tag equality — the 8-byte tag compared as the two words at +`0xC` and +`0x10`, returning 1 or 0 |
| `0xE6C` | `(new.version >> 8) - (old.version >> 8)` — the **major** difference |
| `0xE84` | `(new.version & 0xFF) - (old.version & 0xFF)` — the **minor** difference |

and the caller combines them as:

- tag differs → not this library, move to the next registry entry;
- **major** differs → treated as a *different* library, move on;
- otherwise same library, and registration is accepted **only if the new minor
  version is strictly greater**; equal or lower returns −1.

So a library is identified by *tag + major version*, and the minor version is a
generation counter deciding who wins.

## Superseding: SYSCLIB lowers its own version

That rule is exactly what `SYSCLIB` exploits. Its init does not register the
`stdio` table as stored — it edits the version first:

```sh
python3 tools/irxinfo.py <outdir>/SYSCLIB --dump-load <outdir>/SYSCLIB.text
python3 tools/romdis.py <outdir>/SYSCLIB.text --cpu iop --vma 0 --range 0x20 0x60
```

```
  28:  lui   $s0, 0
  2c:  addiu $s0, $s0, 0x148    # 0x140 (stdio table) + 8 -> its version halfword
  34:  lhu   $v0, 0($s0)        # 0x0102 as stored
  38:  lui   $a0, 0
  3c:  addiu $a0, $a0, 0x70     # the sysclib table
  40:  addiu $v0, $v0, -1       # 0x0101
  44:  jal   0x1894             # loadcore 6: register sysclib
  48:  sh    $v0, 0($s0)        # write 1.01 into the stdio table
  4c:  addiu $a0, $s0, -8       # the stdio table, now at 1.01
  50:  jal   0x1894             # loadcore 6: register stdio
```

The stored halfword really is `0x0102`, so the value registered is `0x0101`:

```sh
python3 - <<'PY'
import struct
d = open('<outdir>/SYSCLIB', 'rb').read()
phoff, = struct.unpack_from('<I', d, 28)
es, n = struct.unpack_from('<HH', d, 42)
for i in range(n):
    t, off, va, pa, fsz, msz = struct.unpack_from('<6I', d, phoff + i * es)
    if t == 1:
        print(hex(struct.unpack_from('<H', d, off + 0x148)[0]))
PY
# 0x102
```

`SYSCLIB` therefore publishes a **provisional `stdio` at 1.01**, deliberately one
generation below the `stdio` 1.02 that `STDIO` will register eight modules
later. Nothing collides: the early one exists so that modules loading in between
have a `stdio` to bind to at all, and the real one supersedes it when it
arrives.

This also explains why `irxinfo` reports both tables as 1.02 — the lowering
happens at run time, in RAM. The stored file cannot be read as the registered
truth.

## Superseding re-binds existing clients

Accepting a higher minor version is not a bare list insertion. The accept path
walks the *old* library's chain of bound clients and moves them across:

```sh
python3 tools/romdis.py <outdir>/LOADCORE.text --cpu iop --vma 0 --range 0x918 0x970
```

It detaches the old library's client list (`sw $zero, 4($18)`), then for each
client reads the flags halfword at +`0xA` and tests bit 0: clients with the bit
clear are collected for re-binding against the new library, while clients with
it set stay attached to the old one. That is what makes supersession meaningful
rather than cosmetic — modules that already bound to `SYSCLIB`'s provisional
`stdio` are re-pointed at `STDIO`'s implementation when it registers.

Flags bit 0 is thus a "pin me" marker. It sits alongside the low-three-bits test
`06` found on the import side, which the same flags field feeds.

## HEAPLIB builds on SYSMEM

`HEAPLIB` (18 exports, no `.bss`) imports `sysmem` ordinals 4, 5 and 10 — and
ordinal 4 is the allocator `05` identified by disassembling `SYSMEM` offset
`0x2EC` (rejects an uninitialised heap, validates an allocation-mode argument
below 3, then delegates). A second independent line of evidence for that slot,
of the same kind `07` used to name `loadcore` 6.

So the layering is explicit: `sysmem` owns the one region carved out at boot,
and `heaplib` provides finer-grained heaps on top of it rather than managing
memory itself.

## What this pins for the rebuild

- A library's identity is **tag + major version**; the minor version is a
  generation counter, and registration requires it to be strictly greater.
- A module may export several libraries, and may rewrite a table's version
  before registering it. `SYSCLIB` must publish `stdio` at 1.01 for `STDIO`
  1.02 to supersede it — a rebuild that registers the stored 1.02 would make
  `STDIO`'s registration fail silently.
- Supersession re-binds unpinned clients (flags bit 0) from the old library to
  the new one.

Next in boot order: `EECONF` and `THREADMAN` — the thread manager being the
largest module in the IOP kernel (`docs/project-state.md` §5).
