# Project State and Working Method

Read this first when picking the project up. It records where the work stands,
how the work is done, and what is next. `README.md` is the front door; this is
the working document and is kept current.

> **Where the work is right now:** the analysis is complete on both CPUs and
> **the image boots end to end on the simulators**. Its EE comes up on our own
> kernel, meets the IOP across the SIF, fetches `rom0:OSDSYS` over the bus in
> the reference's own packet framing and runs it — `spec/03` BOOT-1 to BOOT-11
> and `spec/04` EE-1 to EE-9, on our own code rather than the reference's. What
> is thin is *depth*: eighteen of the boot list's twenty-nine modules exist
> plus one loadable on request, seventy-nine of the 125 syscall slots are
> served, and both processors take interrupts and switch threads from them.
>
> Five specifications are written and every one has a gate that has been tested
> in both directions. Six simulators and checkers judge an image — ours and the
> reference's on the same terms — and `ninja -C build check` states what our
> image is currently expected to do, printing what it is *not* yet expected to
> do on every success so the list cannot go stale.
>
> **And it boots end to end on the working target.** PCSX2 accepts the image as
> a BIOS and runs it through: both processors reset, the IOP loads and links our
> modules, the two meet across the SIF, a file crosses the bus in each direction
> and `rom0:OSDSYS` is placed and entered. The blocker that stood there —
> `docs/analysis/29-sif0-on-pcsx2.md` — was one bit: the EE's DMAC uses bit 31
> of `MADR`/`TADR` to select the scratchpad, and a KSEG0-linked kernel hands it
> that bit set on every pointer.

---

## 1. Goal

A 4 MiB image that an emulator (PCSX2 is the working target) accepts in place
of a retail PS2 BIOS and boots software with: analysis of retail ROMs →
behavioural specifications → implementation from the specifications. `docs/clean-room-policy.md` states the
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

One group of functionality at a time:

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
| CDVD NVM words + OSD configuration blocks | analysed — `docs/analysis/26-cdvd-nvm-and-config.md` |
| `OSDSYS` payload, expanded and read | analysed — `docs/analysis/27-osdsys-payload-and-config.md` |
| OSD configuration fields | analysed — `docs/analysis/28-osd-config-fields.md` |
| EE main-thread setup: `0x3C`–`0x3E`, the thread record, how a program gets its arguments | analysed — `docs/analysis/30-ee-thread-setup.md` |
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
| SIF data path (framing) | analysed — `docs/analysis/24-sif-data-path.md` |
| SIF packet framing | **specified and gated** — `docs/spec/03-boot-chain.md` BOOT-11 |
| DMAC addressing (BOOT-11k) | **specified and gated** — `tools/ps2sim.py`, tested in both directions |
| First run on PCSX2 | analysed — `docs/analysis/25-first-run-on-pcsx2.md` |
| The IOP kernel, the reboot, and the title's own start-up | analysed as the pull demanded it — `docs/analysis/31`–`52`, each written for a fault M1 or M2 hit, in the order the faults came |
| SIF0 on PCSX2, and why it delivered nothing | analysed — `docs/analysis/29-sif0-on-pcsx2.md` |
| PCSX2 as a target | **boots there end to end** — accepted as a BIOS, both CPUs, both directions of the bus, `rom0:OSDSYS` entered |

### What is built

`docs/implementation.md` covers all of this, including every deviation from the
reference and the reason for it.

