# Specification: The IOP Module and Link ABI

Derived from `docs/analysis/04-iop-module-format.md`,
`05-sysmem-and-loadcore.md`, `08-c-library-and-heap.md`,
`09-threads-and-eeconf.md`, `10-module-loading-and-boot-configs.md` and
`12-ee-facing-services.md`.

Where `docs/spec/01-rom-archive.md` fixes the container, this fixes what goes in
it: how a module is encoded, how modules find each other, and what the loader
does with one. Every rule here is checked against all 57 IRX modules of the
reference image by `tools/irxinfo.py --check` (see "Verification").

The archive holds **two** kinds of ELF and this specification governs only the
first: 57 IOP relocatable modules (`e_type 0xFF80`, no arch flags) and 4 EE
executables (`e_type ET_EXEC`, MIPS-III arch flags) — `OSDSYS`, `PS1DRV`,
`PS2LOGO` and `TESTMODE`. The EE executables are a separate format, specified
when the EE side is analysed.

## IRX-1: Container

A module is a little-endian **ELF32 for MIPS-I** with `e_type = 0xFF80`, a
processor-specific type in the `ET_LOPROC` range. It carries exactly two program
headers:

| `p_type` | Contents |
| --- | --- |
| `0x70000080` | the `.iopmod` metadata section |
| `PT_LOAD` | `.text`, `.rodata`, `.data`, `.bss`, linked at `p_vaddr = 0` |

The single load segment at virtual address 0 is what makes a module
relocatable: the loader chooses a base, copies the segment there, appends zeroed
`.bss`, and applies the relocations of IRX-3.

## IRX-2: `.iopmod`

| Offset | Type | Field |
| --- | --- | --- |
| 0 | u32 | address of the module-info pair, or `0xFFFFFFFF` |
| 4 | u32 | entry point |
| 8 | u32 | `gp` value |
| 12 | u32 | text size |
| 16 | u32 | data size |
| 20 | u32 | bss size |
| 24 | u16 | version, BCD |
| 26 | char[] | name, NUL-terminated |

The module-info pair at the address in field 0 is `{const char *name; u16
version}`.

**IRX-2a:** The name and version here must equal the `0x03` comment and `0x02`
version of this file's `EXTINFO` entry (`ARC-6c`). One source produces both.

**IRX-2b:** The module version and any exported library version are independent
fields and need not agree. `ROMDRV` ships `.iopmod` 1.03 with `romdrv` 2.01.
Only the library version participates in IRX-11.

## IRX-3: Relocation

The load segment is relocated with standard MIPS `REL` entries (8 bytes:
`r_offset`, `r_info`) in `.rel.text` and `.rel.data`. Exactly four types occur,
all resolved against the chosen load base `B`:

| Type | Applied as |
| --- | --- |
| `R_MIPS_32` | word `+= B` |
| `R_MIPS_26` | jump target field rebased into the loaded segment |
| `R_MIPS_HI16` | high half of an address literal |
| `R_MIPS_LO16` | low half |

**IRX-3a:** `HI16` and `LO16` are **paired**: each `HI16` is followed by the
`LO16` it belongs to, and a loader must hold the `HI16` until that `LO16` so the
carry from the rebased low half is applied to the high half. This is the only
non-mechanical part of the fixup. In the reference archive the two occur in
equal counts — one pair per address — in all 57 modules.

*Deviation, our modules only:* a compiler that keeps one `lui` live across
several accesses to the same address emits one `HI16` followed by several
`LO16`, so our modules may carry more `LO16` than `HI16`; and one that hoists
several `lui` of one address apart from their uses emits a short run of `HI16`
followed by the one `LO16` the object writer paired them all with, the pairing
a static link resolves them by. The first is sound only when every `LO16`
sharing a high half names the same address — one high half cannot serve two
addresses once a load-time delta is added — and `tools/mkirx.py` refuses a
module where it cannot see that: a `HI16` must be followed, after at most
eight other `HI16`, by a `LO16` of its symbol, and a `LO16` with no `HI16`
before it must repeat, symbol and low half, one that was paired. A loader
holds the run of `HI16` rather than one, applies the `LO16`'s carry to every
one of it, and keeps the run after applying it, rather than dropping it, so
that each further `LO16` recomputes the same high half; a `LO16` with nothing
held is left alone. `tools/irxinfo.py --check` requires the pairing and no
longer equal counts.

