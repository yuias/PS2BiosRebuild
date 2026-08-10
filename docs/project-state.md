# Project State and Working Method

Read this first when picking the project up. It records where the work stands,
how the work is done, and what is next. `README.md` is the front door; this is
the working document and is kept current.

> **Where the work is right now:** the analysis phase is under way. The ROM's
> file table is parsed and documented (`docs/analysis/01-rom-layout.md`), and
> the `RESET` boot block is disassembled and documented
> (`docs/analysis/02-boot-block.md`): the CPU dispatch, the IOP reset path and
> the self-locating ROMDIR search that enters `IOPBOOT`. Tooling is
> `tools/romdir.py` (archive) and `tools/romdis.py` (IOP/EE disassembly).
> Nothing is implemented yet; there is no build system and no `docs/spec/` —
> both arrive with the first implementation milestone.

---

## 1. Goal

A 4 MiB image that an emulator (PCSX2 is the working target) accepts in place
of a retail PS2 BIOS, produced the way the sibling PS1 project produced its
512 KiB image: analysis of retail ROMs → behavioural specifications →
implementation from the specifications. `docs/clean-room-policy.md` states the
rules and their current limits.

## 2. Reference images

Analysis targets `SCPH-50000` primarily; `SCPH-70000` is the comparison image
that keeps observations honest about what is model-specific.

| File (in `assets/`, not committed) | ROMVER | Date | SHA-256 |
| --- | --- | --- | --- |
| `SCPH-50000.bin` | `0170JC20030206` (v1.70, J, consumer) | 2003-02-06 | `6c003a0ca1beb501fba2063b12573521c9b3ec8cdcfb7f5e6a310771ebf166da` |
| `SCPH-70000.bin` | `0200JC20040614` (v2.00, J, consumer) | 2004-06-14 | `3b377ce5f7bb8d880260851867a5452b59149aeb8bb189c5df41e3bace7b8e09` |

`tools/romdir.py <image> --romver` reproduces the identification.

## 3. Working method

Inherited from the PS1 project, one group of functionality at a time:

1. **Analyse**: read the reference image with the tools in `tools/`, write
   `docs/analysis/NN-*.md`. Every claim names the command that reproduces it.
2. **Specify**: distil the observations into `docs/spec/NN-*.md` with numbered
   requirement IDs. Deviations from the reference get their own requirement
   saying what differs and why.
3. **Implement** from the specification, with build-time and post-build checks
   enforcing what the specs pin down.

The PS2 changes the shape of the problem in one important way the analysis has
already confirmed: the ROM is not a monolithic kernel image but a **file
archive** (the ROMDIR table) holding a boot block, ~90 IOP kernel modules, the
EE kernel, the OSD program and its resources. Analysis therefore proceeds
per-file, and the eventual build will assemble the image the same way.

## 4. Where it stands

| Area | State |
| --- | --- |
| ROM file table (ROMDIR/EXTINFO/ROMVER) | analysed — `docs/analysis/01-rom-layout.md` |
| Boot block (`RESET`): dispatch + IOP path | analysed — `docs/analysis/02-boot-block.md` |
| Boot block: EE path detail | outlined only (`02` §"EE reset path") |
| `IOPBOOT` + `IOPBTCONF` boot list | analysed — `docs/analysis/03-iopboot-and-boot-list.md` |
| IOP module (IRX) file format | analysed — `docs/analysis/04-iop-module-format.md` |
| `SYSMEM`, `LOADCORE` + the link algorithm | analysed — `docs/analysis/05-sysmem-and-loadcore.md` |
| `EXCEPMAN`, `INTRMANP`/`INTRMANI` | analysed — `docs/analysis/06-exceptions-and-interrupts.md` |
| `SSBUSC`, `DMACMAN` | analysed — `docs/analysis/07-bus-and-dma.md` |
| Remaining IOP kernel modules | not started |
| EE kernel (`KERNEL`) | not started |
| OSD (`OSDSYS` and resources) | not started |
| Build system / implementation | not started |

Notable observations to keep in mind (details and repro commands in the analysis
document each cites):

- `KERNEL` is byte-identical between the two reference images — the EE kernel
  did not change between ROM v1.70 (2003) and v2.00 (2004). (`01`)
- Both images place the ROMDIR table at `0x2740` and end their contents around
  `0x3ba000`, with the remainder of the 4 MiB zero-filled. (`01`)
- One discriminator, `PRId < 0x10 || (*(u32*)0xBF801450 & 8)`, runs from the
  reset vector through the kernel modules: it picks the boot block's bus table
  (`02`) and selects between the `P`/`I` module variant pairs (`06`).
- Ordinals identified so far, from how importers call them: `loadcore` 6 is
  export-library registration, `intrman` 17/18 are the critical-section pair
  (aliased at 19/20). (`05`, `07`)

### Open questions

- What convention makes a module entry's return of 1 leave it non-resident?
  Belongs with `IOPBOOT`'s per-module load step. (`06`)
- Which selector chooses `IOPBTCON2` over `IOPBTCONF`, and where it is read.
  (`03`)

## 5. Next steps

In rough order; each becomes a `docs/analysis/` document:

1. **C library and heap**: `SYSCLIB` and `HEAPLIB`, the next modules in boot
   order and the base later modules are written against.
2. From there, module by module, the PS1 pattern: analysis → spec → (later)
   implementation.
3. **EE boot path detail**: promote `02` §"EE reset path" to a full document
   once the EE kernel is under analysis (how `EELOAD` is staged to RAM at
   `0x8000_1000`).