| Area | State |
| --- | --- |
| Build system | **CMake + Ninja + LLVM** — no cross-gcc needed; C++26, assembly only where the machine requires it |
| Boot block (`RESET`), both paths | **built** — EE reaches our kernel; IOP emits BOOT-5a's full POST sequence |
| `IOPBOOT` + `IOPBTCONF` | **built** — parses the boot list, places `SYSMEM` and `LOADCORE`, and hands the rest to `LOADCORE` with BOOT-8c's block; `LOADCORE` loads them and publishes the boot records of BOOT-8. The list's name is built from the boot mode (BOOT-9e), so a reboot picks `IOPBTCON2` |
| The IOP reboot (`docs/analysis/45`, `51`) | **built** — an argument-less `sceSifIopReset` re-enters `IOPBOOT` with mode 1 and reloads the list; one naming an image boots `IOPBTCON2`, and `UDNL` merges the image over `rom0` by version, stages the result and hands over. A retail title runs on the kernel it staged |
| IRX producer + loader | **built** — `tools/mkirx.py`; all three modules pass `irxinfo --check` |
| Binding + registration (IRX-9, IRX-10) | **built** — `LOADCORE` calls `SYSMEM` across a bound stub |
| EE handshake (BOOT-10) | **built** — `SIFMAN` ordinal 5 and the kernel's `sif.S` release each other, and `EESYNC`, last on the list, tells the EE the IOP is listening |
| SIF data path | **built and gated** — BOOT-11's framing both ways and BOOT-11k's addressing; the EE fetches an archive file |
| The disc and the EE's file service (IOP-8, IOP-9) | **built and exercised against a retail disc** — `CDVDMAN`'s `cdrom0:` device and 2048-byte sector reads, `FILEIO`'s RPC, and `CDVDFSV`'s five RPC services, of which `sceCdInit`, `sceCdSearchFile` and `sceCdDiskReady` are served in full and the two `fno` tables only as deep as `CDVDMAN` goes (`docs/implementation.md`). After a title's reboot the disc's own `CDVDMAN`/`CDVDFSV` replace these (see the reboot row) |
| IOP timers and alarms (IOP-3j, IOP-3k, IOP-7) | **built** — `TIMEMANI` on the boot list; `DelayThread`, `SetAlarm`, the clock on timer 5, and an interrupt handler per timer (IOP-7f) |
| Boot tail (EE-9) | **built** — `EELOAD` is staged and asks the IOP's `LOADFILE` for `rom0:OSDSYS`, which is placed and entered through `ExecPS2`; PCSX2's fast-boot hook and its `-elf` launch work on it |
| The disc's own boot target | **booted from the disc** — the program in the `OSDSYS` slot waits in `sceCdInit(0)` for the drive, binds `FILEIO` over the SIF, opens `cdrom0:\SYSTEM.CNF;1` through `CDVDMAN`, and hands its `BOOT2=` path back to syscall `0x06`, which stages `EELOAD` and enters the title. With no disc it says which step refused and stops |
| `RDRAM`, `ROMVER` | **built** — minimal, spec-derived |
| EE kernel: vector page, dispatch, syscall table | **built** — the slots a title's runtime and the disc-boot pull have asked for are served, the rest report themselves; the gate prints the count |
| EE interrupt delivery (SYS-12) | **built and driving a client** — handler lists, the interrupt entry and the reschedule on exit; a retail title's own SIF handler runs on our channel-5 interrupt and its `SifInitRpc` completes, which is what the `$gp` and quadword-alignment requirements of SYS-12b exist for |
| EE main-thread setup (SYS-8) | **built and gated** — `0x3C`/`0x3D`/`0x3E`, the argument block, entry with the launcher's registers |
| EE threads and semaphores (SYS-9, SYS-10, SYS-11) | **built and gated** — records, ready queues, the switch through the dispatcher's block; 256-slot table |
| M1 test program (`tests/m1/`) | **met on both emulators** — main, arguments, a semaphore, a second thread, `SifInitRpc` over the interrupt-driven SIF, and `SifLoadModule` of `rom0:SIO2MAN` through `LOADFILE`/`MODLOAD`; PCSX2's `-elf` launch of it is a gate |
| The shell's own screen | **a menu** — with nothing in the drive the program shows three entries and the controller moves between them: start what is in the drive, what this build is, read the settings again. The first is a real action and calls the same code the boot path does |
| The controller (IOP-14) | **built, gated on one emulator** — `PADMAN` reads port 0 slot 0 through `SIO2MAN` once per vertical blank and pushes a 0x40-byte record into EE memory; the shell loads both modules itself and names the buttons held. PS2e carries it; PCSX2 loads the modules and runs the driver but completes no transfer whose command bytes went through the data register (`docs/implementation.md`) |
| EE-facing IOP services (IOP-5, IOP-9, IOP-10, IOP-11) | **built** — `LOADFILE` with the version query a title gates on, `FILEIO`, `VBLANK`, a `SECRMAN` interface that answers without the MagicGate exchange, and an `IOPTTY` console the drivers print through |
| The title on the merged kernel (`docs/analysis/45`, `51`) | **plays** — after the title's reboot eight of our modules stay in the merged kernel (`EXCEPMAN`, `INTRMAN`, `DMACMAN`, `HEAPLIB`, `VBLANK`, `IOPTTY`, `SECRMAN`, `REBOOT`); the rest of what the title runs on is the disc's. Where the play stands is in §6 |
| EE syscall entry: 128-bit context (EE-7e) | **built and gated** — `imgcheck` plants a marker in every register |
| EE cache and CP0 band (EE-8e, EE-6f, SYS-4) | **built and gated** — the three KSEG1 slots and the CP0 reader, called and read back |
| Source language | **C++26 where the machine allows it** — assembly only for the reset path, entry stubs, the vector page, the syscall context save, and the exact instruction words IRX-8 specifies for an import stub; `EESYNC`, `RDRAM` and the loader entries are compiled, since 2026-08-17 `mkirx` accepts what the compiler emits, `IOPBOOT` is compiled since the same day, once its archive offset was knowable before it is built, and `SYSMEM`/`LOADCORE` followed once IRX-8's stub words could be kept as a small top-level `asm` block (`docs/implementation.md`) |
| Everything else in the image | not started — `ninja -C build check` prints the list at the end of every run, and `docs/implementation.md` says why each item waits |

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
- `OSDSYS`'s compressed payload never had to be decoded: its `.text.Expand*`
  group is plain MIPS-III, so `tools/eesim.py --call` **runs** it and the
  expander returns the size its own header word states. Executing a routine
  beat reading it, again. (`27`)
- The OSD's configuration is **two 15-byte blocks**, reached over `CDVDFSV`
  RPC service `0x80000593` functions 14–17. Block 0 is passed through
  untouched; block 1's first byte carries flags whose top bits gate whether
  its second byte is read at all. (`27`)
- That gate is a **generation marker**: with the top three bits of block 1
  byte +0 clear, the language is one bit — Japanese or English — and with them
  set it is a five-bit index into an **eight**-entry table that is **not bounds
  checked**, so a forged 9 leaves the OSD with a null string table and nothing
  to draw. The field map was measured by sweeping single bits through the
  decoder under `eesim`, then confirmed against the accessor bank. (`28`)
- An OSD configuration block is **15 data bytes plus a one-byte sum of them**,
  which `CDVDMAN` verifies on read and generates on write, in two independent
  stretches of code. The driver never interprets the fifteen bytes, so the
  "is this console configured" rule is above it, in `OSDSYS`. (`26`)
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
- **Bit 31 of the EE's `MADR`/`TADR` selects the scratchpad**, so an address
  handed to the DMAC must be physical rather than merely untranslated. A
  KSEG0-linked kernel sets that bit on every pointer, and the simulators mask it
  away. (`29`)
- On the SIF **the sender frames the transfer**. A receiving channel is armed
  with a mode and nothing else; the address its data lands at arrives in front
  of that data — in a header quadword going one way, in a peer-facing tag the
  sender carries in its send block going the other. Every "where do I put this"
  question on the bus is answered by whoever asked it. (`24`, `spec/03` BOOT-11)
- The EE's outgoing SIF channel has `TTE` clear, so the tag quadwords do not
  travel: the packet header the IOP reads is the first quadword of a tag's
  *data*, not the tag. Reading it as the tag puts everything one quadword out.
  (`24`)
- Syscall `0x60` **zeroes `Config`** rather than setting the cache mode: the
  reference ANDs the argument into a value whose low three bits it has just
  cleared, where `0x61` ORs in the analogous place. A defect in the shipped
  ROM, reproduced deliberately — the raw instruction word is quoted in `18` so
  the claim does not rest on a disassembler. (`18`, `spec/05` SYS-4a)
- A syscall returns with `$v1` still holding the **scaled** number — the
  dispatcher's byte index, `number × 4` — on both reference images. `$at`,
  `$sp` and the upper half of `$t9` do not come back at all. Measured by
  planting a 128-bit marker in every register, not read off the prologue.
  (`spec/04` EE-7e, EE-7e2)

### Open questions

**None on the boot path.** The one that stood here — why SIF0 delivered nothing
under PCSX2 — is answered in `docs/analysis/29-sif0-on-pcsx2.md`: the EE's DMAC
reads bit 31 of `MADR` and `TADR` as *select the scratchpad*, and a kernel
linked in KSEG0 sets that bit on every pointer it hands over. The reading that
EE-to-IOP transfers worked and IOP-to-EE ones did not was the wrong way round:
the request never left the EE, so the IOP replied, correctly framed, to the
address zero it had been given.

