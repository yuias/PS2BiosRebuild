# The IOPRP's Timer Manager: the `timrman` ordinals rom0 does not have

`docs/analysis/44` read the ROM's `TIMRMAN` and found a `timrman` **v1.01**
library of 17 ordinals, `0`–`16`. A title's own modules do not all bind
against that one. `SLPS-25918` ships `MODULES/IOPRP310.IMG`, and the image
carries `TIMEMANI` — a different, later timer manager — which `docs/analysis/45`
lists among the modules the `UDNL` merge takes from the disc rather than from
`rom0`.

This reads that module, because one of the title's own drivers imports four
ordinals only it has.

```sh
python3 tools/romdir.py --list <iso-dir>/IOPRP310.IMG
python3 tools/romdir.py --extract <outdir> --only TIMEMANI <iso-dir>/IOPRP310.IMG
python3 tools/irxinfo.py <outdir>/TIMEMANI --exports
python3 tools/irxinfo.py <outdir>/TIMEMANI --dump-load <outdir>/TIMEMANI.text
```

## 0. Who needs it, and what happens without it

`EZMIDI.IRX` imports `timrman` **v1.03** ordinals `[4, 6, 20, 22, 23, 24]`.
Ordinals 4 and 6 are `AllocHardTimer` and `FreeHardTimer`, which rom0 has;
**20, 22, 23 and 24 are past the end of rom0's table**, so IRX-9 binds them
to the shared `jr $ra` and the calls disappear, leaving `$v0` as the caller
left it.

`EZMIDI` checks all of them. Its own diagnostics name the three steps —
`"Can NOT allocate hard timer ..."`, `"Can NOT set timeup timer handler ..."`,
`"Can NOT setup hard timer ..."` — and the second one is what a rebuild
without these ordinals reaches:

```
7d080:  AllocHardTimer(1, 0x20, 1)          ; ordinal 4, bound
7d0a0:  bgtz  -> id <= 0: "Can NOT allocate hard timer ...", return -1
7d0ec:  <ordinal 20>(id, compare, handler, arg)
7d0f4:  beqz  -> non-zero: "Can NOT set timeup timer handler ...", return -1
7d128:  <ordinal 22>(id, 1, 0, 1)
7d130:  beqz  -> non-zero: "Can NOT setup hard timer ...", return -1
```

An unbound stub leaves `$v0` holding the pointer the caller loaded two
instructions earlier, so the `beqz` fails and `EZMIDI`'s timer set-up
**always** takes its error return. It then falls back to a 5 ms
`DelayThread` poll — which is visible from outside as a `DelayThread(5000)`
loop with no timer interrupt behind it.

`PADMAN.IRX` also imports `timrman` v1.03, but only `[4, 6, 7, 9, 11, 16]`,
all of which rom0 has. **`EZMIDI` is the only module on this disc that needs
anything past 16.**

## 1. The module and its table

`TIMEMANI` is `Timer_Manager` **v2.02** and exports `timrman` **v1.03** with
**28 entries**, `0`–`27`. Through ordinal 16 it matches rom0's v1.01 table
one for one (§`docs/analysis/44` §1). Past it:

| Ord | Addr | What it is |
| --- | --- | --- |
| 17 | `0x4e8` | not read here |
| 18 | `0x6c0` | not read here |
| 19 | `0xe30` | the shared reserved `jr $ra` stub |
| 20 | `0x6f0` | `SetTimerHandler(id, compare, handler, arg)` (§3) |
| 21 | `0x7bc` | not read here; the ISR's second handler pair says it is the overflow twin of 20 (§2, §5) |
| 22 | `0x990` | `SetupHardTimer(id, source, mode, prescale)` (§4) |
| 23 | `0xbac` | `StartHardTimer(id)` (§5) |
| 24 | `0xce4` | `StopHardTimer(id)` (§6) |
| 25–27 | `0xe30` | the reserved stub again |

