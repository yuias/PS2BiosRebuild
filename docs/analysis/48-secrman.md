# SECRMAN: the `secrman` Interface, and What a Memory-Card Driver Needs From It

`docs/analysis/12` recorded `SECRMAN` as a fourteen-entry library between the
CDVD driver and the loader and stopped there, deliberately. It became worth
going further when a retail title's `MCMAN.IRX` turned out to import `secrman`
ordinals 4, 5 and 6: nothing in this project exports that tag, so the module is
refused at link time and never runs — the same silent wall `docs/analysis/47`
and `docs/analysis/38` §3.3 describe for `vblank` and `thmsgbx`.

**Scope, and why it is drawn here.** `docs/clean-room-policy.md`
§"Trademarks and compatibility" puts the authentication *mechanism* out of
scope and only its interface in. This document therefore reads the export
table, the three ordinals a card driver uses, what that driver branches on,
and the module's own registrations. Where a body is the card/mechacon
exchange it is named as such and its control flow summarised — no step
values, no packet contents, no accept constants.

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
python3 tools/irxinfo.py <outdir>/SECRMAN --exports --imports
python3 tools/irxinfo.py <outdir>/SECRMAN --dump-load <outdir>/SECRMAN.text
python3 tools/romdis.py <outdir>/SECRMAN.text --cpu iop --vma 0
# the client, MODULES/MCMAN.IRX;1 out of the disc image
python3 tools/irxinfo.py <outdir>/MCMAN.IRX --exports --imports
python3 tools/irxinfo.py <outdir>/MCMAN.IRX --dump-load <outdir>/MCMAN.text
python3 tools/romdis.py <outdir>/MCMAN.text --cpu iop --vma 0
```

Addresses are `SECRMAN.text` / `MCMAN.text` offsets (module vaddr, base 0).
Names marked `[header]` come from ps2sdk; the binary is the authority.

## 1. The ordinal table

`.iopmod` name `secrman_for_cex`, version 1.03. Export table at `0x2c40`, tag
`secrman`, version 1.03, **14 entries**.

| Ord | Addr | Name [header] | Read? |
|---|---|---|---|
| 0 | `0x18` | module entry | §4 |
| 1, 3 | `0x2c90` | reserved | a bare `jr $ra` |
| 2 | `0x5c` | — | two instructions: returns 1. No caller found |
| 4 | `0x64` | `SecrSetMcCommandHandler` | §2.1 |
| 5 | `0x84` | `SecrSetMcDevIDHandler` | §2.2 |
| 6 | `0xa4` | `SecrAuthCard` | §2.3 |
| 7 | `0x750` | `SecrResetAuthCard` | §2.4 |
| 8–13 | `0xb7c`–`0xf0c` | the `Secr*Boot{Header,Block,File}` six | **not read** — the encrypted-module loading path; nothing this document's clients import touches them |

Imports: `loadcore` 6, `stdio` 4, `cdvdman` 29, `ioman` 6/8, `modload` 12.
**No `intrman`, no `thbase`, no `sio2man`** — so no ordinal here can refuse
interrupt context, there is no thread and no interrupt handler, and SIO2
traffic is not this module's business (§2.1).

One thing worth knowing before anyone goes looking for a trace: **the module's
own debug output is inert.** Two identical empty varargs bodies absorb 53 of
the 54 formatted-print call sites, including every one on the authentication
path. The single live `printf` is inside a KELF helper.

## 2. The ordinals a card driver uses

The module's whole mutable state is two words of `.bss`, both cleared by the
entry: a **card-command handler** (written by ordinal 4) and a **device-ID
handler** (ordinal 5).

### 2.1 Ordinal 4 — `SecrSetMcCommandHandler(handler)`

Stores its argument in the first slot and returns nothing in particular —
neither the ordinal nor the helper it calls writes `$v0`. **Nothing validates
the pointer**, and NULL is a real argument: the client passes NULL on unload
(§3).

The handler is how `SECRMAN` reaches the card at all. It is called as
`handler(port, slot, descriptor)`, where the descriptor is a SIO2 transfer
block on `SECRMAN`'s own stack; a **zero return is failure**, and a nonzero
one is then qualified against the descriptor's status word. That inversion of
control is why `SECRMAN` names no `sio2man` import: the driver that registers
the handler owns the transport.

### 2.2 Ordinal 5 — `SecrSetMcDevIDHandler(handler)`

The same shape into the second slot, equally NULL-tolerant. The stored pointer
is consulted through one wrapper, which answers `-1` when the slot is null,
and that wrapper's only caller is ordinal 8. **Nothing on ordinal 6's path
reads it.**

### 2.3 Ordinal 6 — `SecrAuthCard(port, slot, cnum)`

The one that matters. Its **return set is exactly `{0, 1}`**, and the paths to
each are:

| Answer | When | What it does first |
|---|---|---|
| `0` | the card-command handler slot is null | nothing at all |
| `0` | the first card-side step, or the first mechacon-side step, fails | nothing |
| `0` | any later step fails | a card-side reset, and for the steps past the fourth a mechacon-side reset too |
| `1` | every step succeeded | — |

Between those ends is an alternating ladder of card-side steps (through the
registered handler) and mechacon-side steps (through `cdvdman` ordinal 29, the
S-command sender, whose single call site in this module they share). **That
ladder is the authentication exchange and was not read past its control
flow.** What a reimplementation needs from it is above: which conditions
answer 0, which answers 1, and that no other value exists.

### 2.4 Ordinal 7 — `SecrResetAuthCard(port, slot, cnum)`

The card-side reset and the mechacon-side reset, in that order, answering the
latter's result — 1 when the S-command round trip reported success, else 0.
It is the same pair ordinal 6 runs on its own failure paths. Neither the
title's `MCMAN.IRX` nor rom0's own `MCMAN`/`XMCMAN` imports it.

## 3. What the client branches on

`MCMAN.IRX` (`mcman` 2.30) imports exactly 4, 5 and 6.

**At init** it registers both handlers and examines neither return value: the
card-command handler is a four-call wrapper over `sio2man` (transfer-init,
transfer, unlock), and the device-ID handler is four instructions computing an
index from `port` and `slot`. **On unload** it calls both again with NULL — so
a stand-in must accept NULL.

**The authentication call** is one site, and one branch decides this whole
task:

```
        cnum = f(port, slot)
        SecrAuthCard(port + 2, slot, cnum)
