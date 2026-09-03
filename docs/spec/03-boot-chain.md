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
(`ARC-8`) carrying their own `IOPBTCONF`, not as a flag. The archive also holds
`IOPBTCON2`, and the boot loader names it by **building** the name: it copies
`"IOPBTCONF"` and overwrites the ninth byte with `'0' + mode`, using the result
when the archive has it and the unmodified name when it does not. Mode `0` (a
cold boot) therefore takes `IOPBTCONF`, and mode `2` — the intermediate stage
of a reboot that carries an argument (`docs/analysis/45` §2) — takes
`IOPBTCON2`, whose list can read a disc and cannot talk to the EE.

## BOOT-10: The two CPUs meet

The IOP boot of BOOT-4 to BOOT-9 does not end at the last module of the boot
list. It ends **waiting for the EE**, and the EE is waiting for it. Six shared
registers carry that meeting; a rebuild of either side must take part in it.
Derived from `docs/analysis/23-joining-the-two-cpus.md`.

| Register | EE address | IOP address |
| --- | --- | --- |
| `MSCOM` | `0x1000F200` | `0x1D000000` |
| `SMCOM` | `0x1000F210` | `0x1D000010` |
| `MSFLG` | `0x1000F220` | `0x1D000020` |
| `SMFLG` | `0x1000F230` | `0x1D000030` |
| control | `0x1000F240` | `0x1D000040` |
| identification | — | `0x1D000060` |

**BOOT-10a:** The EE publishes an address in its own RAM in `MSCOM` and then
raises a bit in `MSFLG`. The IOP's boot polls `MSFLG` and proceeds only when
that bit appears.

**BOOT-10b:** The IOP answers with an address in *its* RAM in `SMCOM` and a bit
in `SMFLG`, which the EE is polling for. Neither side may proceed on its own.

**BOOT-10c:** The flag registers are **asymmetric**, and this is the part a
rebuild is most likely to get wrong: a write from the EE *sets* bits in `MSFLG`
and *clears* them in `SMFLG`; a write from the IOP does the reverse. Registers
that merely stored what was written would let each side's acknowledgement erase
the other's request, and the handshake would livelock rather than fail.

**BOOT-10d:** After the handshake the IOP finishes its boot list and reaches an
idle loop — a jump to itself — rather than returning anywhere. Reaching it is
what allows the last one-shot modules, `SIFINIT` among them, to be torn down
per `spec/02` IRX-12.

**BOOT-10e:** The identification register at `0x1D000060` must read back its own
address, or read with its top twenty bits clear; `SIFMAN` refuses the bus
otherwise (`docs/analysis/11`).

## BOOT-11: A transfer carries its own destination

The six registers of BOOT-10 let the two CPUs meet; they do not let them talk.
Data crosses on two DMA channels per side, and the governing rule is that
**the sender frames the transfer**. Neither CPU can read the other's memory, so
a receiving driver has no address to arm its channel with; every address a
channel uses arrives ahead of the bytes it applies it to. Derived from
`docs/analysis/24-sif-data-path.md`, which also states which parts of this are
observed here and which are taken from outside.

| Channel | EE side | IOP side | Direction |
| --- | --- | --- | --- |
| SIF0 | channel 5, `0x1000C000` | channel 9, `0x1F801520` | IOP → EE |
| SIF1 | channel 6, `0x1000C400` | channel 10, `0x1F801530` | EE → IOP |

**BOOT-11a:** Every EE→IOP packet in the SIF1 stream begins with one quadword
of header: `{ address | flags, word count, pad, pad }`. Bit 31 of the first
word ends the receiving channel's run and bit 30 asks for an interrupt; the
rest is the address in **IOP** memory the payload is stored at, and it is the
address that side published in `SMCOM`. The payload follows the header and is
padded up to a quadword boundary, so the word count and the quadwords moved
are two different numbers and both must be right.

**BOOT-11b:** The EE drives SIF1 as a **source chain**: `CHCR` = `0x184` —
`MOD` = chain, `TIE`, `STR`, with `TTE` **clear** — and `TADR` pointing at a
list of quadword tags, each `{ quadword count | id at bits 28-30, address }`.
Ids `refe` (0) and `ref` (3) name data elsewhere; `refe` is the last of a list.
Because `TTE` is clear the tags themselves do not travel, so BOOT-11a's header
is the first quadword of a tag's *data*, not of the tag. `TADR` advances past
each tag as it is consumed, which is what lets a driver re-arm the channel
without rewriting it.