Ordinals 17, 18 and 21 are left unread: nothing on this disc imports them,
and §0's rule is that a rebuild serves what a title asks for.

## 2. The per-timer descriptor

Every one of the four walks the same table: the id's **top four bits** are the
timer number, `(id >> 28) - 1`, and must be below 6 (else `-0x97`); the
descriptor is `table + 44 * that`. That works because **this module's ids are
not rom0's**: its `AllocHardTimer` (`0x90`, the claim path at `0xc8`) builds

    id = ((n + 1) << 28) | (register_base >> 4)

where `n` is the descriptor's index, while rom0's v1.01 is `register_base >> 2`
(`docs/analysis/44` §1) — which puts `2` in the top nibble of **every** timer's
id and so carries no index at all. §8 says what a rebuild does about that.

Fields, from what the four functions and the ISR read and write:

| Off | Size | What |
| --- | --- | --- |
| `+0x00` | 4 | the timer's register base |
| `+0x04` | 1 | the sources it supports, as IOP-7a's bitmask |
| `+0x06` | 2 | the largest prescale it can take |
| `+0x08` | 1 | its IRQ number |
| `+0x0a` | 1 | set once the ISR has been registered on that IRQ |
| `+0x0c` | 4 | **running**: zero while stopped |
| `+0x10` | 4 | set by `SetupHardTimer`; `StartHardTimer` refuses while it is zero |
| `+0x14` | 2 | the MODE bits the compare handler wants |
| `+0x16` | 2 | the MODE bits the overflow handler wants |
| `+0x18` | 4 | the compare value |
| `+0x1c` | 4 | the compare ("timeup") handler |
| `+0x20` | 4 | its argument |
| `+0x24` | 4 | the overflow handler |
| `+0x28` | 4 | its argument |


## 3. Ordinal 20 — `SetTimerHandler(id, compare, handler, arg)`

Bounds-checks the timer (`-0x97`), takes `intrman`'s critical section, and
refuses a **running** timer — `+0x0c` non-zero — with `-0x9a`. Otherwise it
stores `compare` at `+0x18`, `handler` at `+0x1c`, `arg` at `+0x20`, and
writes `+0x14` with **`0x58`** when the handler is non-null and `0` when it
is null. It answers 0.

`0x58` is the MODE the compare interrupt needs: reset-on-compare, compare
interrupt enable, and repeat. Installing a null handler is how a caller takes
the compare interrupt back out.

## 4. Ordinal 22 — `SetupHardTimer(id, source, mode, prescale)`

Refuses **interrupt context** first (`intrman`'s `QueryIntrContext`,
`-0x64` = `KE_ILLEGAL_CONTEXT`), then the same bounds check (`-0x97`) and the
same running check (`-0x9a`).

If `+0x0a` says the ISR is not registered on this timer's IRQ yet, it
registers it — `intrman` `RegisterIntrHandler(irq, 1, <the ISR>, descriptor)`
— after releasing whatever was there, and sets `+0x0a`. A failed registration
is returned as it comes back.

What it settles is a **MODE word**, built in one register and stored at
`+0x10`. It starts as `0x80000000` — a marker in a half the 16-bit MODE
register never sees, which is also how `StartHardTimer` tells a set-up timer
from a fresh one — and gains bits from all three arguments:

- **`mode`** indexes a jump table of eight entries (`0xed0`). Entries 0, 1, 3,
  5 and 7 fall through to `or` the value itself into the low bits; **2, 4 and
  6 answer `-0x195`** (`KE_ILLEGAL_MODE`), and so does anything from 8 up.
- **`source`** must be 1, 2 or 4 and must be a bit the timer's `+0x04` mask
  has, else `-0x98`. Sources 2 and 4 — PIXEL and HLINE — add **`0x100`** and
  consult the prescale no further.
