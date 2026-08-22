// What every compiled IOP module shares: the library table's binary layout,
// and the two-instruction import stubs the loader binds.
//
// docs/spec/02-module-abi.md IRX-4 (the 20-byte table header), IRX-5 (an
// export table's entries and terminator), IRX-8 (an import stub is `jr $ra`
// then `addiu $v0, $zero, ordinal`, exactly) and IRX-9 (the loader rewrites
// only the stub's first word). The export side is a struct, because a table
// of function pointers gets its R_MIPS_32 fixups from the linker for free;
// the import side is assembly, because IRX-8 specifies instruction words and
// a compiler would choose its own. The macros below write that assembly so
// a module states its imports as a list rather than as a block it must get
// byte-exact by hand.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ps2::module {

inline constexpr uint32_t kExportMagic = 0x41C00000;

// IRX-4 and IRX-5: a table of N entries plus the zero terminator (IRX-5b).
// Kept mutable on purpose: the loader overwrites `magic` with the registry
// link when it registers the table (IRX-4c).
template <size_t N>
struct ExportTable {
    uint32_t magic;
    uint32_t clients;
    uint16_t version;
    uint16_t flags;
    char tag[8];
    void (*entries[N + 1])();
};
static_assert(offsetof(ExportTable<1>, entries) == 0x14);

// IRX-7: slots 0 and 1 are reserved hooks that nothing imports, kept
// occupied rather than removed (IRX-6a), so they need somewhere to point.
inline void reservedHook() {}

// An export slot is any function; the table holds them as one type. A call
// through a bound import lands in the real signature, not this one.
template <typename F>
[[nodiscard]] constexpr void (*slot(F *function))() {
    return reinterpret_cast<void (*)()>(function);
}

}  // namespace ps2::module

// An import table, as IRX-8 wants it, from a list:
//
//     PS2_IMPORTS_BEGIN("intrman\0", 0x0102)
//     PS2_IMPORT(_import_intrman_register, 4)
//     PS2_IMPORTS_END()
//
// The tag is the eight bytes of IRX-4a, so a shorter name carries its own
// NUL padding. The table lives in `.data`, like the assembly it stands in
// for: IOP memory carries no execute permission to lose. `.set noreorder`
// keeps the assembler from touching the delay slot -- the `addiu` must be
// the literal next word, ordinal and all. Each macro is one top-level `asm`
// statement, and the compiler emits them in source order.
#define PS2_IMPORTS_BEGIN(tag8, version)                                  \
    asm(".pushsection .data,\"aw\"\n"                                     \
        ".set noreorder\n"                                                \
        ".align 2\n"                                                      \
        ".word 0x41E00000\n"                                              \
        ".word 0\n"                                                       \
        ".short " #version "\n"                                           \
        ".short 0\n"                                                      \
        ".ascii \"" tag8 "\"\n");
#define PS2_IMPORT(symbol, ordinal)                                       \
    asm(".globl " #symbol "\n" #symbol ":\n"                              \
        "jr $ra\n"                                                        \
        "addiu $v0, $zero, " #ordinal "\n");
#define PS2_IMPORTS_END()                                                 \
    asm(".word 0\n"                                                       \
        ".set reorder\n"                                                  \
        ".popsection\n");
