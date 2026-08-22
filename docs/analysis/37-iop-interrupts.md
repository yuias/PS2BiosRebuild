# IOP Interrupt Delivery: EXCEPMAN and INTRMAN

`docs/analysis/34` left the IOP-side DMA interrupt dispatcher unresolved beyond
a single call site — `SIFCMD` was seen doing `RegisterIntrHandler(0x2b, ...)`
and nothing past it. `docs/analysis/35` then read the *EE* side end to end:
vector, INTC/DMAC tables, per-cause trampolines, register-preservation modes,
the reschedule hook. This reads the IOP's matching half: `EXCEPMAN`'s
exception-vector install and handler chains, and `INTRMAN`'s ordinal table,
registration record, and the full vector-to-handler-to-scheduler path, so the
rebuild's own kernel has a concrete target for
`RegisterIntrHandler`/`EnableIntr`/`CpuSuspendIntr` and for servicing
`SIFMAN`/`SIFCMD`'s and `SIO2MAN`'s registrations.

**Correction to this document's own first pass:** the `INTRMAN` half below now
covers `INTRMANI`, not `INTRMANP`. The first pass assumed `docs/analysis/06`'s
predicate meant `INTRMANP` self-registers on SCPH-50000-class hardware; §0
below shows that assumption was backwards — on the traced retail image it is
`INTRMANP` that returns without registering, and `INTRMANI` that stays
resident. Every offset in §2 and §3 is now `<outdir>/INTRMANI`'s; `INTRMANP`'s
own offsets survive only where the text explicitly compares the two.

## 0. Which variant is resident, and how that was checked

