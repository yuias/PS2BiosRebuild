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
    u32 psize : 8;   // packet size in 16-byte units, 1..7 (max packet = 112 B)
    u32 dsize : 24;  // payload ("extra") size in bytes
    void *dest;       // destination address for the extra payload (may be NULL)
    int   cid;        // command id; cid < 0 (bit31 set) => "system" range
    u32   opt;         // caller-defined
};
```
`_SifSendCmd` packs the header + up to 6 more quadwords of caller data into a
stack buffer (`sll $3,$17,4` — 16-byte slots, loop bound matches "packet size
max 7×16" from the header comment) before handing it to the DMA layer, i.e.
the wire packet literally *is* this struct followed by up to 96 bytes of
inline body, with a separate out-of-band "extra" DMA (`src_extra`/`dest_extra`
/`size_extra`) for anything bigger — this is the mechanism doc 24 already
observed framed on the wire (a `0x14`-byte `psize` for the boot's `INIT_CMD`-
shaped packet).

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
- The unexported helper at `SIFCMD 0x1660` (called by nearly every RPC
  function with a single pointer argument) was not identified — guessed as
  "get a correlation/rec id," not confirmed.
- `LOADFILE`'s RPC argument/return layout (fno values, the `0x200`-byte
  request body beyond path+args, the 8-byte reply) was not decoded — only the
  bind `sid = 0x80000006` and call-site buffer sizes are confirmed from
  `m1.dis`. `<outdir>/LOADFILE`'s own handler (imports `sifcmd` ordinals
  14/17/19/22 = InitRpc/RegisterRpc/SetRpcQueue/RpcLoop, per `irxinfo
  --imports`) was not disassembled in this pass.
- `SIF_SYSREG_MAINADDR`/`SIF_SYSREG_SUBADDR` (software regs 0/1) were named
  from the header only; no code path setting or reading them was traced.
