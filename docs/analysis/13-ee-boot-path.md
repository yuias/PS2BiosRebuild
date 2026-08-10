# The EE Boot Path

`docs/analysis/02-boot-block.md` split the reset vector between the two CPUs and
followed the IOP. This follows the other branch: `0xBFC00800`, the EE's entry,
through to the point where it hands control to the `KERNEL` file. It also
corrects a guess `02` made about which file that is.

Disassembly uses `--cpu ee`, whose limits matter here and are demonstrated
below:

```sh
python3 tools/romdis.py assets/SCPH-50000.bin --cpu ee --range 0xbfc00800 0xbfc008f0
```

## The path in outline

| Step | What |
| --- | --- |
| `0xBFC00800` | R5900 `Config` = `0x00073003`, `Status` = `0x70400000` |
| | one TLB entry written, mapping the scratchpad |
| `0xBFC0087C` | stack set to `0x70003FF0` — in the scratchpad, before RAM is usable |
| `0xBFC00884` | `jalr` to `0x9FC41000` — the **`RDRAM`** archive file |
| `0xBFC008BC` | `jalr` to `0x9FC00BF0` — find and load `KERNEL` |
| `0xBFC008CC` | `jal 0xBFC008EC` — invalidate the instruction and data caches |
| `0xBFC008E4` | `jr` to `0x80001000` — enter the kernel |

Two details of that sequence are worth stating plainly.

**The first call target is a file in the archive.** `0x9FC41000` is ROM offset
`0x41000`, and `tools/romdir.py --list` puts `RDRAM` there — the memory
initialisation the EE must run before main RAM answers at all. The EE reaches it
by hard-coded address rather than by name, so `RDRAM`'s position in the archive
is fixed in a way no other file's is.

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --list | grep -E 'RDRAM|^name'
```

**Both calls are made through KSEG0.** At `0xBFC008A8` the target
`0xBFC00BF0` is masked with `0x9FFFFFFF`, turning the uncached KSEG1 address
into its cached KSEG0 alias `0x9FC00BF0`. The reset vector itself runs uncached;
the routines it calls are run cached, which matters because the next step copies
a hundred kilobytes.

## The fifth ROMDIR scan

The routine at `0xBFC00BF0` locates the archive itself, using the same
self-locating scan the IOP side uses — the constants are identical:

```sh
python3 tools/romdis.py assets/SCPH-50000.bin --cpu ee --range 0xbfc00ce8 0xbfc00d18
# lui $t0, 0x4553 / ori $t0, $t0, 0x4552 / addiu $t2, $zero, 0x54
```

That is the **fifth** independent implementation of the scan in this ROM, after
the boot block, `IOPBOOT`, `MODLOAD` and `ROMDRV`
(`docs/analysis/11-sif-and-rom-driver.md`). Four of them are IOP code; this one
is R5900. Whatever else changes, `ARC-3` and `ARC-4` are not negotiable.

The search range passed in is `0x9FC00000..0x9FC10000` — 64 KiB. That bounds
where the *table* may be found, not where files may live: the table sits at
`0x2740` and the file it then resolves is at `0x3A2EE0`, far outside the range.

## It loads KERNEL, not EELOAD

The name resolved is a plain string in the boot block:

```sh
python3 -c "d=open('assets/SCPH-50000.bin','rb').read(); print(d[0x1c98:0x1ca2])"
# b'KERNEL\x00\x00\x00\x00'
```

`02` §"EE reset path" guessed that the staged file was `EELOAD`. **It is
`KERNEL`.** `EELOAD` is a separate archive file and is loaded later by something
else; the reset vector does not touch it.

## The copy, and where the kernel actually lands

The copy loop is the clearest example of `tools/romdis.py --cpu ee`'s stated
limitation — LLVM has no R5900 target, so its two most important instructions
decode as `<unknown>` and have to be read as raw words:

```sh
python3 tools/romdis.py assets/SCPH-50000.bin --cpu ee --range 0xbfc00c90 0xbfc00cac
python3 -c "
import struct; d=open('assets/SCPH-50000.bin','rb').read()
for a in (0xc90,0xc94): print(hex(struct.unpack_from('<I',d,a)[0]>>26))"
# 0x1e, 0x1f -- lq and sq
```

Read as R5900, the loop is:

```
 c90:  lq    $2, 0($6)        # 128-bit load
 c94:  sq    $2, 0($5)        # 128-bit store
 c98:  addiu $5, $5, 0x10
 c9c:  sltu  $2, $5, $4
 ca4:  bne   $2, $zero, 0xc90
 ca8:  addiu $6, $6, 0x10     # delay slot
```

so sixteen bytes per iteration, using the R5900's native quadword width.

The destination is set at `0xBFC00C78`: `lui $5, 0xa000` — **`0xA0000000`, which
is physical address 0**, the uncached alias. The end bound is the file's size
rounded up to 16. So `KERNEL` is copied to the very bottom of RAM.

That explains the entry address. `0x80001000` is physical `0x1000`, so the entry
is `0x1000` bytes into the `KERNEL` file, and everything before it is the EE's
**exception vector area**:

```sh
python3 -c "
d=open('assets/SCPH-50000.bin','rb').read(); k=0x3a2ee0
print('KERNEL+0x0080:', d[k+0x80:k+0x88].hex())    # 0x80000080, general exception
print('KERNEL+0x1000:', d[k+0x1000:k+0x1008].hex())"
# KERNEL+0x0080: 5750000800000000   -> the word 0x08005057, a j
# KERNEL+0x1000: 0070023cf03f428c   -> lui $2,0x7000 / lw $2,0x3ff0($2)
```

At `+0x80` — the R5900 general exception vector at `0x80000080` — there is a
jump, exactly as a vector table requires. And the entry at `+0x1000` opens by
reading `0x70003FF0`, the scratchpad word the reset vector wrote before calling
the loader: the boot passes its result to the kernel through the scratchpad
rather than in a register.

`KERNEL` is linked for this address; its first word is `lui $k0, 0x8001`.

## What this pins for the rebuild

- The EE entry is `0xBFC00800`; it must initialise `Config`/`Status`, map the
  scratchpad through the TLB, and use the scratchpad for its stack, because no
  RAM is available until `RDRAM` has run.
- `RDRAM` is entered by **hard-coded address** `0x9FC41000`, so unlike every
  other file its archive offset is load-bearing.
- The EE carries its own copy of the ROMDIR scan and resolves `KERNEL` by name.
- `KERNEL` is copied to physical 0 with `lq`/`sq`, supplies the EE exception
  vectors in its first `0x1000` bytes, and is entered at `0x80001000`.
- The boot hands its result to the kernel in the scratchpad at `0x70003FF0`.

Next: the `KERNEL` image itself — byte-identical across both reference ROMs
(`01`) and now known to be an exception-vector table followed by kernel code
(`docs/project-state.md` §5).