## IRX-4: Library table header

Export and import tables share a 20-byte header:

| Offset | Size | Stored file | After registration |
| --- | --- | --- | --- |
| 0 | 4 | magic — `0x41C00000` export, `0x41E00000` import | registry `next` link |
| 4 | 4 | 0 | head of the bound-client list |
| 8 | 2 | version, BCD | unchanged |
| 10 | 2 | flags | flags, possibly with bit 0 set (IRX-10b) |
| 12 | 8 | library tag, ASCII, NUL-padded | unchanged |
| 20 | … | entries | unchanged |

**IRX-4c:** The magic is a **validation token consumed at registration**, not a
persistent field: `loadcore` writes the previous registry head over offset 0 when
it links a table in. A tool inspecting RAM after boot therefore cannot find
registered export tables by their magic — it must walk the registry, or match on
the header's shape. Import tables keep theirs, because binding rewrites only
stubs.

**IRX-4a:** The tag field is a fixed 8 bytes with no terminator when the tag
fills it (`loadcore` does).

**IRX-4b:** A table header is word-aligned within the load segment. The magic
byte pattern occurring at an unaligned offset is data, not a table — `CDVDMAN`
contains exactly such a false positive.

## IRX-5: Export tables

Entries begin at header + `0x14` and are absolute function pointers, terminated
by a zero word.

**IRX-5a:** Every entry carries an `R_MIPS_32` relocation, and the entries are
the **contiguous run** of such relocations starting at header + `0x14`. This is
the only reliable way to determine the extent *in the stored file*, because an
entry whose unrelocated value is 0 is indistinguishable from the terminator by
inspection. Scanning for a zero word is valid only after relocation.

**IRX-5b:** The word following the last entry is zero in the stored file.

## IRX-6: Ordinals are ABI

Importers bind by **ordinal** — the index into the export entry array — so slot
positions are fixed for the life of a library version. Inserting or removing an
entry renumbers every later one and silently breaks every importer.

**IRX-6a:** Unused slots are kept occupied rather than removed, and several may
share one `jr $ra` return stub. `SYSMEM` slots 2, 11, 12 and 13 all point at the
same two instructions.

**IRX-6b:** A slot may deliberately alias another. `intrman` slots 19 and 20
repeat the addresses of 17 and 18 — two published names for one function pair.

## IRX-7: Slots 0 and 1 are reserved

Slot 0 is a library-level hook, **not** reliably the module entry point.

Across the reference image: of the 41 modules exporting a single library, 39
have slot 0 equal to the `.iopmod` entry and 2 (`ROMDRV`, `XFLASH`) do not; and
in the 2 modules exporting several libraries (`SYSCLIB`, `THREADMAN`), every
secondary library's slot 0 is a `jr $ra` stub.

**IRX-7a:** No module imports ordinal 0 or 1 from any library. The lowest
ordinal bound anywhere in the reference is 2. Slots 0 and 1 are therefore
reserved hooks reached by the library machinery, if at all, and never by an
importer.

A build must keep both slots present so that ordinals from 2 upward land where
importers expect them, but is free to fill them with a return stub.

## IRX-8: Import tables

Entries are 8-byte stubs, two instructions each, terminated by a zero word:

```
jr    $ra                      # 0x03E00008
addiu $zero, $zero, <ordinal>  # 0x2400xxxx: opcode 9, ordinal in the low 16 bits
```

**IRX-8a:** Unbound, a stub returns harmlessly. An unresolved import is
survivable rather than fatal, and a build must preserve that property.

## IRX-9: Binding

To bind an importer's table against an exporter's:

1. Count the exporter's entries to the zero terminator.
2. For each stub, require its **second** word to decode as `addiu` (opcode 9);
   take the ordinal from that word's low 16 bits.
