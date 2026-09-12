// The EE's side of the SIF, for the programs the boot runs.
//
// docs/spec/03-boot-chain.md BOOT-12 and docs/analysis/34: the command layer
// over syscalls 0x77-0x7A, one packet buffer the IOP answers into, and the RPC
// bind and call on top. It differs from the SDK's in one way: it polls that
// buffer for an answer rather than installing a channel-5 handler
// (docs/implementation.md), since the simulators that gate the boot deliver no
// EE interrupt.
//
// Unlike `src/boot/archive.hpp` this is a header over a real translation unit
// rather than a header of `static` functions: only EE programs use it, so
// there is no second instruction set to keep the definitions away from. Each
// program that links it gets its own copy of the state below, which is what
// EELOAD handing off to OSDSYS needs -- the second program initialises the
// command layer again from scratch.

#pragma once

#include <stdint.h>

namespace ps2::sifclient {

// The syscalls a program reaches the bus and its own successor through
// (spec/05 SYS-13, SYS-8d, EE-9a).
inline constexpr int kSysLoadProgram = 0x06;
inline constexpr int kSysExecPs2 = 0x07;
inline constexpr int kSysSifDmaStat = 0x76;
inline constexpr int kSysSifSetDma = 0x77;
inline constexpr int kSysSifSetDChain = 0x78;
inline constexpr int kSysSifSetReg = 0x79;
inline constexpr int kSysSifGetReg = 0x7A;
inline constexpr int kSysDeci2Call = 0x7C;

// A syscall with the arguments the caller gives it. The clobber list is every
// caller-saved register, because a handler is ordinary compiled code on the
// other side of the dispatcher and only the ABI's callee-saved set survives.
template <typename R = uint32_t, typename... Args>
[[nodiscard]] R syscall(int number, Args... args) {
    uint32_t words[4] = {0, 0, 0, 0};
    uint32_t k = 0;
    ((words[k++] = static_cast<uint32_t>(args)), ...);
    register uint32_t a0 asm("a0") = words[0];
    register uint32_t a1 asm("a1") = words[1];
    register uint32_t a2 asm("a2") = words[2];
    register uint32_t a3 asm("a3") = words[3];
    register int v1 asm("v1") = number;
    register uint32_t v0 asm("v0");
    asm volatile("syscall"
                 : "=r"(v0), "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3)
                 : "r"(v1)
                 : "memory", "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15",
                   "$24", "$25", "$31");
    return static_cast<R>(v0);
}

// BOOT-12c: the two INIT_CMDs, and the IOP's SET_SREG that ends the second.
// Blocks until the IOP has agreed that RPC may start.
void initRpc();

// Bind one server by its id; false if the IOP has no such service.
[[nodiscard]] bool bindRpc(uint32_t sid);

// Load an IOP module by the path the archive knows it as (spec/03 BOOT-12e).
// Binds the module loader itself, so it leaves that service bound and not
// whatever the caller had: the client keeps exactly one.
[[nodiscard]] bool loadIopModule(const char *path);

// One call on the bound server. `receive` takes the server's reply and must be
// quadword-aligned -- it is a DMA destination, not a return value.
void callRpc(uint32_t fno, const void *send, uint32_t send_size, void *receive,
             uint32_t receive_size);

}  // namespace ps2::sifclient