**BOOT-11c:** The IOP drives SIF0 from a `TADR` too, but what sits there is a
sixteen-byte **send block** per packet: `{ address | flags, word count, EE tag
low, EE tag high }`. The last two words are a destination-chain tag —
`{ quadword count | id, address }` — made for the *other* side and pushed into
the FIFO ahead of the data. The EE's channel 5 runs as a destination chain: it
pops that quadword, and stores what follows at the address in it. A rebuild
must supply the peer's tag from the sending end; there is nowhere else it can
come from.

### What a simulator will not tell you

BOOT-11a to BOOT-11d describe the framing, and an image can satisfy all four
and still move nothing on real hardware. The requirements below were found by
running our image on PCSX2 after it passed every simulator gate, and each one
is a thing the simulators were not asking about. They are recorded here because
a rebuild will otherwise meet them one at a time, in the dark.

**BOOT-11e:** A channel needs more than its busy bit. Its `CHCR` carries the
direction and sync mode too, and the block size belongs in `BCR`. What the
reference's IOP driver writes is `CHCR = 0x41000300` with `BCR = 0x20` for the
receiving channel and `CHCR = 0x01000701` with the same `BCR` for the sending
one — the low bit being the direction, which is fixed per channel and must
agree with it.

**BOOT-11f:** The control register's low bits gate the two data paths — `0x20`
for SIF0 and `0x40` for SIF1 — and **the bit is consumed by the transfer it
enables**. Setting them once during the handshake is not enough; the IOP raises
the relevant bit before each transfer.

**BOOT-11g:** On the EE side the DMA controller has a master enable
(`D_CTRL` bit 0) and a hold register (`D_ENABLEW`, bit 16) that starts out
holding every channel. Until both are dealt with, starting a channel does
nothing at all.

**BOOT-11h:** A transfer is not instantaneous. The EE waits for its channel's
`STR` to clear before treating the transfer as done — before signalling the
peer that a request is there, and before reading an answer. The IOP's busy bit
is a separate question and is *not* to be waited on the same way: under PCSX2
it does not clear, and a rebuild that waits for it stops there
(`docs/analysis/25`).

**BOOT-11i:** The receiving end must be armed **first**. A transfer runs only
when both ends are ready, so a side that arms its receiver only after asking
for something has arranged for neither end to move.

**BOOT-11j:** The IOP's second-bank channels have a per-channel enable in
`DPCR2` (`0x1F801570`), one nibble each, whose **high bit is the enable** — a
nibble of `7` is priority with the channel still off. The reference leaves
`0x0777FF77`, enabling exactly the two SIF channels. Above them the bank has a
global enable at `0x1F801578`, which the reference toggles around its critical
sections and leaves set. A channel missing either does not run however its own
`CHCR` is programmed.

**BOOT-11k:** Every address handed to the EE's DMA controller is **physical**,
and bit 31 of `MADR` and `TADR` is not part of it: that bit **selects the
scratchpad**. A pointer taken in a KSEG0-linked kernel therefore does not merely
arrive untranslated, it names somewhere else, and a channel given one reads
sixteen kilobytes of scratchpad and finishes having moved nothing. This applies
to `TADR`, to the address a side publishes in `MSCOM`, and to any address a
request carries for the other processor to place in a tag.
(`docs/analysis/29-sif0-on-pcsx2.md`.)

**BOOT-11d:** A channel that has been started and cannot yet be satisfied stays
**busy**. Both sides' drivers arm a receiver before the sender has pushed
anything and read `CHCR.STR` going clear as the transfer having happened; a
channel that reported itself idle with nothing moved would be indistinguishable
from a completed transfer of zero bytes.

## BOOT-12: The SIF command layer

Derived from `docs/analysis/34-sif-command-protocol.md`. On top of BOOT-11's
framing the two CPUs exchange **commands**: a packet is a sixteen-byte header
followed by up to 96 bytes of body, and a program's SDK-side library speaks
this protocol to the IOP's `SIFCMD`, so an image must speak it too.

