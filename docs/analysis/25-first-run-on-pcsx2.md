# The First Run on PCSX2

`docs/project-state.md` listed this as the step that would "find whatever we
have modelled too kindly". It did, and this document records what — both the
part that worked immediately and the list of things six simulator gates were
not asking about.

```sh
# PCSX2 v2.6.3, the AppImage, extracted rather than mounted
~/tools/squashfs-root/AppRun -bios -batch -nogui
# with build/rom.bin copied to ~/.config/PCSX2/bios/ and, in
# ~/.config/PCSX2/inis/PCSX2.ini, [Logging] EnableEEConsole = true
```

Two notes on the harness before the results. PCSX2 **rewrites its ini on
startup**, so a logging setting written before the first run is silently
replaced by the default `false`; it has to be set again once PCSX2 has
generated its own file. And `-batch` exits when the *emulation* shuts down,
which booting to a BIOS never does, so the run must be given a timeout rather
than waited on.

## What worked first time

The image is **accepted as a BIOS**:

```
BIOS Found: Test    v01.00(10/08/2026)   iosRebuild_boot
EE/iR5900 Recompiler Reset
VM subsystems initialized in 737.50 ms
```

So the archive of `spec/01`, the ROMVER of ARC-6 and the boot block's layout
are all good enough for a real emulator's BIOS detector, which is a claim no
simulator here was in a position to make.

And the boot runs:

```
# PS2BiosRebuild EE kernel: entered at 0x80001000.
# The IOP answered; the SIF handshake is complete.
```

That is `spec/03` BOOT-1 through BOOT-10 and `spec/04` EE-1 to EE-4 on the
working target: both processors reset, the EE path reaches our kernel, the IOP
path loads and links our modules, and the six-register handshake completes
between them. Everything the reset paths and the archive do is confirmed.

## What did not, and what each turned out to be

The line after the handshake came back empty, and then the boot failed to find
its program:

```
# ROMVER, fetched from the archive across the SIF: # no program: ...
```

The data path moved nothing. Five separate causes, each of which our simulators
had been letting through, and each now a requirement in `spec/03` (BOOT-11e to
BOOT-11j):

| What was missing | Why nothing here caught it |
| --- | --- |
| `D_CTRL` and `D_ENABLEW` — the EE DMA controller's master enable and its hold register | `tools/eesim.py` runs a channel on `CHCR.STR` alone |
| The IOP channels' direction and sync mode in `CHCR`, and `BCR` | `tools/ps2sim.py` started a channel on the busy bit alone |
| `DPCR2`, the IOP's per-channel enable | nothing modelled it |
| Waiting for `STR` to clear | a modelled transfer completes within the store that starts it |
| Arming the receiver **before** asking | with instantaneous transfers, order does not matter |

The first four are plain omissions. The fifth is the interesting one: a
transfer runs only when both ends are ready, so our arrangement — each side
arming its receiver *after* being told something was coming — could not work on
hardware and could not fail on a simulator whose transfers complete inside a
single store.

`tools/ps2sim.py` now refuses to start an IOP channel whose direction bit
disagrees with the channel, which is the one of these the simulator can check
cheaply. The rest are not simulator bugs so much as the simulator's scope, and
the honest response is this document rather than a pretence of fidelity.

## Where it stands

**SIF1, the EE-to-IOP direction, works.** Its channel completes: reading
`CHCR` after the transfer gives `0x00000084`, with `STR` clear.

**SIF0, the IOP-to-EE direction, does not yet.** The EE arms its destination
chain, the IOP builds and starts its send, and the EE's channel never reports
itself finished.

One observation stands out and is the thread to pull next. Reading the SIF
control register from the EE after the handshake gives `0xF0000102` — the IOP's
path bits, `0x20` and `0x40`, are **gone**, though the same register read
`0xF0002162` moments earlier with both present. Something consumes them, which
is what BOOT-11f records; raising the relevant bit before each transfer was not
by itself enough to make SIF0 deliver, so either the timing of that write or a
further condition on the receiving end is still wrong.

The debugging technique worth keeping: the EE's serial console is the only
instrument that reaches inside the running image, and printing a register from
our own kernel — `CHCR` here, the control register there — answered in one run
what guessing had not in several. A bounded wait that then reports beats an
unbounded one that hangs, because the value it prints is the diagnosis.