`docs/analysis/06`'s predicate is `PRId < 0x10 || (*(u32*)0xBF801450 & 8)` —
true selects `INTRMANP`, false selects `INTRMANI` (`06` §"The P/I variant
pair": each module tests the same condition and takes the opposite branch).
Three independent checks, run against `assets/SCPH-50000.bin`, agree that the
condition is **false** on this image, so `INTRMANI` is the resident variant:

1. **`tools/iopsim.py`'s own module-release tracking.**
   `python3 tools/iopsim.py assets/SCPH-50000.bin` reports

   ```
   module images released (IRX-12): 4
      0x003d00  at step 118437
      ...
   libraries registered in RAM: 23
      ...
      0x0051b0  intrman    v1.02  flags 0x0
      ...
   ```

   The boot list's fourth entry (`SYSMEM LOADCORE EXCEPMAN INTRMANP INTRMANI
   ...`, `docs/analysis/03`) is `INTRMANP`, which loads at `0x3d00` —
   cross-checked against `INTRMANI`'s own registered-library address:
   `0x51b0 - 0x1480` [`INTRMANI`'s own export-table module offset, §"Ordinal
   parity" in `06`] `= 0x3d30`, which rounds down to the same `0x100`-aligned
   block `0x3d00` that IRX-12 frees — consistent with the freed block being
   reused immediately for the next module load.
   `INTRMANP`'s image is released at step 118437, before any `intrman`
   registration has happened; the *only* `intrman` library that ends up
   registered is the one at `0x51b0`, and `iopsim.py --check`'s own
   `SINGLE_VARIANT_LIBRARIES` invariant requires there be exactly one.

2. **Re-disassembling `INTRMANP`'s own entry predicate** (`<outdir>/INTRMANP.text`
   offset `0x0`–`0x3c`, `python3 tools/romdis.py <outdir>/INTRMANP.text --cpu
   iop --vma 0 --range 0x0 0x100`):

   ```
    8:  mfc0 $2,$15,0x0        ; PRId
   10:  slti $2,$2,0x10 ; bnez $2,0x40      ; PRId<0x10 -> proceed (branch to 0x40)
   1c:  lui $2,0xbf80 ; ori $2,$2,0x1450
   24:  lw $2,0x0($2)
   2c:  andi $2,$2,0x8 ; bnez $2,0x44        ; bit3 set -> proceed (branch to 0x44)
   38:  j 0xec                                ; neither held -> return 1 (delay slot: addiu $2,$0,1)
   ```

   `tools/iopsim.py` models `IOP_PRID = 0x1F` and initialises the discriminator
   register `0xBF801450` (`DISCRIMINATOR_REG`) to `0`, matching the tool's own
   comment that these constants are chosen to model "retail" (`PRId >= 0x10`
   satisfies `BOOT-3`, and any in-between value satisfies both boundary checks
   the boot path makes). Under those values neither branch at `0x14` nor `0x30`
   is taken, so `INTRMANP`'s own code, as written, falls through to `j 0xec`
   and returns 1 without registering — exactly the outcome (a released,
   non-resident image) that check 1 observed independently.

3. **A targeted trace**, importing `tools/iopsim.py` as a module and watching
   `cpu.pc` while the boot runs, confirms the CPU actually executes that same
   instruction span at `INTRMANP`'s real load address (`0x3d00`–`0x3d44`) in
   the run that produces the `0x3d00` release — i.e. this is not merely true
   of the on-disk bytes, it is the code path the simulated boot takes.

`INTRMANI`'s own entry (`<outdir>/INTRMANI.text` offset `0x0`–`0x34`) is the
exact mirror — `bnez $2,0xf4` (return 1) on *either* branch taken, falling
through to the real init body only when both are false — so the same
`PRId`/discriminator values that make `INTRMANP` bail make `INTRMANI` proceed,
consistent with `06`'s "each module tests the same condition and takes the
opposite branch." This matches the task's own framing: `INTRMANI` is the
variant with second-DMAC-bank code (`06` §"What actually differs"; confirmed
independently below in §3.4), and this generation of IOP — the one this image
actually boots on — has that second bank.

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
python3 tools/irxinfo.py <outdir>/EXCEPMAN --exports --imports
python3 tools/irxinfo.py <outdir>/INTRMANI --exports --imports
python3 tools/irxinfo.py <outdir>/EXCEPMAN --dump-load <outdir>/EXCEPMAN.text
python3 tools/irxinfo.py <outdir>/INTRMANI --dump-load <outdir>/INTRMANI.text
python3 tools/romdis.py <outdir>/EXCEPMAN.text --cpu iop --vma 0
python3 tools/romdis.py <outdir>/INTRMANI.text --cpu iop --vma 0
# cross-references
python3 tools/irxinfo.py <outdir>/SIFCMD --imports; python3 tools/irxinfo.py <outdir>/SIFMAN --imports
python3 tools/irxinfo.py <outdir>/SIO2MAN --imports; python3 tools/irxinfo.py <outdir>/THREADMAN --imports
# §0's residency check
python3 tools/iopsim.py assets/SCPH-50000.bin
```

ps2sdk's `iop/system/{excepman,intrman,dmacman}/include/*.h` headers are used
only to name ordinals and structure fields; every such name is marked
`[header]`. The retail binaries are the authority on behaviour. `tools/iopsim.py`
has no interrupt model, so it was used only for §0's variant-residency check
(module-release tracking and a `PRId`/discriminator-register trace) — every
other claim below is disassembly-derived, cross-checked across at least two
independent call sites where the text says so.

## 1. EXCEPMAN

`docs/analysis/06` already covers the variant-selection predicate, the
16-slot cause-chain-head table, and the vector-install copy loop's broad
shape. This section goes past that into what the copy loop's "immediate
patch" actually installs, what the two blocks it installs *do*, and how
`RegisterException(Priority)Handler` builds the chains those blocks walk.

### 1.1 A 32-node pool backs every registered handler

At entry (`<outdir>/EXCEPMAN.text` offset `0x0`), after the variant predicate
and the 16-word cause-table clear (`06`), a helper at `0x4f0` is called:

```
4f0:  addiu $sp,$sp,-0x18
4f4:  move  $4,$zero
4f8:  addiu $5,$zero,0x100
500:  jal   0x694                 ; sysmem import ord.4 (AllocSysMemory-class)
...
51c:  sw    $2,0x748($1)          ; [0x748] = buffer
520:  addu  $2,$6,$4 ; sw $2,0($5); addiu $5,$5,8; addiu $3,$3,1
530:  sltiu $2,$3,0x1f ; bnez ... ; addiu $4,$4,8
```

This allocates `0x100` bytes and links them into 32 nodes of 8 bytes each
(`next` at `+0`, `info` at `+4`), threading `node[i].next = &node[i+1]` and
zero-terminating the last. `[0x748]` is the free-list head. `0x478` pops a
node (lazily calling `0x4f0` the first time the list is empty) and `0x4d4`
pushes one back — a private slab allocator for exception-handler bookkeeping
nodes, entirely separate from `sysmem`'s own allocator after the initial
`0x100`-byte grant.

### 1.2 Registration: priority-ordered insertion, `next` rewritten to point at code

`RegisterExceptionHandler` (ordinal 4, `0x110`) is a thin wrapper that calls
`RegisterPriorityExceptionHandler` (ordinal 5, `0x134`) with `priority=2`,
matching the header's doc comment. `RegisterPriorityExceptionHandler` itself:

```
150:  lw $2,0($18)         ; handler->next   (caller's own struct; must be unlinked)
158:  beqz $2,0x168 ; ... ; addiu $2,zero,-0x34   ; error if already linked
168:  sltiu $2,$17,0x10 ; beqz ...,0x1f8 ; addiu $2,zero,-0x32  ; error if exception>=16
174:  jal 0x478             ; pop a pool node -> $5
178:  andi $16,$16,0x3      ; priority &= 3
198:  or $2,$18,$2 ; sw $2,4($5)     ; pool_node.info = (handler & ~3) | priority
```

then walks `table[exception]` (module offset `0x704`, the 16-word chain-head
array `06` found), comparing each existing node's packed 2-bit priority
against the new one, and links the pool node in at the resulting position —
an ordinary priority-ordered singly-linked-list insert.

The interesting step is what happens next, in the shared rebuild routine at
`0x38c` (called after every insert and every remove):

```
3c8:  lw $3,4($4)     ; this_node.info
3cc:  lw $2,4($2)     ; next_node.info
3d0:  and $3,$3,$6    ; strip priority bits -> this handler's struct base
3d4:  and $2,$2,$6    ; strip priority bits -> next handler's struct base
3d8:  addiu $2,$2,0x8 ; -> next handler's funccode[0]
3dc:  sw $2,0($3)      ; *this_handler_struct.next = &next_handler.funccode[0]
```

For the *last* node in a cause's chain, the same loop instead writes the
current value of `[0x744]` (below) into that node's `next` field. So
`EXCEPMAN` does not walk its bookkeeping list at exception time at all: it
maintains a separate slab-allocated priority list purely for insertion order,
and on every change it **rewrites each caller-supplied handler struct's own
`next` field (offset 0) to hold the address of the next handler's `funccode`
entry point (struct base + 8)** — turning registration into a native code
chain a dispatcher can `jr` through with no table lookup, falling through to
a shared default-handler chain when a cause's own handlers are exhausted.

`RegisterDefaultExceptionHandler` (ordinal 6, `0x210`) links onto a single,
cause-independent chain head at `[0x744]` the same way (`handler.next =
*[0x744]`; `*[0x744] = &handler.funccode[0]`), then calls the same `0x38c`
rebuild so every cause's *last* node now falls through to it.
`ReleaseExceptionHandler`/`ReleaseDefaultExceptionHandler` (ordinals 7/8,
`0x264`/`0x30c`) unlink from the pool list and return the node with `0x4d4`,
then also call `0x38c`.

### 1.3 The installed vector: two blocks, one shared 16-entry table

The copy-with-patch loop (`06`) copies module offset `0x560`–`0x630` to
address 0. `0x560`–`0x59c` is filler (4 words of `nop`, then 12 `break 1`
words) landing at destination `0x00`–`0x3c` — left as traps, unused by
anything this pass found. Two real blocks follow:

- **`0x5a0`→ destination `0x40`.** Saves `$26`, `EPC`(cop0 14), `Cause`(13),
  `Status`(12), and cop0 register 7 to fixed cells `0x420`–`0x430`; loads a
  pointer from `0x3c($27)` with `$27` preset to `0x3c`; clears cop0 register
  7; `jr`s to the loaded pointer.
- **`0x5e0`→ destination `0x80`** — the general exception vector proper.
  Saves `$at`,`EPC`,`Status`,`Cause` to fixed cells `0x400`–`0x40c`, then:

  ```
  604:  andi $26,$26,0x3c        ; Cause -> ExcCode*4, 0/4/.../0x3c
  608:  lw   $26,0x0($26)        ; unpatched: reads absolute address (ExcCode*4)
  610:  jr   $26
  ```

Both `lw`s are the two placeholder encodings (`0x8f5a0000`/`0x8f7b0000`) the
copy loop treats specially, and both get the *same* 16-bit value OR-ed in —
read, at patch time, from module offset `0x700`. That cell is not
runtime-computed: the entry function stores a literal constant into it before
the copy loop runs (`0x44`: `addiu $2,$0,0x440`; `0x50`: `sw $2,-0x44($16)`
with `$16=&0x744`, i.e. `[0x700]=0x440`, confirmed by reading the raw words).
So after installation, block B's `lw` becomes `lw $26, 0x440($26)` and block
A's becomes `lw $27, 0x440($27)` with `$27` preset to `0x3c` — **both read the
same 16-entry, cause-indexed table at absolute `0x440`–`0x47c`**; block B
computes the index from `Cause` at run time, block A hardcodes index `0xf`
(`IOP_EXCEPTION_HDB` [header], "Hardware DeBug") and additionally touches cop0
register 7 and clears it — consistent with it being a dedicated breakpoint
path, though exactly what hardware condition reaches destination `0x40`
rather than `0x80` was not established (§5). `GetExHandlersTable` (ordinal 3,
`0x37c`) simply returns the address `0x700` — a pointer to this table's own
base pointer, so relocating the table only ever requires patching one cell.

`0x440`–`0x47c` sits **immediately before** `INTRMAN`'s own IRQ table at
`0x480` (§2) — the two kernel modules' fixed low-memory tables are
contiguous, both baked in as literal constants rather than allocated at
runtime. (True of both variants: `INTRMANI` clears the same absolute `0x480`
table, §2.2.)

## 2. INTRMANI

`INTRMANI` (§0) shares `INTRMANP`'s ordinal layout, its `0x480` handler table,
and its overall control flow, but is a larger module (`text 0x1570` against
`INTRMANP`'s `0x1200`, `06` §"What actually differs") because it fully wires
up the second DMAC bank and adds a second hardware gate, `I_CTRL`
(`0xBF801078`), that `INTRMANP` never touches. Both are noted where they
appear.

### 2.1 Ordinal table

Cross-checked against every import site this pass read (`SIFCMD`, `SIFMAN`,
`DMACMAN`, `THREADMAN`, `VBLANK`, `SIO2MAN`, `CDVDMAN`, `PADMAN`, `MCMAN`) and
against ps2sdk's `intrman.h` [header]. Offsets are `<outdir>/INTRMANI.text`;
the `INTRMANP` column is `06`'s ordinal-parity check (both export exactly 32
entries at matching slots) carried over from this document's first pass, kept
only for cross-reference.

| ord | `INTRMANI` offset | `INTRMANP` offset | name [header unless noted] |
|---|---|---|---|
| 0 | `0x0` | `0x0` | module entry |
| 1 | `0x104` | `0xfc` | reserved, bare `jr $ra; move $2,$0` (returns 0) |
| 2 | `0x10c` | `0x104` | reserved, bare stub (zeroes `I_MASK`/`DICR`/`DICR2`) |
| 3 | `0x138` | `0x124` | `GetIntrmanInternalData` (returns `&0x15f0`) |
| 4 | `0x148` | `0x134` | `RegisterIntrHandler` |
| 5 | `0x270` | `0x25c` | `ReleaseIntrHandler` |
| 6 | `0x458` | `0x474` | `EnableIntr` |
| 7 | `0x638` | `0x584` | `DisableIntr` |
| 8 | `0x388` | `0x398` | `CpuDisableIntr` |
| 9 | `0x3b4` | `0x3c4` | `CpuEnableIntr` |
| 10 | `0x408` | `0x424` | unnamed — bare `syscall 4` trampoline, never called internally |
| 11 | `0x418` | `0x434` | unnamed — bare `syscall 8` trampoline (9's internal call target) |
| 12 | `0x3dc` | `0x3e4` | unnamed — reads `I_CTRL` raw (8's internal call target) |
| 13 | `0x3f0` | `0x404` | unnamed — writes `I_CTRL=1` (9's internal call target) |
| 14 | `0x448` | `0x464` | `CpuInvokeInKmode` (itself a bare `syscall 0xc`) |
| 15 | `0x854` | `0x738` | `DisableDispatchIntr` |
| 16 | `0x7b4` | `0x698` | `EnableDispatchIntr` |
| 17 | `0x348` | `0x334` | `CpuSuspendIntr` |
| 18 | `0x374` | `0x378` | `CpuResumeIntr` |
| 19 | `0x348` | `0x334` | alias of 17 (identical address) |
| 20 | `0x374` | `0x378` | alias of 18 |
| 21 | `0x428` | `0x444` | unnamed — bare `syscall 0x10` trampoline, never called internally |
| 22 | `0x438` | `0x454` | unnamed — bare `syscall 0x14` trampoline, never called internally |
| 23 | `0xd70` | `0xa30` | `QueryIntrContext` (falls through into 24's body with `$4=$sp`) |
| 24 | `0xd74` | `0xa34` | `QueryIntrStack` |
| 25 | `0xd98` | `0xa58` | `iCatchMultiIntr` — partially decoded, §2.4 |
| 26 | `0x1518` | `0x11a8` | reserved, bare `jr $ra` stub |
| 27 | `0x8fc` | `0x7e0` | `intrman_internals_t.dmac2_interrupt_handler_mask` setter (`sw $4,0x15fc($1)`) — read by §2.5's gate, unlike `INTRMANP`'s (§3.4's `INTRMANI` cross-check already noted this; confirmed here directly) |
| 28 | `0xd20` | `0x9dc` | `SetNewCtxCb` |
| 29 | `0xd30` | `0x9ec` | (`ResetNewCtxCb`) — restores a built-in default at `0x12c4` |
| 30 | `0xd48` | `0xa04` | `SetShouldPreemptCb` |
| 31 | `0xd58` | `0xa14` | (`ResetShouldPreemptCb`) — restores a built-in default at `0x12d0` |

The ordinal-9/17 split is the load-bearing difference from `INTRMANP`: on
`INTRMANI`, ordinals 10/21/22 (the bare `syscall 4`/`0x10`/`0x14` trampolines)
are exported but **never called from anywhere inside the module** — grepped
across the full disassembly, the only internal `jal` to any of the four
syscall trampolines is `CpuEnableIntr` (ord. 9) calling ord. 11's `syscall 8`.
`CpuDisableIntr`/`CpuSuspendIntr`/`CpuResumeIntr` (8/17/18) instead read or
write `I_CTRL` directly, with no trap at all (§2.5). The SYSCALL exception
handler (§3.1) still implements the full `Status` IEp/IEo case dispatch for
syscalls 4/8/0xc/0x10/0x14, identically to `INTRMANP` (§2.5) — it is simply
not reached through these four particular wrappers on this variant, only
through `CpuEnableIntr`'s syscall 8 and `CpuInvokeInKmode`'s syscall 0xc.

### 2.2 The registration record and the handler tables

Init (module entry, before any registration) clears **128 words at absolute
address `0x480`** — `64: sw $0,0x480($5)` in a 128-iteration loop where `$5`
counts down from `0x1fc` used as a *literal* base address, not module-relative
— zeroes `I_MASK`(`0xBF801074`), `DICR`(`0xBF8010F4`), and (unlike `INTRMANP`)
**`DICR2`(`0xBF801574`)** three separate ways before the loop even starts
(`0x58`–`0x60`), and stores `0x480` itself, plus `-1,-1` for
`masked_icr_1`/`masked_icr_2`, into a bss struct at module offset
`0x15f0`–`0x15f8`, exactly matching `intrman_internals_t` [header]
`{interrupt_handler_table; masked_icr_1; masked_icr_2;
dmac2_interrupt_handler_mask;}` (the fourth field, `0x15fc`, is ordinal 27's
target, §2.1 — and, unlike `INTRMANP`'s corresponding field, actually read
back later, §2.5).

`RegisterIntrHandler(irq, mode, handler, arg)` (`0x148`) packs `handler` and
`mode&3` into one word (same low-bit-packing trick as `EXCEPMAN`'s priority)
and, for `irq<0x2e`, stores it at **absolute `0x480 + irq*8`** (`handler|mode`
at `+0`, `arg` at `+4`); for `irq` in `{0x3e,0x3f}` (`IOP_IRQ_SW1`/`SW2`
[header]) it stores at **absolute `0x400 + irq*8`** — `0x5f0`/`0x5f8` — a
formula independently confirmed by `ReleaseIntrHandler` (`0x270`, same three
range checks, same two bases) and by the dispatcher's own SW1/SW2 path (§3.3,
reads `0x5f0($6)` with `$6 = swindex*8`). Any other `irq` is rejected
(`-0x65`); a duplicate registration on an already-occupied slot is rejected
(`-0x68`/`-0x69` depending on range); calling from interrupt context
(`QueryIntrContext()!=0`, §2.4) is a silent no-op. `CpuSuspendIntr`/
`CpuResumeIntr` bracket the whole table mutation, exactly as in `INTRMANP`.

The `irq<0x2e` range is internally split into three storage buckets — `irq<0x20`
(packs `mode&3` into the stored word), `0x20<=irq<0x28`, and `0x28<=irq<0x2e`
(the latter two both store the raw handler pointer with **no mode bits OR'd
in** — `0x1f8: sw $18,0x480($4)`, identical to `INTRMANP`'s finding for its
own `0x20`–`0x2d` range, §3.6). Note the first sub-boundary here is `0x28`,
not `0x27`: `RegisterIntrHandler` will happily store a handler at `table[0x27]`,
even though `EnableIntr` (§2.3) treats `0x27` as invalid — a one-slot gap
between the two banks' valid channel ranges that exists in the storage
format but is never enable-able.

The `mode` argument is not consumed by `RegisterIntrHandler` beyond storage —
it is read back by the dispatcher (§3.2) to decide how much of the register
file to save before calling the handler, matching the header's mode 0/1/2
description exactly. Handlers registered at `irq>=0x20` are always invoked as
mode 0 as a result (no mode bits are ever stored for them), and the DMA
sub-dispatch's own call sites (§3.4) don't consult the mode bits at all —
they always call `handler(arg)` with just the four-register baseline saved.

`0x480`'s 128-word span (`0x480`–`0x67c`) comfortably holds `irq` 0 through
`0x2d` at 8 bytes/entry (46 entries, `0x480`–`0x658`). Unlike `INTRMANP`,
`INTRMANI`'s `EnableIntr`/`DisableIntr` (§2.3) and its DMA sub-dispatch (§3.4)
reach the *full* `0x20`–`0x2d` span, not just `0x20`–`0x26` — see §3.4.

### 2.3 `EnableIntr`/`DisableIntr`: I_MASK for irq<0x20, DICR+DICR2 for 0x20–0x26 and 0x28–0x2d

Unlike `INTRMANP`, `INTRMANI`'s `EnableIntr` (`0x458`) reaches the *entire*
`0x20`–`0x2d` span, arming both DMAC banks. For `irq<0x20` it is the same
plain `I_MASK |= 1<<irq` as `INTRMANP`:

```
494:  lui $2,0xbf80 ; ori $2,$2,0x1074      ; I_MASK
4a0:  lw $4,0($2) ; sllv $3,1,$16 ; or $4,$4,$3 ; sw $4,0($2)   ; I_MASK |= 1<<irq
```

For `0x20<=irq<0x27` (bank 1, `4b8: slti $2,$16,0x27`):

```
4dc:  addiu $2,$5,0x10 ; sllv $2,1,$2 ; or $7,$7,$2      ; DICR per-channel enable bit (16+channel)
4ec:  lui $2,0x80 ; or $7,$7,$2                             ; DICR bit 23 (IRQ master ENABLE — see below)
524:  sw $2,0($3)                                             ; DICR |= computed bits (channel enable + master)
548:  sw $5,0($6)                                             ; DICR2, read-modify-write with the flag byte masked off (no bits change for this range unless an internal-only flag is set — never observed set by any traced caller)
558:  lw $2,0($3) ; ori $2,$2,8 ; sw $2,0($3)                  ; I_MASK |= 1<<3 (IOP_IRQ_DMA)
```

**The "master enable" bit is 23, not 31.** `lui $2,0x80` computes
`0x00800000`, not `0x80000000` — this document's first pass mislabeled the
identical `INTRMANP` sequence (`<outdir>/INTRMANP.text` `0x510`: also
`lui $2,0x80`) as "DICR bit 31"; re-checked by direct instruction decode, it
is bit 23. Bit 31 of `DICR` is a separate, read-only *master flag* bit (the
OR of all enabled channel flags) that neither variant's `EnableIntr` ever
writes — consistent with `DICR`'s own force-IRQ bit 15 and per-channel flag
bits 24–30 being distinct fields from the two bits `EnableIntr` actually
touches (16+channel, and 23).

For `0x28<=irq<0x2e` (bank 2, `564: addiu $4,$16,-0x28 ; sltiu $2,$4,0x6`,
i.e. exactly the 6 channels `IOP_IRQ_DMA_SPU2`..`IOP_IRQ_DMA_SIO2_OUT`
[header]):

```
590:  addiu $2,$5,0x10 ; sllv $2,1,$2 ; or $7,$7,$2      ; DICR2 per-channel enable bit (16+channel, channel=irq-0x28)
5dc:  sw $2,0($3)                                          ; DICR2 |= channel-enable bit (mask preserves DICR2's other bits/flags)
5ec:  or $2,$2,$3 ; sw $2,0($4)                              ; DICR (not DICR2) |= bit 23 — same master-enable bit as bank 1
600:  sw $2,0($5)                                             ; I_MASK |= 1<<3 — the SAME umbrella bit as bank 1
```

So `DICR2` gets its own per-channel enable bit (bits 16–21 for the 6 bank-2
channels, mirroring `DICR`'s own 16–22 layout), but the *master* enable stays
in `DICR` bit 23 and `I_MASK` bit 3 stays the single umbrella for both banks —
there is no separate `DICR2` master bit or `I_MASK` bit for bank 2. `irq==0x27`
itself satisfies neither range check and falls through to the error path
(`-0x65`) — a genuine gap between the two banks' enable-able ranges, distinct
from the storage gap noted in §2.2.

`DisableIntr` (`0x638`) is the structural mirror, now over the *same* full
range: clears the `I_MASK` bit directly for `irq<0x20`; for `0x20<=irq<0x27`
clears `DICR`'s channel-enable bit and reports via `*res` (bit `0x100` of the
accumulator written to `*res` at `0x784`) whether that channel's own flag bit
was *also* set at the time; for `0x28<=irq<0x2e` clears `DICR2`'s
channel-enable bit and reports via the parallel `0x200` bit.

`DisableIntr` has the identical `0x27` gap for the same reason (`0x6a8:
slti $2,$16,0x27` routes anything `>=0x27` to the bank-2 check
`0x70c: addiu $2,$16,-0x28 ; sltiu $2,$2,0x6`, which `irq==0x27` also fails) —
so both functions agree on exactly which `irq` values are enable/disable-able,
even though `RegisterIntrHandler`'s storage buckets (§2.2) are coarser.

### 2.4 `QueryIntrContext`/`QueryIntrStack`: no flag, a stack-pointer range check — unchanged from `INTRMANP`

```
d70:  move $4,$sp
d74:  lui $3,0 ; addiu $3,$3,0x1e00       ; top of the interrupt stack
d7c:  sltu $2,$4,$3 ; beqz $2,0xd90
d88:  addiu $3,$3,-0x800 ; sltu $2,$3,$4    ; bottom = top-0x800
```

`QueryIntrContext()` (ordinal 23, `0xd70`) is `QueryIntrStack($sp)`: `0xd70`
sets `$4=$sp` and falls straight through into `0xd74`'s body (ordinal 24's own
entry point, taking `$4` as an explicit argument) — the exact same
fallthrough-into-the-next-ordinal shape `INTRMANP` uses at `0xa30`/`0xa34`.
Both return whether the given pointer lies within the fixed 0x800-byte
interrupt stack, `[0x1600, 0x1e00)` on this variant (`INTRMANP`'s is
`[0x1290, 0x1a90)`, §2.4's first pass). There is no separate "in interrupt"
flag — the check is purely `$sp`-range-based — and this is what the
dispatcher's nesting check calls (§3.5), unchanged from `INTRMANP`.

**Ordinal 25 (`iCatchMultiIntr`, `0xd98`) is a separate function that
resembles this range check but is not it.** It repeats a similar-looking
`[top-0x800, top)` test, then — only when the pointer falls in a *narrower*
sub-range near the top of that stack (`dbc: addiu $3,$3,0x160 ; dc0: sltu
$2,$3,$sp`) — additionally reads `Status` (`mfc0 $2,$12`) and, if bit 0
(`IEc`) is clear, sets it (`ori $3,$2,1 ; mtc0 $3,$12`) before returning. This
was not called from anywhere this pass traced (the dispatcher's own nesting
check goes through ordinal 24, not 25, §3.5); what triggers a call to
`iCatchMultiIntr`, and what the conditional `IEc` set is for, remain
undecoded — see §5.

### 2.5 `CpuSuspendIntr`/`CpuResumeIntr`/`CpuEnableIntr`/`CpuDisableIntr`: a new `I_CTRL` gate, alongside the same `Status` IEp/IEo mechanism

`INTRMANI` introduces a second hardware register these four ordinals manage,
`I_CTRL` at `0xBF801078` — a plain memory-mapped word, not part of COP0, that
the interrupt dispatcher itself also saves and restores around every dispatch
(§3.2). Three small internal helpers back it:

```
3dc:  lui $2,0xbf80 ; ori $2,$2,0x1078 ; lw $2,0($2) ; jr $ra            ; ord.12: raw read
3f0:  lui $3,0xbf80 ; ori $3,$3,0x1078 ; addiu $2,$0,1 ; sw $2,0($3) ; jr $ra   ; ord.13: write 1
374:  lui $2,0xbf80 ; ori $2,$2,0x1078 ; sw $4,0($2) ; jr $ra                    ; ord.18 (CpuResumeIntr): write caller's value
```

`CpuSuspendIntr` (ord. 17, `0x348`) reads `I_CTRL`, stores it to `*state` if
non-null, and returns `-0x66` if the value read was already 0 — but, checked
instruction-by-instruction, **never itself writes `I_CTRL`**. `CpuDisableIntr`
(ord. 8, `0x388`) calls only ord. 12 (the raw read) and does the same
zero-check — also no write. `CpuResumeIntr` (ord. 18) writes back whatever
`I_CTRL` value it is passed. Only `CpuEnableIntr` (ord. 9, `0x3b4`) writes
`I_CTRL` at all, and it does so *in addition to* the syscall-based mechanism:

```
3bc:  jal 0x418     ; ord.11's syscall 8 trampoline — the SAME Status IEp/IEo/Im2 path INTRMANP uses
3c4:  jal 0x3f0      ; ord.13 — I_CTRL = 1
```

So on this variant, turning interrupts *on* touches both gates, but the
`CpuDisableIntr`/`CpuSuspendIntr` bodies this pass read touch neither the
`Status` syscalls nor an `I_CTRL`-clearing write — they only validate and
save. Whether disabling really happens through some other path (a use of
`I_CTRL` this pass didn't find, or reliance on the dispatcher's own
save/force-disable at return, §3.5) or is genuinely asymmetric on this variant
was not settled; noted in §5.

The `Status` IEp/IEo mechanism itself is unchanged from `INTRMANP` (§0's first
pass) — it lives in the SYSCALL exception handler `INTRMANI` registers for
cause 8 (§3.1, module offset `0x1300`), dispatched by syscall number to case
bodies at `0x1390`–`0x13e8` that are **bit-for-bit identical** to `INTRMANP`'s
`0x1020`–`0x1088` (same `0x414`/`-0x405`/`-0x415`/`0x404` constants):

```
1390:  lw $8,0x408($0)          ; saved Status
1394:  addiu $9,zero,0x414 ; and $2,$8,$9        ; (Status & (IEp|IEo|Im2)) -> old-state result
13a4:  addiu $1,zero,-0x405 ; and $8,$8,$1       ; syscall 4 (CpuDisableIntr's trampoline): clear IEp(bit2)+Im2(bit10)
13ac-13b8:  addiu $9,zero,-0x415 ; and $8,$8,$9 ; or $8,$8,$4    ; syscall 0x14 (CpuResumeIntr's trampoline): clear, then OR in caller's saved state
13bc-13c4:  ori $8,$8,0x404                                         ; syscall 8 (CpuEnableIntr): set IEp+Im2 unconditionally
13e8:  mtc0 $8,$12                                                  ; write Status back
```

`0x414`/`0x404` are the same bits 2 (`IEp`), 4 (`IEo`), 10 (`Im2`) `INTRMANP`
uses, and the same "edit the *previous* KU/IE stack level, let the syscall's
own return promote it" logic applies (§0's first pass, unchanged). The
difference from `INTRMANP` is purely which ordinals reach this handler:
`INTRMANI`'s own `CpuSuspendIntr`/`CpuDisableIntr`/`CpuResumeIntr` (17/8/18)
never call any of the four syscall trampolines — only `CpuEnableIntr` and
`CpuInvokeInKmode` (ord. 14, a bare `syscall 0xc`, `0x448`, unchanged in
shape from `INTRMANP`) do.

## 3. The delivery path

### 3.1 What INTRMAN registers with EXCEPMAN, at boot

`INTRMANI`'s entry (before any of its exported functions can be called) makes
the same three `EXCEPMAN` calls, at the same cause numbers and priority,
`INTRMANP` does — only the handler addresses differ:

```
9c:  jal   0x1558      ; RegisterExceptionHandler(exception=0 /* INT */, handler=&[0xe04])
b0:  jal   0x1560      ; RegisterPriorityExceptionHandler(exception=0, priority=3, handler=&[0x12d8])
c0:  jal   0x1558      ; RegisterExceptionHandler(exception=8 /* SYS */, handler=&[0x1300])
```

So the **External Interrupt** exception (`IOP_EXCEPTION_INT`=0 [header]) still
gets two handlers chained onto it — the main dispatcher at module offset
`0xe04` (implicit priority 2) and a second, higher-priority (3) handler at
`0x12d8` — and the **SYSCALL** exception (cause 8) still gets the handler at
`0x1300` that backs the `Status` IEp/IEo mechanism (§2.5). `INTRMANI` also
calls its own `RegisterIntrHandler(irq=3 /* IOP_IRQ_DMA */, mode=1,
handler=&[0x9a8], arg=0)` (`0xd0`–`0xdc`) — the DMA-channel sub-dispatcher,
now covering both banks (§3.4). Since `EXCEPMAN` itself is unchanged (§1 —
`INTRMANI` registers with it the same way `INTRMANP` does, same cause
numbers, same priority), no change to §1 was needed.

The priority-3 handler at `0x12d8` is the same shape as `INTRMANP`'s `0xf60`:
a zero (`nop`-encoded) `next`/`info` header and, at `funccode` (`0x12d8+8 =
0x12e0`), four straight-line instructions restoring `$at`/`Status` from
`[0x400]`/`[0x408]` and `jr`-ing to `[0x404]` (`EPC`) — an unconditional,
minimal exception-return with no branch and no DMA-table involvement, no more
decoded on this variant than on `INTRMANP` (§5).

### 3.2 Vector → EXCEPMAN → the main dispatcher: what's saved, where, and the nested-interrupt case

The general vector (`§1.3`, destination `0x80`) has already saved `$at`,
`EPC`, `Status`, `Cause` to fixed cells `0x400`–`0x40c` before `jr`-ing into
`EXCEPMAN`'s cause-0 chain, which (for a fresh boot, before `0x12d8`'s handler
is understood, §3.1) lands on `INTRMANI`'s `0xe04` struct's `funccode` at
`0xe0c`:

```
e0c:  addiu $sp,$sp,-0x98
e10:  lw $1,0x400($0)                 ; $at, as EXCEPMAN's vector saved it
e18-e30:  sw $1,4(sp) ... sw $7,0x1c(sp)     ; $at,$2,$3,$4-$7 (mode-0 baseline, always)
e3c-e58:  mfhi/mflo, Status, EPC saved into the frame too
e5c-e70:  lw $3,0(I_CTRL) ; sw $3,0x90(sp) ; sw 1,0(I_CTRL)    ; save I_CTRL, then force it to 1 (§2.5)
e74-e7c:  lui $2,0xac00 ; ori $2,$2,0xfe ; sw $2,0(sp)     ; frame marker = 0xac0000fe (mode-0)
e80:  jal 0xd70                       ; QueryIntrContext()  (ordinal 23, §2.4 — same call INTRMANP makes at the equivalent point)
e88:  beqz $2,0xea0
e90-e9c:  (nested) sp -= 0x18 on the CURRENT stack; old sp saved at frame+0x14
ea0-eac:  (fresh)  sp = fixed interrupt-stack top (module offset 0x1de0); old sp saved at frame+0x14
```

The structure — mode-0 baseline save, frame marker, nested-vs-fresh stack
switch keyed off the same stack-range check — is unchanged from `INTRMANP`
(only the fixed addresses move: interrupt-stack top `0x1de0` here vs
`INTRMANP`'s `0x1a70`, matching §2.4's `0x1e00`/`0x1a90` pair once the 0x800
offset used inside the frame-switch code is accounted for). The one genuine
addition is `I_CTRL`: the dispatcher reads it, saves the old value into the
frame (`+0x90`), and unconditionally writes `1` back, *before* deciding
nested vs. fresh — i.e. every dispatch, nested or not, re-affirms `I_CTRL=1`
for its own duration and restores the saved value on the way out (§3.5). This
mirrors `Status`/`EPC` being saved into the same frame; `INTRMANP` has no
equivalent step because it has no `I_CTRL` register at all (§2.5).

So a **nested** interrupt (one that fires while `$sp` is already inside the
interrupt-stack range, §2.4) does *not* switch stacks again — it just grows
the existing interrupt-stack frame by `0x18` bytes. A **fresh** interrupt
switches to the dedicated interrupt-stack top. Register preservation beyond
the mode-0 baseline is deferred until a handler is actually found and its
packed `mode` bits are known (§3.4) — `$8`-`$15`, `$24`,`$25`,`gp`,`fp` for
mode≥1 (frame marker becomes `0xff00fffe`), plus `$16`-`$23` for mode≥2
(marker becomes `-2`) — identical convention to `INTRMANP`.

Before the source scan, `Cause` bits 8/9 (`IP0`/`IP1`, the two software
interrupt lines) are checked first (`sll`/`srl`-shifted test at `eb8`-`ec0`)
and routed to a separate path (`0xf8c`, §3.3) if set; otherwise control falls
into the hardware scan.

### 3.3 Finding the source: I_STAT & I_MASK & software-mask, ack before the handler runs

```
ec8:  lui $26,0xbf80 ; ori $26,$26,0x1070      ; I_STAT
ed0:  lw $27,4($26)                              ; I_MASK  (+4 = 0x1074)
ed4:  lw $4,0($26)                                ; I_STAT
ed8:  lw $5,0x41c($0)                              ; software overlay mask, init -1
edc:  and $4,$4,$27 ; and $4,$4,$5                  ; pending & enabled & ~software-disabled
ee4:  beq $0,$4,0xf4c                                ; nothing pending -> chain onward (§3.5)
```

Identical in every respect to `INTRMANP`'s scan, just at `INTRMANI`'s own
offsets. `[0x41c]` is still never touched by `EnableIntr`/`DisableIntr`
(§2.3) — it is `DisableDispatchIntr`/`EnableDispatchIntr`'s target
(ordinals 15/16, `0x854`/`0x7b4`) for `irq<0x20`; those two functions are now
fully decoded (they also manage the two `masked_icr_1`/`masked_icr_2`
software overlays for `0x20`–`0x27` and `0x28`–`0x2d`, read by the DMA
sub-dispatch below — §3.4).

The pending set is then scanned for its **lowest set bit** — the same
nibble-at-a-time scan with the packed 4-bit lookup constant `0x01020103` —
i.e. ascending IRQ-number priority, unchanged from `INTRMANP`. Having the
index (`$2`):

```
f1c:  addiu $6,zero,1 ; sllv $6,$6,$2 ; sw $6,0x10(sp)   ; save (1<<irq)
f28:  not $6,$6 ; and $7,$6,$27 ; sw $7,4($26)             ; I_MASK &= ~(1<<irq)  -- mask it off
f34:  sw $6,0($26)                                          ; I_STAT: write-0-to-this-bit, 1s elsewhere
f3c:  lw $7,0x480($6') ; lw $4,0x484($6')                    ; table[irq].handler|mode, .arg
f44:  bne $0,$7,0xfcc                                         ; handler registered? call it
```

Same write-0-to-the-target-bit acknowledgement, same ordering (ack, then
temporary `I_MASK` mask-off, before the handler is looked up or called), same
silent-drop behaviour for an unregistered pending bit — unchanged from
`INTRMANP`.

The software-interrupt path (`0xf8c`, reached when `Cause.IP0`/`IP1` were
set) is structurally parallel but acknowledges by clearing the bit directly
in `Cause` and reads its handler from the same separate 2-entry table at
absolute `0x5f0` (`0x400 + irq*8` for `irq∈{0x3e,0x3f}`, §2.2) rather than
`0x480` — unchanged from `INTRMANP`.

### 3.4 DMA channel sub-dispatch: `IOP_IRQ_DMA` (irq 3) walks *both* DICR and DICR2, indices `0x20`–`0x26` and `0x28`–`0x2d`

`INTRMANI`'s own handler for irq 3 (registered at boot, §3.1, module offset
`0x9a8`) is what services the "combined" `IOP_IRQ_DMA` cause, and — unlike
`INTRMANP`'s version — it genuinely scans both DMAC banks. At entry it loads
both software overlays from the internals struct (`0x15f4`/`0x15f8`, §2.2)
and reads both hardware registers:

```
9dc:  lw $6,0x0($2) ; ... sw $6,0x10(sp)      ; masked_icr_1  (struct+4)
9e8:  lw $2,0x4($2) ; ... sw $2,0x14(sp)      ; masked_icr_2  (struct+8)
a1c:  lw $2,0($DICR2) ; lw $6,0x14(sp)          ; DICR2, masked_icr_2
a24:  lw $3,0($DICR)  ; lw $6,0x10(sp)          ; DICR,  masked_icr_1
a28:  and $2,$2,$6 ; srl $2,$2,0x18 ; andi $23,$2,0x3f    ; DICR2 bits 24-29 & mask -> 6 bank-2 channel flags
a38:  and $17,$3,$6 ; srl $3,$17,0xf                        ; DICR bit 15 (force/BERR-class)
a40:  srl $2,$17,0x18 ; andi $17,$2,0x7f                     ; DICR bits 24-30 & mask -> 7 bank-1 channel flags
```

If neither bank has a pending flag and the force bit is clear, control jumps
straight to the completion tail (`0xc60`, below); otherwise a force-bit branch
(`0xa60`–`0xa90`) runs first, clearing `DICR`'s force bit and — if a callback
pointer at the fixed absolute cells `0x5b8`/`0x5bc` is non-null — calling it
(not traced further; likely a hook for whatever installs a forced DMA IRQ, out
of scope here).

**Bank 1** (`0xaac`–`0xb84`, reached when `$17!=0`) loops over the 7 channels
exactly as `INTRMANP`'s handler does: for each set flag bit, it computes the
channel's `DICR` enable-bit position, acks that channel's flag in `DICR`, then
reads a handler/arg pair from **`0x580 + N*8`** — `0x480 + (0x20+N)*8`, i.e.
`table[0x20]`..`table[0x26]` — and calls `handler(arg)` if non-null, *after*
the ack (same ack-before-handler order as the main dispatcher, §3.3).

**Bank 2** (`0xb84`–`0xc58`, reached when `$23!=0`, or once bank 1's loop is
exhausted) is the new code: it loops over the 6 bank-2 channels the same way,
but through `DICR2` instead of `DICR`:

```
bc0:  lw $3,0($DICR2) ; and $3,$3,$2 ; sw $3,0($DICR2)     ; ack this channel's DICR2 flag bit (mask-preserve the rest)
bc4:  lw $16,0x5c0($17)                                       ; table[0x28+channel].handler   ($17 = channel*8)
be0:  lw $4,0x5c4($17)                                         ; table[0x28+channel].arg
be4:  jalr $16                                                  ; handler(arg), if non-null
```

`0x5c0` is exactly `0x480 + 0x28*8` — **the same `0x480` table, at indices
`0x28`–`0x2d`**, reached by channel index `irq-0x28` for `irq` in that range.
Ack precedes the handler call here too, matching bank 1 and the main scan.
Once both banks are drained, the whole thing loops back to re-read `DICR`/
`DICR2` (`0xa10`) — so a channel that re-asserts its flag while a sibling
handler is still running gets picked up again before this irq-3 dispatch
returns, rather than waiting for the next hardware edge. The tail (`0xc60`
onward) busy-waits for `DICR`'s bit 31 (the read-only master OR-of-flags bit)
to clear, re-asserts `DICR`'s bit 23 (the master *enable* bit, §2.3 — cheap
insurance that acking channel flags didn't clobber it), and returns 1
(`0xd14: addiu $2,zero,1`), exactly like `INTRMANP`'s equivalent handler.

**Consequence: `IOP_IRQ_DMA_SIF0`/`SIF1` (irq `0x2a`/`0x2b` [header]) are live
on the resident variant.** `irq=0x2a` is bank-2 channel `0x2a-0x28=2`;
`irq=0x2b` is channel 3 — both squarely inside the 6-channel bank-2 loop
above, indexing `table[0x2a]`/`table[0x2b]`. This overturns this document's
own first-pass conclusion (§3.6, below), which was reached against
`INTRMANP` — the variant that turns out not to be resident (§0). `EnableIntr`
(§2.3) also now genuinely arms `DICR2`'s channel-enable bit for `0x2a`/`0x2b`
and `DICR`'s master-enable bit, where on `INTRMANP` it was a silent no-op for
`irq>=0x27`. So on `INTRMANI`, `RegisterIntrHandler(0x2a/0x2b, ...)` +
`EnableIntr(0x2a/0x2b)` — exactly what `SIFMAN`/`SIFCMD` do at boot,
unchanged from `docs/analysis/34`'s and this document's own §3.6 sighting —
is a complete, reachable, hardware-armed registration. §3.6 below traces the
full path from vector to `SIFCMD`'s registered handler.

### 3.5 Calling the handler, its return value, and the reschedule hook

```
fcc:  sll $6,$7,0x1e ; bnez $6,0xff0         ; packed mode bits != 0 ?
fd8:  srl $7,7,2 ; sll $7,7,2                  ; strip mode bits -> clean function pointer
fe0:  jalr $7                                    ; call handler(arg)   ($4 = table[irq].arg, untouched since f40)
...
109c:  lw $7,0x10(sp)                              ; the (1<<irq) bit saved at f24
10a0:  mtc0 $0,$12                                  ; Status = 0 (force disabled) before restoring I_MASK
10a8:  beq $0,$2,0x10c0                               ; handler's $v0 == 0 ?
10b0:  lw $6,0($I_MASK) ; or $7,$7,$6 ; sw $7,0($I_MASK)  ; else: I_MASK |= (1<<irq) again
```

Unchanged from `INTRMANP`: **the handler's return value gates whether
`INTRMAN` re-enables the IRQ it just masked off**, mode≥1/2 callers get
progressively more of the register file saved and a fixed kernel `$gp`
supplied before the same `jalr`, and the frame's marker word records which
mode ran.

After I_MASK handling, `INTRMANI` checks whether the *interrupted* context
was itself inside the interrupt-stack range — i.e. whether this whole
dispatch was nested:

```
10c0:  lw $4,0x14(sp)          ; the old-sp pointer
10c4:  jal 0xd74                 ; QueryIntrStack(old_sp)  -- ordinal 24, §2.4, same plain range check as INTRMANP
10cc:  bnez $2,0x1184              ; nested -> skip straight to restore
10d4:  lw $3,0x15a4($0) ; jalr $3   ; else: call the ShouldPreemptCb hook
10ec:  beqz $2,0x1184                 ; hook says "no" -> restore normally
10f4-1168:  (mode-dependent) top up the interrupted context's saved-register set to full
1170:  lw $3,0x15a0($0) ; jalr $3 ; move $4,$2                          ; call the NewCtxCb hook
1184:  move $sp,$4                                                        ; resume from whichever frame resulted
```

Unchanged from `INTRMANP`: this nesting check calls the plain
`QueryIntrStack(old_sp)` (ordinal 24), not `iCatchMultiIntr` (§2.4's newly
partially-decoded ordinal 25) — so §2.4's `IEc`-setting behaviour is not on
this particular path.

`[0x15a4]`/`[0x15a0]` are exactly `SetShouldPreemptCb`/`SetNewCtxCb`'s storage
cells (ordinals 30/28, §2.1) — the same `THREADMAN`-only hook mechanism as
`INTRMANP`, unchanged in every other respect: a non-nested interrupt asks
`ShouldPreemptCb`, and if due, finishes saving the interrupted context and
hands off to `NewCtxCb`; a nested interrupt never calls either hook.

The tail (`0x1204`–`0x125c`, mode-0 case; `0x1268`–`0x12bc` for mode≥2)
restores `I_CTRL` from the frame (`+0x90`, §3.2) *before* restoring the saved
registers and `Status` — the one step with no `INTRMANP` analogue — then
restores `Status` (bit 0 cleared before the write, same as `INTRMANP`) and
`jr`s to the saved `EPC` with `$sp` restored from the frame's saved old-`sp`
in the delay slot. No `rfe` opcode appears anywhere in this path, same as
`INTRMANP` (§5).

### 3.6 The second DMAC bank: the vector-to-`SIFCMD` path for irq 0x2a/0x2b

This traces the second DMA controller bank (channels 7–13, ps2sdk's
`IOP_IRQ_DMA_SPU2`..`IOP_IRQ_DMA_SIO2_OUT` = irq `0x28`–`0x2d` [header]) across
`INTRMANI`, `DMACMAN`, `SIFMAN`, and `SIFCMD` — the resident variant this time
(§0) — and follows the complete path a real `SIF1` completion takes: vector →
`EXCEPMAN` → `INTRMANI`'s main dispatcher → its irq-3 sub-dispatch → `SIFCMD`'s
registered handler. It also corrects this document's own earlier working
assumption about where the bank's interrupt-control register actually sits.

**The register map, established from `DMACMAN`'s own pointer table, not
inferred.** `DMACMAN` exports raw accessors for each register (`dmac_set_dicr2`
etc. [header], ordinals 14–27) that all follow the same shape — load a pointer
out of a small data table, then `lw`/`sw` through it:

```
9e8:  lui $2,0x0 ; lw $2,0x1c18($2) ; sw $4,0x0($2)   ; dmac_set_dpcr  (ord.14)
a18:  lui $2,0x0 ; lw $2,0x1c1c($2) ; sw $4,0x0($2)   ; dmac_set_dpcr2 (ord.16)
a48:  lui $2,0x0 ; lw $2,0x1c20($2) ; sw $4,0x0($2)   ; dmac_set_dpcr3 (ord.18)
a78:  lui $2,0x0 ; lw $2,0x1c24($2) ; sw $4,0x0($2)   ; dmac_set_dicr  (ord.20)
aa8:  lui $2,0x0 ; lw $2,0x1c28($2) ; sw $4,0x0($2)   ; dmac_set_dicr2 (ord.22)
ad8:  lui $2,0x0 ; lw $2,0x1c2c($2) ; sw $4,0x0($2)   ; dmac_set_BF80157C (ord.24, unnamed in ps2sdk)
b08:  lui $2,0x0 ; lw $2,0x1c30($2) ; sw $4,0x0($2)   ; dmac_set_BF801578 (ord.26, unnamed in ps2sdk)
```

Reading the pointer table's actual words out of `<outdir>/DMACMAN.text` (it's
baked-in `.data`, no runtime init needed — `DMACMAN`'s `bss` is 0 bytes):

| cell | value | register |
|---|---|---|
| `0x1c18` | `0xBF8010F0` | `DPCR` (bank 1 priority) |
| `0x1c1c` | `0xBF801570` | `DPCR2` (bank 2 priority) |
| `0x1c20` | `0xBF8015F0` | `DPCR3` (a *third* priority register, presumably channels 13–15) |
| `0x1c24` | `0xBF8010F4` | `DICR` (bank 1 interrupt control) |
| `0x1c28` | `0xBF801574` | **`DICR2`** (bank 2 interrupt control) |
| `0x1c2c` | `0xBF80157C` | unnamed (`dmac_set_BF80157C`) |
| `0x1c30` | `0xBF801578` | unnamed (`dmac_set_BF801578`) |

So `0xBF801570` is **`DPCR2`, not `DICR2`** — `docs/analysis/34` already had
this right (`0xbf801570 |= 0x8800` was correctly read as `DPCR2`, the
*priority*-enable register); this document's own task framing, and by
extension `06`'s shorthand "`0xBF8015xx`", was imprecise on that point. The
real `DICR2` is `0xBF801574`, four bytes further in — exactly where `DICR`
sits relative to `DPCR` in bank 1 (`0x10F4` vs `0x10F0`), the same
+4-byte pairing repeated for bank 2. `0x1578`/`0x157C` are two more registers
in the same cluster that even ps2sdk's own header has never named.

**`INTRMANP` — the non-resident variant (§0) — touches none of it; `INTRMANI`
— the resident one — uses all four addresses.** Grepping
`0x1570`/`0x1574`/`0x1578`/`0x157C` as immediate operands across
`<outdir>/INTRMANP.text` gives zero hits for every one of the four; the same
grep against `<outdir>/INTRMANI.text` (§2.2's init clearing `DICR2`, §2.3's
`EnableIntr`/`DisableIntr`, §3.4's irq-3 handler, and the `0x1578` gate in
§2.5's discussion of ordinal 27) hits repeatedly. `06`'s "`INTRMANP` never
references the `0xBF8015xx` bank" is confirmed address-by-address, and is now
understood as a property of the *non-resident* variant.

**`DMACMAN` writes `DPCR2`/`DPCR3`/`0x1578` at boot, but never `DICR2`, and
never touches `INTRMAN`.** `DMACMAN`'s entry (`<outdir>/DMACMAN.text` `0xb38`)
initializes all three priority registers with a fixed per-channel priority
pattern:

```
b60-b7c:  dmac_set_dpcr(0x07777777); dmac_set_dpcr2(0x07777777); dmac_set_dpcr3(0x777)
b84-bb0:  loop channel=0..0xc: dmac_ch_set_madr/bcr/chcr(channel, 0)   ; clears MADR/BCR/CHCR, channels 0-12
bb4-be8:  dmac_ch_set_tadr(4,0); dmac_ch_set_tadr(9,0);
          dmac_set_4_9_a(4,0); dmac_set_4_9_a(9,0); dmac_set_4_9_a(0xa,0)
bf0:      dmac_set_BF801578(1)
```

— explicitly special-casing channels 4 (`SPU`), 9 (`SIF0`), and 10 (`SIF1`),
the linked-list-capable channels, and setting the unnamed `0x1578` register to
`1`. `DICR2` itself is never written by `DMACMAN`'s own code — `dmac_set_dicr2`
(ordinal 22) exists only as an accessor for *other* modules to call, and grep
across `<outdir>/SIFMAN.text` and `<outdir>/SIFCMD.text` found no call to any
`dmacman` ordinal at all — neither module imports `dmacman`. `DMACMAN`'s
`intrman` imports are exactly `[17, 18]` (`CpuSuspendIntr`/`CpuResumeIntr`,
confirming `06`'s finding) — it never calls `RegisterIntrHandler`/`EnableIntr`
and never writes `INTRMAN`'s handler table at `0x480`/`0x5f0` directly (no
`0x480` or `0x5f0` immediate appears anywhere in `<outdir>/DMACMAN.text`,
regardless of which variant is resident — that table's *address* is fixed
either way, §2.2). `DMACMAN` is purely a register-poking accessor library; it
has no opinion at all about how (or whether) DMA completion reaches a
handler.

**`SIFMAN` and `SIFCMD` both register real handlers for `0x2a`/`0x2b`, with
`EnableIntr`, at boot.** `SIFMAN::sceSifInit` (`0x148`, `docs/analysis/34`'s
BOOT-10 handshake) sets `DPCR2 |= 0x8800` (§1 of `34`, a priority/channel-enable
action — never touches `DICR2`) and then, at `0x1d4`, calls an unexported
internal helper at `0x460`:

```
490-4b8 (SIFMAN.text 0x460's tail):
  $4=0x2a ; $5=1 ; $6=&[0x364] ; $7=$16-4 (=&[0xf80])
  jal RegisterIntrHandler          ; RegisterIntrHandler(0x2a, mode=1, handler=&[0x364], arg=&[0xf80])
  $4=0x2a
  jal EnableIntr                    ; EnableIntr(0x2a)
```

`SIFMAN`'s own exported ordinal 2 (`0x268`) is the exact mirror teardown:
`DisableIntr(0x2a, &res)` then `ReleaseIntrHandler(0x2a)`, confirming `0x2a`
is a real, intentional registration, not leftover debug code. `SIFCMD`'s
module entry (`0xd0`, not a separate `InitCmd` call — the whole span `0xd0`–
`0x290` is one function, containing only a single `jr $ra` at `0x28c`) does
the parallel thing for `0x2b` immediately after zeroing its 32-slot
`CmdHandler`/`SysCmdHandler` tables:

```
238-264 (SIFCMD.text, inside the entry function):
  $4=0x2b (RegisterIntrHandler's irq)
  ... $6=&[0x590] (handler)
  jal RegisterIntrHandler           ; RegisterIntrHandler(0x2b, mode=1, handler=&[0x590], arg=$17)
  jal EnableIntr                     ; EnableIntr(0x2b)   $4=0x2b
```

confirming and completing `docs/analysis/34`'s single-call-site sighting.
`SIFCMD`'s registered handler at `0x590` is a genuine SIF-RPC command
dispatcher: it reads a ready-flag byte from a queue header, copies the
command's payload words into a local buffer, then looks the command ID up in
whichever of two tables `AddCmdHandler`/`AddSysCmdHandler` populate and calls
`handler(cmd, userdata)` — exactly the shape you'd want an `IOP_IRQ_DMA_SIF1`
completion handler to have. `SIFMAN`'s `0x364` is the function `docs/analysis/37`'s
earlier pass (§3.4) already partly read: it calls through the callback cell
`SetDmaIntrHandler`/`ResetDmaIntrHandler` (`0x33c`/`0x350`) write — confirmed
here by address arithmetic, since `0x364`'s argument struct base is
`&[0xf80]` and it reads `0x40c($16)`/`0x410($16)`, landing at `0xf80+0x40c =
0x138c` and `0xf80+0x410 = 0x1390`, the exact two cells `SetDmaIntrHandler`
writes.

**On the resident variant, `RegisterIntrHandler`/`EnableIntr` accept the whole
`0x28`–`0x2d` range and genuinely arm the hardware.** §2.2/§2.3 already
established this instruction-by-instruction: `RegisterIntrHandler`'s storage
buckets put `0x2a`/`0x2b` in `table[irq] = 0x480 + irq*8` with no mode bits
packed (so, same as on `INTRMANP`, `SIFMAN`'s and `SIFCMD`'s handlers always
run as mode 0 regardless of the `mode=1` they pass), and `EnableIntr`'s
`0x28<=irq<0x2e` branch sets `DICR2`'s own channel-enable bit *and* `DICR`'s
master-enable bit (bit 23) *and* `I_MASK`'s bit 3 — none of which is a no-op
on this variant. So `SIFMAN`'s `EnableIntr(0x2a)` and `SIFCMD`'s
`EnableIntr(0x2b)` calls (both traced below) both succeed *and* arm real
hardware state, unlike on `INTRMANP` (§2.3, §3.4).

**The full path, vector to handler, naming every function:**

1. A `SIF1` DMA completion sets `DICR2`'s flag bit for channel `0x2b-0x28=3`
   and asserts the shared physical cause; `I_STAT` bit 3 (`IOP_IRQ_DMA`)
   goes pending.
2. The R3000 traps through the general vector (`§1.3`, destination `0x80`),
   which saves `$at`/`EPC`/`Status`/`Cause` and `jr`s into `EXCEPMAN`'s
   cause-0 chain (§1.2).
3. That chain's priority-2 node is `INTRMANI`'s main dispatcher, registered
   at boot (§3.1) and entered at module offset `0xe0c` (§3.2) — it saves the
   mode-0 register baseline, saves and re-arms `I_CTRL`, and switches to the
   interrupt stack.
4. The dispatcher's I_STAT scan (§3.3) finds bit 3 set, acknowledges it
   (write-0-to-the-bit), masks it off in `I_MASK`, and reads `table[3]` —
   `INTRMANI`'s own irq-3 registration (§3.1, `RegisterIntrHandler(3, mode=1,
   handler=&[0x9a8], arg=0)`) — finding a handler, and calls it.
5. `INTRMANI`'s irq-3 sub-dispatch (§3.4, module offset `0x9a8`) reads
   `DICR`/`DICR2` masked by `masked_icr_1`/`masked_icr_2`, finds bank 2's
   channel-3 flag set, acknowledges it in `DICR2`, and reads
   `table[0x28+3] = table[0x2b]` — exactly the slot `SIFCMD`'s module entry
   populated (below) — and calls `handler(arg)`.
6. That handler is `SIFCMD`'s own registered function at `<outdir>/SIFCMD.text`
   offset `0x590` — a SIF-RPC command dispatcher: it reads a ready-flag byte
   from a queue header, copies the command's payload words into a local
   buffer, looks the command ID up in whichever of the two tables
   `AddCmdHandler`/`AddSysCmdHandler` populate, and calls
   `handler(cmd, userdata)`.
7. Control returns up through `INTRMANI`'s irq-3 dispatch (re-scanning both
   banks before returning 1, §3.4), then through the main dispatcher's
   I_MASK re-enable, nesting check, and `THREADMAN` reschedule hooks (§3.5),
   and finally back to the interrupted context via the manual `Status`-write-
   plus-`jr` return.

**How `SIFMAN` and `SIFCMD` populate `table[0x2a]`/`table[0x2b]` at boot** —
unchanged from this document's first pass, since neither file's own code
depends on which `INTRMAN` variant is resident (only the ordinals they call
through do). `SIFMAN::sceSifInit` (`0x148`, `docs/analysis/34`'s BOOT-10
handshake) sets `DPCR2 |= 0x8800` (a priority/channel-enable action — never
touches `DICR2` directly) and then, at `0x1d4`, calls an unexported internal
helper at `0x460`:

```
490-4b8 (SIFMAN.text 0x460's tail):
  $4=0x2a ; $5=1 ; $6=&[0x364] ; $7=$16-4 (=&[0xf80])
  jal RegisterIntrHandler          ; RegisterIntrHandler(0x2a, mode=1, handler=&[0x364], arg=&[0xf80])
  $4=0x2a
  jal EnableIntr                    ; EnableIntr(0x2a)
```

`SIFMAN`'s own exported ordinal 2 (`0x268`) is the exact mirror teardown:
`DisableIntr(0x2a, &res)` then `ReleaseIntrHandler(0x2a)`. `SIFCMD`'s module
entry (`0xd0`, not a separate `InitCmd` call — the whole span `0xd0`–`0x290`
is one function, containing only a single `jr $ra` at `0x28c`) does the
parallel thing for `0x2b` immediately after zeroing its 32-slot
`CmdHandler`/`SysCmdHandler` tables:

```
238-264 (SIFCMD.text, inside the entry function):
  $4=0x2b (RegisterIntrHandler's irq)
  ... $6=&[0x590] (handler)
  jal RegisterIntrHandler           ; RegisterIntrHandler(0x2b, mode=1, handler=&[0x590], arg=$17)
  jal EnableIntr                     ; EnableIntr(0x2b)   $4=0x2b
```

`SIFMAN`'s `0x364` (step 6's counterpart for `0x2a`) calls through the
callback cell `SetDmaIntrHandler`/`ResetDmaIntrHandler` (`0x33c`/`0x350`)
write — confirmed by address arithmetic, since `0x364`'s argument struct base
is `&[0xf80]` and it reads `0x40c($16)`/`0x410($16)`, landing at
`0xf80+0x40c = 0x138c` and `0xf80+0x410 = 0x1390`, the exact two cells
`SetDmaIntrHandler` writes. Both `0x364` and `0x590` are shaped like genuine
completion/command handlers, not leftover debug code — consistent with the
registration now being reachable rather than dead.

**`masked_icr_2`/`dmac2_interrupt_handler_mask` are genuinely used on this
variant**, unlike the scaffolding-only fields `INTRMANP` leaves unread.
`masked_icr_2` (struct offset `0x15f8`, §2.2) is read directly by the irq-3
sub-dispatch (§3.4) to mask `DICR2` before scanning it, and
`dmac2_interrupt_handler_mask` (`0x15fc`, ordinal 27, §2.1) gates the
`0x1578`-register acquire/release helpers (§2.5's `0x90c`/`0x95c`) the irq-3
dispatch brackets each channel-handler call with — a serialization gate
around `DICR2`-adjacent hardware access, consistent with `DMACMAN`'s
boot-time `dmac_set_BF801578(1)` (above), though which module (if any) ever
calls ordinal 27 to arm that gate was not traced (§5).

**The priority-3 cause-0 handler (§3.1's `0x12d8`) still has no `0x28`/`0x2d`
arithmetic** — same minimal, unconditional exception-return shape as
`INTRMANP`'s `0xf60`/`0xf68` (§3.1), unrelated to the second-bank story.

**Verdict.** On the resident variant, `IOP_IRQ_DMA_SIF0`/`SIF1` delivery is
**live**, not dead: `RegisterIntrHandler(0x2a/0x2b, ...)` and
`EnableIntr(0x2a/0x2b)` both succeed and arm real `DICR2`/`DICR`/`I_MASK`
state, and `INTRMANI`'s own irq-3 sub-dispatch (§3.4) scans both `DICR` and
`DICR2`, reaching `table[0x2a]`/`table[0x2b]` and calling `SIFMAN`'s and
`SIFCMD`'s registered handlers through the seven-step path above. This
document's first pass reached the opposite conclusion by tracing `INTRMANP`
— which really is dead in exactly the way described (its `EnableIntr` is a
silent no-op for `irq>=0x27`, and its irq-3 sub-dispatch never reads `DICR2`
at all) — but `INTRMANP` is not the variant that boots on this hardware (§0).
That earlier, negative analysis is preserved as one paragraph, not the
document's conclusion: *on `INTRMANP` specifically*, `RegisterIntrHandler`
and `EnableIntr` for `0x2a`/`0x2b` both return success, `RegisterIntrHandler`
because its three-way range split covers `0x20`–`0x2d` uniformly regardless
of function, `EnableIntr` because its own range check (`irq<0x27`) simply
treats anything `>=0x27` as an early, error-free return — but neither
`DICR2`'s channel-enable bit nor `DICR`'s master bit gets touched, and
`INTRMANP`'s irq-3 handler (`<outdir>/INTRMANP.text` `0x7f0`) is
hard-bounded to 7 iterations over `DICR` alone, so `table[0x2a]`/`table[0x2b]`
are populated but structurally unreachable on that variant. `SIFMAN`'s and
`SIFCMD`'s own handler bodies (`0x364`, `0x590`) still poll `CHCR`/queue
state directly rather than assuming an interrupt-driven wakeup, which would
have let them survive even if booted against `INTRMANP` — but on the
hardware this image actually targets, the interrupt path they register for
is the one that runs.

## 4. What this pins for the rebuild

- **`EXCEPMAN`**: a 16-word absolute-address cause table at a fixed location
  (`0x440` on this image, itself named by one literal constant so it can move)
  immediately adjacent to `INTRMAN`'s own table, a general-exception vector at
  installed offset `0x80` doing `Cause&0x3c` indexing into it, and a
  registration path that **rewrites each registered handler's own leading
  `next` word** to the next handler's code entry point rather than maintaining
  a separate jump table — our own `EXCEPMAN` must reproduce the *effect*
  (chained, `jr`-through dispatch, priority-ordered, falling through to a
  shared default-handler chain) even if the storage layout differs. Both
  `INTRMAN` variants register with it identically (§3.1), so this holds
  regardless of which variant a given target selects.
- **`INTRMAN`**: `RegisterIntrHandler`/`EnableIntr`/`DisableIntr` must accept
  irq 0–`0x2d` and `{0x3e,0x3f}`; the mode argument must gate how much of the
  register file is preserved around a call; `EnableIntr`/dispatch must reach
  `I_MASK` directly for irq<0x20, `DICR` (channel + master-enable bit 23 — not
  31, §2.3) for `0x20`–`0x26`, and `DICR2` (channel bit) + `DICR` (the *same*
  master-enable bit) for `0x28`–`0x2d`, and must set `I_MASK`'s own
  `IOP_IRQ_DMA` bit for either bank to ever fire; the handler is called as
  `handler(arg)` and its return value (0 vs nonzero) controls whether the
  dispatcher re-enables that one IRQ — this is load-bearing for
  `SIFCMD`/`DMACMAN`-style handlers that expect to stay enabled. A second,
  memory-mapped gate (`I_CTRL` at `0xBF801078`, §2.5) exists on this hardware
  generation, saved/restored by the dispatcher around every entry, but this
  pass could not establish that anything in `INTRMAN` itself ever clears it —
  our own kernel does not need to reproduce that specific asymmetry, only the
  save/restore-around-dispatch behaviour, unless a clearer picture of
  `I_CTRL`'s role emerges.
- **DMA sub-dispatch, second bank**: `DICR2` (`0xBF801574`, distinct from
  `DPCR2` at `0x1570`) exists and is fully wired up and *used* by the
  resident variant (§3.4, §3.6) — its irq-3 sub-dispatch scans both `DICR`
  and `DICR2`, acks each bank's flags before calling the matching
  `table[0x480+irq*8]` handler, and both banks share a single physical cause
  (`I_STAT` bit 3) rather than independent lines. This is not a gap to close
  in our own kernel — it is a template to mirror directly: `SIF0`/`SIF1` (and
  `SPU2`/`DEV9`/`SIO2IN`/`SIO2OUT`) are delivered via interrupt on real
  hardware, through exactly this two-bank scan, and our kernel's irq-3
  handler needs the same shape to match it.
- **`SIO2MAN`'s init** imports `intrman` ordinals `[4,5,6,7,17,18]` —
  `RegisterIntrHandler`/`ReleaseIntrHandler`/`EnableIntr`/`DisableIntr`/
  `CpuSuspendIntr`/`CpuResumeIntr` — exactly the ordinal set this document
  pins the behaviour of; nothing SIO2MAN-specific beyond that import list was
  needed for its side of the contract.
- **The reschedule hook** (`SetNewCtxCb`/`SetShouldPreemptCb`, ordinals
  28/30) is how `THREADMAN` reaches back into the interrupt-return path — a
  minimal kernel needs at least a `ShouldPreemptCb` that can say "no" (so
  interrupts return to the interrupted thread) before a real scheduler is
  needed at all, matching how the EE side's reschedule-flag check degrades
  gracefully when nothing sets the flag.

## 5. Unresolved

- **Block A** (`EXCEPMAN` install offset `0x40`, saving cop0 register 7 and
  vectoring through cause-table index 15) — its exact hardware trigger (a
  genuinely separate R3000 vector, or code only ever reached by an explicit
  jump from elsewhere) was not established; only its own instruction sequence
  and its shared use of the `0x440` table were confirmed.
- **The priority-3 cause-0 handler `INTRMANI` registers at `0x12d8`**
  (`RegisterPriorityExceptionHandler(0, 3, &[0x12d8])`, §3.1) — its `funccode`
  (`0x12e0`) is fully read (§3.1): four straight-line instructions restoring
  `$at`/`Status` and `jr`-ing to the saved `EPC`, with no branch and no
  DMA-table involvement — same shape as `INTRMANP`'s equivalent at `0xf60`.
  What remains open is *when* control reaches it: it out-ranks the main
  dispatcher (priority 2), and the priority-chain insertion order itself
  (§1.2) was only partially disambiguated, so whether it runs before or after
  `0xe0c` — and, given its body never loads its own `next` field to chain
  onward, how the main dispatcher ever runs at all if this handler is first —
  was not settled.
- **`I_CTRL`'s exact semantics** (§2.5) — `INTRMANI` reads/writes it in
  `CpuDisableIntr`/`CpuEnableIntr`/`CpuSuspendIntr`/`CpuResumeIntr` and saves
  and force-sets it to 1 around every dispatch (§3.2/§3.5), but no code path
  this pass found ever writes it back to 0. Whether some other, untraced
  caller does, whether it is a reentrancy flag rather than a mask, or whether
  `CpuDisableIntr`/`CpuSuspendIntr` are genuinely incomplete on this variant,
  was not settled.
- **`iCatchMultiIntr`** (ordinal 25, `0xd98`) — only partially decoded (§2.4):
  a stack-range check similar to `QueryIntrContext`/`QueryIntrStack`'s, plus a
  conditional `Status` bit-0 (`IEc`) set when the pointer falls in a narrower
  sub-range near the interrupt stack's top. No caller for it was found this
  pass (the dispatcher's own nesting check goes through ordinal 24, §3.5), so
  what triggers it and what the conditional `IEc` set accomplishes remain
  open. (Ordinals 12/13, unnamed in `INTRMANP`'s table, are now identified
  for `INTRMANI` as the `I_CTRL` read/write-1 helpers behind ordinals 8/9,
  §2.5.)
- **Who (if anyone) calls ordinal 27** (`intrman_internals_t
  .dmac2_interrupt_handler_mask` setter, `0x8fc`) to arm the `0x1578`-register
  acquire/release gate the irq-3 dispatch brackets each handler call with
  (§3.6) — grepped across `SIFMAN`/`SIFCMD`/`DMACMAN`, none imports `intrman`
  ordinal 27; some other, untraced module may.
- **No `rfe` instruction was observed** anywhere in the traced return paths;
  the manual `Status`-write-plus-`jr` sequence's exact bit-for-bit
  equivalence to hardware `rfe` (particularly around `KUo`/`IEo`) was not
  checked against the R3000 COP0 field layout in full.
- **The `0x5b8`/`0x5bc` force-IRQ callback cells** the irq-3 sub-dispatch
  calls when `DICR`'s force bit is set (§3.4) — fixed absolute addresses, not
  struct-relative, and not traced to whatever installs them.
