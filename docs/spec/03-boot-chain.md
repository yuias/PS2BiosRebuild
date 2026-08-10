# Specification: The Boot Chain

Derived from `docs/analysis/02-boot-block.md`,
`03-iopboot-and-boot-list.md`, `06-exceptions-and-interrupts.md` and
`10-module-loading-and-boot-configs.md`.

This covers everything between reset and the first module of the IOP kernel
running: the reset vector, the IOP's own initialisation, and the boot list that
drives the rest. The EE half of the reset vector is deliberately out of scope
(`BOOT-3a`) until the EE side is analysed.

Addresses are given as the CPU sees them. The ROM is mapped at `0xBFC00000`, so
`0xBFC0xxxx` is ROM offset `0xxxxx`.

## BOOT-1: Both CPUs reset to the same address

The EE and the IOP both begin execution at `0xBFC00000`, running the same
instructions, and the code must separate them before doing anything
machine-specific. The reset vector reads the processor-ID register `CP0 $15` and
dispatches on it:

| Condition | Entry |
| --- | --- |
| `PRId >= 0x59` | `0xBFC00800` — the EE |
| `PRId < 0x59` | `0xBFC02000` — the IOP |

**BOOT-1a:** The comparison is `slti` against `0x59` on the full word. The two
entry addresses are fixed: a rebuild may not relocate them, because the
dispatch is the only code both processors execute and nothing else can redirect
them.

**BOOT-1b:** The dispatch, and the whole IOP reset core `0xBFC02000..0xBFC02478`,
are byte-identical between the two reference images. The EE path is not.

## BOOT-2: The identification block

`0xBFC00100` holds a build date followed by vendor identification text. Nothing
in the boot path reads it.

**BOOT-2a — deviation:** the vendor and product strings are original branding and
must not be reproduced (`docs/clean-room-policy.md` §3). Our own text goes at
this offset. Only the placement is preserved, and only because the offset is
where tools look.

## BOOT-3: The machine discriminator

One predicate distinguishes the two machine configurations this ROM serves:

```
PRId < 0x10  ||  (*(u32*)0xBF801450 & 8)
```

It is evaluated at four points and must give the same answer at each:

| Where | Effect when true |
| --- | --- |
| `0xBFC02028` | selects the bus-configuration table at `0xBFC024A8` (POST 1) |
| `0xBFC022EC` | selects one of two stack/parameter constants |
| `0xBFC02368` | boots `TBIN` instead of `IOPBOOT` (POST 6, 7) |
| `INTRMANP`/`INTRMANI`, `TIMEMANP`/`TIMEMANI` entries | selects which variant of the pair becomes resident |
| `SIFMAN` entry | makes `SIFMAN` non-resident |

**BOOT-3a:** A retail machine evaluates it **false**. The true branch is the
factory/development configuration. A rebuild targeting emulators implements both
branches — they are cheap and the predicate is already required — but only the
false branch is exercised.

## BOOT-4: IOP reset sequence

From `0xBFC02000`, in order:

1. **Bus and RAM controller configuration.** A table of `(register_address,
   value)` pairs is applied, terminated by a zero address. The table is
   `0xBFC024A8` when BOOT-3 is true, `0xBFC02560` when false.

   The **first** pair is special: bit `0x1000` of that register's *current*
   value is OR-ed into the stored value before writing, preserving one
   hardware-determined bit. Every later pair is written verbatim. A rebuild must
   keep that asymmetry — writing the first pair verbatim would clear a bit the
   hardware set.
2. **Register clear.** All 32 general-purpose registers are zeroed, then COP0
   `Status`, `Cause` and several other COP0 registers.
3. **Low RAM clear.** The range `0x0..0xF80` is zeroed. This is the region the
   IOP kernel builds its structures in, including the exception vector at
   address 0 that `EXCEPMAN` later installs (`spec/02` context) and the
   boot-parameter table at `0x3F0` (BOOT-8).
4. **Cache priming.** Eight loads from `0xA0000000`, the uncached alias.
5. **RAM-size latch.** A record supplies a word written to the RAM-size register
   `0xBF801060` and a byte kept as the argument for the module about to run.
   The record is `0xBFC024A0` on the true branch, `0xBFC02498` on the false one.
