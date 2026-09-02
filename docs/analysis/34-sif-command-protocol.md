# The SIF Command and RPC Protocol

The M1 program's third stage (`docs/project-state.md` §6) is `SifInitRpc`, and
it waits forever: the IOP has no command service to answer it. `docs/analysis/11`
placed `SIFMAN`/`SIFCMD`/`SIFINIT` in the boot and `24` recovered the framing
underneath them; this reads the protocol on top — what the IOP's `SIFCMD` says
and expects, and what the SDK's client (the program's own copy of the same
library) sends and waits for — so a service can be written that the client
will talk to. The client side is read from the M1 program's disassembly, since
that is the consumer whose expectations are the contract; the SDK's headers were
consulted only to put names to ordinals and struct fields, and every such use is
marked "[header]".

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
python3 tools/irxinfo.py <outdir>/SIFCMD; python3 tools/irxinfo.py <outdir>/SIFMAN
python3 tools/romdis.py <outdir>/SIFCMD --cpu iop --vma 0 --range 0x0 0x1700
python3 tools/romdis.py <outdir>/SIFMAN --cpu iop --vma 0 --range 0x0 0xe00
/usr/lib/llvm-22/bin/llvm-objdump -d --triple=mipsel --mcpu=mips3 build/m1/m1.debug.elf   # the client
```

## 0. Ordinal maps (retail export slot → ps2sdk-documented name)

Retail `SIFCMD` (32 exports) and `SIFMAN` (36 exports) export tables were diffed
against the ps2sdk IOP-side `DECLARE_IMPORT` ordinal tables in
`iop/include/sifcmd.h` / `iop/include/sifman.h` [header]. Every ordinal ps2sdk
names lands on a distinct, plausible function body in the retail binary (arg
counts, call graph, and the constants below all agree), and every "reserved/
future" slot (sifcmd 1,2,3,28-31; sifman 1,3,32-35) resolves to the *same*
address `0x15f8`/`0xe48` — a bare `jr $ra` stub — which is strong corroboration
this mapping is right, not coincidental. Key sifcmd.text addresses (base 0,
entry 0xd0) used below: `28` GetSreg, `40` SetSreg, `2c8` InitCmd, `300`
ExitCmd, `32c`/`344` Set{Cmd,SysCmd}Buffer, `35c`/`3b4` Add/RemoveCmdHandler
(`3e4` unexported `_SifSendCmd`, shared by `4e0` SendCmd / `524` isceSifSendCmd),
`568`/`57c` Set/ClearSif1CB, `6c0` InitRpc, `ad4` GetOtherData, `d08` BindRpc,
`e08` unexported RPC_CALL system handler, `ed4` CallRpc, `1040` CheckStatRpc,
`1088` SetRpcQueue, `1130` RegisterRpc, `1210`/`12a8` Remove{Rpc,RpcQueue},
`133c`/`13a4` GetNextRequest/ExecRequest, `1520` RpcLoop, `c48`/`a68`
unexported RPC_BIND/RPC_RDATA system handlers. Key sifman.text addresses (base
0, entry 0x0): `0` entry (residency gate, doc 11), `148` Init, `268` ord.2
(unnamed by ps2sdk), `2d8` CheckInit, `2e8` SetDChain, `33c`/`350`
Set/ResetDmaIntrHandler, `598` SetDma, `cac`/`cf8`/`d48` Set{MS,SM}Flag/SetSubAddr.

## 1. The SIF flag protocol at boot

`SIFMAN::sceSifInit` (0x148) **is** the BOOT-10 handshake, and `SIFINIT`
(doc 11) is only the trigger that calls it via `sifman` ordinal 5.

Traced at 0x148-0x264:
1. `0xbf801570 |= 0x8800` — `DPCR2`, sets the enable bit (bit 15, bit 11) for
   the two second-bank nibbles that gate SIF0/SIF1 (chan 9, chan 10) —
   confirms BOOT-11j's "high bit is the enable" from the *setter's* side, not
   just the reference dump.
2. Clears `0xbf801528`/`0xbf801538` (CHCR of chan 9 / chan 10, `base+8` — these
   are the **uncached KSEG1 mirrors** of the physical DMA registers;
   `0xBD00xxxx = 0x1D00xxxx | 0xA0000000`, confirming BOOT-10's IOP register
   column is the physical/KUSEG address and this code always uses the KSEG1
   form).
3. Two unexported helpers at `0x8c` (`GetMSFlag`, reads `0xBD000020` twice
   until stable) and `0xc0` (`GetSMFlag`, reads `0xBD000030` the same way) —
   `0xBD000020`/`0xBD000030` are exactly `0x1D000020`/`0x1D000030` from BOOT-10's
   table (`MSFLG`/`SMFLG`), confirming the physical addresses via an
   independent code path from doc 23's simulator trace.
4. `sceSifInit` calls `GetMSFlag` in a `CpuSuspendIntr`/`CpuResumeIntr`-guarded
   spin loop, masking for bit `0x10000` (**`SIF_STAT_SIFINIT`** [header],
   confirmed live here as the literal `lui $17,1` mask) — this *is* BOOT-10a:
   "the IOP polls `MSFLG` and proceeds only when that bit appears," now pinned
   to a specific function rather than inferred from boot-list ordering.
5. Once seen: `jal sceSifSetDChain` (0x2e8, arms SIF0's receive channel —
   `CHCR=0x41000300`, `BCR=0x20`, matching BOOT-11e exactly), `jal
   sceSifSetSubAddr(0)` (clears `SMCOM` — the *real* address gets published
   later, once a command buffer exists), then writes `SMFLG = 0x10000` at
   `0xBD000030` (raising `SIF_STAT_SIFINIT` back at the EE, matching doc 23's
   traffic trace `IOP write SMFLG 0x00010000`) and sets an internal
   "SIF is up" latch (`0xf7c` in `SIFMAN`'s `.bss`).

`SIFCMD`'s own module entry (`0xd0`) runs after this (boot-list order:
`SIFMAN` then `SIFCMD`, doc 3) and does **not** repeat the MSFLG wait — it
consults the boot record (doc 11) and, on the normal path, zero-initialises a
32-entry, 8-byte-stride cmd-handler table at `0x18c0` and a 32-word SREG table
at `0x19c0` (`sceSifGetSreg`/`sceSifSetSreg` index this second table directly,
`stride 4`), installs a system-cmd-buffer descriptor, and registers its DMA
interrupt dispatcher (calls at `0x230`-`0x274`; not fully traced past
"RegisterIntrHandler(0x2b, ...)" / a DMA-intr-handler install / an
event-flag-ish create with an internal name string — **unresolved**, see §5).
It does **not** write `SIF_STAT_CMDINIT` (`0x20000`) here in the disassembled
range — that bit's setter was not located; likely set alongside the cmd-buffer
install by code past what was traced, or by `SIFCMD`'s own `sceSifInitCmd`
being called once from the boot path (distinct from the EE calling it again
from `sceSifInitRpc`). **Unresolved**: exact writer of `SIF_STAT_CMDINIT` on
the IOP side.

On the **client (EE, `m1.dis`)**:
- `sceSifInitCmd` (`0x111c10`) registers the EE's DMAC handler on **channel 5**
  (`AddDmacHandler($a0=5, handler=_SifCmdIntHandler-equivalent)`,
  `EnableDmac($a0=5)` at `0x111d54`-`0x111d68`) — **this is the confirmation
  the task asked for**: the client's completion path is driven by the EE's
  DMAC **channel 5** interrupt (SIF0, IOP→EE, per BOOT-11's channel table),
  not polling. `sceSifExitCmd` (`0x111eb0`) tears the same handler down.
- After enabling the DMAC handler it polls `sceSifGetReg(4)` (SBUS/SIF control
  bit) and `sceSifGetReg(2)` (`SIF_SYSREG_MAINADDR`? — a local, software SREG,
  ordinal 2 in the `0x80000000|n` software-register space [header
  `sifdma.h`'s `_sif_regs` enum]) before calling `sceSifSetDChain` (SIF1 send
  chain) and, on the first-init path (`0x111c50`-`0x111df8`), sends
  `sceSifSendCmd(cid = $16|1 = 0x80000001 = SIF_CMD_SET_SREG, ...)`, a
  self-directed register push. This is plumbing beneath `SifInitRpc`, not the
  RPC handshake itself — see next.
- `sceSifInitRpc` (`0x10ffb8`) is the actual client handshake, traced in full:
  1. If already active (a static flag at data `+0x4384`) and mode matches:
     early-return.
  2. `jal sceSifInitCmd`.
  3. `DIntr()`; register **four** system cid handlers via `sceSifAddCmdHandler`:
     `0x80000008` (RPC_END), `0x80000009` (RPC_BIND), `0x8000000A` (RPC_CALL),
     `0x8000000C` (RPC_RDATA) — all four, because the same source compiles for
     both EE and IOP and an EE program can itself host an RPC server
     (`sceSifRegisterRpc` is exported identically on both sides); `EIntr()`.
  4. `sceSifGetReg($17|2)` — reads **`SIF_SYSREG_RPCINIT`** (`0x80000002`,
     software register 2 [header `sifdma.h`]). If already nonzero, RPC is
     already up (someone else initialised it) — done.
  5. Otherwise: `sceSifSendCmd(cid = $17|2 = 0x80000002, packet, size=0x40,
     dest_extra=0, size_extra=0x10)` — **this is `SIF_CMD_INIT_CMD`
     (`0x80000000|2`)**, sent with a 0x40-byte header/body and 0x10 bytes
     (4 words) of "extra" data appended past the packet body. This lines up
     exactly with doc 24's observed boot packet: `psize 0x14, cid 0x80000002`
     carrying a 32-bit EE RAM address for the IOP to reply into — the extra
     4 words are (at least) that receive-buffer address. **The IOP's handler
     for `INIT_CMD` was not directly located in `SIFCMD`'s traced code range**
     (see §5) but doc 24 already showed the IOP answering an `INIT_CMD`-shaped
     packet's EE-address field by targeting the reply there, so the receiving
     side does parse and retain it.
  6. Spin: `sceSifGetSreg(0)` (**`SIF_SREG_RPCINIT`** [header
     `sifcmd-common.h`], a *local* SREG, index 0) until nonzero. The natural
     mechanism (not directly re-traced here, but implied by `SIF_CMD_SET_SREG`
     existing precisely for this) is that the IOP answers `INIT_CMD` with a
     `SET_SREG(index=0, value=1)` command back at the EE, which the EE's own
     `SET_SREG` system handler (installed by `sceSifInitCmd`, not separately
     traced) applies to local SREG 0, breaking this loop. **Partially
     inferred — mark as unresolved in detail, see §5.**
  7. Once unblocked: `sceSifSetReg($4=0x80000000|2=RPCINIT, $5=1)` — marks the
     local software register so a second `SifInitRpc` call short-circuits at
     step 4.

So a minimal IOP satisfying `SifInitRpc`:
- Must run `SIFMAN`'s handshake (BOOT-10, already implemented per doc 23) so
  `SifInitCmd`'s DMAC/SBUS polls succeed.
- Must accept a `0x80000000|2` (`INIT_CMD`) packet whose body carries the EE's
  receive-buffer address (and, per BOOT-11a, obey the header word-count/flags
  framing already implemented for the boot transfer) and, in response, send a
  `0x80000000|1` (`SET_SREG`) packet with `index=0, value=1` back to that
  address, framed as SIF0 per BOOT-11c/BOOT-11d (IOP send block + EE
  destination tag). Without that reply, `SifInitRpc` spins forever at step 6.

## 2. The command packet

Header layout, from `sifcmd-common.h` [header] and corroborated by the packet
assembly code at `SIFCMD`'s internal `_SifSendCmd` (`0x3e4`, shared by
`sceSifSendCmd`/`isceSifSendCmd`) and the EE's mirror at `m1.dis 0x1127a0`:

```
struct SifCmdHeader_t {
    u32 psize : 8;   // packet size in BYTES, 16..112 -- see the correction below
    u32 dsize : 24;  // payload ("extra") size in bytes
    void *dest;       // destination address for the extra payload (may be NULL)
    int   cid;        // command id; cid < 0 (bit31 set) => "system" range
    u32   opt;         // caller-defined
};
```

**Correction: `psize` is a byte count, not a count of 16-byte units.**
`_SifSendCmd` (`0x3e4`) opens with `addiu $2,$7,-0x10` / `sltiu $2,$2,0x61`,
so the accepted range is `16 <= psize <= 112`, and it stores the value with
`sb` into the header's byte 0 and passes the same value as the DMA transfer's
size. The boot's `0x14`-byte `INIT_CMD` packet doc 24 observed is `psize =
0x14` on the wire, which only makes sense as bytes.

`_SifSendCmd` does **not copy the packet**. The stack area it builds is the
DMA descriptor list; the packet is transferred in place out of the caller's
own buffer, which therefore has to stay valid until the transfer completes.
The out-of-band "extra" block (`src_extra`/`dest_extra`/`size_extra`) is a
second descriptor placed *ahead* of the packet in the same run, so the EE's
channel interrupt fires once, after both.

**Dispatch on receipt** (`sceSifAddCmdHandler`, `0x35c`): `cid` is tested
`bgez` — nonnegative `cid` indexes the **user** handler table (installed via
`sceSifSetCmdBuffer`, storage at `0x18a4`); negative `cid` (i.e. the top bit
set, `0x80000000`+) indexes the **system** handler table (`sceSifSetSysCmdBuffer`,
storage at `0x189c`). Both tables are simple `{handler, harg}` arrays indexed
by the low bits of `cid`. This is how the reserved commands and ordinary
user-registered RPC servers share one receive path without a lookup by value.

**Reserved commands** (`SIF_CMD_ID_SYSTEM | n`, `n` from `sifcmd-common.h`
[header], cross-checked against the addresses actually installed by
`sceSifInitRpc` at `0x6c0`-`0x7b0`):

| n | name | installed by (IOP) | body |
|---|---|---|---|
| 0 | CHANGE_SADDR | `SIFCMD` module entry (system table, not individually traced) | not traced — **unresolved** |
| 1 | SET_SREG | ditto | writes `sreg[index] = value` via `sceSifSetSreg` — inferred from `sceSifSetSreg`'s own body (`0x40`) matching `SifCmdSRegData_t{header,index,value}` [header] exactly |
| 2 | INIT_CMD | ditto | receives EE's receive-buffer address; replies `SET_SREG(0,1)` — inferred (§1 step 5/6), not directly traced |
| 3 | RESET_CMD | ditto | not traced — **unresolved**; header shape is `SifCmdResetData_t{header,arglen,mode,arg[80]}` [header], matches `REBOOT`'s role (doc 12) |
| 8 | RPC_END | **client only** (registered by `sceSifInitRpc`, both platforms) | client's completion signal — see §3 |
| 9 | RPC_BIND | `SIFCMD` `0xc48` (registered by `sceSifInitRpc` at `0x768`) | see §3 |
| 0xA | RPC_CALL | `SIFCMD` `0xe08` (registered at `0x780`) | see §3 |
| 0xC | RPC_RDATA | `SIFCMD` `0xa68` (registered at `0x798`) | see §3 |

The IOP's reply path is `isceSifSendCmd`/`sceSifSendCmd` → `_SifSendCmd`
(`0x3e4`) → (unexported helper at `0x1660`, not traced past the stub table —
likely `GetThreadId` for a rec_id, called by nearly everything, so probably
just a correlation id) → `sceSifSetDma`/hardware. Framing on the wire (SIF0
send block + EE destination tag, id=1 `cnt`) already matches BOOT-11c/BOOT-11d
as implemented; nothing new was found to contradict it. **Acknowledgement**:
the CMD layer has no separate ack — a system-command handler simply replies
with a new packet (SET_SREG for INIT_CMD, RPC_END for BIND/CALL/RDATA); there
is no "I got your packet" handshake beneath that.

### `sceSifSendCmd` and `isceSifSendCmd` (ordinals 12 and 13), in full

Read because `MSIFRPC.IRX` imports both and a rebuild that leaves them
reserved gives a title's SIF bridge a working-looking no-op.

```sh
python3 tools/irxinfo.py <outdir>/SIFCMD --dump-load <outdir>/SIFCMD.text
python3 tools/romdis.py <outdir>/SIFCMD.text --cpu iop --vma 0 --range 0x3e4 0x4e0
```

Both are two-instruction shims onto `_SifSendCmd(cid, mode, pkt, psize,
src_extra, dest_extra, size_extra)` — `0x4e0` passes `mode = 0`, `0x524`
passes `mode = 1` — and in this version **mode is used for exactly one
thing**: `andi $5,1` at `0x484` selects whether the `sceSifSetDma` call is
bracketed by `CpuSuspendIntr`/`CpuResumeIntr`. Nothing else differs: same
range check, same header writes, same descriptor list, same destination, same
return value. Ordinal 13 is ordinal 12 for a caller that is already inside an
interrupt handler.

`_SifSendCmd`, in order:

1. `psize` outside `[16, 112]` → **return 0**, nothing written.
2. `size_extra > 0` (signed): descriptor 0 is `{src_extra, dest_extra,
   size_extra, attr 0}`, the header's `dsize` field becomes `size_extra` and
   its `dest` becomes `dest_extra`. Otherwise `dest` is zeroed and there is
   one descriptor.
3. The header's byte 0 takes `psize` and `+8` takes `cid`. **`opt` at `+0xc`
   and everything from `+0x10` on are left exactly as the caller wrote them**
   — the library never touches the body.
4. The last descriptor is `{pkt, <the EE's packet buffer>, psize, attr 4}`.
   The EE address comes from the module's own state word, which only the
   `CHANGE_SADDR` handler and the `opt == 0` `INIT_CMD` handler ever write;
   before either arrives it is zero and the reference transfers to EE address
   0 with no guard.
5. `sceSifSetDma(list, n)`, bracketed by `CpuSuspendIntr`/`CpuResumeIntr`
   unless `mode & 1`.

**The return value is the DMA layer's, and it is a real signal.**
`sceSifSetDma` (`SIFMAN` `0x598`) answers **0 when its 32-entry pending queue
cannot take `n` more descriptors**, and otherwise a nonzero id
(`seq << 16 | index << 8 | n`). So 12 and 13 return 0 both for a rejected
`psize` and for a full queue, and `MSIFRPC` leans on it: its SET_SREG
handshake and its `cid 0x18` reply both **loop until the answer is nonzero**,
delaying in between. A rebuild that always answers 0 hangs those loops; one
that always answers nonzero drops packets silently.

### The SREG file, and the handshake that actually uses it

Ordinal 6 (`0x28`) is `sceSifGetSreg(index)` [header] and ordinal 7 (`0x40`)
`sceSifSetSreg(index, value)`: a plain load and store into a **32-word array
in `SIFCMD`'s own `.bss`** (`0x19c0` in the rom0 module), zeroed by the module
entry, with **no bounds check on either**. The array's address is kept at
`+0x1c` of the module's state record, which is how the third writer finds it:

- the **`SET_SREG` system handler** (`0x0`), installed as the handler for
  `cid 0x80000001`, does `sreg[packet + 0x10] = packet + 0x14` — so any such
  packet the EE sends writes straight into the file, at an index the sender
  chooses and nothing validates.

That is the whole mechanism, and §1 step 6's "partially inferred" note can be
closed: the two sides really do talk through it, symmetrically.

- The IOP's own `sceSifInitRpc` (`0x6c0`) sends `SET_SREG(0, 1)` *outbound*
  the moment its four RPC handlers are installed, then waits on bit `0x800`
  of the system status flag — the bit the `INIT_CMD` handler raises for
  `opt != 0`. It never writes its own register 0.
- The SDK's EE client never sends `SET_SREG` at all: `sceSifInitCmd` sends
  `INIT_CMD` (`opt 0`) the first time and `CHANGE_SADDR` on a re-announce,
  and `sceSifInitRpc` sends `INIT_CMD` (`opt 1`) and spins on its **own**
  register 0. **The IOP's register 0 is therefore never written by the SDK.**
- `MSIFRPC` and the title's own EE half of it are the first thing that uses
  the file in earnest, and they use register **1**: each side sends
  `SET_SREG(1, 1)` to the other — looping while `sceSifSendCmd` answers 0 —
  and then spins on `GetSreg(1)` of its own file until it reads nonzero.
  `MSIFRPC`'s spin delays between reads.

Two consequences for a rebuild. Its command dispatcher needs a
`cid 0x80000001` case writing that file, or the title's half of the handshake
never lands and `MSIFRPC` spins for ever. And ordinal 6 must read the same
file — a reserved slot that returns whatever happens to be in `$v0` will
sometimes look nonzero and let the loop out early, which is worse than
hanging because it is intermittent.

### The ordinals past this project's tables, read

`CRI_ADXI`, `MCSERV`, `PADMAN` and `EZMIDI` — the modules a title loads —
import six entries that fall past a 23-slot table and so bind to `jr $ra`.

```sh
python3 tools/romdis.py <outdir>/SIFMAN.text --cpu iop --vma 0 --range 0x148 0x268
python3 tools/romdis.py <outdir>/SIFMAN.text --cpu iop --vma 0 --range 0x2d8 0x2ec
python3 tools/romdis.py <outdir>/SIFCMD.text --cpu iop --vma 0 --range 0x1210 0x133c
```

**`sifman` 5 and 29 are a pair, and this document's §0 map has them right
where `src/iop/eesync.cpp` did not.** Ordinal 5 (`0x148`) is `sceSifInit`,
not `SetDChain` — `SetDChain` is ordinal **6** (`0x2e8`). Ordinal 29
(`0x2d8`) is `sceSifCheckInit`: a bare load of the one `.bss` word ordinal 5
sets as the last thing it does, which is also the word ordinal 5 tests on
entry to make itself idempotent. Every client in the set above is written the
same way — `if (!sceSifCheckInit()) sceSifInit();` — so a rebuild that leaves
29 unbound has a client "initialise" a bus already carrying traffic, and if
its ordinal 5 is something else entirely, that call does something else
entirely.

**`sifman` 22 and 24 are likewise a pair**, and the same correction applies:
22 writes **MSFLG** (`0xBD000020`) and 24 writes **SMFLG** (`0xBD000030`).
Since a write from the IOP *clears* MSFLG and *sets* SMFLG (BOOT-10c), they
are opposite actions, not variants.

**`sifman` 32** (`sceSifSetDmaIntr` [header]) exists only in the newer
`SIFMAN` a title's `IOPRP` carries; rom0's slot 32 is the shared `jr $ra`.
It is ordinal 7 with two extra arguments, `(function, arg)`, appended to a
per-batch table of completion callbacks; the sending channel's completion
handler runs every callback of the batch that just finished, once, from
interrupt context, before starting the next batch. A null function makes it
ordinal 7 exactly, and the range check, the queue-full `0` and the non-zero
id are 7's unchanged. `sifcmd` **28** and **29** are the same relationship to
12 and 13, routed through 32 instead of 7.

**`sifcmd` 24 and 25** (`sceSifRemoveRpc`, `sceSifRemoveRpcQueue` [header],
`0x1210`/`0x12a8`) are the inverses of 17 and 19: under an interrupt bracket,
unlink the record from its list and answer it — the reference answers the
*predecessor* where it had one — or answer null when it was not on the list.
Nothing else is touched: a removed server's own link is left dangling and the
queue's pending requests are not looked at. Both observed callers use them on
a shutdown path and ignore the result.

**Why 32 and 28 matter more than they look.** `PADMAN` has a mode in which
it sends its pad batch through 32 (or through `sifcmd` 28), stores the id,
and — if the id is non-zero — **waits on a semaphore its completion callback
signals**. Bound to `jr $ra`, ordinal 32 answers whatever is in `$v0`: zero
takes the "not queued" branch and loses the data silently, and non-zero
parks the thread for ever on a callback that will never run. That second
shape — a no-op that presents as success — is the one worth designing
against.

## 3. The RPC layer

**Server registration (IOP)**: `sceSifRegisterRpc` (`0x1130`) fills a
`SifRpcServerData_t` [header, `sifrpc-common.h`] — `sid`, `func`, `buf`,
`size`, `cfunc`, `cbuf`, `size2` from the caller — and links it onto the
queue's list (either directly as `qd->link` if empty, or appended via the
`link` chain, matching the header's `link`/`next` fields). No packet is sent;
this is purely local bookkeeping. `sceSifSetRpcQueue`/`sceSifRemoveRpcQueue`
maintain a *global* list of all queues (`0x2a60`) so `sceSifGetOtherData`'s
lookup helper (`0xbe8`, walks queues → servers by matching `sid`) can find a
server across threads.

**BIND request** (`SIF_CMD_RPC_BIND`, `0x9`): client → server. Handler at
`0xc48` (matches import ordinal for `sceSifBindRpc`'s counterpart): looks up
the requested `sid` via the `0xbe8` walk, and if found builds a reply in-place
(copies `buf`/`size`/`cbuf`/`size2` from the found `SifRpcServerData_t` into
the reply packet's corresponding fields) and sends it back as `isceSifSendCmd
(cid = 0x80000008 /* RPC_END */, ...)`. The reply packet embeds a `cid` field
(distinct from the outer header's cid) set to the *original* request's system
id (`0x80000009` here) — this is `SifRpcRendPkt_t.cid` [header] — so a single
client-side END handler can tell BIND/CALL/RDATA replies apart.

**Client bind** (`sceSifBindRpc`, `0xd08`): looks up its own send-slot via
`0x818` (a get-a-free-request-packet helper shared with `CallRpc`/`GetOtherData`,
walking a fixed pool at data `0x2a40`), fills `cd->hdr`, sends the BIND packet
(`cid=0x80000009`, size `0x40`) via `sceSifSendCmd`/`isceSifSendCmd` depending
on the `NOWAIT` mode bit (tested `andi $19,0x1`). It does **not** itself wait
for the reply here — completion is via the client's registered `cid=8`
(RPC_END) handler updating `cd`, and `sceSifCheckStatRpc`/blocking inside
`CallRpc` is what actually waits (see below). `LOADFILE`'s bind id, read
directly from `m1.dis` (`SifLoadFileInit`, `0x112c30`): **`sid =
0x80000006`**, bound with a retry loop (up to `0x1001` attempts) — matches the
well-known ps2sdk `LOADFILE_IRX` bind id.

**CALL request** (`SIF_CMD_RPC_CALL`, `0xA`): handler at `0xe08` links the
incoming request onto the target server's queue (`sd->link`/`0x38(sd)` chain)
for later dispatch by `sceSifRpcLoop`/`sceSifGetNextRequest`/`sceSifExecRequest`
running on an IOP thread — it does **not** call the handler function inline
from interrupt context; it defers to the registered `SifRpcDataQueue_t`
thread. `sceSifExecRequest` (`0x13a4`) is what actually calls `sd->func(fno,
buf, size)` (loaded from `sd+4`), takes the returned pointer/size, and (unless
that returned size is 0, in which case it treats the call as "answered
directly by side channel") sends the result back as an `RPC_END` packet whose
embedded `cid = 0x8000000A`.

**Client call** (`sceSifCallRpc`, `0xed4`): grabs a free request slot (`0x818`
again), copies `sendbuf`/`ssize`/`recvbuf`/`rsize`/`end_function`/`end_param`
into it and the bound `cd`, sets mode (`NOWAIT` bit steers straight vs.
interrupt-context send), and sends `cid=0x8000000A` sized `0x40`, with
`src_extra=sendbuf, size_extra=ssize` — i.e. the *argument buffer* rides as
the CMD layer's "extra" payload, not inline in the fixed 0x40-byte header,
confirming `SifCallRpc`'s `sendbuf`/`ssize` map directly to `SifSendCmd`'s
`src_extra`/`size_extra` parameters (`sifcmd-common.h`'s own signature already
says this [header]; this confirms it end-to-end in both the client and the
`SIFCMD` internals).

**RDATA** (`SIF_CMD_RPC_RDATA`, `0xC`, handler `0xa68`): services
`SifRpcGetOtherData` — server-initiated extra copy from the server's buffer
straight to an EE address, independent of the CALL/END cycle, replying
`RPC_END` with `cid=0x8000000C`. `sceSifGetOtherData` (client, `0xad4`) sends
this request the same way BIND/CALL do.

**Client-side completion — confirmed, not inferred**: `m1.dis`'s
`sceSifInitCmd` wires `_SifCmdIntHandler`-equivalent to the EE's **DMAC
channel 5** (`AddDmacHandler(5, ...)`, `EnableDmac(5)`), which is SIF0 (IOP→EE)
per BOOT-11's channel table. So the reply path is interrupt-driven, not a bare
poll: every incoming SIF0 packet fires the EE's channel-5 DMA interrupt, the
handler dispatches by `cid` through the same registered-handler mechanism as
the IOP side, and a `cid=8` (RPC_END) packet is what resolves a pending
`BindRpc`/`CallRpc`/`GetOtherData`. `sceSifCheckStatRpc` (`0x1040`) is a
non-blocking poll of `cd->server`/`cd->hdr.rpc_id` matching the bound server's
generation counter — usable for `SIF_RPC_M_NOWAIT` callers — but the ordinary
blocking `SifCallRpc` path relies on the DMAC interrupt, not on spinning this.

## 4. Minimal IOP checklist

To satisfy `SifInitRpc` on an SDK-built EE client:
1. Implement BOOT-10 exactly (already done): raise `SIF_STAT_SIFINIT
   (0x10000)` in `SMFLG` only after `MSFLG`'s same bit is observed.
2. Accept `SIF_CMD_INIT_CMD` (`cid=0x80000002`), framed per BOOT-11a/b, whose
   body carries the EE's receive-buffer address (and other fields not fully
   decoded — §5); persist that address as the reply target.
3. Reply with `SIF_CMD_SET_SREG` (`cid=0x80000001`, body `{index=0,
   value=1}` per `SifCmdSRegData_t` [header]) framed per BOOT-11c/d (SIF0 send
   block + EE destination tag, id 1 `cnt`, to the address from step 2). This
   unblocks the EE's `sceSifInitRpc` spin at `sceSifGetSreg(0)`.
4. Maintain a system-cid dispatch table and register handlers for `0x9`
   (BIND), `0xA` (CALL), `0xC` (RDATA) — bodies as described in §3 — plus a
   user-cid table for RPC servers registered via `sceSifRegisterRpc`.
5. To let `LOADFILE`'s client succeed: host (or proxy) an RPC server with
   `sid = 0x80000006`. Its request/reply argument layout was not traced in
   this pass (`_SifLoadModule`'s send buffer is `0x200` bytes: a zeroed
   header, the module path `strlcpy`'d at `+0x18` up to 252 bytes, then
   caller args `memcpy`'d at `+0x114`; call uses `send_size=0x200,
   recv_size=8`) — **left for a follow-up pass**, flagged in §5.
6. Every reply (`SET_SREG` in step 3, and every `RPC_END` in §3) must go out
   as a properly-framed SIF0 packet (BOOT-11c/d), or the EE's channel-5 DMAC
   interrupt never fires and the client hangs — this is not optional plumbing,
   it is the entire completion signal (§3, "client-side completion").

## 5. Unresolved

- Exact writer of `SIF_STAT_CMDINIT` (`0x20000`) on the IOP side, and the
  handler bodies for `SIF_CMD_CHANGE_SADDR` (0) / `SIF_CMD_RESET_CMD` (3),
  were not located in the traced range of `SIFCMD`'s entry function (only
  their table slots and, for RESET_CMD, the header's struct shape).
- The exact body of `SIF_CMD_INIT_CMD`'s IOP-side handler (who parses the
  EE's receive-buffer address, what the other 3 extra words carry) was not
  directly located — inferred from the client's send parameters (§1 step 5)
  and doc 24's independently-observed boot traffic, not a disassembled handler.
  *Settled from the client's side since:* the client sends `INIT_CMD` twice.
  `sceSifInitCmd` sends it with `opt = 0` and its receive-buffer address as
  the one word after the header (`psize 0x14`); `sceSifInitRpc` sends it with
  `opt = 1` and nothing after the header (`psize 0x10`), and that is the one
  a `SET_SREG(0, 1)` answers. An IOP service that keeps the address from the
  first and answers the second completes `SifInitRpc` on both emulators
  (`src/iop/eesync.cpp`, `spec/03` BOOT-12c).
- The unexported helper at `SIFCMD 0x1660` (called by nearly every RPC
  function with a single pointer argument) was not identified — guessed as
  "get a correlation/rec id," not confirmed.
- `LOADFILE`'s RPC argument/return layout (fno values, the `0x200`-byte
  request body beyond path+args, the 8-byte reply) was not decoded — only the
  bind `sid = 0x80000006` and call-site buffer sizes are confirmed from
  `m1.dis`. `<outdir>/LOADFILE`'s own handler (imports `sifcmd` ordinals
  14/17/19/22 = InitRpc/RegisterRpc/SetRpcQueue/RpcLoop, per `irxinfo
  --imports`) was not disassembled in this pass. *Settled from the client's
  side since: §6.*

## 6. The loader's client, read

`_SifLoadModule` (`m1.dis 0x1121b0`), which `SifLoadModule` tail-calls with
`fno = 0` and `mode = 0`:

```
addiu $a0, $sp, 0x10 ; memset(buf, 0, 0x200)         the request, 512 bytes
addiu $a0, $sp, 0x18 ; strlcpy(buf + 8, path, 0xfc)   the path at +8
sw    $a2, 0x10($sp)                                   arg_len at +0 (0 here)
addiu $a0, $sp, 0x114; memcpy(buf + 0x104, args, n)   the arguments at +0x104
sceSifCallRpc(cd = 0x1260f0, fno, mode, buf, 0x200, buf, 8, 0, 0)
lw    $v0, 0x10($sp)                                   the answer's word 0
lw    $v0, 0x14($sp); sw $v0, 0($a3)                   word 1 -> *modres
```

So function 0 takes `{ arg_len, result, path[252], args[252] }` and answers
`{ id | error, modres }` into the same buffer's first two words, and
`SifLoadModule` returns word 0. `SifLoadFileInit` (`0x112970`) binds
`0x80000006` in a loop until `cd->server` (`cd + 0x24`) is non-null.

What the reference answers, run on the reference under PCSX2 (`-elf` with
the M1 program, and a copy of it asking for `rom0:NOSUCH`):

```
# m1: CreateThread -> 3
# m1: SifLoadModule rom0:SIO2MAN -> 25
# m1: SifLoadModule rom0:NOSUCH -> -203
# m1: done
```

— a module id for a file it has, **-203** for one it has not, and `done`
afterwards with every thread of the program waiting, which is what settles
`spec/05` SYS-10k: the boot thread is still there to be picked. (Our kernel
answers `CreateThread -> 2` for the same program while the program runs on
the boot thread itself; with a thread of its own it answers 3 too.)
- `SIF_SYSREG_MAINADDR`/`SIF_SYSREG_SUBADDR` (software regs 0/1) were named
  from the header only; no code path setting or reading them was traced.