0x38ac: beqz  -> return -1
        else  -> return 0
```

`MCMAN` treats **any nonzero answer as success** and **zero as failure**. Note
the `port + 2`: what reaches `SECRMAN` is the memory-card port, not the pad's.

A failed authentication surfaces two frames up as **`-90`**
(`sceMcResFailAuth` [header]; `-11` is `sceMcResFailResetAuth` from the reset
sequence just before it), and the driver carries on and reports the card as
unusable. **So both answers are safe**: with 0 `MCMAN` runs and declines the
card, with 1 it proceeds to identify and mount it. Neither hangs.

## 4. Registrations and state

The entry registers the library, clears the two handler slots, hands
`modload` ordinal 12 a triple — ordinals 10 and 13 plus an unexported third
callback that exchanges five words of loader state for two function pointers
— and returns resident. It **ignores** what `modload` 12 answers.

No threads, no interrupt handlers, no event flags, no semaphores; `.bss` is
`0x2c` bytes. The `cdvdman` dependency is hard for the reference — ordinal 29
is a real import, so `SECRMAN` itself would fail to link without `CDVDMAN` —
and matches the boot list, where `CDVDMAN` precedes it. The `ioman` imports
and the one live `printf` belong to the KELF helpers only.

## 5. What else stands between `MCMAN.IRX` and running

`secrman` is the only **whole-tag** miss. Three more of its imports fall past
this project's own tables and so, by IRX-9's rule, become silent `jr $ra`
stubs rather than link errors:

| Import | Our table | Note |
|---|---|---|
| `sio2man` 26, 57, 59, 67 | 26 entries, so 26 and up are out of range | the title ships its own `SIO2MAN.IRX`, `sio2man` **v2.07** with 68 entries, and that is a different library by IRX-10a's tag+major rule |
| `sifcmd` 23 | 23 entries | `sceSifGetOtherData` [header]; two call sites |
| `modload` 13 | present but answers `-1` | the result is not checked |

The `sio2man` rows are not this project's work **provided the disc's own
`SIO2MAN.IRX` is registered before `MCMAN.IRX` binds** — which the observed
load order does satisfy. It is worth stating the dependency because rom0's own
`SIO2MAN` has 26 entries too, so `MCMAN.IRX`'s ordinal 26 is out of range
against the reference ROM as well: the title must ship its own for its own
`MCMAN` to work on retail at all.

## 6. What a stand-in must do

- Export `secrman` v1.03 with at least seven slots so ordinal 6 is in range;
  the reference's fourteen keeps every later ordinal where a client expects
  it.
- Ordinals 4 and 5 store the pointer, NULL included, and answer nothing in
  particular.
- **Ordinal 6 answers 1 to let a card driver proceed, and 0 when the
  card-command handler was never registered** — which is what the reference
  does, and which keeps a driver that skipped registration from being told a
  success it never set up. No SIO2 or CDVD traffic is needed for either.
- The entry registers the library and does not depend on `modload` 12.

## 7. Unresolved

- **Ordinals 8–13 and the loader exchange** — the encrypted-module path.
  Deliberately unread; out of scope by policy, and unimported by every client
  this document looked at.
- **Ordinal 2**, which returns 1 and has no caller.
- **What `cnum` means on the mechacon side** — not needed for a stand-in.
- **What the EE sees after a `-90`**: the driver's PS1-card fallback's answer
  for a PS2 card was not traced.
