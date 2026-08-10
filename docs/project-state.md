# Project State and Working Method

Read this first when picking the project up. It records where the work stands,
how the work is done, and what is next. `README.md` is the front door; this is
the working document and is kept current.

> **Where the work is right now:** the **IOP boot list is fully surveyed** —
> all twenty-nine modules, in twelve documents covering the ROM archive format,
> the boot block, `IOPBOOT` and the boot list, the IRX module format, and every
> module from `SYSMEM` to `EESYNC`, with no questions outstanding. The EE side
> is next and is untouched apart from an outline. **Two specifications are
> written and mechanically checked**: `01-rom-archive.md`, verified by a
> round-trip that rebuilds both reference images and a nested archive byte for
> byte, `02-module-abi.md`, verified by a conformance checker that passes on
> every IRX module of both images, and `03-boot-chain.md`, whose static claims
> are each reproducible by disassembly. **`tools/iopsim.py` now boots the
> reference image** all the way to where the IOP waits for the EE, reproducing
> the spec's retail POST sequence and leaving 23 registered libraries and 55
> bound import tables in RAM — so both the boot chain and the run-time half of
> the module ABI are verified rather than described.
> Tooling is `tools/romdir.py` and `tools/mkromdir.py` (archive read/write),
> `tools/irxinfo.py` (modules) and `tools/romdis.py` (IOP/EE disassembly).
> There is still no build system for the image's *contents*.

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
| Boot block: EE path detail | analysed — `docs/analysis/13-ee-boot-path.md` |
| `IOPBOOT` + `IOPBTCONF` boot list | analysed — `docs/analysis/03-iopboot-and-boot-list.md` |
| IOP module (IRX) file format | analysed — `docs/analysis/04-iop-module-format.md` |
| `SYSMEM`, `LOADCORE` + the link algorithm | analysed — `docs/analysis/05-sysmem-and-loadcore.md` |
| `EXCEPMAN`, `INTRMANP`/`INTRMANI` | analysed — `docs/analysis/06-exceptions-and-interrupts.md` |
| `SSBUSC`, `DMACMAN` | analysed — `docs/analysis/07-bus-and-dma.md` |
| `SYSCLIB`, `STDIO`, `HEAPLIB` | analysed — `docs/analysis/08-c-library-and-heap.md` |
| `EECONF`, `THREADMAN` | analysed — `docs/analysis/09-threads-and-eeconf.md` |
| `VBLANK`, `IOMAN`, `MODLOAD` | analysed — `docs/analysis/10-module-loading-and-boot-configs.md` |
| `ROMDRV`, `SIFMAN`, `SIFCMD`, `SIFINIT` | analysed — `docs/analysis/11-sif-and-rom-driver.md` |
| `IGREETING`, `REBOOT`, `LOADFILE`, CDVD, `FILEIO`, `SECRMAN`, `EESYNC` | analysed — `docs/analysis/12-ee-facing-services.md` |
| **IOP boot list** | **complete — all 29 modules surveyed** |
| EE kernel: vectors, exceptions, syscall table | analysed — `docs/analysis/14-ee-kernel-syscalls.md` |
| EE kernel: syscall groups, boot tail | analysed — `docs/analysis/15-ee-syscall-groups.md` |
| EE kernel: individual syscall behaviour | not started |
| OSD (`OSDSYS` and resources) | not started |
| ROM archive format | **specified and proven** — `docs/spec/01-rom-archive.md` |
| IOP module + link ABI | **specified and checked** — `docs/spec/02-module-abi.md` |
| Boot chain (reset → boot list) | **specified and executed** — `docs/spec/03-boot-chain.md` |
| IOP simulator | **a gate** — `tools/iopsim.py --check`, tested in both directions |
| Build system / implementation | not started |

Notable observations to keep in mind (details and repro commands in the analysis
document each cites):

- `KERNEL` is byte-identical between the two reference images — the EE kernel
  did not change between ROM v1.70 (2003) and v2.00 (2004). (`01`)
- Both images place the ROMDIR table at `0x2740` and end their contents around
  `0x3ba000`, with the remainder of the 4 MiB zero-filled. (`01`)
- One discriminator, `PRId < 0x10 || (*(u32*)0xBF801450 & 8)`, runs from the
  reset vector through the kernel modules: it picks the boot block's bus table
  (`02`), selects between the `P`/`I` module variant pairs (`06`), and gates
  `SIFMAN`'s residency outright (`11`).
- Ordinals identified so far, from how importers call them: `loadcore` 6 is
  versioned export registration and `loadcore` 10 the pinned variant,
  `loadcore` 12 looks up a boot record, `intrman` 17/18 are the
  critical-section pair (aliased at 19/20), `sysmem` 4 and 5 are the allocator
  and deallocator. (`05`, `07`, `08`, `09`, `10`)