**BOOT-12a — the header.** `{ psize:8 | dsize:24, dest, cid, opt }`: `psize`
the packet's size in bytes (header included, so 16..112), `dsize` the size of
an out-of-band payload the sender delivers separately to `dest`, `cid` the
command id, `opt` the sender's. A `cid` with bit 31 set is a **system**
command; the rest are the receiver's registered handlers.

**BOOT-12b — flags and addresses at boot.** After BOOT-10, the IOP publishes
in `SMCOM` the address of its command receive buffer and raises `0x20000`
(the SDK's `SIF_STAT_CMDINIT`) in `SMFLG` when the command layer is ready to
receive; `0x10000` (`SIF_STAT_SIFINIT`) is BOOT-10's own bit. The EE's client
waits for `0x20000`, reads `SMCOM`, keeps both in its software registers
(`spec/05` SYS-13a) and sends its packets to that address.

**BOOT-12c — INIT_CMD.** The client's first command is `cid 0x80000002`,
`psize 0x14`: the header and one word, the address of the **EE's** receive
buffer. The IOP keeps that address as where every reply and every command of
its own goes, and answers with **SET_SREG**: `cid 0x80000001`, `psize 0x18`,
body `{ index 0, value 1 }`, delivered as a BOOT-11c/d packet to that address
— which raises the EE's DMAC channel-5 interrupt, whose handler (SYS-12b)
dispatches the packet to the client's SET_SREG handler and so completes
`SifInitRpc`. Without that reply the client spins forever.

**BOOT-12d — RPC.** `cid 0x80000009` binds a client to a server by id,
`0x8000000A` calls (function number, arguments as the out-of-band payload,
result buffer and size), `0x8000000C` reads server data; each is answered
with `cid 0x80000008` (RPC_END) whose body names the request it answers. A
server is a registered id with a dispatch function; the SDK's module loader
binds server `0x80000006`. The bodies are as `docs/analysis/34` §3 recovers
them. **The server and queue records belong to the caller, not to the
library**: a title's module allocates them and lays its own data out around
them, so their length is ABI. The server record is **17 words (68 bytes)** --
measured, not taken from a header: `SLPS-25918`'s `PADMAN` places its
argument buffer exactly 68 bytes after the record it registers, and a
library that writes an eighteenth word puts it in that buffer, where the
module then reads it as its own command. The queue record is three words.
A `BIND` is answered with the server's record address and its argument
buffer (`sd`, `buf`, `cbuf` in the `END`), or with none of them for an id no
server has, which the client reads as "try again". A `CALL`'s arguments have
already landed in that buffer when the packet arrives — the client sends
them ahead of it, as the out-of-band payload — and its answer is the result
bytes to the client's receive buffer **followed by** the `END` packet, in one
run of the sending channel, so the EE's channel stops (and the client's
handler runs) only once both are there.

**BOOT-12e — the module loader's server.** `sid 0x80000006`, function
**0** loads a module: a 512-byte request — the argument length at `+0`, the
path as a string at `+8` (up to 251 bytes), the arguments at `+0x104` — and
an 8-byte answer, the module id or an error at `+0`, the module's own return
at `+4`. A name the loader has no file for answers **-203**, observed on the
reference (`docs/analysis/34` §6).

**BOOT-12f — the SREG file, and `SET_SREG`.** Each side keeps a **32-word
register file** of its own, and `cid 0x80000001` writes one word of the
*receiver's* file: index at body `+0`, value at body `+4`, with no bounds
check in the reference. The `sifcmd` library publishes a read (ordinal 6) and
a local write (ordinal 7) over it. A receiver that does not implement the
command silently drops every write, and a read that is not backed by the file
answers whatever happens to be in the return register — which is worse than
answering zero, because it lets a caller's spin loop out at random. Both are
required of a rebuild.

Nothing in the boot's own handshake writes the IOP's file: `sceSifInitCmd`
sends `INIT_CMD`, `sceSifInitRpc` sends `INIT_CMD` with `opt` set and spins
on the **EE's** register 0, and the IOP's `sceSifInitRpc` answers by sending
`SET_SREG(0, 1)` outward. The file first carries traffic when a title's own
SIF bridge arrives: it and its IOP half each send `SET_SREG(1, 1)` to the
other and then spin on their own register 1 (`docs/analysis/34` §2).

