// STDIO: the IOP's printf, for the modules that import it.
//
// The reference's STDIO formats through SYSCLIB and writes to the IOP's
// serial console over IOMAN (docs/analysis/08). SIO2MAN and the loader
// modules import ordinal 4, `printf` [header], only for diagnostics they do
// not reach on a good day; until the IOP has a console of its own, the
// library exists so those imports bind, and its printf says nothing.

#include "module.hpp"

#include <stdint.h>

namespace {

using ps2::module::ExportTable;
using ps2::module::reservedHook;
using ps2::module::slot;

int printfStub(const char *) {
    return 0;
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
        slot(printfStub),               // 4  printf
        nullptr,
    },
};

}  // namespace

extern "C" {

int _module_start(int, char **) {
    return 0;                           // resident
}

}  // extern "C"
