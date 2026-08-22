// The EE kernel's entry point, and the serial console the whole boot logs to.
//
// docs/spec/04-ee-kernel.md EE-4: KERNEL is copied to physical zero and entered
// at 0x80001000, so the vector page of `vectors.S` sits below this and the entry
// point is the first thing after it. The layout is not a choice; it is what the
// reset path's copy makes true.

#include <stdint.h>

namespace {

// The EE's serial transmit register and its readiness flag. The ROM prints a
// byte at a time through these, which is how a boot log is observable at all
// (docs/analysis/22-ee-execution.md).
constexpr uintptr_t kSioIsr = 0xB000F130;
constexpr uintptr_t kSioTx = 0xB000F180;
constexpr uint32_t kSioTxReady = 0x8000;

// EE-4a: the boot's result is in this scratchpad word, not in a register.
constexpr uintptr_t kBootResult = 0x70003FF0;

constexpr uint32_t kStatusBev = 1u << 22;

// What a program runs under: interrupts enabled (IE and EIE, SYS-12d) with the
// INTC and DMAC lines unmasked (IM2, IM3) and `ei`/`di` allowed (EDI). The
// SDK's client toggles EIE itself and relies on the rest being set already;
// the reference's interrupt exit leaves IE set on its way back to a program
// (docs/analysis/35).
constexpr uint32_t kStatusProgram = 0x00030C01;

[[nodiscard]] uint32_t readWord(uintptr_t address) {
    return *reinterpret_cast<volatile uint32_t *>(address);
}

void writeByte(uintptr_t address, uint8_t value) {
    *reinterpret_cast<volatile uint8_t *>(address) = value;
}

void putByte(uint8_t value) {
    while ((readWord(kSioIsr) & kSioTxReady) != 0) {
    }
    writeByte(kSioTx, value);
}

// `Status` is the one register this file has to reach for, and `mfc0`/`mtc0`
// name it in the instruction. `sync.p` has no mnemonic in this assembler, so it
// goes in as the word it is.
[[nodiscard]] uint32_t readStatus() {
    uint32_t value;
    asm volatile("mfc0 %0, $12" : "=r"(value));
    return value;
}

void writeStatus(uint32_t value) {
    asm volatile("mtc0 %0, $12\n\t.word 0x0000040F" : : "r"(value) : "memory");
}

// The name the archive holds it under, in the fixed-width field the request
// carries (spec/01 ARC-2: ten bytes, NUL-padded).
alignas(4) const char kWanted[16] = "ROMVER";

alignas(16) uint8_t romver[16];

}  // namespace

extern "C" {

// sif.cpp
uint32_t sifHandshake();
void *sifExchange(const char *name, uint32_t verb, void *destination,
                  uint32_t offset, uint32_t length);

// thread.cpp
extern uint32_t memory_size;

// tlb.cpp -- EE-12
void installTlb();

// program.cpp -- EE-9c. It does not return when it succeeds, because the
// program it loads replaces this one; when the archive has nothing to load it
// comes back with -1, which is why it is not declared `[[noreturn]]`.
int bootDefault();

// `entry.S` points `$sp` at the top of this before calling `kernelMain`.
alignas(16) uint8_t kernel_stack[0x1000];

// Written in C++ but named for the assembly that calls it: `syscall.S`
// reports through these two.
void print(const char *text) asm("_print");
void printHex8(uint32_t value) asm("_print_hex8");
void printHex32(uint32_t value);

void print(const char *text) {
    for (const char *at = text; *at != '\0'; at++) {
        putByte(static_cast<uint8_t>(*at));
    }
}

void printHex8(uint32_t value) {
    for (int shift = 4; shift >= 0; shift -= 4) {
        const uint32_t nibble = (value >> shift) & 0xF;
        putByte(static_cast<uint8_t>(nibble < 10 ? '0' + nibble
                                                 : 'a' + nibble - 10));
    }
}

// Kept without a caller on purpose: docs/analysis/29 records that printing a
// register from the running kernel is the instrument that reaches inside the
// image on the working target, and it is wanted the moment something else
// does not behave.
void printHex32(uint32_t value) {
    for (int shift = 28; shift >= 0; shift -= 4) {
        const uint32_t nibble = (value >> shift) & 0xF;
        putByte(static_cast<uint8_t>(nibble < 10 ? '0' + nibble
                                                 : 'a' + nibble - 10));
    }
}

[[noreturn]] void kernelMain() {
    // EE-4a: the boot's result is RDRAM's return, the size of main memory
    // (EE-2b); SYS-8a measures the top of memory from it.
    memory_size = readWord(kBootResult);

    print("# PS2BiosRebuild EE kernel: entered at 0x80001000.\n");

    // The vector page and the dispatch tables came with us -- they are part of
    // this file's image -- so there is nothing to install. What does have to
    // change is `Status`: EE-1 leaves BEV *set*, which sends exceptions to the
    // ROM's vectors at 0xBFC00200. Clearing it is what points them at the page
    // this kernel just brought with it.
    writeStatus(readStatus() & ~kStatusBev);

    // EE-12: the reset mapped the scratchpad and nothing else. The kernel
    // itself runs through KSEG0 and KSEG1 and needs no more; a program reaches
    // memory and hardware through KUSEG, and what it finds there is this.
    installTlb();

    // BOOT-10: meet the IOP. Until this returns the machine has one working
    // processor; after it, both know where the other's memory is.
    sifHandshake();
    print("# The IOP answered; the SIF handshake is complete.\n");

    // Ask the IOP for a file out of the archive. Only the IOP can read the
    // ROM's file table on its side of the bus, so this is the shape every later
    // `rom0:` read has: a name out, bytes back.
    sifExchange(kWanted, 1, romver, 0, sizeof(romver));
    print("# ROMVER, fetched from the archive across the SIF: ");
    print(reinterpret_cast<const char *>(romver));

    // EE-9c: and then the boot ends the way the reference's does, by running
    // the program the archive holds for it. Nothing is unmasked in either
    // controller yet, so enabling interrupts here arms them for the program
    // without delivering any to the kernel's own boot.
    writeStatus(readStatus() | kStatusProgram);
    bootDefault();

    // Only reached when there was nothing to run. Stop where it can be seen.
    for (;;) {
    }
}

}  // extern "C"
