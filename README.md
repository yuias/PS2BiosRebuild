# PS2BiosRebuild

A reimplementation of the PlayStation 2 BIOS, written from a specification
derived by analysing retail ROM images. The target is a 4 MiB image that an
emulator accepts in place of a retail BIOS.

Status: **analysis complete; implementation started.** The image builds, its
EE boots into our own kernel, and the IOP path reaches its handoff — see
[`docs/implementation.md`](docs/implementation.md). Both CPUs are
surveyed — the ROM archive format, the boot block, all twenty-nine modules of
the IOP boot list, and the EE kernel down to the arguments of each of its 125
syscalls — and five specifications are written and mechanically gated: an
archive built from `docs/spec/01-rom-archive.md` reproduces both reference
images byte for byte, every IRX module passes `tools/irxinfo.py --check`,
`tools/iopsim.py --check` boots the IOP side and judges the result, and
`tools/eesim.py --check` boots the EE side on a simulated R5900, reaching the
kernel and calling its syscalls. Run together by `tools/ps2sim.py`, the two
sides complete their handshake and the IOP boot finishes. The image's actual
contents are not started.
[`docs/project-state.md`](docs/project-state.md) is the working document and
records exactly where things stand.

This project follows the method of its sibling PS1 BIOS reimplementation:
observations from the reference images go into `docs/analysis/` with the
command that reproduces each claim, behavioural specifications distilled from
them go into `docs/spec/`, and the implementation is written from the
specifications. [`docs/clean-room-policy.md`](docs/clean-room-policy.md) states
the rules — and their current limits — honestly.

## Requirements

| Tool | Version used |
| --- | --- |
| Python | 3.11+ |
| CMake | 3.28+ |
| Ninja | any |
| LLVM (clang, ld.lld, llvm-objcopy) | 22 |

No cross-gcc is needed: clang assembles MIPS for both processors.

```sh
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mipsel-ps2.cmake
ninja -C build          # build/rom.bin
ninja -C build check    # judge it against docs/spec/
```

## Tooling

Reference images go in `assets/` (gitignored — see
[`docs/clean-room-policy.md`](docs/clean-room-policy.md)). Extracted contents
must also stay outside the repository.

| Tool | Purpose |
| --- | --- |
| `tools/romdir.py` | Parse the ROM's ROMDIR file table: list, extract, decode ROMVER/EXTINFO, diff two images. |
| `tools/mkromdir.py` | Build a ROM archive from a manifest, per `docs/spec/01-rom-archive.md`. |
| `tools/irxinfo.py` | Inspect an extracted IOP module: `.iopmod` metadata, library tables, export ordinals. |
| `tools/romdis.py` | Disassemble a raw image or module, for either CPU, at a chosen address. |
| `tools/iopsim.py` | Boot an image's IOP side on a simulated R3000 and judge it against `docs/spec/`. |
| `tools/eeksys.py` | Report or check the EE kernel's exception and syscall tables. |
| `tools/eeabi.py` | Infer each EE syscall's arguments and return value, and check them against `docs/spec/05-ee-syscall-abi.md`. |
| `tools/eesim.py` | Boot an image's EE side on a simulated R5900, call its syscalls, and judge both against `docs/spec/`. |
| `tools/ps2sim.py` | Boot both CPUs together across a modelled SIF, so each stops waiting for the other. |
| `tools/mkromver.py` | Write the archive's ROMVER file. |
| `tools/mkirx.py` | Turn a linked ELF into an IOP module, per `docs/spec/02-module-abi.md`. |
| `tools/imgcheck.py` | Judge our own built image at the depth the build has reached. |

```sh
# what the archive holds, with computed offsets
python3 tools/romdir.py assets/SCPH-50000.bin --list

# identify an image
python3 tools/romdir.py assets/SCPH-50000.bin --romver

# per-file metadata
python3 tools/romdir.py assets/SCPH-50000.bin --extinfo SYSMEM

# what changed between two models
python3 tools/romdir.py assets/SCPH-50000.bin --compare assets/SCPH-70000.bin

# build an archive from a manifest
python3 tools/mkromdir.py manifest.txt -o out.bin --size 0x400000 --list
```