- Fixed absolute addresses are part of the ABI: the exception vector at `0`
  (`06`) and the boot-parameter table anchored at `0x3F0`, consulted by
  `EECONF`, `SIFINIT` and `SIFCMD` (`09`, `11`).
- The self-locating ROMDIR scan is written out **five** times over — boot block,
  `IOPBOOT`, `MODLOAD`, `ROMDRV` and the EE reset path — with no shared routine,
  so the archive's self-location is firmly ABI. (`02`, `03`, `10`, `11`, `13`)
- `RDRAM` is entered by hard-coded address `0x9FC41000`, so its archive offset
  is load-bearing where no other file's is. (`13`)
- `KERNEL` is copied to physical 0 and entered at `0x80001000`: its first
  `0x1000` bytes are the EE exception vectors. (`13`)
- The EE syscall table is at `0x80014F40`: 125 slots, none null, 98 distinct
  targets, number passed in `$v1` with negatives negated. Thirteen slots share
  a handler that reports the undefined number; three are published through
  KSEG1 so they run uncached. (`14`)
- Syscall aliasing comes in consecutive blocks duplicated at a fixed offset
  (`0x14`–`0x19` → `0x1A`–`0x1F`, `0x63`–`0x66` → `0x67`–`0x6A`). Slots `0x03`
  and `0x3F` are *retired*, kept occupied so an old caller is diagnosed. (`15`)
- The boot tail: syscall `0x06` is the program loader and uses `EELOAD` as its
  stub; syscall `0x7B` is `0x06` with the path pinned to `rom0:OSDSYS`; the
  default boot passes `argv = { "BootBrowser" }`. `rom0:` is served by the
  IOP's `ROMDRV` across the SIF. (`15`)
- A library is identified by **tag + major version**; the minor version is a
  generation counter and a higher one supersedes, inheriting the old library's
  unpinned clients. Modules rewrite their own table versions in RAM to arrange
  this, so a stored table's version is not what gets registered. (`08`)
- Module entries are called as `entry(argc, argv, 0, module_record)` with the
  module's own `$gp` installed, and **`return & 3` decides residency** — clear
  keeps the module, set frees it. (`10`)
- Alternative IOP configurations are nested ROMDIR archives (`EELOADCNF`,
  `OSDCNF`) carrying their own `IOPBTCONF`, so the archive format must nest and
  the `X`-variant modules are chosen by which list is booted. (`10`)
- The boot list has three phases — kernel core, OS services, then EE-facing
  services. No phase-one module imports `sifman`/`sifcmd`; every phase-three one
  does. Alternative configurations extend phase three only. (`12`)
- Four module shapes exist: library, multi-library, export-free resident
  service, and one-shot action (never resident, distinguished only by
  `return & 3`). (`12`)
- The archive holds two kinds of ELF: 57 IOP relocatable modules
  (`e_type 0xFF80`) and 4 EE executables (`ET_EXEC`, MIPS-III) — `OSDSYS`,
  `PS1DRV`, `PS2LOGO`, `TESTMODE`. (`spec/02`)
- Export slots 0 and 1 are reserved hooks that nothing imports; the lowest
  ordinal bound anywhere is 2. Slot 0 is *not* reliably the module entry.
  (`spec/02` IRX-7, correcting `05`)

### Open questions

None outstanding. Both questions carried since `03` and `06` were settled in
`10`.

## 5. Next steps

In rough order; each becomes a `docs/analysis/` document:

1. **The EE side.** Promote `02` §"EE reset path" to a full document: how the
   EE path stages `EELOAD` to RAM at `0x8000_1000`, and what the `KERNEL` image
   — byte-identical across both reference ROMs — contains. This needs the R5900
   caveats in `tools/romdis.py` kept in mind.
2. **`OSDSYS`** and the boot flow that reaches it.
3. **Residency (IRX-12) stays unverified for now.** The one-shot modules that
   would demonstrate it — `SIFINIT` (26) and `IGREETING` (20) — sit at or past
   the point where the IOP boot waits for the EE, so an IOP-only simulator
   cannot reach them being freed. It needs either an EE stub answering the SIF
   handshake or a targeted harness that loads a single module.
4. **`OSDSYS`**, the last unexamined major component, and the boot flow that
   reaches it now that the tail is understood.
5. **The first EE specification.** The vector page, exception dispatch and
   syscall entry are settled enough to write as `docs/spec/04`, with the
   syscall table's slot-by-slot contract following as the behaviour of each
   group is established.
