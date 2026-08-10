# The IOP Module Format

`IOPBOOT` loads each module named in `IOPBTCONF`
(`docs/analysis/03-iopboot-and-boot-list.md`). This document surveys what those
modules are as files — the container the loader parses, the metadata it reads,
the relocations it applies, and how modules name the libraries they import from
one another. The loader *algorithm* (what `IOPBOOT` and `LOADCORE` do with this
structure) is the next document; here the concern is the on-disc format the
rebuild must be able to produce.

All observations use `readelf`, `xxd` and small `struct` decoders against
modules extracted with `tools/romdir.py --extract` (extract outside the repo).

## Container: a specialised ELF

Every stored IOP module is a little-endian ELF32 for MIPS-I:

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
readelf -h <outdir>/SYSMEM
```

The header is ordinary except for `e_type = 0xFF80`, a processor-specific type
(in the `ET_LOPROC` range) that marks an **IOP relocatable module** — an "IRX".
`e_machine` is MIPS R3000 and `e_flags` carries `mips1`. `e_entry` is the module
entry point (`0x60` for `SYSMEM`).

The program headers are two:

```sh
readelf -l <outdir>/SYSMEM
```

| p_type | Contents | Notes |
| --- | --- | --- |
| `0x70000080` (`PT_LOPROC + 0x80`) | the `.iopmod` section | module metadata, below |
| `PT_LOAD` | `.text` `.rodata` `.data` `.bss` | one RWE segment, `p_vaddr = 0` |

Because the single load segment is linked at virtual address 0, a module is not
tied to any address on disc: the loader chooses a base, copies the segment
there, extends it by `.bss`, and fixes up every address with the relocations
below. That is what makes the modules relocatable and why the base can come from
`IOPBTCONF`'s `@` directive.

Sections seen (`readelf -S`): `.iopmod`, `.text`, `.rodata`, `.data`, `.bss`,
the relocation tables `.rel.text` / `.rel.data`, and a minimal
`.symtab`/`.strtab`/`.shstrtab`.

## The `.iopmod` metadata

The `.iopmod` section (file offset `0x74` in these modules, right after the ELF
and program headers) is a fixed header followed by the module name:

| Offset | Type | Field | `SYSMEM` value |
| --- | --- | --- | --- |
| 0 | u32 | moduleinfo address (module's export/info struct, or `0xFFFFFFFF`) | `0xC60` |
| 4 | u32 | entry | `0x60` |
| 8 | u32 | `gp` value | `0x8C70` |
| 12 | u32 | text size | `0xC40` |
| 16 | u32 | data size | `0x40` |
| 20 | u32 | bss size | `0x10` |
| 24 | u16 | version (BCD) | `0x0101` |
| 26 | char[] | name, NUL-terminated | `System_Memory_Manager` |

```sh
python3 - <<'PY'
import struct
d = open('<outdir>/SYSMEM', 'rb').read()
mi, entry, gp, t, da, b = struct.unpack_from('<6I', d, 0x74)
ver, = struct.unpack_from('<H', d, 0x74 + 24)
name = d[0x74 + 26:].split(b'\0', 1)[0].decode()
print(entry, hex(gp), (t, da, b), hex(ver), name)
PY
```

Two of these fields cross-check against `01-rom-layout.md`'s EXTINFO decode:
the `.iopmod` **name** equals the EXTINFO comment (`System_Memory_Manager`) and
the **version** equals the EXTINFO version (1.01). The archive metadata and the
module's own header agree — a useful invariant when rebuilding, since both must
be produced and kept consistent.

`text/data/bss` sizes tell the loader how much to copy and how much zeroed
`.bss` to append; `gp` is the small-data pointer to install before entry.

`LOADCORE`'s header decodes the same way: name `Module_Manager`, version 1.01,
`gp` `0x9C60`, entry `0x0`.

## Relocations

The load segment is relocated with standard MIPS REL entries (8 bytes:
`r_offset`, `r_info`), split into `.rel.text` and `.rel.data`:

```sh
readelf -r <outdir>/SYSMEM
for m in SYSMEM LOADCORE THREADMAN; do
  echo -n "$m: "; readelf -r <outdir>/$m | awk '/R_MIPS/{print $3}' | sort | uniq -c | tr '\n' ' '; echo
done
```

Four relocation types appear, all resolved against the chosen load base `B`:

| Type | Applied as |
| --- | --- |
| `R_MIPS_32` | word `+= B` (absolute pointers in `.data`, jump tables) |
| `R_MIPS_26` | `jal`/`j` target field rebased into the loaded segment |
| `R_MIPS_HI16` | high half of an address literal; **always paired** with the next |
| `R_MIPS_LO16` | low half; combined with the pending `HI16` to carry the `+B` |

`HI16` and `LO16` occur in equal counts (e.g. `SYSMEM` 22 and 22), reflecting
their pairing — the loader must hold each `HI16` until its matching `LO16` to
apply the carry correctly, the one subtlety in an otherwise mechanical fixup.
The counts scale with module size (`THREADMAN`: 697 `R_MIPS_26`), confirming the
scheme is uniform across the archive, not special-cased per module.

## Inter-module linking: import and export libraries

`SYSMEM` is the base and imports nothing, but most modules call functions in
others. They do so through **library stub tables**, marked by fixed words the
loader scans for:

```sh
python3 - <<'PY'
import struct
for m in ('SYSMEM','LOADCORE','THREADMAN','IOMAN','SIFMAN'):
    d = open(f'<outdir>/{m}', 'rb').read()
    imp = d.count(struct.pack('<I', 0x41E00000))   # import table marker
    exp = d.count(struct.pack('<I', 0x41C00000))   # export/table marker
    print(f'{m:10} imports~{imp} exports~{exp}')
PY
```

A module names each library it imports by a short lowercase tag, distinct from
the archive file name. `THREADMAN` imports seven — visible as plain strings:

```sh
strings <outdir>/THREADMAN | grep -iE 'sysmem|loadcore|intrman|stdio|sysclib|timrman|heaplib'
```

Note the tag/file-name split: the timer library is `timrman` though the files
are `TIMEMANP`/`TIMEMANI`, and the interrupt library is `intrman` from
`INTRMANP`/`INTRMANI`. So a module resolves imports by **library tag**, not by
the ROMDIR name the boot list uses — two independent namespaces the rebuild
must keep straight. The exact byte layout of the stub tables and the algorithm
`LOADCORE` uses to bind an importer's stubs to an exporter's entries are
deferred to the `LOADCORE` analysis.

## What this pins for the rebuild

- Each module is an ELF32 MIPS-I IRX (`e_type 0xFF80`) with a `.iopmod`
  metadata section and a single relocatable load segment at vaddr 0.
- `.iopmod` name and version must match the module's EXTINFO entry in the
  archive; the rebuild produces both and keeps them equal.
- The loader relocates with `R_MIPS_32/26/HI16/LO16` against a runtime base;
  `HI16`/`LO16` are paired.
- Inter-module calls bind by lowercase library tag, a namespace separate from
  the ROMDIR file names — the subject, with `LOADCORE`, of the next document.

Next: `SYSMEM` and `LOADCORE` themselves — the memory manager the whole kernel
allocates from, and the module manager that loads, relocates and links every
module after it (`docs/project-state.md` §5, steps 2–3).