6. **Handoff** (BOOT-6).

**BOOT-4a:** Steps 2 and 3 must precede any use of RAM. Step 1 must precede
step 3, since the memory controller decides what RAM responds at all.

## BOOT-5: POST codes

A one-byte progress code is written to `0xBF802070` throughout. Eleven writes
occur in the reset path, and the **first is not in the code at all**: the
bus-configuration tables of BOOT-4 step 1 each contain an entry targeting the
POST register, so applying the table emits a code as a side effect.

| Code | Written at | Meaning |
| --- | --- | --- |
| `0xFE` | table `0xBFC024A8` entry | bus table being applied (BOOT-3 true) |
| `0xFC` | table `0xBFC02560` entry | bus table being applied (BOOT-3 false) |
| `0x01` | `0xBFC020A0` | bus table `0xBFC024A8` applied (BOOT-3 true) |
| `0x02` | `0xBFC020FC` | bus table `0xBFC02560` applied (BOOT-3 false) |
| `0x03` | `0xBFC02204` | low RAM cleared |
| `0x04` | `0xBFC02340` | stack/parameter constant selected |
| `0x05` | `0xBFC02360` | COP0 `Status`/`Cause` cleared |
| `0x06` | `0xBFC02398` | entering the `TBIN` handoff |
| `0x07` | `0xBFC023BC` | `TBIN` RAM size latched |
| `0x08` | `0xBFC023F4` | entering the `IOPBOOT` handoff |
| `0x09` | `0xBFC02418` | `IOPBOOT` RAM size latched |
| `0xFA` | `0xBFC02464` | module not found — stop |

**BOOT-5a:** A retail boot therefore emits `0xFC, 2, 3, 4, 5, 8, 9`. Codes
`0xFE`, `1`, `6` and `7` belong to the BOOT-3-true configuration.

**BOOT-5b:** `0xFA` is a terminal stop: the code loops rather than continuing.
Any unrecoverable boot failure must be observable this way rather than by
running on into undefined behaviour.

The code-driven writes are reproducible by disassembly, but the table-driven
one is only visible by running the boot:

```sh
python3 tools/romdis.py assets/SCPH-50000.bin --cpu iop \
    --range 0xbfc02000 0xbfc02740 | grep -B4 '0x2070(\$1)'   # the ten in code
python3 tools/iopsim.py assets/SCPH-50000.bin                 # all eleven
```

**`0xFC` was found by execution, not by reading.** A static sweep of the reset
path cannot see a POST write whose address and value both live in a data table,
which is a fair warning about how far disassembly alone can be trusted.

## BOOT-6: Handoff by name

The boot block does not know where the next module is. It calls a routine at
`0xBFC02640` — `findModule(start, end, name)` — which scans the ROM window for
the archive and resolves a 10-byte name, returning the module's ROM address or 0.

**BOOT-6a:** The scan validates a candidate table by matching the first entry's
name against `RESET` (the constants `0x45534552` and `0x54`) and requiring its
`size` field to be 16-aligned, then walks entries accumulating
`align16(size)` — that is, it re-implements `ARC-3` and `ARC-4` in ROM code.
This is one of four independent implementations of the same scan
(`docs/analysis/11` §"The self-locating scan, four times over").

**BOOT-6b:** The search range is `0xBFC00000..0xBFC80000` — the first 512 KiB of
the ROM only. Every module the boot block itself resolves must be stored within
it.

**BOOT-6c:** The resolved module is entered with `jr`, not `jal`, with the
latched RAM-size byte in `$a0`. It never returns.

**BOOT-6d:** A failed lookup writes POST `0xFA` and stops (BOOT-5b).

## BOOT-7: IOPBOOT

`IOPBOOT` is **raw IOP code, not an IRX module** — the boot block enters it by
address, so it has no `.iopmod`, no relocation and no library tables.

It sizes a stack from the RAM-size argument, sets `$gp`, and then carries **its
own** copy of the BOOT-6a scan to resolve `IOPBTCONF` by name. It loads the
modules that list names, in order.

**BOOT-7a:** `IOPBOOT` must be stored in the archive like any other file and
must be locatable by the boot block's scan, which is why the reference aligns it
with a padding entry (`ARC-5`).

## BOOT-8: The boot-parameter table at 0x3F0

