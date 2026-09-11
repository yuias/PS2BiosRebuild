# M+ BITMAP FONTS

The OSD's glyphs come from this font, not from the reference ROM's `FONTM`,
`FNTIMAGE`, `KROM` or `KROMG`, which `docs/clean-room-policy.md` item 3 puts
out of bounds. That rule asks for provenance under `licenses/`; this is it.

| | |
| --- | --- |
| Distribution | M+ BITMAP FONTS 2.2.4 |
| Upstream | http://mplus-fonts.sourceforge.jp/mplus-bitmap-fonts/index.html |
| Copyright | 2002-2005 COZ \<coz@users.sourceforge.jp\> |
| Licence | [`LICENSE`](LICENSE), copied verbatim from the distribution's `LICENSE_E` |
| Face taken | `fonts_e/mplus_f12r.bdf` -> `third_party/fonts/mplus_f12r.bdf` |
| SHA-256 of that file | `d5934b9599ed5c8d65cbd9e93c2f3b944686d0b99be1ba8dc5cb04c6a948d877` |

Nothing else from the distribution is in the tree. The file is unmodified, so
the hash above is checkable against a fresh download.

## Why this face

The licence grants unlimited permission to use, copy and distribute, modified
or not, commercially or not, with no attribution requirement and no warranty.
Keeping the notice is the whole obligation.

Of the six Latin faces, only `mplus_f10r` and `mplus_f12r` are monospaced --
the XLFD spacing field is `C` for those two and `P` for `h10r`, `h12r`, `s10r`
and `q06r`. A proportional face would need per-glyph advance widths and a
layout pass; a fixed one needs neither.

The two monospaced faces have the identical cell: all 224 glyphs in both files
declare `DWIDTH 6` and `BBX 6 13`, so the box is 6x13 either way and the
"10 versus 12 pixel" label is not a size choice. What differs is ink weight
inside the box -- a capital `A` is 7 rows tall in `f10r` and 9 in `f12r`.
`f12r` was taken for the heavier stroke, which survives a television better.

Only the ASCII range is used: every string the OSD draws is the project's own
and is written in ASCII, so the distribution's Japanese faces under `fonts_j/`
are not needed.

## How it reaches the image

`tools/makefont.py` reads the BDF at build time and writes a C header of
packed glyph rows for the printable ASCII range. The header is generated, not
committed; the BDF is committed because it is the input a reader needs to
reproduce it.