3. If the ordinal is within the exporter's count, overwrite the stub's **first**
   word with `0x08000000 | ((target >> 2) & 0x03FFFFFF)` — a `j` to
   `export[ordinal]`.
4. If the ordinal is out of range, write `0x03E00008` (`jr $ra`) instead, so an
   over-range import degrades to a return.
5. Advance 8 bytes; stop at the terminator.

**IRX-9a:** Only the first word is rewritten. The `addiu` remains as the new
jump's delay slot — harmless, because `$v0` is the return-value register the
callee overwrites, and it leaves the bound stub still recording its ordinal.

**IRX-9b:** Import tables whose flags have any of the low three bits set are
skipped by the binder.

## IRX-10: Registration

Two entry points register an export table, and they are not interchangeable.

**IRX-10a — versioned registration** (`loadcore` ordinal 6). Validates the
export magic, then walks the registry:

- different tag → not this library, continue;
- same tag, different **major** version → a different library, continue;
- same tag and major → the same library, and registration succeeds **only if the
  new minor version is strictly greater**. Equal or lower returns −1.

So a library's identity is **tag + major version**, and the minor version is a
generation counter.

**IRX-10b — pinned registration** (`loadcore` ordinal 10). Validates the magic,
sets **flags bit 0** on the table, and links it at the registry head with no
comparison at all. The flag is set at run time; the stored table has `flags 0`.

## IRX-11: Supersession

When IRX-10a accepts a higher minor version, the old library's list of bound
clients is walked and each client's flags halfword at +`0xA` is tested:

- bit 0 clear → the client is re-bound against the new library;
- bit 0 set → the client stays attached to the old one.

Supersession therefore actually redirects callers, and IRX-10b's flag is what
exempts a library from having its clients taken.

**IRX-11a:** A module may rewrite its own table's version in RAM before
registering it, to arrange the generation ordering. `SYSCLIB` decrements its
`stdio` table from 1.02 to 1.01 so that `STDIO`'s 1.02 supersedes it eight
modules later. **A build that registers the stored version instead would make
the later registration fail with no diagnostic anywhere.**

## IRX-12: Module entry and residency

A module entry is called as

```
entry(argc, argv, 0, module_record)
```

with the module's own `$gp` installed from the module record and the loader's
`$gp` restored afterwards.

**IRX-12a:** Residency is decided by `return & 3` — **clear keeps the module
resident, set frees it**. It is a mask, not an equality test.

**IRX-12b:** A non-resident module is torn down inside an `intrman` 17/18
critical section, and its memory is released through `sysmem` ordinal 5 with the
base rounded **down to a `0x100` boundary**.

**IRX-12c:** Module record offsets `+0x10` (entry) and `+0x14` (`gp`) are ABI.

## IRX-13: Module shapes

All four shapes must be expressible:

| Shape | Exports | Resident | Example |
| --- | --- | --- | --- |
| Library | yes | yes | `SYSMEM` |
| Multi-library | several | yes | `THREADMAN` (7) |
| Resident service | none | yes | `REBOOT`, `FILEIO` |
| One-shot action | none | no | `SIFINIT`, `IGREETING` |

Only the last is distinguished by IRX-12a. A one-shot module may legitimately
have an empty `.iopmod` name and version `0.00`.

## IRX-14: Identified ordinals

These are fixed by how importers call them and must not move:

| Library | Ordinal | Function |
| --- | --- | --- |
| `loadcore` | 6 | versioned export registration (IRX-10a) |
| `loadcore` | 10 | pinned export registration (IRX-10b) |
| `loadcore` | 12 | boot-record lookup by key |
| `intrman` | 17, 18 | critical section enter / leave (aliased at 19, 20) |
| `sysmem` | 4, 5 | allocate / free |

## Verification

`tools/irxinfo.py --check` asserts the mechanically checkable requirements
against a module and exits non-zero on any failure: IRX-1's container shape,
IRX-2a's metadata agreement, IRX-3a's `HI16`/`LO16` pairing, IRX-4a/4b's header
placement, IRX-5a/5b's export extent and terminator, IRX-7a's reserved slots,
and IRX-8's stub encoding.

