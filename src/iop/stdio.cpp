// STDIO: the IOP's printf, for the modules that import it.
//
// The reference's STDIO formats through SYSCLIB and writes to the IOP's
// serial console over IOMAN (docs/analysis/08). Ours formats through the
// same `prnt` and puts the bytes on the console directly, because there is
// no `tty:` device to open -- the deviation is in docs/implementation.md.
//
// This is worth more than it looks. Nearly every module a title loads calls
// ordinal 4 to say what went wrong with it, and a printf that says nothing
// is why two of this project's walls took a session each to find instead of
// a line each: `EZMIDI`'s "Can NOT set timeup timer handler ..." and
// `CRI_ADXI`'s "sceSdInit failed." were both being written into a stub.

#include "module.hpp"

#include <stdint.h>

extern "C" {
// SYSCLIB ordinal 18: `prnt(writer, ctx, format, args)`, the formatter the
// reference's own STDIO uses.
int _import_sysclib_prnt(void (*out)(void *, int), void *ctx, const char *format,
                         void *args);
int _import_loadcore_register(void *table);
}

namespace {

using ps2::module::ExportTable;
using ps2::module::reservedHook;
using ps2::module::slot;

// The IOP's console byte, the address PCSX2 decodes as STDOUT and PS2e
// decodes with it. Only the low byte of the store is taken.
constexpr uintptr_t kConsole = 0xBF80380C;

void writeByte(void *, int c) {
    *reinterpret_cast<volatile uint8_t *>(kConsole) = static_cast<uint8_t>(c);
}

// `prnt` opens with a 0x200 marker of its own; drop anything that is not a
// byte the caller asked for.
void emit(void *ctx, int c) {
    if (c >= 0 && c < 0x100) {
        writeByte(ctx, c);
    }
}

int printfImpl(const char *format, ...) {
    __builtin_va_list args;
    __builtin_va_start(args, format);
    const int count = _import_sysclib_prnt(emit, nullptr, format, args);
    __builtin_va_end(args);
    return count;
}

[[gnu::used]] ExportTable<5> stdio_exports = {
    ps2::module::kExportMagic,
    0,
    0x0102,
    0,
    {'s', 't', 'd', 'i', 'o', 0, 0, 0},
    {
        slot(reservedHook),             // 0
        slot(reservedHook),             // 1
        slot(reservedHook),             // 2
        slot(reservedHook),             // 3
        slot(printfImpl),               // 4  printf
        nullptr,
    },
};

}  // namespace

PS2_IMPORTS_BEGIN("sysclib\0", 0x0101)
PS2_IMPORT(_import_sysclib_prnt, 18)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("loadcore", 0x0101)
PS2_IMPORT(_import_loadcore_register, 6)
PS2_IMPORTS_END()

extern "C" {

// IRX-10a: supersedes SYSCLIB's provisional 1.01 stub with this module's own
// 1.02 table.
int _module_start(int, char **) {
    return _import_loadcore_register(&stdio_exports) < 0 ? 1 : 0;
}

}  // extern "C"
