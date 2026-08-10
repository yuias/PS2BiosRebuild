# Project State and Working Method

Read this first when picking the project up. It records where the work stands,
how the work is done, and what is next. `README.md` is the front door; this is
the working document and is kept current.

> **Where the work is right now:** **the analysis is complete on both CPUs.**
> The IOP boot list is fully surveyed — all twenty-nine modules, in twelve
> documents — and so is the EE: its boot path, kernel tables, syscall groups,
> and now the arguments and return value of every one of the 125 syscall slots.
> **Five specifications are written and mechanically checked**:
> `01-rom-archive.md`, verified by a round-trip that rebuilds both reference
> images and a nested archive byte for byte; `02-module-abi.md`, verified by a
> conformance checker that passes on every IRX module of both images;
> `03-boot-chain.md`, executed by `tools/iopsim.py`, which boots the reference
> image to where the IOP waits for the EE, reproducing the spec's retail POST
> sequence and leaving 23 registered libraries and 55 bound import tables in
> RAM; and `04-ee-kernel.md` and `05-ee-syscall-abi.md`, gated statically by
> `tools/eeksys.py --check` and `tools/eeabi.py --check`.
> Tooling is `tools/romdir.py` and `tools/mkromdir.py` (archive read/write),
> `tools/irxinfo.py` (modules), `tools/romdis.py` (IOP/EE disassembly),
> `tools/iopsim.py` (IOP execution), `tools/eeksys.py`/`tools/eeabi.py` (EE
> kernel, statically) and `tools/eesim.py`, which **boots the EE on a simulated
> R5900**, reaches the kernel, and calls its syscalls — so the EE's dynamic
> requirements are executed rather than described, and `tools/ps2sim.py`, which
> **runs both CPUs against each other** across a modelled SIF — completing the
> handshake that each was blocked on and finishing the IOP boot. **What remains
> is implementation**: there is still no build system for the image's
> contents.

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
| EE syscalls: exceptions and interrupts | analysed — `docs/analysis/16-ee-interrupt-syscalls.md` |
| EE syscalls: the scheduler group | analysed — `docs/analysis/17-ee-scheduler-syscalls.md` |
| EE syscalls: cache and CP0 control | analysed — `docs/analysis/18-ee-cache-syscalls.md` |
| EE syscalls: initialisation and configuration | analysed — `docs/analysis/19-ee-config-syscalls.md` |
| **EE syscall group survey** | **complete — every band characterised** |
| EE syscalls: arguments and return values | analysed — `docs/analysis/21-ee-syscall-abi.md` |
| OSD (`OSDSYS`) | analysed — `docs/analysis/20-osdsys.md` |
| ROM archive format | **specified and proven** — `docs/spec/01-rom-archive.md` |
| IOP module + link ABI | **specified and checked** — `docs/spec/02-module-abi.md` |
| Boot chain (reset → boot list) | **specified and executed** — `docs/spec/03-boot-chain.md` |
| EE reset path + kernel interface | **specified and checked** — `docs/spec/04-ee-kernel.md` |
| EE syscall signatures, slot by slot | **specified and checked** — `docs/spec/05-ee-syscall-abi.md` |
| IOP simulator | **a gate** — `tools/iopsim.py --check`, tested in both directions |
| EE static gates | `tools/eeksys.py --check`, `tools/eeabi.py --check`, both tested in both directions |
| EE simulator | **a gate** — `tools/eesim.py --check`, boots both reference images and calls their syscalls |
| EE execution | analysed — `docs/analysis/22-ee-execution.md` |
| Joining the two CPUs (SIF handshake) | analysed — `docs/analysis/23-joining-the-two-cpus.md` |
| Joint simulator | **a gate** — `tools/ps2sim.py --check`, tested with `--no-bridge` |
| **Residency (IRX-12)** | **executed** — four teardowns in `iopsim`, `SIFINIT` in `ps2sim` |
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
- `OSDSYS` is 99% one compressed blob behind a 3 KB decompressor. Only its
  container and entry contract need reproducing; the payload is material the
  clean-room policy already excludes. (`20`)
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
- **No EE syscall takes a fifth argument or reads the caller's stack** — it
  cannot, since the handler runs on the kernel stack. Where the kernel needs a
  fifth value it is an internal `$t0` mode written by a wrapper. (`21`)
- Slot `0x74` writes the syscall table itself, so **all three dispatch tables**
  — exception, interrupt, syscall — are installable at run time and must be in
  writable memory. (`21`, extending `16`)
- Slots `0x04` and `0x05` never return to their caller: `0x04` re-enters the
  browser, `0x05` resumes from two stored kernel words. (`21`)
- The eight `EE-8h` pairs agree on their argument lists although their two
  entry paths are different code of very different size — an independent check
  that the signature analysis is sound. (`21`)
- Executed, the kernel announces its subsystems in a **different order** from
  the one its messages are stored in: GS, INTC and TIMER come first, not DMAC.
  A rebuild driven by the stored order would be wrong. (`22`, `spec/04` EE-10b)
- The kernel prints its own TLB layout — scratchpad at entry 0, its own
  mappings at 1–12, allocatable from 13 — which settles what `spec/05` SYS-7b
  had to leave open about syscall `0x09` choosing a `Random` index. (`22`)
- The IOP frees **four** module images during its own boot and a fifth,
  `SIFINIT`, only after the EE handshake. Two of the four are the rejected
  halves of the `P`/`I` variant pairs, which `06` predicted from the code.
  (`23`)
- `MSFLG`/`SMFLG` are **asymmetric**: each is set by one CPU and cleared by the
  other. Modelled as plain storage the handshake livelocks. (`23`, `spec/03`
  BOOT-10c)
- The two images' EE reset paths are **not** the same code: v2.00 clears the
  upper 64 bits of four registers with `padduw` first. `tools/romdis.py` cannot
  show this, since LLVM has no R5900 target; it surfaced only under execution.
  (`22`)

### Open questions

None outstanding. Both questions carried since `03` and `06` were settled in
`10`.

## 5. Next steps

The survey phase is finished: every major component has a document and every
component that can be gated has a gate. What is left is one large piece of work
and three loose ends.

1. **Implementation.** This is now the main line: a build system that assembles
   the image from per-file inputs the way `tools/mkromdir.py` already can, and
   the first contents written from `docs/spec/`. Nothing in the analysis is
   blocking it.
2. **SIF as a data path, not just registers.** `tools/ps2sim.py` shares the six
   handshake registers, which is enough to finish the IOP boot and tear down
   `SIFINIT`. It does not move data: after the handshake the EE waits on a SIF1
   DMA that nothing drains. Modelling SIF0/SIF1 as real channels on both sides,
   with their packet headers and an interrupt to wake the IOP's driver, is what
   `spec/04` EE-9's boot tail — `rom0:OSDSYS` across the bus — needs.
3. **Nine EE slots have an inferred rather than observed return.** `0x63` and
   `0x67` leave through the CP0 jump table, and seven others take their result
   from a callee (`spec/05` SYS-1c). Following those by hand would replace an
   inference with an observation, but nothing depends on it yet.