The simulators could not have found it. They resolve an address by masking it
with `0x1FFFFFFF`, which is right for a CPU access and erases exactly the bit
the DMAC cares about. `tools/ps2sim.py` now models that bit, so this is a gate
— `spec/03` BOOT-11k, tested in both directions.

Everything else carried since `03` and `06` was settled in `10`.

### Leads carried in from outside

`docs/analysis/24` used, and says so, a set of observations contributed from a
sibling project of the same author's — notes taken while bringing an emulator
up against the same SCPH-50000 image. The SIF ones are analysed and gated, and
the IOP's `I_MASK` of `0x1080D` was confirmed by reading the reference's own
state at the point it parks. The rest are recorded here so they are not lost,
**unverified against this repository's tools**, each against the document that
would confirm it:

| Lead | Where it belongs |
| --- | --- |
| SBUS interrupt: the IOP's `SMFLG` writes raise EE INTC bit 1; the EE's handler folds the flags into an `SREG` array and acknowledges by clearing | `16`, `23`, `24` |
| SIF control register: EE reads OR in `0xF0000102`, IOP `0xF0000002`; the IOP sets bits `0x20`/`0x40`/`0x80` per path and polls that they stick | `11`, `23` |
| EE timer 3: the compare interrupt is a latch, fires only while `EQUF` is clear, and the kernel parks the timer by leaving `EQUF` set | `16`, `22` |
| EE TLB: after boot the kernel relies on real mappings; address folding stops working once `OSDSYS` loads | `18`, `22` |
| `RDRAM`: `0x1000F520` reads `0x1201` at reset and keys a table of configurations; `MCH_RICM`/`MCH_DRD` run a serial device handshake | `13` |
| `ROMGSCRT` command interface, and the value that makes the ROM deliberately hang | not yet analysed |
| SIO2: `CTRL` bit 0 must read back clear with `I_STAT` bit 17 raised, or `SIO2MAN` spins | not yet analysed |
| `sceSifIopReset`: SIFCMD cid `0x80000003` reboots the IOP without the ROM stub, and in-flight FIFO state must be discarded | `12` |

The traffic goes both ways: the same project asks questions of this one, and
`docs/analysis/26-cdvd-nvm-and-config.md` was written to answer one — why an
OSD with a zeroed NVRAM stops on the first-boot screen and rejects a hand-made
configuration block. Answering it cost nothing extra, since `CDVDMAN` is boot
list module 24 and owed to §6's M2 regardless.

## 5. Resuming

```sh
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mipsel-ps2.cmake
ninja -C build && ninja -C build check      # build the image and judge it
```

Configuring without a build type gives `MinSizeRel`; `-DCMAKE_BUILD_TYPE=Debug`
builds the same sources unoptimised and both boot.

**What a good run looks like.** Our own image produces exactly these five lines
on PCSX2 (the commit in the fourth is the build's own, with a `+` when the tree
was not clean). Anything shorter is a regression, and the line it stops at says
where:

```
# PS2BiosRebuild EE kernel: entered at 0x80001000.
# The IOP answered; the SIF handshake is complete.
# ROMVER, fetched from the archive across the SIF: 0100XP20260810
# OSDSYS: PS2BiosRebuild 0.1.0 (5747e7f)
# OSDSYS: loaded from the archive and running. Argument: BootBrowser
```

```sh
python3 tools/ps2sim.py build/rom.bin | grep '#'
```

prints the last three of them — its report is headed *the EE's last words* and
shows the tail, so the banner's absence there is the display and not a fault.

The reference gates, which must keep passing whatever we change in the tools:

```sh
python3 tools/iopsim.py assets/SCPH-50000.bin --check
python3 tools/eesim.py  assets/SCPH-50000.bin --check
python3 tools/eesim.py  assets/SCPH-70000.bin --check
python3 tools/ps2sim.py assets/SCPH-50000.bin --check
python3 tools/ps2sim.py assets/SCPH-70000.bin --check
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
python3 tools/eeksys.py <outdir>/KERNEL --check
python3 tools/eeabi.py  <outdir>/KERNEL --check
for f in <outdir>/*; do python3 tools/irxinfo.py "$f" --check; done   # 57 modules
```

`docs/implementation.md` is the output-side document: how the image is built,
what it does today, and every deviation from the reference with its reason.

### Running it on PCSX2

The working target, and since `docs/analysis/25` the thing that finds what the
simulators do not. It is not installed by the repository; this is the setup
that was used.

```sh
# v2.6.3, the Linux AppImage. Extracting it beats mounting it: the inner
# binary can then be run directly and its diagnostics are visible.
mkdir -p ~/tools && cd ~/tools
curl -sL -o pcsx2.AppImage https://github.com/PCSX2/pcsx2/releases/download/\
v2.6.3/pcsx2-v2.6.3-linux-appimage-x64-Qt.AppImage
chmod +x pcsx2.AppImage && ./pcsx2.AppImage --appimage-extract
```

It needs `libopengl0`, `libxcb-cursor0` and `libxkbcommon-x11-0`, which are not
part of a default Ubuntu install. Then:

```sh
cp build/rom.bin ~/.config/PCSX2/bios/PS2BiosRebuild.bin
# and in ~/.config/PCSX2/inis/PCSX2.ini: [Filenames] BIOS = PS2BiosRebuild.bin
timeout 40 ~/tools/squashfs-root/AppRun -bios -batch -nogui
grep -E '^\[.*\] #' ~/.config/PCSX2/logs/emulog.txt      # our kernel's output
```

To exercise the disc path the run needs a disc: point the emulator at a retail
ISO and the last console line becomes the `BOOT2=` path read off it rather than
the "no disc" one. Any 2048-byte-sector ISO with a `SYSTEM.CNF` will do; the
image itself is not part of this repository.

```sh
timeout 120 ~/tools/squashfs-root/AppRun -batch -nogui -slowboot <abs path to the iso>
```

`-slowboot` is what makes the run go through the BIOS: given a disc without
it, PCSX2 fast-boots the disc's ELF itself and `OSDSYS` never runs. The IOP
console is off by default and prints only what the disc's own modules say
(`[Logging] EnableIOPConsole = true`, subject to the same rewrite as the EE
line above). PCSX2 models the drive's spin-up: for about five seconds of
emulated time after a boot the CDVD reports itself busy while it detects the
disc, and a read issued before that is refused -- the reason `OSDSYS` waits
in `sceCdInit` before it opens `SYSTEM.CNF`.

