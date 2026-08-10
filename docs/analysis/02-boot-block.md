# The RESET Boot Block

`RESET` is the first file in the archive (`docs/analysis/01-rom-layout.md`): raw
code at ROM offset 0, mapped at `0xBFC00000`, where both CPUs begin execution
after reset. It is not a container — its first words are MIPS instructions —
and its job is to tell the two CPUs apart, bring each to a usable state, and
hand control to the first loadable module.

Disassembly is via `tools/romdis.py`, which decodes at a run-time address so
branch and jump targets read correctly. The boot block runs from the ROM
window, so addresses are given as `0xBFCxxxxx`:

```sh
python3 tools/romdis.py assets/SCPH-50000.bin --range 0xbfc00000 0xbfc00030
```

The boot block is *not* wholly identical between `SCPH-50000` and
`SCPH-70000`, but the parts this project depends on first are. The differences
fall in two clusters — an ID/date block at `0x100` and the EE path
(`0x874..0x1CAD`) — while the CPU dispatcher (`0..0x30`) and the whole **IOP
reset core** (`0x2000..0x2478`) are byte-identical across both images:

```sh
python3 -c "a=open('assets/SCPH-50000.bin','rb').read(); \
b=open('assets/SCPH-70000.bin','rb').read(); \
print('dispatch', a[:0x30]==b[:0x30]); \
print('iop core', a[0x2000:0x2478]==b[0x2000:0x2478]); \
print('ee path ', a[0x800:0x2000]==b[0x800:0x2000])"
# dispatch True / iop core True / ee path False
```

The block at `0x100` holds an EXTINFO-style BCD build date (`0x100..0x102`, the
only difference before `0x108`) followed by a vendor copyright string and a
"PS compatible mode" identifier — original branding text, out of bounds for
reproduction per `docs/clean-room-policy.md` §3 and noted here only by offset.

## CPU dispatch at 0xBFC00000

Both the EE (R5900) and the IOP (R3000-class) reset to `0xBFC00000` and run the
same first instructions, which read the processor-ID register and split:

```
bfc00000  mfc0  $26, $15        # $15 = PRId
bfc00008  slti  $1, $26, 0x59
bfc0000c  bnez  $1, 0xbfc00024  # PRId < 0x59 -> IOP entry
bfc00014  lui   $26, 0xbfc0
bfc00018  ori   $26, 0x800
bfc0001c  jr    $26             # PRId >= 0x59 -> 0xBFC00800 (EE)
...
bfc00024  lui   $26, 0xbfc0
bfc00028  ori   $26, 0x2000
bfc0002c  jr    $26             # -> 0xBFC02000 (IOP)
```

`PRId` is compared as a full word. The R5900 reports a much larger value than
the IOP, so the EE falls through to `0xBFC00800` and the IOP branches to
`0xBFC02000`. The routing is corroborated by the I/O each path then touches:
the `0x800` path uses EE registers (`0x1000_F500`, R5900 `Config`/`Status`,
`tlbwi`) and the `0x2000` path uses IOP registers (`0xBF80_1450`,
`0xBF80_2070`) — so the assignment of entry to CPU does not rest on the PRId
argument alone.

```sh
python3 tools/romdis.py assets/SCPH-50000.bin --cpu ee  --range 0xbfc00800 0xbfc00920
python3 tools/romdis.py assets/SCPH-50000.bin --cpu iop --range 0xbfc02000 0xbfc024a8
```

## IOP reset path (0xBFC02000)

The IOP path is the one that ends by loading the first real module, so it is
the one this project follows first. In order:

1. **POST tracing.** Throughout, a one-byte progress code is written to the IOP
   POST register at `0xBF80_2070` (`sb $3, 0x2070($1)` with `$1 = 0xBF80_0000`).
   The sequence observed is `1, 2, 4, 5, 6, 7` on the normal path (and `8, 9`
   on the alternate, plus `0xFA` on failure) — an equivalent of the PS1 BIOS's
   `0x0..0xFF` POST codes, and the first thing to reproduce so a boot can be
   traced.

2. **Bus / RAM configuration.** Two small tables at `0xBFC0_2560` and
   `0xBFC0_24A8` are walked as `(register_address, value)` pairs, OR-ing a bit
   into each named register — the SSBUS / memory-controller setup. Which of the
   two runs is selected by `PRId < 0x10` and bit 3 of `0xBF80_1450`
   (development vs retail signalling). POST `1` marks one branch, `2` the other.

3. **GPR and COP0 clear.** All 32 general-purpose registers are zeroed, and
   COP0 `Status`/`Cause` and several other COP0 registers are cleared, putting
   the CPU in a known state before any RAM is touched.

