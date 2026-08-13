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

// sif.S
void sifHandshake() asm("_sif_handshake");
void sifExchange(const char *name, int request, void *destination);

// program.S -- EE-9c, and it does not return.
[[noreturn]] void bootDefault() asm("_boot_default");

// `entry.S` points `$sp` at the top of this before calling `kernelMain`.
alignas(16) uint8_t kernel_stack[0x1000];

// Written in C++ but named for the assembly that calls it: `syscall.S` and
// `program.S` report through these two.
void print(const char *text) asm("_print");
void printHex8(uint32_t value) asm("_print_hex8");

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

[[noreturn]] void kernelMain() {
    // Nothing consumes the boot's result yet; reading it where the
    // specification says it is keeps the dependency recorded in code.
    [[maybe_unused]] const uint32_t boot_result = readWord(kBootResult);

    print("# PS2BiosRebuild EE kernel: entered at 0x80001000.\n");

    // The vector page and the dispatch tables came with us -- they are part of
    // this file's image -- so there is nothing to install. What does have to
    // change is `Status`: EE-1 leaves BEV *set*, which sends exceptions to the
    // ROM's vectors at 0xBFC00200. Clearing it is what points them at the page
    // this kernel just brought with it.
    writeStatus(readStatus() & ~kStatusBev);

    // BOOT-10: meet the IOP. Until this returns the machine has one working
    // processor; after it, both know where the other's memory is.
    sifHandshake();
    print("# The IOP answered; the SIF handshake is complete.\n");

    // Ask the IOP for a file out of the archive. Only the IOP can read the
    // ROM's file table on its side of the bus, so this is the shape every later
    // `rom0:` read has: a name out, bytes back.
    sifExchange(kWanted, 1, romver);
    print("# ROMVER, fetched from the archive across the SIF: ");
    print(reinterpret_cast<const char *>(romver));

    // EE-9c: and then the boot ends the way the reference's does, by running
    // the program the archive holds for it.
    bootDefault();
}

}  // extern "C"