Two things about the harness will otherwise cost an hour each:

- **PCSX2 rewrites its ini on startup.** `[Logging] EnableEEConsole = true` set
  before the first run is silently replaced by `false`. Set it again *after*
  PCSX2 has generated its own file, and re-set it before each run.
- **`-batch` exits when the emulation shuts down**, which booting to a BIOS
  never does. Give the run a timeout rather than waiting on it.

The EE's serial console is the only instrument that reaches inside the running
image. Printing a register from our own kernel has answered in one run what
guessing did not in several — and a **bounded** wait that then reports beats an
unbounded one that hangs, because the value it prints is the diagnosis.
`printHex32` in `src/kernel/entry.cpp` is kept for exactly that and has no
caller of its own.

`docs/analysis/29` is the worked example, and it added a second instrument worth
remembering: **`SMCOM` is a diagnostic channel that needs no working data path.**
Both processors can read it, the EE has latched what the handshake put there by
the time any transfer starts, so the IOP can report a value through it and the
EE can print it — which is the property that matters when the data path is the
thing under suspicion.

## 6. Next steps

**The plan was restated on 2026-08-17**, and this section is the restatement.
What prompted it: the boot reaching `OSDSYS` made §1's target — *an emulator
accepts the image* — true to the letter and empty in substance, since the
program the boot ends in is a stub that prints one line and spins. The list
that followed ordered work by counts (slots served, modules built) that no
consumer was asking for and no gate could finish, and the days after the boot
went to the OSD's configuration bits and a language rewrite — depth with no
program waiting on it. The corrective is the one that carried a smaller project
of the same shape to its end: **the image is done when it boots software**, and
from here work is *pulled* by a program that needs it, not pushed by a table.

### Where it stands, and what comes next (2026-09-09)

M1 is met and M2 holds on both targets: the title boots on PCSX2 and plays
through its third in-game day on PS2e (the narrative below, newest entries
last, is the record of how). Nothing found so far blocks the play from going
on, and the play itself has become an endurance run rather than a source of
new faults, so it is parked and picked up when there is time. The work in
front of the project, in order:

1. **Documentation.** Keep this file and `docs/implementation.md` true to the
   image; the gate's list is the authority on what is missing.
2. **`OSDSYS`, the minimal version.** Done. The program a disc-less boot ends
   in says which build it is (version and commit) on the console *and on the
   screen*: `src/ee/display.cpp` programs the display and puts that one line
   on a 640x448 NTSC raster. The geometry is the reference console's own,
   traced rather than derived (`docs/analysis/53`), and the glyphs come from a
   freely-licensed bitmap font packed out of `third_party/fonts/` at build
   time. **It reads the machine's configuration record too**, through the CDVD
   service's own RPC and the mechacon's S-commands, and puts the language,
   the clock and whether the machine has been set up on the screen beside the
   version. What is still missing for a menu is pad input, which is M3.
3. **The IOP modules whose analysis is complete.** Ordinals that have a spec
   line and no implementation are built on the spec, whether or not a title
   exercises them; the boot before a reboot and `tools/iopsim.py` are their
   gates. After a title's reboot only eight of our modules stay resident (§4),
   so most of this is not on the play path, and that is understood.
4. **The reference's unread corners.** The open questions each analysis
   document ends with, worked when one is pulled by 2 or 3.
5. **The play.** The next leg from the third day's choice box, and the two
   observations that are not divergences but are not explained either (a
   different RPC count in the boot window, one day-map frame).

### The finish line

Two milestones, each with a gate that runs on the target rather than a
simulator, in this order:

- **M1 — an independently built program runs on our image.** A small program
  written here in a few lines of C, but built with the PS2SDK toolchain and
  linked against its runtime (`ps2dev/ps2dev`, run through the container; the
  daemon on the working machine is up), so its start-up is *someone else's*
  idea of what a PS2 kernel provides: the C runtime's syscalls, `SifInitRpc`,
  `SifLoadModule` of a `rom0:` module, a thread and a semaphore, and output the
  harness can read. It stands in for `OSDSYS` in the manifest — the boot already
  places and enters that slot, so no new machinery is needed to run it — and is
  never committed, like the reference images. Gate: PCSX2 prints the program's
  own final line.
