# PS2BiosRebuild

A reimplementation of the PlayStation 2 BIOS, written from a specification
derived by analysing retail ROM images. The target is a 4 MiB image that an
emulator accepts in place of a retail BIOS.

Status: **analysis phase, just begun.** The ROM's file-archive structure
(ROMDIR/EXTINFO/ROMVER) is parsed and documented, and the extraction tooling
works against both reference images. Nothing is implemented yet; there is no
build system. [`docs/project-state.md`](docs/project-state.md) is the working
document and records exactly where things stand.

This project follows the method of its sibling PS1 BIOS reimplementation:
observations from the reference images go into `docs/analysis/` with the
command that reproduces each claim, behavioural specifications distilled from
them will go into `docs/spec/`, and the implementation is written from the
specifications. [`docs/clean-room-policy.md`](docs/clean-room-policy.md) states
the rules — and their current limits — honestly.

## Requirements

| Tool | Version used |
| --- | --- |
| Python | 3.11+ |

That is all, for now: the project is analysis tooling and documentation until
the first implementation milestone, which will bring a cross-compiling
toolchain the way the PS1 project's did.

## Tooling

Reference images go in `assets/` (gitignored — see
[`docs/clean-room-policy.md`](docs/clean-room-policy.md)). Extracted contents
must also stay outside the repository.

| Tool | Purpose |
| --- | --- |
| `tools/romdir.py` | Parse the ROM's ROMDIR file table: list, extract, decode ROMVER/EXTINFO, diff two images. |

```sh
# what the archive holds, with computed offsets
python3 tools/romdir.py assets/SCPH-50000.bin --list

# identify an image
python3 tools/romdir.py assets/SCPH-50000.bin --romver

# per-file metadata
python3 tools/romdir.py assets/SCPH-50000.bin --extinfo SYSMEM

# what changed between two models
python3 tools/romdir.py assets/SCPH-50000.bin --compare assets/SCPH-70000.bin
```

## Layout

```
assets/                 reference ROM images (not committed)
docs/project-state.md   start here: status, method, next steps
docs/analysis/          observations from the reference images, with repro commands
docs/clean-room-policy.md
tools/                  host-side analysis tooling
```

`docs/spec/`, `src/` and the build system arrive with the first implementation
milestone.

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
- [`docs/clean-room-policy.md`](docs/clean-room-policy.md) — what is and is
  not enforced, and why the "clean room" label is not yet accurate.

## License

[Zero-Clause BSD](LICENSE) (0BSD) — public-domain-equivalent: use, copy,
modify and distribute for any purpose, with no attribution requirement and no
warranty.