The absolute address `0x3F0` holds a pointer to a table of boot records, each
keyed by a small integer. `loadcore` ordinal 12 looks up a key and returns the
record, whose flag word gates behaviour in the caller.

Keys 1, 3 and 4 are used by `EECONF`, `SIFINIT`, `SIFCMD` and `IGREETING`. The
address is ABI: a rebuild cannot relocate it.

## BOOT-9: IOPBTCONF grammar

`IOPBTCONF` is an ASCII file, parsed as whitespace-separated tokens. Any byte
below `0x20` ends a token.

| Leading byte | Meaning |
| --- | --- |
| `@` | the rest of the token is a hex base load address; the reference uses `@800` |
| `#` | a directive; the parser matches one keyword, `"!addr "` |
| anything else | a module name, resolved in the archive |

**BOOT-9a:** A name that does not resolve aborts the boot. Names are resolved
against the archive by `ARC-9`.

**BOOT-9b:** The `#` directive is **unused** by every boot list in the reference
image, including the nested ones. Its effect is unspecified here; a rebuild need
not implement it, but must not treat a `#` token as a module name.

**BOOT-9c:** The order of names is the **load order**, which is independent of
the storage order the archive imposes. Both are meaningful and neither can be
derived from the other.

**BOOT-9d:** The load order has three phases — kernel core, OS services, then
EE-facing services — and a phase-three module may not be moved before `SIFMAN`.

**BOOT-9e:** Alternative configurations are supplied as nested archives
(`ARC-8`) carrying their own `IOPBTCONF`, not as a flag. `IOPBTCON2` exists in
the reference archive but is named by nothing in the image.

## Verification

Everything in BOOT-1, BOOT-3, BOOT-5 and BOOT-6 is a statement about specific
instructions at specific addresses, checkable by disassembly today:

```sh
# BOOT-1: the dispatch
python3 tools/romdis.py assets/SCPH-50000.bin --range 0xbfc00000 0xbfc00030

# BOOT-1b: the IOP reset core is model-independent
python3 -c "a=open('assets/SCPH-50000.bin','rb').read(); \
b=open('assets/SCPH-70000.bin','rb').read(); \
print(a[:0x30]==b[:0x30], a[0x2000:0x2478]==b[0x2000:0x2478])"

# BOOT-3: the discriminator is tested three times in the reset path
python3 tools/romdis.py assets/SCPH-50000.bin --cpu iop \
    --range 0xbfc02000 0xbfc02740 | grep 0x1450     # 3 sites

# BOOT-5: ten POST writes
python3 tools/romdis.py assets/SCPH-50000.bin --cpu iop \
    --range 0xbfc02000 0xbfc02740 | grep -c '0x2070(\$1)'   # 10

# BOOT-9: the grammar, against every boot list in the image
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>
python3 tools/romdir.py <outdir>/EELOADCNF --extract <outdir>/eeloadcnf
head -1 <outdir>/IOPBTCONF <outdir>/eeloadcnf/IOPBTCONF   # both '@800'
```

The *sequence* — that BOOT-4's steps happen in that order and produce BOOT-5's
trace — needs a machine, and `tools/iopsim.py` is it. Booting the reference
image on it emits exactly BOOT-5a's retail sequence and carries on through
BOOT-6 and BOOT-7 into the loaded modules:

```sh
python3 tools/iopsim.py assets/SCPH-50000.bin
# POST sequence: ['0xfc', '0x2', '0x3', '0x4', '0x5', '0x8', '0x9']
```

`--check` makes that a gate: it fails, naming BOOT-5a, if the sequence differs —
verified by altering the POST value inside the bus table, which yields
`POST sequence ['0xab', ...] != ['0xfc', ...]`.

So BOOT-1 and BOOT-3 through BOOT-7 are verified rather than merely described.
BOOT-8 and BOOT-9 are exercised on the way (the boot reaches `IOPBTCONF` and
loads modules from it) but not yet asserted; that needs the simulator to model
exceptions, which is where it currently stops.

**BOOT-5 corrected `docs/analysis/02`.** Extracting the POST writes mechanically
rather than reading them off a listing found a code the prose had missed
(`0x03`) and showed the retail path is `8, 9` where the prose had called `6, 7`
the normal one.