- **M2 — a retail title boots from disc.** Our `OSDSYS` reads `SYSTEM.CNF`
  from `cdrom0:` and runs `BOOT2` through the loader; the title reaches its own
  steady state on PCSX2. With a disc attached the boot now reads `BOOT2=` off
  the disc, loads the named ELF and enters it, and the title's own runtime
  comes up far enough to complete `SifInitRpc` over our SIF and start binding
  RPC servers. Those binds are answered now: it calls `sceCdInit`, asks
  `0x80000593` for an `fno` only the later `XCDVDFSV` generation has — and is
  correctly told nothing, as the reference would — and then **reboots the
  IOP**, `cid 0x80000003` with `"rom0:UDNL cdrom0:\MODULES\IOPRP310.IMG;1"`.
  `REBOOT` answers that with the merge `docs/analysis/45` describes (built
  later; see below), and the title goes on. It then binds `FILEIO`'s second RPC service and the
  module loader, and — since 2026-09-02 — **asks the loader for all twelve of
  the modules it ships**, in the reference's own order: `SIO2MAN`, `MSIFRPC`,
  `PADMAN`, `CDVDSTM`, `MCMAN`, `MCSERV`, `LIBSD`, `SDRDRV`, `MODMIDI`,
  `MODHSYN`, `EZMIDI`, `CRI_ADXI`. Getting there took the `LOADFILE` version
  query a title gates every load on (IOP-5f), `thmsgbx` (IOP-3l), the `sifcmd`
  SREG file and its `SendCmd` pair (BOOT-12f/12g), `sysmem`'s `Kprintf` slot,
  a `VBLANK` module (IOP-10) and a `SECRMAN` interface (IOP-11). **Since
  2026-09-03 the last of those loads returns**, and the title goes on to drive
  its drivers: it binds and calls `PADMAN`'s two services, `CDVDFSV`'s
  `sceCdSearchFile`, and the memory-card server, and then starts a traffic of
  its own on command ids the `MSIFRPC` it loaded registers handlers for.

  **Since 2026-09-03 the title's own threads run.** What held them was the
  shape of the EE's ready queues (`spec/05` SYS-10b): the reference leaves a
  running thread linked in its priority's queue, at the head, so a
  rescheduling syscall reselects its caller; this project took the running
  thread out and put it back at the tail, so every such syscall handed the
  processor to an equal-priority peer. The title's sound driver creates a
  worker at the caller's own priority, starts it and *then* lowers it — with
  the queue moving under it, the start never returned, and the worker, whose
  body is a wait to be told to stop, span for ever. The queues now hold the
  running thread. The same run reaches eleven EE threads at seven priorities,
  the drivers' workers among them, and its SIF traffic goes from 159 commands
  to several thousand.

  What stood in the way was the IOP's memory manager, and it is worth
  recording why, because the shape recurs: `sysmem`'s allocation *mode* is a
  contract, not a hint (`spec/02` IRX-15). `MODLOAD` reads each module's raw
  file into a mode-1 block and builds the image in a mode-0 one, so that the
  file is still the topmost block when it releases it a moment later. A
  one-ended allocator refuses that release — and since the caller does not
  read the return, every raw file leaked, twelve of them, roughly half the
  heap. The last module then could not create a thread and hung in its own
  fatal-error trap, so the loader never answered and the EE waited for ever.
  With the two ends served, and the heap widened to the memory above the boot
  image, the same run leaves about 1.2 MiB free.

  The wall after it was the same shape one library along. A title's own
  drivers bind against the `timrman` its `IOPRP` image carries, which has 28
  ordinals where `rom0`'s has 17, and the disc's MIDI driver imports four of
  the ones past the end — so all four bound to `jr $ra`, its timer set-up
  failed on a register left in `$v0`, and it fell back to polling a timer that
  was never programmed. `TIMRMAN` now owns an interrupt handler per timer and
  serves them (IOP-7f). With that, the boot's SIF-bridge phase runs to the end
  and matches the reference command for command, as far as a bind the
  reference makes next and ours does not yet reach.

  **The wall now is the IOP reboot this project does not perform.** The
  title's `sceCdSearchFile` request is 0x12c bytes with the path at `+0x24`
  and the return address at `+0x124`; `rom0`'s `CDVDFSV` reads `+0x20` and
  `+0x120` and nothing else, so the search is asked for an empty path, fails,
  and the title settles into a loop that gets nowhere — its thread table and
  RPC counts are byte-identical at 12e9 and 30e9 cycles while its sound
  driver pushes buffers of silence. The `CDVDFSV` v2.26 in the disc's own
  `IOPRP310.IMG` switches on the request's *size* — `0x12c` and `0x128` take
  the `+0x24` shape, anything else the `+0x20` one — and that is the module
  the title expects to be talking to, because its `UDNL` reboot replaces
  `rom0`'s with it. Matching that rule temporarily, to see what lies behind
  it, gets the title past the search and on to a bind of `sid 0x80000595`,
  `CDVDFSV`'s read table, and the twelve `CDVDMAN` ordinals behind it. So
  this is not a `CDVDFSV` gap to close in the image: it is the merge
  `docs/analysis/45` describes, and the question that section left open —
  whether the merge is needed at all — is now answered yes.

  **The merge is built and the title runs on the merged kernel** (`UDNL`,
  `docs/analysis/51`): a reset that names an image reboots the IOP with the
  disc's own `SYSMEM`, `LOADCORE`, `THREADMAN`, `CDVDFSV` and the rest, and
  the post-reset SIF traffic matches the reference's line for line. What
  stopped the title after that was on the EE, twice, and neither was on the
  bus: a thread created by the main thread read a heap end of 0 (the
  reference's `CreateThread` copies the creator's, `spec/05` SYS-10d), so the
  SDK's `malloc` off the main thread returned null and the title copied a
  file over the exception vector; and `Deci2Call`'s open answered the caller's
  own register where the reference answers `-3` -- because its OSD leaves the
  EE TTY protocol open -- so the title took the socket for real and polled
  for a write-done for ever (`docs/analysis/52`). Both were found by the same
  instrument: the reference BIOS on PS2e with the same disc, the PS2e EE
  debugger on the instruction after the `syscall`, and the answers compared
  per call. The command stream is now identical to the reference's through
  the memory-card polling.

  **The copy loop that then went wrong was the EE kernel's third wall**, and
  a scheduler one: a thread parked by an interrupt and picked again by
  another thread's syscall came back with the syscall exit's `$v0`/`$v1` --
  a result and a byte index (`spec/04` EE-7e2) -- where it needed its own
  two registers, and the SDK's `memcpy` keeps its destination in `$3`. Found
  by bisecting save states over the copy with no debugger attached, since
  the debugger's timing hid it. With that restored, the run went idle
  instead: a wakeup the SIF handler had requested was cleared by an
  interrupt nesting inside it, and nothing but the boot thread was left to
  run. The kernel now keeps the request across nesting.

  **The IOP wall after that was a layout one.** `UDNL` staged the title's
  image right above our resident kernel, which is smaller than the
  reference's, so the merged kernel's later modules were placed *above* the
  reserved images and a 0x44000-byte hole opened below them when the reserve
  was released. A title buffer in that hole, overrun by a 0x11a0-byte RPC
  payload into 0x800 bytes -- harmless on the reference, where the overrun
  lands in free memory -- overwrote `CDVDMAN`'s code with a sound bank, and
  the next `sceCdSearchFile` jumped into it. The buffer now comes from the
  top of RAM, the module map has the reference's shape, and **the disc run
  now issues every RPC the reference issues in 12e9 cycles**, and a few
  more: the title is running ahead of the reference's own pace on the same
  emulator, with no console line, panic or exception in the log.

  **The merge is the committed default, and the title reaches its main
  menu.** `REBOOT` hands every reset to the reboot core now, so the image in
  `build/rom.bin` boots the title as it is; `tests/iopreset`'s named reset
  goes through `UDNL` and comes back through the staged kernel's `EESYNC`.
  With Start held for a while on the title screen the game shows its main
  menu, the same frame the reference shows on the same emulator; the pad is
  read through the disc's own `SIO2MAN` and `PADMAN` on our IOP. Two more
  reference-shaped fixes went in alongside: the EE interrupt frame carries
  `HI`/`LO`/`SA` in the reference's slots, and `EnableIntr`'s bank-1 path
  writes DICR2 again -- on the merged kernel the command stream has the same
  shape with and without it.

  **The game proper is reached on the committed image, and the wall past
  the menu was the DMA bank handler.** Selecting the first menu entry made the
  disc's `CDVDMAN` time out on a read: it disables channel 3's interrupt
  and then kicks a read's last chunk anyway, relying on the next DMA
  interrupt from any channel to deliver the completion -- which the
  reference's bank handler does, because it walks DICR's flag bits alone
  (IOP-2h), and ours did not, because it walked flag-and-enable. The
  emulator's ordering (drive "done" before the last chunk has drained,
  which hardware cannot produce) is what puts the driver on that path; the
  reference survives it, and now so do we. The title's own log, visible
  since PS2e prints `Deci2Call`'s TTY writes, is identical to the
  reference's, and with the same presses -- Start, then Circle at the menu
  -- the two images show the same frame every 10e9 cycles from the main
  menu through the name-entry screen the game opens with, its confirmation
  dialog, the prologue's day screen, and on through the prologue: its first
  choice (which needs a cursor move before Circle), the first voiced lines
  (the disc's `VOICE.AFS`, streamed by its CRI ADX driver over our
  `DMACMAN`, with the same SPU2 output on both), the system menu, and its
  load screen, which reads the memory card through the disc's `MCMAN` --
  and then the game's day loop through its third in-game day, about
  2900e9 cycles of play, identical every 10e9 when the presses are
  short taps (a held Circle advances a variable number of lines, on both
  images alike, so held presses scatter the sampled lines). The disc's `SIO2MAN`
  and `PADMAN` read the pad through our `INTRMAN` and `DMACMAN`, and the
  card probe the load screen makes is the same transfer sequence, count
  for count, on both images. The memory-card and multitap probes only the
  reference issues at boot are its OSD's, not the title's.

  **The title saves.** With a card the reference's OSD had formatted, and
  an emulator fix for how a card announces itself (a card powers up with
  terminator 0x66, which is what a freshly loaded `MCMAN` needs to see
  before it will identify one; PS2e started at 0x55), the title's load
  screen reads the card and its save -- from the school map's menu,
  about 2360e9 cycles in -- writes a slot: the same 11984 page writes and
  182 block erases on both images, and the two card images are
  byte-identical afterwards. The one reference-only traffic left in that
  stream is `SecrAuthCard`'s MagicGate exchange (`0xF3`, `0xF7`, twenty-one
  `0xF0` steps through the mechacon) before the card is identified; our
  `SECRMAN` answers 1 without running it, as the spec allows (IOP-11c), and
  the card behaves the same either way on the emulator.

  **A lost wakeup on the third in-game day, and the play chain rebuilt.**
  Replaying the story from a cold boot on the rebuilt emulator, the title
  stopped in a scene on its third day: the IOP idle with every RPC answered,
  the EE in the kernel's idle loop -- with two threads *ready* in their
  queues and the reschedule request clear. The interrupt exit's check ran
  with interrupts enabled whenever a handler had left them so (the SDK's SIF
  command handler does), and a second interrupt nesting between the check
  and the `eret` woke a thread whose request the next outermost entry then
  cleared. The exit now disables interrupts before it reads the request
  (`spec/05` SYS-12c says the read and the exit are one unit). The stall was
  timing-sensitive -- a replay from a save state a few billion cycles
  earlier did not show it -- which is what a race looks like from outside.
  With the fix the chain plays from the title screen through the third
  day's afternoon, about 2900e9 cycles, to the day's choice box, and the
  reference chain rebuilt the same way on the same emulator shows the same
  scenes at the same 60e9-cycle offset it has had since the boot.

  **The instrument that made both of these findable** is the reference BIOS
  run on the same emulator with the same disc, and its command stream diffed
  against ours. Reasoning forward from our own log found neither. `EELOAD` at the address PCSX2's fast boot hooks is
  done: its `-elf` launch of the M1 program on our image is a gate now
  (`AppRun -batch -nogui -elf <path to m1.elf>` prints M1's last line).

  **And the title boots on the acceptance target.** Every disc observation
  above was made on PS2e; the same image on PCSX2 stopped three times, each
  time on something PCSX2 models exactly and PS2e does not, and each fix is a
  reading of the reference rather than an accommodation. First, PCSX2 models
  the drive's spin-up -- the CDVD reports itself busy for about five seconds
  of emulated time after a boot while it detects the disc -- and `OSDSYS`
  went to `FILEIO` without waiting, so the driver's open ran out of retries
  and a present disc read as none. The retail `OSDSYS` calls `sceCdInit`
  first (`docs/analysis/27` finds its banner in the payload) and that call's
  mode 0 is the wait (`42` §1); ours now makes the same call over `CDVDFSV`'s
  init service. Second, the title's pad never configured: PCSX2 runs the
  SIO2 out channel only when its CHCR is exactly `0x41000200`, and our
  `sceSetSliceDMA` wrote `0x200`; the reference's sets bit 30 for a transfer
  into memory, and now so does ours. Third, after the title's `SetGsCrt`
  no vblank interrupt reached either CPU: PCSX2 raises none while SMODE1's
  SINT bit is set, and our sequence stopped at SRFSH with it set. The
  reference closes the sequence with a second SMODE1 write that clears it
  (`46` §1e, an open question there until now), and so does ours; the gate
  and `spec/05` SYS-14a now say seven writes. With those, a `-slowboot` run
  of the disc on PCSX2 (§5) goes through our `OSDSYS`, the reboot into the
  disc's `IOPRP310.IMG` merged over `rom0`, all twelve of the title's module
  loads, the pad's configuration and the title's own start-up, printing the
  same fifteen lines of the title's DECI2 console that PS2e shows, and runs
  on with no error, panic or unimplemented-syscall report. Headless PCSX2 has
  no pad input and no screenshot, so that console is where the gate stops;
  the play beyond it is PS2e's.

**M3 — an OSD that draws** comes *after* M2, since a BIOS that boots titles is
useful without a menu and a menu without titles is not. Two of its three parts
are now built: the display path and a freely-licensed font, which together put
the version line on screen. What is left is the configuration field map of
`26`–`28`, and what blocks it is a chain rather than a single thing:

1. **`CDVDMAN`'s side is specified and built.** `spec/06` IOP-8i to IOP-8k
   give the S-command register block and its sender, NVM read and write, and
   the configuration session with its 15-byte blocks and sum byte; IOP-8h,
   which used to put all of that out of scope, is narrowed to say so. The
   driver serves ordinals 26, 27 and 31 to 34. Nothing in the boot calls
   them, so `tools/scmdcheck.py` executes them against a modelled controller
   as part of `check` rather than leaving them untried.
2. **`CDVDFSV` has no spec section at all.** `src/iop/cdvdfsv.cpp` ships in
   the archive and nothing in `docs/spec/` states its RPC surface. `27` has
   the mapping the configuration record needs — service `0x80000593`, fnos 14
   to 17 onto `CDVDMAN` ordinals 31 to 34, and fnos 8 and 9 onto 26 and 27 —
   but the request and reply layouts of those fnos are decoded in none of
   `26`, `27` or `28`. That is analysis work, not specification work.
3. **The decoder's field map has no spec home.** `28` has it completely and
   there is no document for the OSD's own behaviour to put it in; `spec/04`
   is the kernel's and `spec/05` is the syscall ABI. Deciding where it goes
   is part of doing it.

Feasibility is not in question: PS2e models the whole S-command side,
including the configuration session and NVRAM persistence, and reads the
image's NVM from `<image>.nvm`. The "configured" flag is no longer part of
the blockage — it is settled (§4).

### How M1 is worked

The order inside M1 is the order the program faults in, first fault first, and
each fault names its own next piece: a syscall slot the program calls, an IOP
module it needs across the SIF, the scheduler when it blocks on a semaphore.
Every piece is still done analysis → spec → implementation, and the analysis is
already there — `spec/05` covers all 125 slots and `spec/02`/`spec/03` all 29
modules — so what remains per piece is implementation and its gate. The
expected shape of the pull, for orientation only:

1. The runtime's start-up syscalls (a bounded set, readable off the SDK's
   `kernel.h` against `spec/05`'s table).
2. SIF RPC on both sides — `SIFMAN`/`SIFCMD` with the RPC layer on the IOP,
   `SifSetDma`/`SifDmaStat`/`SifSetReg`/`SifGetReg` on the EE — which is also
   where interrupt-driven SIF service (`spec/03` BOOT-11, currently a flag
   rendezvous) becomes required rather than deferred.
3. `LOADFILE`, `MODLOAD`, `IOMAN`, `ROMDRV` for `SifLoadModule`, and
   `THREADMAN`/`INTRMAN`/`DMACMAN` underneath them.
4. The EE scheduler (`spec/04` EE-7g, `spec/05` SYS-2a), the first time the
   program blocks.

*Where M1 stands (2026-08-17, end of day):* the program loads whole (the
transfer was the first fault, `implementation.md`), its runtime's
`0x3C`/`0x3D` come back right (SYS-8), `_InitSys` runs through, and on both
emulators the program prints its first stages —

```
# m1: main entered
# m1: argc = 1
# m1: argv[0] = BootBrowser
# m1: CreateSema -> 18
# m1: CreateThread -> 2
# m1: worker thread running
# m1: thread and semaphore ok
```

— which is SYS-8, SYS-9 and SYS-10 exercised by someone else's runtime: step
4 arrived early and is built. (The C library's own start-up would have
stopped in `SifInitRpc` earlier, reading the clock over CDVD's RPC; the test
program opts out of that weak hook so the stages before it can report.)

*Where M1 stands (2026-08-22):* step 2 is built and gated on both targets.
The EE's SIF slots (`spec/05` SYS-13, `src/kernel/sif.cpp`) and the IOP's
command service (`spec/03` BOOT-12, `src/iop/eesync.cpp`) speak the command
layer to the SDK's client, and the first interrupt the image has ever taken —
channel 5's, on the IOP's `SET_SREG` reply — went through SYS-12's entry into
the SDK's handler on PS2e and PCSX2 alike:

```
# m1: SifInitRpc returned
```

Two things the step pulled in that the plan had not listed: the kernel's TLB
(`spec/04` EE-12, `docs/analysis/36` — PCSX2 answers an unmapped uncached read
with zeroes, and the SDK's client reads its packet that way), and a loader
that holds a run of `HI16` (`spec/02` IRX-3a — the compiled service hoisted
its `lui`). The IOP side polls the packet's size byte rather than taking its
DMA interrupt, a deviation `docs/implementation.md` records; the busy-bit
workaround (§7) stays until that interrupt exists.

*Where M1 stands (2026-08-22, later):* the program runs to its last line on
both targets —

```
# m1: SifLoadModule rom0:SIO2MAN -> -203
# m1: done
```

— through the IOP's RPC layer (`spec/03` BOOT-12d/e, `src/iop/eesync.cpp`:
bind, call, the result and the `END` in one run) and the kernel's first
interrupt-driven reschedule: the program's threads all wait for the reply,
the boot thread idles in their place (`spec/05` SYS-10k — read off the
reference by running this same program on it under PCSX2, which also gave
the `-203`), and the `RPC_END`'s interrupt wakes the waiter. The `-203` is
the reference's own answer for a name it has no file for, and it is the
honest answer here too: the archive has no `SIO2MAN`, and the IOP has no
loader to hand a module to.

So M1's gate as written — *PCSX2 prints the program's own final line* — is
met, and met hollowly in the same way the boot reaching `OSDSYS` was: the
fourth stage asked for a module and got an error the program accepts. What
the stage means is a module loaded across the SIF, and that is the pull now:

1. **`MODLOAD`, `IOMAN` and `ROMDRV` on the IOP** (`spec/02`, `spec/03`):
   the loader the boot block already is, as a module exporting
   `LoadStartModule`; a device table with `rom0:` served from the archive;
   and `LOADFILE`'s server calling them instead of answering `-203`. The
   kernel's own file fetch (command `0x10`) becomes `ROMDRV` reads over RPC
   at the same time, which retires the deviation.
2. **A module to load.** `SIO2MAN` is what the test asks for and what a
   title asks for first; its analysis is in `spec/02`/`spec/03`'s module
   tables and is the first of the twenty-six unbuilt modules M1 pulls.

The gate is unchanged in text and changed in meaning: `# m1: SifLoadModule
rom0:SIO2MAN -> <id>` with a non-negative id, on PS2e first and PCSX2 second
(`tools/eesim.py` takes no interrupt, so the simulators cannot judge this).
`tests/m1/main.c` prints a line per stage.

**M1 is met (2026-08-22, end of day).** Both targets print

```
# m1: SifLoadModule rom0:SIO2MAN -> 16
# m1: done
```

with `SIO2MAN` loaded from the archive through `IOMAN`/`ROMDRV`, relocated
and linked by `LOADCORE`, started by `MODLOAD` on `LOADFILE`'s thread, and
parked on its event flag. What it took was the IOP kernel of `spec/06` —
`EXCEPMAN`, `INTRMAN`, `THREADMAN`, `DMACMAN`, `IOMAN`, `MODLOAD`, `ROMDRV`,
`STDIO`, `LOADFILE`, `SIFMAN` and `SIFCMD` — from four analysis
documents (`37`–`40`) written for it, and one toolchain fault found by it
(`mkirx` sized a module's bss short; `docs/implementation.md`). The IOP now
takes interrupts and switches threads from them, so §7's busy-bit row is
retired from "worked around" to "not waited on".

**M2, first step done: `EELOAD`.** The program is now entered the
reference's way — slot `0x06` stages `EELOAD`, `EELOAD` asks `LOADFILE` for
the ELF (IOP-5h) and `ExecPS2` (slot `0x07`) enters it — and PCSX2's hooks
engage: the `jal` at `0x8209C`, the `"rom0:OSDSYS"` literal it overwrites with
`host:<elf>`, and its IOP-side `host:` HLE, which intercepts `ioman` import
stubs and only recognises the reference's encoding of them (`addiu $zero,
$zero, ordinal`, `0x2400xxxx` — spec/02 IRX-8 was corrected; our modules had
used `$v0`). Two faults found on the way and fixed: the IOP scheduler dropped
the thread an interrupt's wake-up preempted, so the IOP died after its first
RPC (invisible under M1, which needed nothing more); and `LOADFILE` sent
segments to unaligned addresses, which the simulator's SIF accepts and an
emulator's DMA does not. A third, found at the same time, is not a fault:
PS2e's console breaks a line at the SDK's `\r\n`, so `argv[0]` prints on the
line after its label there.

**Next: M2, the rest.** In the order the boot meets them: `OSDSYS` reading
`SYSTEM.CNF` needs `CDVDMAN`/`CDVDFSV` (the disc over the SIF) and `FILEIO`
(`IOMAN` over the SIF), and running `BOOT2` needs the EE kernel's
`LoadExecPS2` over the `EELOAD` path that now exists; a title's own start-up
will pull `PADMAN`, `MCMAN` and whatever else it loads with `SifLoadModule`,
which works. `TIMRMAN` and the alarms behind `DelayThread` (IOP-3j, IOP-3k,
IOP-7) are built, since drivers' retry loops use them.

The gate keeps printing the slot and module counts, but they are no longer the
ordering. New analysis documents are written only for what M1 or M2 faults on
— the analysis phase is complete and stays that way.

### Instruments

- **PCSX2** remains the acceptance target; §5 is its harness.
- **PS2e**, the same author's emulator (the sibling project §4 draws its leads
  from; not part of this repository), becomes the *debugging* target. It exposes a gdb-remote server per CPU
  (`--debug-ee <port>`, `--debug-iop <port>`, `--wait-debugger` to hold at the
  reset vector), with breakpoints, write watchpoints that also see DMA, single
  step and memory peek — which replaces print-and-rerun on PCSX2 as the way to
  find out what a fault is. It runs the retail image through the boot to a
  drawing `OSDSYS`, so it is mature enough for the boot path; it is also under
  construction, so where it and PCSX2 disagree the disagreement is a question
  to settle, not a verdict.
- **The simulators** are regression gates and stay so. They are extended only
  when a target has proved a gate wrong — the rule `29` established — and not
  because the next observation looks cheap.

### Carried, dropped, deferred

- The busy bit (§7, first row) is no longer worked around: the IOP takes the
  channel's interrupt instead. Why the bit never clears on PCSX2 is still not
  understood, and nothing waits for it to be.
- The lead recorded from PS2e in §4 — block 1 byte +2 bit 7 as the OSD's
  "configured" flag — is **closed**. Sweeping all 240 input bits of both
  blocks through the decoder under `eesim` and watching its *return* rather
  than its output struct: that one bit is the only input that moves the
  return, it moves no struct bit, and the return is the bit inverted. The
  OSD's own branch on it is in `28`. Clear means not configured.
- *Deferred:* the remaining unnamed OSD configuration fields (M3), the nine
  inferred returns of `spec/05` SYS-1c (settled as M1 reaches each slot).
- *Dropped as an ordering:* filling syscall slots by band, building modules by
  boot-list position.
- *Dropped:* `SecrAuthCard`'s MagicGate exchange. It is the one thing the
  reference's card traffic has that ours does not, and it stays that way:
  the clean-room policy keeps the mechanism out of scope, the card
  identifies and saves the same with the stub (IOP-11c), and the emulator
  answers the exchange's mechacon side with zeroed shapes, so a rebuilt
  ladder would be a protocol played against stubs with nothing observable
  to gain. Revisit only if a title refuses a card on the stub's answer.

## 7. Known problems, in one place

| Problem | Where it is written up | State |
| --- | --- | --- |
| The IOP's DMA busy bit never clears on PCSX2 | `spec/03` BOOT-11h, `docs/analysis/25` | no longer waited on anywhere: the IOP takes the channel's interrupt instead; cause still unknown |
| Some syscall slots report themselves rather than working | the gate's "not required of the image yet" list, `docs/implementation.md` | deliberate — the count is the gate's, not this table's |
| Not every boot-list module exists, and only `SIO2MAN` is loadable on request | the gate's list, `docs/implementation.md` | deliberate — `SSBUSC`, `EECONF` and `SIFINIT` are the three the list still lacks, and two more of the reference's twenty-nine names are the rejected halves of its `INTRMANP`/`INTRMANI` and `TIMEMANP`/`TIMEMANI` pairs, which are one module each here; the title brings its own drivers |
| Slot `0x60` zeroes `Config` | `spec/05` SYS-4a | **not a problem** — the reference's own defect, reproduced on purpose |
| The clean-room role separation is not enforced | `docs/clean-room-policy.md` | recorded, not fixed |

The distinction that matters when picking this up: everything marked
*deliberate* is depth the build has not reached yet and is listed by the gate
on every successful run. The first row is the one that is not understood,
and it has stopped mattering: nothing waits on that bit since the IOP's
drivers became interrupt-driven, so it is recorded rather than worked around.