Modules are analysed after extracting them (to a directory outside the
repository), then inspected and disassembled:

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>
python3 tools/irxinfo.py <outdir>/SYSMEM --exports
python3 tools/irxinfo.py <outdir>/SYSMEM --dump-load <outdir>/SYSMEM.text
python3 tools/romdis.py <outdir>/SYSMEM.text --cpu iop --vma 0 --range 0x60 0xd8
```

The IOP side of an image can be booted and judged without any emulator:

```sh
python3 tools/iopsim.py assets/SCPH-50000.bin           # report the boot
python3 tools/iopsim.py assets/SCPH-50000.bin --check   # judge it, exit 1 on failure
```

The EE kernel is read out of the archive and judged against its two
specifications:

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>
python3 tools/eeksys.py <outdir>/KERNEL --check   # tables and vectors, spec/04
python3 tools/eeabi.py  <outdir>/KERNEL --check   # syscall signatures, spec/05
python3 tools/eeabi.py  <outdir>/KERNEL --slot 0x18   # one slot, in detail
```

The EE side can also be booted, which needs no extraction — it reaches the
kernel, prints what the kernel prints, and can call individual syscalls:

```sh
python3 tools/eesim.py assets/SCPH-50000.bin            # report the boot
python3 tools/eesim.py assets/SCPH-50000.bin --check    # judge it, exit 1 on failure
python3 tools/eesim.py assets/SCPH-50000.bin --syscall 0x14 3
```

Run against each other, the two simulators unblock one another: the IOP boot
runs to completion, tears down its last one-shot module, and data moves between
the two processors over the SIF.

```sh
python3 tools/ps2sim.py assets/SCPH-50000.bin            # report the joint boot
python3 tools/ps2sim.py assets/SCPH-50000.bin --check    # judge it
python3 tools/ps2sim.py assets/SCPH-50000.bin --traffic  # the SIF conversation
```

## Layout

```
assets/                 reference ROM images (not committed)
docs/project-state.md   start here: status, method, next steps
docs/analysis/          observations from the reference images, with repro commands
docs/spec/              behavioural specifications - the input to the build
docs/clean-room-policy.md
tools/                  host-side analysis tooling
```

```
src/boot/               the boot block and the memory-controller bring-up
src/kernel/             the EE kernel
src/link/               one link script per component
src/rom/                the archive manifest and its EXTINFO sources
cmake/                  the LLVM toolchain file
```

## Target notes

The PS2 is a two-CPU machine and its BIOS serves both:

- The **EE** (Emotion Engine, MIPS R5900) runs the kernel (`KERNEL` in the
  archive) and the OSD.
- The **IOP** (I/O processor, a PS1-class R3000 derivative) runs a modular
  kernel: ~90 relocatable ELF modules stored as individual files in the ROM,
  loaded in the order a text file (`IOPBTCONF`) dictates.

So unlike the PS1 ROM — one monolithic kernel — this ROM is a file archive,
and the reimplementation is correspondingly per-file:
[`docs/analysis/01-rom-layout.md`](docs/analysis/01-rom-layout.md) documents
the archive format itself.

## Documentation index

- [`docs/project-state.md`](docs/project-state.md) — where the work stands,
  how it is done, what is next. The handover document, kept current.
- `docs/analysis/NN-*.md` — observations from the reference images, every
  claim naming the command that reproduces it.
- `docs/spec/NN-*.md` — behavioural specifications with numbered requirement
  IDs (`ARC-1`, `ARC-6b`, …), the input to the implementation.
- [`docs/implementation.md`](docs/implementation.md) — how the image is built,
  what it does so far, and every deviation from the reference with its reason.
- [`docs/clean-room-policy.md`](docs/clean-room-policy.md) — what is and is
  not enforced, and why the "clean room" label is not yet accurate.

## License

[Zero-Clause BSD](LICENSE) (0BSD) — public-domain-equivalent: use, copy,
modify and distribute for any purpose, with no attribution requirement and no
warranty.