**BOOT-12g — sending a command.** `sceSifSendCmd(cid, packet, psize,
src_extra, dest_extra, size_extra)` (`sifcmd` ordinal 12) and
`isceSifSendCmd` (ordinal 13) differ **only** in whether the transfer is
started inside an interrupt-disabled bracket; 13 exists for a caller already
in a handler. Both: reject a `psize` outside BOOT-12a's `16..112` by
returning `0`; fill in the header's `psize` and `cid` and leave `opt` and the
body as the caller wrote them; describe an out-of-band block ahead of the
packet when `size_extra > 0`, setting `dsize` and `dest` from it; and
transfer the packet **in place from the caller's buffer** — it is not copied,
so it must stay valid until the transfer completes.

**The return value is load-bearing.** It is the DMA layer's: `0` when the
transfer could not be queued, non-zero otherwise. Real clients loop until it
is non-zero rather than treating a send as unconditional, so a rebuild that
always answers `0` hangs them and one that always answers non-zero drops
packets with no diagnostic.

**And it must be a queue, not a single slot.** Clients send in bursts far
larger than one: `SLPS-25918`'s own SIF bridge registers **145 channels back
to back**, one command each, with no round trip between them, and the
reference answers each in turn. A sender with one slot answers `0` to all but
the first of a burst, which is within the letter of the contract and leaves
the client to loop or to defer itself on an alarm — recovery a rebuild should
not be relying on. The queue must be at least the 32 runs the reference
chains, hold each run's tag list for as long as the channel is reading it,
and start the next run from the sending channel's own interrupt, which is
also where that run's completion callback belongs.

**BOOT-12h — `sifman`'s init pair, and two ordinals that are not what they
look like.** `sceSifInit` is ordinal **5** and `sceSifSetDChain` ordinal
**6**; `sceSifCheckInit` is ordinal **29** and reads the same latch word
ordinal 5 sets, which is also what makes ordinal 5 idempotent. Every module a
title loads is written as `if (!sceSifCheckInit()) sceSifInit();`, so a
rebuild must answer 29 truthfully — an unbound 29 makes a client re-run
whatever its ordinal 5 happens to be, on a bus already carrying traffic.
Likewise ordinal **22 writes MSFLG** and ordinal **24 writes SMFLG**: since a
write from the IOP clears the first and sets the second (BOOT-10c), they are
opposite actions and a rebuild that puts one at the other's number inverts
it.

**BOOT-12i — the completion-callback variants, and the removals.** `sifman`
**32** is ordinal 7 with `(function, arg)` appended: the function is called
once, from the sending channel's interrupt, after the whole run has gone, and
a null function makes it ordinal 7 exactly. `sifcmd` **28** and **29** stand
in the same relation to 12 and 13. These matter beyond the feature they
serve, because a caller **waits on a semaphore the callback signals**: an
implementation that answers "queued" and never calls back parks that thread
for ever, which is worse than answering `0`. `sifcmd` **24** and **25**
(`sceSifRemoveRpc`, `sceSifRemoveRpcQueue`) are the inverses of 17 and 19 —
unlink under an interrupt bracket and answer the record, or null when it was
not on the list.

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

BOOT-11 is gated the same way BOOT-10 is, and on the reference rather than only
on our own image:

```sh
python3 tools/ps2sim.py assets/SCPH-50000.bin --traffic   # the tags and packets
python3 tools/ps2sim.py assets/SCPH-50000.bin --check     # judge them
```

`--check` requires that something crossed to the IOP with a header in front of
it and that the header sent it to the address the IOP published in `SMCOM` —
BOOT-11a's substance, since a receiver that chose its own address would pass
the first test and fail the second. `--no-bridge` fails both, alongside
BOOT-10's.

BOOT-11c is observed on the reference too, once its IOP is woken by the one
interrupt its SIF driver is asleep on: it builds a send block, and the tag in
that block's second half lands the reply at the EE receive buffer the EE had
nominated in its own first packet. The destination tag the reference writes is
id 1, `cnt`, with bit 31 set — a rebuild may use `end` instead, as ours does,
but should know it is not what the reference does.

**BOOT-5 corrected `docs/analysis/02`.** Extracting the POST writes mechanically
rather than reading them off a listing found a code the prose had missed
(`0x03`) and showed the retail path is `8, 9` where the prose had called `6, 7`
the normal one.