- **`prescale`**, on the SYSCLOCK path only, must not exceed `+0x06` (else
  `-0x99`) and must then be one of four values: 8 adds **`0x200`** on a
  16-bit timer and **`0x2000`** on a 32-bit one, 16 adds **`0x4000`**, 256
  adds **`0x6000`**, and "no prescale" adds nothing. The last is tested as
  `prescale == source` rather than `prescale == 1` — the same thing wherever
  it is reached, since only source 1 gets here, but worth recording as it
  reads. Anything else is `-0x99`.

For `EZMIDI`'s own call — `(id, source 1, mode 0, prescale 1)` — every one of
those contributes nothing, so a rebuild that ignored the whole word would
still be right for this disc and wrong for the next driver.

## 5. Ordinal 23 — `StartHardTimer(id)`

Bounds (`-0x97`), critical section, refuses a running timer (`-0x9a`), and
refuses one `SetupHardTimer` has not been through — `+0x10` zero — with
**`-0x9b`**.

Then, in this order: MODE is written **0** first, so the hardware is quiet
while the rest is programmed; the compare from `+0x18` goes to the register
block's `+8`, as a halfword for timers 0–2 and a word for 3–5 (IOP-7d); and
MODE is written from `+0x10`, `+0x14` and `+0x16` together — §4's source and
prescale bits with the two handlers' interrupt bits — which is what actually
starts it. `+0x0c` becomes non-zero.

## 6. Ordinal 24 — `StopHardTimer(id)`

Bounds (`-0x97`), critical section, and the **opposite** check: a timer that
is not running answers `-0x9c`. It releases the IRQ handler when the running
flags say it owns it, and clears the state 23 set.

## 7. The ISR (`0x878`)

Registered per timer with the descriptor as its argument. It reads the
timer's MODE register once — which is also how the hardware is acknowledged
(IOP-7d) — and dispatches on the two flags:

- **`0x1000`, overflow.** If `+0x16` is non-zero, call `*(+0x24)(*(+0x28))`.
- **`0x0800`, compare.** If `+0x14` is non-zero, call `*(+0x1c)(*(+0x20))`.

Each handler's return decides whether that half stays armed; the ISR folds
the two answers into its own return.

So the two handler pairs are independent, and a caller that installs only the
compare handler — which is all `EZMIDI` does — leaves `+0x16` zero and the
overflow half silent.

## 8. The id is opaque, so a rebuild picks one encoding

The two managers encode a timer id differently, and it does not matter.
`EZMIDI` never builds an id: it takes what `AllocHardTimer` returns and hands
that same word back to 20, 22, 23, 24 and 6. Every module examined does the
same — ps2sdk's header types it as an opaque `int` and none of the callers
read a field out of it.

So a rebuild keeps **one** encoding across its whole `timrman` and decodes
20–24 the same way it decodes 4–16. Reproducing v1.03's `((n+1) << 28) |
(base >> 4)` alongside rom0's `base >> 2` would mean two decodings in one
library and a `THREADMAN` that has to know which it holds, for no observable
gain.

## What this pins for the rebuild

- A title's driver can import `timrman` ordinals **20, 22, 23, 24**, and a
  17-entry table binds them to `jr $ra` with no diagnostic. The rebuild's
  table has to reach them.
- A timer id is opaque to its callers (§8); one encoding across the library.
- These four are the whole of what this disc needs. `EZMIDI` calls
  `AllocHardTimer`, `SetTimerHandler`, `SetupHardTimer`, `StartHardTimer`,
  `StopHardTimer` and `FreeHardTimer`, in that order, and checks every one.
- The compare handler runs from the timer's own IRQ, so `TIMRMAN` — not its
  caller — is what registers with `intrman`, and the MODE read in the ISR is
  the acknowledgement.

## Open questions

- Ordinals 17, 18 and 21, and the seven other entries of `mode`'s jump table
  in §4. Nothing on this disc reaches them.
- Whether `TIMEMANI`'s own ordinals 4–16 read the id the way rom0's do or the
  way its 20–24 do. §8 makes it moot for a rebuild, which ships one encoding.
