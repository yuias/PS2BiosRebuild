# What the console's own OSD puts on the raster

`docs/spec/04` EE-12 maps the GS privileged registers and `docs/spec/05`
SYS-14a says what slot `0x02` programs, but neither says what the *display*
side of the GS is set to: which read circuit is on, where in GS memory it
reads, and how that memory maps onto a 448-line raster. Those are raster
geometry -- numbers the hardware dictates -- and they were missing, so the
first drawing OSD had nothing to derive them from.

This reads them off the reference console while it draws its own browser.

```sh
ps2e --bios assets/SCPH-50000.bin --cycles 3e9 --log 'warn,ps2_core::gs=trace'
```

The emulator logs every write below `0x12001000` with its offset. Filtering
for the display registers, and ignoring the repeats a redraw makes, the
reference settles on:

| Register | Offset | Value |
| --- | --- | --- |
| `PMODE` | `0x0000` | `0x66` |
| `SMODE2` | `0x0020` | `0x03` |
| `DISPFB2` | `0x0090` | `0x1400`, alternating with `0x1446` |
| `DISPLAY2` | `0x00A0` | `0x001BF9FF0183227C` |

## 1. One read circuit, and it is the second

`PMODE` `0x66` is `EN1=0`, `EN2=1`, `CRTMD=1`, `MMOD=1`, `AMOD=1`, `SLBG=0`,
`ALP=0`. Only read circuit 2 is enabled, so `DISPFB1` and `DISPLAY1` are never
written and stay zero -- which the emulator's end-of-run summary confirms
(`dispfb1=0x0`). The three blend fields describe how circuit 1 would mix into
circuit 2 and have nothing to act on with `EN1` clear.

## 2. The framebuffer is half the height of the picture

`DISPFB2` decodes as `FBP=70`, `FBW=10`, `PSM=0` (`PSMCT32`), `DBX=DBY=0`.
`FBW` counts 64-pixel units, so the buffer is 640 pixels wide. `FBP` counts
8 KiB units, so the second buffer starts 573,440 bytes in -- which is
`640 x 224 x 4`. **The framebuffer is 640x224.** The two values alternate
because the reference double-buffers: `0x1400` is `FBP=0` and `0x1446` is
`FBP=70`, the two halves of the same arithmetic.

`DISPLAY2` decodes as `DX=636`, `DY=50`, `MAGH=3`, `MAGV=0`, `DW=2559`,
`DH=447`. `DW` and `DH` are the displayed size minus one, so the picture is
2560 units wide by **448 lines**, and `MAGH=3` spreads each pixel over four of
those units: 640 pixels across. `MAGV=0` means no vertical magnification.

224 lines of buffer, 448 lines of picture, and no vertical magnification. The
missing factor is in `SMODE2`.

## 3. SMODE2's FFMD is what doubles the lines, and slot 0x02 carries it

`SMODE2` `0x03` is `INT=1` and `FFMD=1`. `FFMD` set makes both fields of the
interlaced raster read the *same* buffer lines, so each of the 224 lines is
shown on both fields and fills two of the 448.

`SYS-14a` already had slot `0x02` writing `SMODE2` as `(field & 1) << 1 | 1`
on the interlaced path, so it already named `FFMD` as the call's `field`
argument. What it did not say is what that bit does to the picture, which is
the sentence above -- and therefore which value a caller wants. The
reference's OSD calls it with `field = 1`, not 0.

The consequence is worth stating because it is not obvious from the register
names: a 1-pixel horizontal stroke in a 448-line buffer is drawn by one field
only and blinks at 30 Hz, and the usual answer is to draw everything twice as
tall. `FFMD=1` removes the flicker instead, with a buffer of half the height
and no doubling at all.

## 4. What this does not say

Nothing here reads the reference's *drawing* side beyond one register. The
same run's end-of-run summary reports `frame0=0xa0000`, which is `FBP=0`,
`FBW=10`, `PSMCT32` -- it rasterises into the buffer at 0 while displaying the
one at 70, the other half of the double buffer above. What it puts there is
out of scope here: `XYOFFSET`, `SCISSOR`, the primitives, and how its font
reaches GS memory all need a trace of the GIF rather than of the registers.

Nor does this say where the mode (NTSC against PAL) comes from. That lives in
the OSD configuration record, whose "configured" flag is still unverified
(`docs/project-state.md` §4).
