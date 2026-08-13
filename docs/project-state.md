# Project State and Working Method

Read this first when picking the project up. It records where the work stands,
how the work is done, and what is next. `README.md` is the front door; this is
the working document and is kept current.

> **Where the work is right now:** the analysis is complete on both CPUs and
> **the image boots end to end on the simulators**. Its EE comes up on our own
> kernel, meets the IOP across the SIF, fetches `rom0:OSDSYS` over the bus in
> the reference's own packet framing and runs it — `spec/03` BOOT-1 to BOOT-11
> and `spec/04` EE-1 to EE-9, on our own code rather than the reference's. What
> is thin is *depth*: three of the boot list's twenty-nine modules exist,
> twenty of the 125 syscall slots are served, there is no scheduler, and
> neither processor takes an interrupt.
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
of a retail PS2 BIOS: analysis of retail ROMs → behavioural specifications →
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
| SIF0 on PCSX2, and why it delivered nothing | analysed — `docs/analysis/29-sif0-on-pcsx2.md` |
| PCSX2 as a target | **boots there end to end** — accepted as a BIOS, both CPUs, both directions of the bus, `rom0:OSDSYS` entered |

### What is built

`docs/implementation.md` covers all of this, including every deviation from the
reference and the reason for it.

| Area | State |
| --- | --- |
| Build system | **CMake + Ninja + LLVM** — no cross-gcc needed; C++26, assembly only where the machine requires it |
| Boot block (`RESET`), both paths | **built** — EE reaches our kernel; IOP emits BOOT-5a's full POST sequence |
| `IOPBOOT` + `IOPBTCONF` | **built** — parses the boot list and loads modules from it |
| IRX producer + loader | **built** — `tools/mkirx.py`; all three modules pass `irxinfo --check` |
| Binding + registration (IRX-9, IRX-10) | **built** — `LOADCORE` calls `SYSMEM` across a bound stub |
| EE handshake (BOOT-10) | **built** — `EESYNC` and the kernel's `sif.S` release each other |
| SIF data path | **built and gated** — BOOT-11's framing both ways and BOOT-11k's addressing; the EE fetches an archive file |
| Boot tail (EE-9) | **built** — `rom0:OSDSYS` crosses the SIF, is placed and runs |
| `RDRAM`, `ROMVER` | **built** — minimal, spec-derived |
| EE kernel: vector page, dispatch, syscall table | **built** — 20 slots served, the rest report themselves |
| EE syscall entry: 128-bit context (EE-7e) | **built and gated** — `imgcheck` plants a marker in every register |
| EE cache and CP0 band (EE-8e, EE-6f, SYS-4) | **built and gated** — the three KSEG1 slots and the CP0 reader, called and read back |
| Source language | **C++26 where the machine allows it** — assembly only for the reset path, entry stubs, the vector page, the syscall context save, `IOPBOOT` and the IOP modules, each for a reason `docs/implementation.md` states |
| Everything else in the image | not started — `docs/implementation.md` lists what and why |

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
the DMAC cares about. Making `tools/ps2sim.py` model that bit would turn this
into a gate, and is the first item in §6.

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
list module 24 and owed to next step 4 regardless.

## 5. Resuming

```sh
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mipsel-ps2.cmake
ninja -C build && ninja -C build check      # build the image and judge it
```

Configuring without a build type gives `MinSizeRel`; `-DCMAKE_BUILD_TYPE=Debug`
builds the same sources unoptimised and both boot.

**What a good run looks like.** Our own image produces exactly these four lines
on PCSX2. Anything shorter is a regression, and the line it stops at says where:

```
# PS2BiosRebuild EE kernel: entered at 0x80001000.
# The IOP answered; the SIF handshake is complete.
# ROMVER, fetched from the archive across the SIF: 0100XP20260810
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

**Nothing on the boot path is outstanding.** `29` closed the blocker,
`spec/03` BOOT-11k gates it, and the image runs from reset to `OSDSYS` on the
target. Everything below is depth, in the order that buys the most for the
least.

> A note on where effort goes. The simulators are instruments, not the product,
> and it is easy to keep sharpening them: each one buys a real observation, so
> each next one looks worth it. It stops being worth it where PCSX2 would
> answer the same question. `29` is the counter-example that says when it *is*
> worth it: a gate the target has already proved wrong is worth building, and a
> gate written before the target has an opinion is worth less than it looks.

1. **Fill in the syscall slots** (`spec/05` SYS-1) — the best ratio on the list.
   105 of the 125 still resolve to the reporter, and `ninja -C build check`
   counts that off the image's own table rather than from a number kept by hand.
   The table, the entry and the 128-bit context save are all done, so each slot
   is an isolated piece of work with a gate already watching it;
   `tools/eeksys.py --check` on our `KERNEL` fails on that count alone. The band
   `0x64`–`0x6A` is the natural next group: `0x65`'s algorithm is already
   specified (SYS-4d) and `0x64`/`0x66` are the two of the band still unread.

2. **The scheduler** (`spec/04` EE-7g; `spec/05` SYS-2a). The context save it
   needs is done — EE-7e's 128-bit save and restore, through a block a handler
   can rewrite — so what is left is the part that chooses: `EESYNC` still never
   returns from its entry because there is no thread to put a service on.
   Threads on the IOP (`THREADMAN`) and the EE's scheduling group are the same
   problem twice.
3. **More of the boot list.** `SYSMEM`, `LOADCORE` and `EESYNC` exist; the other
   twenty-six do not. `HEAPLIB` is the natural next one, since `SYSMEM`'s bump
   allocator cannot free out of order and everything above it wants a real
   heap.
4. **`IOPBOOT` in C++.** The last large piece of assembly that is assembly for a
   reason that could be removed: it runs from wherever the archive puts it in
   the ROM window, so every call in it is `bal` and a compiler cannot be used.
   Linking it at its final archive address instead would let it be compiled, and
   the address is a fixed point — code size does not depend on it, so a build,
   a measurement and a relink converge. It costs a two-pass build and a recorded
   deviation from BOOT-7's "runs in place".
5. **The rest of the OSD's configuration fields.** `28` named the language
   (bits 4–8, gated), the timezone (bits 9–19, minutes), its hour flag (bit 29),
   the clock format (bit 30) and bit 3 as an argument to EE syscall `0x4F`, and
   mapped every field's position by measurement. Bit 0, bits 1–2, bits 20–28 and
   the second word are placed but unnamed. The method is cheap now — sweep the
   decoder under `eesim`, then follow one getter's callers — so this is bounded
   work rather than an open question.
6. **One loose end in the analysis.** Nine EE slots have an inferred rather than
   observed return (`spec/05` SYS-1c). The other loose end carried here — the
   EE's unmodelled chain-mode DMA — is closed by `24`.

## 7. Known problems, in one place

| Problem | Where it is written up | State |
| --- | --- | --- |
| The IOP's DMA busy bit never clears on PCSX2, so it cannot be waited on | `spec/03` BOOT-11h, `docs/analysis/25` | worked around; cause unknown |
| Neither processor takes an interrupt in our image; both ends of the SIF rendezvous on the flag registers | `docs/implementation.md`, printed by `ninja -C build check` | deliberate, and the reference does not work this way |
| No scheduler, so `EESYNC` never returns from its entry | §6 step 2 | deliberate |
| 105 of 125 syscall slots report themselves rather than working | §6 step 1 | deliberate |
| Three of twenty-nine boot-list modules exist | §6 step 3 | deliberate |
| Slot `0x60` zeroes `Config` | `spec/05` SYS-4a | **not a problem** — the reference's own defect, reproduced on purpose |
| The clean-room role separation is not enforced | `docs/clean-room-policy.md` | recorded, not fixed |

The distinction that matters when picking this up: everything marked
*deliberate* is depth the build has not reached yet and is listed by the gate
on every successful run. **The first row is the only one that is not** — the
IOP's busy bit is a thing that does not work and is not understood. It is
worked around rather than waited on, and the boot completes anyway, which is
why it sits below the deliberate items rather than above them.
