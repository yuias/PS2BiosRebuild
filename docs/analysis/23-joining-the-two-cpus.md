# Joining the Two CPUs: the SIF Handshake, and IRX-12 Observed

Two documents ended with the same shape of problem. `docs/analysis/12` recorded
that residency (`docs/spec/02-module-abi.md` IRX-12) could not be observed,
because the one-shot modules that would demonstrate it — `SIFINIT` above all —
sit at or past the point where the IOP boot waits for the EE.
`docs/analysis/22` recorded that the EE, run alone, ends up waiting for the
IOP.

Each simulator was blocked on the other. `tools/ps2sim.py` runs them together.

```sh
python3 tools/ps2sim.py assets/SCPH-50000.bin             # report the joint boot
python3 tools/ps2sim.py assets/SCPH-50000.bin --check     # judge it
python3 tools/ps2sim.py assets/SCPH-50000.bin --traffic   # every SIF access
python3 tools/ps2sim.py assets/SCPH-50000.bin --no-bridge # the old, split behaviour
```

## What each side was waiting for

Instrumenting both stalls gives a matched pair:

| CPU | Loop | Polling |
| --- | --- | --- |
| IOP | `0x00004090`, called from `0x00016AC8` | `0x1D000020` |
| EE | `0x80005F04`/`0x80005F58` | `0x1000F230` |

Those are the same two registers viewed from opposite ends of the bus:
`MSFLG`, which the EE raises, and `SMFLG`, which the IOP raises. Each side was
polling a register nothing could ever write, because the two simulators had
separate copies. **Six registers is the whole bridge** — `MSCOM`, `SMCOM`,
`MSFLG`, `SMFLG`, a control word and the identification register at
`0x1D000060` that `docs/analysis/11` found `SIFMAN` insisting reads back its
own address.

## The flags are not symmetric, and that is what makes it work

Sharing the registers as plain storage is not enough, and failing that way is
instructive: `MSFLG` and `SMFLG` are each *raised* by one side and *cleared* by
the other.

| Register | EE write | IOP write |
| --- | --- | --- |
| `MSFLG` | sets bits | clears bits |
| `SMFLG` | clears bits | sets bits |

With plain storage each side's acknowledgement wipes the other's request and
the handshake livelocks. With the asymmetry, the traffic reads as a
conversation:

```sh
python3 tools/ps2sim.py assets/SCPH-50000.bin --traffic
```

```
EE  write CTRL   0x00000100      the EE opens the bus
EE  write MSCOM  0x80021240      and publishes an address in its own RAM
EE  write MSFLG  0x00010000      then raises a flag
IOP write CTRL   0x00000040      the IOP answers
IOP write SMCOM  0x00019600      with an address in its RAM
IOP write SMFLG  0x00010000      and its own flag
EE  write SMFLG  0x00040000      the EE acknowledges by clearing
```

Neither side polls a register the other cannot write again.

## What the join buys

```
IOP: 2562000 instructions, POST ['0xfc', ...], pc 0x0000ae94  (idle loop)
module images released (IRX-12): 5
   0x003d00  0x007d00  0x00aa00  0x017e00
   0x03fd00   <- past the handshake
```

**The IOP boot completes.** It runs off the end of its boot list and reaches
`j 0x0000AE94` — a jump to itself, the idle loop of a machine with no runnable
thread. Alone, it spun on `MSFLG` forever.

**`SIFINIT` is torn down.** The fifth release, at IOP step 2 418 181, is the
one that only the join reaches. Its image is still in RAM at the moment it is
freed, and it identifies itself:

```
loadcore  stdio  sifman  " Skip SIF init (it is DECI1)"
```

Those imports and that message are `SIFINIT` — the module `docs/analysis/12`
named as the one that could not be reached. IRX-12 is now observed at the exact
place it was hardest to observe.

## IRX-12 is observable earlier, too

Watching `sysmem` ordinal 5 during an ordinary `tools/iopsim.py` run — which
`FreeWatcher` now does — shows **four** module images released before the
handshake is ever reached:

| Released | IOP step | What was still in the memory |
| --- | --- | --- |
| `0x003D00` | 118 437 | `intrman`, `Interrupt_Manager` |
| `0x007D00` | 206 590 | `timrman`, `Timer_Manager` |
| `0x00AA00` | 1 650 679 | `eeconfigh`, `EEConfig` |
| `0x017E00` | 1 967 318 | `loadcore`, `intrman`, `sysclib` |

The first two are the **rejected halves of the P/I variant pairs**.
`docs/analysis/06` worked out that `INTRMANP` and `INTRMANI` are both in the
boot list and that each tests the machine discriminator and returns `1` — asking
to be freed — when it is the wrong one. That was read out of the code; here it
is executed, and the wrong half really is released.

Every one of the five bases is `0x100`-aligned, which is IRX-12b's rounding.

So the residency requirement did not need the join after all — the join
extends it to the module that motivated the whole exercise. Both facts are now
gated: `tools/iopsim.py --check` requires the four, and
`tools/ps2sim.py --check` requires the fifth.

## The gate, in both directions

`--no-bridge` gives each CPU its own copy of the registers again, which is
exactly the state the project was in before this document. Running the gate
that way fails, naming what the bridge provides:

```
BOOT-10b: the IOP never answered with an address in SMCOM
BOOT-10b: the IOP never raised a bit in SMFLG
BOOT-10c: the IOP did not reach its idle loop; it stopped at 0x00016c28
IRX-12a: 4 module images were released, want 5
```

Both reference images pass with the bridge, in about six seconds each.

## The boundary: registers, not data

The **data path is not modelled**. After the handshake the EE builds a transfer
and waits for its SIF1 DMA channel (`0x1000C400`) to complete:

```
000842c8  jal   0x80083a80          # "has the channel finished?"
000842d0  beq   $v0, $zero, -3      # spin until it has
```

Nothing drains that channel, so the EE stops there. Reaching further needs SIF0
and SIF1 as real DMA channels on both sides, with the packet headers that say
where each transfer lands, and interrupts to wake the IOP's driver — a
considerably larger piece of work than the six registers here, and the natural
next one. `docs/spec/04-ee-kernel.md` EE-9's boot tail, which loads
`rom0:OSDSYS` across exactly that path, is what it would unlock.
