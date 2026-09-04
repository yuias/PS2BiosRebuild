// IOPTTY: the console the merged kernel would otherwise lose.
//
// Our own STDIO writes the console byte itself, because our IOMAN has no
// `tty:` device to open. After a title's reboot merges its own modules over
// ours (docs/analysis/45), STDIO and IOMAN both come from the disc, and the
// disc's chain is `printf` -> IOMAN's own `tty:` -> `sysmem` ordinal 14. That
// ordinal is a forwarder to a hook nothing in the reference archive installs
// (docs/spec/02-module-abi.md IRX-6c), so the whole chain ends in silence and
// the one instrument this project debugs the IOP with goes dark exactly when
// the merged kernel starts running.
//
// This module installs that hook. It is not on either reference boot list --
// it is ours, and the deviation is in docs/implementation.md -- but it
// survives a merge for the same reason our EXCEPMAN does: no source the merge
// opens carries this name, so rom0's copy is the only candidate.

#include "module.hpp"

#include <stdint.h>

extern "C" {
// SYSCLIB ordinal 18: `prnt(writer, ctx, format, args)`, the same formatter
// STDIO uses.
int _import_sysclib_prnt(void (*out)(void *, int), void *ctx, const char *format,
                         void *args);
// SYSMEM ordinal 15: install the hook ordinal 14 forwards to.
int _import_sysmem_set_kprintf(int (*hook)(void *, const char *, void *),
                               void *context);
}

namespace {

// The IOP's console byte, as in `src/iop/stdio.cpp`.
constexpr uintptr_t kConsole = 0xBF80380C;

// `prnt` opens with a 0x200 marker of its own; drop anything that is not a
// byte the caller asked for.
void emit(void *, int c) {
    if (c >= 0 && c < 0x100) {
        *reinterpret_cast<volatile uint8_t *>(kConsole) = static_cast<uint8_t>(c);
    }
}

// The hook's third argument is already the caller's variadic list: `Kprintf`
// started it and passes it on, so this must not start one of its own.
int consoleHook(void *, const char *format, void *args) {
    return _import_sysclib_prnt(emit, nullptr, format, args);
}

}  // namespace

PS2_IMPORTS_BEGIN("sysclib\0", 0x0101)
PS2_IMPORT(_import_sysclib_prnt, 18)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("sysmem\0\0", 0x0101)
PS2_IMPORT(_import_sysmem_set_kprintf, 15)
PS2_IMPORTS_END()

extern "C" {

int _module_start(int, char **) {
    (void)_import_sysmem_set_kprintf(consoleHook, nullptr);
    return 0;                                    // resident: the hook is here
}

}  // extern "C"
