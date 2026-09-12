// IGREETING: the boot list's one-shot announcement of how the machine got here.
//
// docs/spec/03-boot-chain.md BOOT-8b -- boot record key 4 carries the mode
// `loadcore` was entered with, and this module selects a line from it -- and
// docs/spec/02-module-abi.md IRX-13, which is the other half of what makes
// this module worth having: it is the archive's only *one-shot* module, the
// shape whose entry returns non-resident. Every other module here stays, so
// until now nothing on the boot path exercised IRX-12a's teardown at all and
// `tools/iopsim.py` had to report zero releases against the reference's four.
//
// The wording is ours (docs/clean-room-policy.md §3); what is reproduced is
// the behaviour, which is the selection from key 4 and nothing else.

#include "module.hpp"

#include <stdint.h>

extern "C" {
int _import_loadcore_query_boot_mode(uint32_t key);
// `sysmem` ordinal 14, not `stdio` ordinal 4. The reference's own IGREETING
// uses `stdio`, and on a cold boot either would do -- but after a title's
// reboot the disc's `STDIO` replaces ours and its `printf` reaches no console
// on this machine, while `Kprintf` still does (docs/implementation.md). A
// banner that is silent on the boot it most describes is not worth printing.
int _import_sysmem_kprintf(const char *format, ...);
}

namespace {

// BOOT-8b: the key whose record holds the boot mode.
constexpr uint32_t kKeyBootMode = 4;

// BOOT-8a: a record's own 16-bit value is its header's low half, and ordinal
// 12 answers with the record's *address*, not that value.
[[nodiscard]] uint32_t recordValue(uint32_t record) {
    return *reinterpret_cast<const volatile uint32_t *>(record) & 0xFFFF;
}

// BOOT-8b's four modes. A mode this does not know is reported as its number
// rather than folded into the cold-boot line: the number is the only thing a
// reader could act on, and a wrong line is worse than a bare one.
[[nodiscard]] const char *modeText(uint32_t mode) {
    switch (mode) {
    case 0:
        return "cold start";
    case 1:
        return "restarted by request";
    case 2:
        return "restarting to replace the kernel";
    case 3:
        return "started by a replaced kernel";
    default:
        return nullptr;
    }
}

}  // namespace

PS2_IMPORTS_BEGIN("loadcore", 0x0101)
PS2_IMPORT(_import_loadcore_query_boot_mode, 12)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("sysmem\0\0", 0x0101)
PS2_IMPORT(_import_sysmem_kprintf, 14)
PS2_IMPORTS_END()

extern "C" {

// IRX-12a: residency is `return & 3`, and **set frees the module**. This is
// the one entry in the archive that asks to go.
int _module_start(int, char **) {
    const auto record =
        static_cast<uint32_t>(_import_loadcore_query_boot_mode(kKeyBootMode));
    if (record == 0) {
        // `loadcore` publishes key 4 unconditionally, so its absence means
        // the record table was not built or not found -- worth a line.
        (void)_import_sysmem_kprintf("# IGREETING: no boot mode recorded\n");
        return 1;
    }
    const uint32_t mode = recordValue(record);
    if (const char *text = modeText(mode); text != nullptr) {
        (void)_import_sysmem_kprintf("# IGREETING: %s\n", text);
    } else {
        (void)_import_sysmem_kprintf("# IGREETING: boot mode %d\n", mode);
    }
    return 1;                                    // done; do not stay resident
}

}  // extern "C"
