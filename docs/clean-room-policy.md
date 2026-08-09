# Clean-Room Policy

This project reimplements the PlayStation 2 BIOS so that the result can be used
in place of a retail ROM by emulators. The reference images in `assets/` are
read during analysis. This document states the rules the project follows and,
just as importantly, where the current process falls short of a textbook clean
room.

## What "clean room" means here

A textbook clean-room process separates two roles:

- **Analysts** examine the original artefact and write a specification that
  describes *behaviour and interfaces* only.
- **Implementers** never see the original artefact. They work solely from the
  specification.

The separation is what makes the output defensible: the implementers cannot have
copied expression they never saw.

## Where this project actually stands

**The separation is not currently enforced.** The same contributor (human or
agent) reads the reference disassembly and writes the implementation. That means
the process is *specification-driven reverse engineering*, not a clean room, and
it should not be described as one in any release material until the roles are
genuinely split.

This is a known limitation, recorded here deliberately rather than glossed over.
Anyone intending to rely on the clean-room property should split the roles first.

## Rules that *are* enforced

1. **Reference images are never committed.** `assets/*` is gitignored. The
   images are inputs to analysis on a contributor's machine only. The same goes
   for anything extracted from them: `tools/romdir.py --extract` output must
   stay outside the repository.
2. **No verbatim transfer of original code.** Implementation files are written
   from the specifications in `docs/spec/`, not transcribed from disassembly.
   Where output happens to be instruction-identical to the original — hardware
   init sequences whose values and ordering the hardware dictates — that is a
   fact about the machine, not expression.
3. **No original strings, artwork, or branding.** Vendor names, copyright
   notices, logos and version banners are not reproduced; our own text goes in
   their place, matching placement only where something reads a fixed offset.
   The PS2 ROM raises the stakes here: it carries far more non-code material
   than the PS1 ROM did, all of it out of bounds for reproduction —
   - the boot logos and splash animation (`LOGO`, `PS2LOGO`),
   - the OSD's fonts (`FONTM`, `FNTIMAGE`, `KROM`, `KROMG`),
   - its textures, icons and sounds (`TEXIMAGE`, `ICOIMAGE`, `SNDIMAGE`,
     `OSDSND`, `IGREETING`).
   Where an interface requires equivalent material (a font a published function
   returns glyphs from, for instance), it is built from freely-licensed sources,
   with provenance recorded under `licenses/`.
4. **Specifications state behaviour and rationale, not code.** `docs/spec/`
   describes what must happen and why the hardware requires it.
   `docs/analysis/` holds the observations that produced those requirements and
   is explicitly the analyst-side artefact.
5. **Analysis is reproducible.** Every claim in `docs/analysis/` names the
   command that produces it, so a reviewer can check it without trusting the
   write-up.

## Trademarks and compatibility

"PlayStation" and "PlayStation 2" are trademarks of Sony Interactive
Entertainment. This project is unaffiliated. The goal is interoperability: a
functional replacement that lets software written for the platform run without
a retail ROM.

Protection mechanisms that exist to restrict what retail hardware will run —
disc authentication as an anti-copy measure, the encryption wrapping of
security modules — are implemented only to the extent an emulator's boot path
requires their *interfaces* to exist; defeating or reproducing them for use on
retail hardware is out of scope.

## Prior art

The PS2 has a large body of open reverse-engineering work: community hardware
documentation (ps2tek and its relatives), emulator source (PCSX2), and — the
significant one — the homebrew SDK (ps2sdk), which contains open
reimplementations of many of the very IOP modules this ROM ships. Community
*documentation* is fair to cite where it corroborates an observation. Reading
another implementation's *source* is a separate question from reading the
original ROM; contributors intending to preserve the clean-room property should
treat both as restricted.