4. **Scratch/RAM clear.** A loop zeroes a `0x0..0xF80` region in 32-word
   strides — the low RAM the IOP kernel will build its structures in.

5. **Cache priming.** A short run of eight `lw` from `0xA000_0000` (uncached
   kseg1) warms the read path before the module load.

6. **RAM-size latch.** A small record at `0xBFC0_24A0` (retail) or `0xBFC0_2498`
   (alternate) supplies a word written to the RAM-size register `0xBF80_1060`
   and a byte kept in `$16` as an argument for the module about to run.

7. **Handoff.** The path selects a module by name and jumps to it:

   ```
   bfc023c0  lui   $4, 0xbfc0        # search start  = 0xBFC00000
   bfc023c4  lui   $5, 0xbfc8        # search end    = 0xBFC80000
   bfc023c8  lui   $6, 0xbfc0
   bfc023cc  addiu $6, 0x2488        # name pointer -> "TBIN"
   bfc023d0  jal   0xbfc02640        # findModule(start, end, name)
   bfc023d8  beqz  $2, 0xbfc02454    # not found -> POST 0xFA, hang
   bfc023e0  move  $4, $16           # pass the latched arg
   bfc023e4  jr    $2                # enter the module
   ```

   Bit 3 of `0xBF80_1450` chooses which module: set → `"TBIN"` (the factory /
   test path), clear → `"IOPBOOT"` (the retail path, same shape at `0xBFC023EC`
   with the name pointer `0xBFC0_2478`). Both names are plain strings in the
   boot block:

   ```sh
   python3 -c "d=open('assets/SCPH-50000.bin','rb').read(); \
   print(d[0x2478:0x2481], d[0x2488:0x248d])"
   # b'IOPBOOT\x00' b'TBIN\x00'
   ```

   `IOPBOOT` and `TBIN` are both files in the archive (`--list`), so the boot
   block hands off to another archive member, located at run time by the next
   routine.

## The module-lookup routine (0xBFC02640)

`findModule(start, end, name)` locates a named file by walking the ROMDIR table
in the ROM window, without any stored pointer to it — the same
self-locating scheme `tools/romdir.py` implements, done here in the boot ROM:

```sh
python3 tools/romdis.py assets/SCPH-50000.bin --cpu iop --range 0xbfc02640 0xbfc02730
```

- It first confirms it is looking at a real ROMDIR by matching the fixed first
  entry, whose name is `RESET`: the constants `0x4553_4552` (`"RESE"`) and
  `0x54` (`"T"`) are compared against the candidate's first 10 name bytes, with
  the entry's `size` field required to be 16-aligned (`+0xF & ~0xF`) — exactly
  the ROMDIR entry format and the same consistency check the tool uses.
- It then walks entries (16 bytes each), accumulating each file's storage
  offset as the running sum of 16-aligned sizes, and compares each 10-byte name
  against the requested one (`$6` points at a 10-byte name field). On a match it
  returns `start + offset`, the ROM address of the file; on exhaustion it
  returns 0.

This is the run-time counterpart of `01-rom-layout.md`'s finding that the
archive locates itself through the `RESET` entry: the hardware boot path relies
on precisely that property, so a rebuilt image must preserve the entry format,
the 16-byte alignment and the `RESET`-first invariant, not merely the file
contents.

## EE reset path (0xBFC00800) — outline

The EE path is recorded here in outline; it warrants its own document once the
EE kernel is under analysis. It sets R5900 `Config` (`0x0007_3003`) and
`Status` (`0x7040_0000`), installs a single TLB entry mapping the scratchpad,
sets up a stack in scratchpad (`0x7000_3FF0`), calls a ROM routine at
`0x9FC4_1000` and another near `0x9FC0_0BF0`, invalidates the instruction and
data caches over a `0x2000`-line range, and finally jumps to `0x8000_1000` in
main RAM — the entry point of the code it has staged there (the EE loader,
`EELOAD` in the archive, is the expected source). The specifics of that staging
are deferred.

## What this pins for the rebuild

- The reset vector must begin with the PRId dispatch and route the two CPUs to
  separate entry points; the split cannot be collapsed.
- The IOP path must emit the POST progression, configure the bus/RAM registers,
  clear CPU and low RAM, then locate and enter `IOPBOOT` (retail) via a
  self-locating ROMDIR search — behaviour captured as requirements when
  `docs/spec/` for the boot block is written.
- The archive's self-location through the `RESET` entry is load-bearing in ROM
  code, promoting `01-rom-layout.md`'s observation to a hard constraint.

Next: the format of the module the boot block enters (`IOPBOOT`) and how it
consumes `IOPBTCONF` to load the rest of the IOP kernel
(`docs/project-state.md` §5, step 2).