Run across every IRX module of the reference image, it must report no failures.
It does — **57 modules, 0 failures**:

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
for f in <outdir>/*; do
  head -c4 "$f" | grep -q ELF || continue
  python3 tools/irxinfo.py "$f" --check --quiet || echo "FAIL $f"
done
```

The four EE executables fail IRX-1 by design — that is the check correctly
refusing to treat them as IOP modules, and is how the 57/4 split above was
established in the first place.

`SCPH-70000` passes too, at 56 IRX modules and 0 failures, so nothing here is
specific to the primary reference image.

### The dynamic half

IRX-9 through IRX-12 describe run-time behaviour, and `tools/iopsim.py` now
exercises them by booting the reference image and reporting what the boot left
in RAM:

```sh
python3 tools/iopsim.py assets/SCPH-50000.bin
```

The IOP boot reaches `SIFMAN` and then waits on the SIF for an EE that an
IOP-only simulator does not provide — the correct place for it to stop. By then:

| Observation | Confirms |
| --- | --- |
| 55 import tables in RAM, **all 55 bound** | IRX-9 |
| 23 libraries registered, in `IOPBTCONF` order | IRX-10a |
| `thrdman` alone carries `flags 0x1` | IRX-10b — the pin is set at run time |
| **both `stdio` 1.01 and `stdio` 1.02 are present** | IRX-11a |
| `intrman` and `timrman` each appear once | the `P`/`I` selection of `06` |
| `romdrv` registers 2.01 while its module is 1.03 | IRX-2b |

The `stdio` row is the one that matters most. IRX-11a was flagged above as
having no static signature and being the likeliest thing to break silently;
seeing `SYSCLIB`'s deliberately lowered 1.01 sitting alongside `STDIO`'s 1.02
is direct evidence that the supersession this specification describes is what
the hardware actually does.

**IRX-4c came out of the same run.** Scanning RAM for the export magic found
exactly one table where there should have been twenty-three, because
registration overwrites the magic with the registry link.

### The gate, and proof that it bites

`--check` turns the run into a judgement rather than a report, and exits
non-zero naming the requirement that failed:

```sh
python3 tools/iopsim.py assets/SCPH-50000.bin --check
# assets/SCPH-50000.bin: ok -- POST [...], 23 libraries, 55 import tables all bound
```

A gate is only worth having if it fails when it should, so it is tested against
deliberately broken images. Removing the single `addiu $v0, $v0, -1` at
`SYSCLIB` module offset `0x40` — the version-lowering of IRX-11a — produces:

```
IRX-10a: registered libraries [... 'romdrv', 'sifman'] != [... 'romdrv', 'stdio', 'sifman']
IRX-11a: stdio versions ['0x102'] != ['0x101', '0x102']
         -- the provisional stdio was not superseded
```

**The mutated boot does not crash.** It runs to the same place as a correct one
and leaves twenty-two libraries instead of twenty-three, with nothing else to
distinguish it. That is exactly the silent failure IRX-11a warns about, now
demonstrated rather than predicted — and the reason this requirement needs a
gate and not a code review.

**IRX-12 is executed, not only described.** `tools/iopsim.py --check` watches
`sysmem` ordinal 5 during the boot and requires the four module images the
reference releases, each on the `0x100` boundary IRX-12b specifies; two of them
are the rejected halves of the `P`/`I` variant pairs. The fifth release —
`SIFINIT`, the module `docs/analysis/12` named as unreachable — happens only
after the EE handshake, and `tools/ps2sim.py --check` requires that one
(`docs/analysis/23-joining-the-two-cpus.md`). What is still not observed is
IRX-12's `intrman` 17/18 critical section around the teardown.

**IRX-7 exists because this sweep contradicted an earlier document.**
`docs/analysis/05` had generalised "slot 0 is the module entry" from two
examples; running it across every module produced nine counterexamples, and the
rule that survives is the weaker, checkable IRX-7a. The sweep also corrected the
module count itself, by failing on the four files that are not IRX at all.
