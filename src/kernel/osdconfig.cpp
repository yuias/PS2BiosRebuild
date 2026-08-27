// The OSD configuration slots: 0x4a, 0x4b and 0x6f.
//
// docs/analysis/46-ee-syscalls-for-a-title.md §3 and §5, and
// docs/spec/05-ee-syscall-abi.md SYS-15. All three move bytes between a caller
// and a kernel-resident block, and none of them touches hardware. The blocks
// are plain kernel variables here: the reference has them at fixed addresses,
// but no caller names those -- they are reached only through these syscalls --
// so the address is not part of the interface, and only the layout and the
// clamping are.

#include <stdint.h>

namespace {

// 0x4a/0x4b's block. The reference copies its low half a field at a time
// through these six masks and its high half whole, which together account for
// every bit of the word. What any individual field configures is not settled
// (analysis 46 §7), so the masks are given as masks and left unnamed.
constexpr uint32_t kFieldMasks[] = {0x1, 0x6, 0x8, 0x10, 0x1fe0, 0xe000};

// 0x6f's block, and the bound every argument to it is clamped against.
constexpr int32_t kParam2Size = 0x80;

[[nodiscard]] uint32_t mergeFields(uint32_t into, uint32_t from) {
    uint32_t result = into;
    for (const uint32_t mask : kFieldMasks) {
        result = (result & ~mask) | (from & mask);
    }
    return result;
}

}  // namespace

extern "C" {

int sysSetOsdConfigParam(void *address) asm("_sys_set_osd_config_param");
int sysGetOsdConfigParam(void *address) asm("_sys_get_osd_config_param");
int sysGetOsdConfigParam2(void *config, int32_t size, int32_t offset)
    asm("_sys_get_osd_config_param2");

// gs.cpp: the boot-configuration doubleword the display slots keep. 0x6f
// answers out of it, which is the one thing about that slot a caller could not
// guess from its arguments.
extern uint64_t gs_mode_config;

// The two blocks. Both start at rest: nothing in this image has chosen a
// language, a screen shape or a time zone yet, and a caller that reads before
// anything has written gets what a freshly formatted console's would be.
uint32_t osd_config_param;
uint8_t osd_config_param2[kParam2Size];

// Slot 0x4a: caller -> kernel. The low half by field, the high half as one
// halfword. The masks tile the low half exactly, so the net effect is the
// whole word; they are written out because the field boundaries are the part
// of this that is observable, and a rebuild that merged the word in one store
// would agree today and diverge the moment a field is added.
int sysSetOsdConfigParam(void *address) {
    auto *caller = static_cast<uint32_t *>(address);
    osd_config_param = mergeFields(osd_config_param, *caller);

    auto *high = reinterpret_cast<uint16_t *>(&osd_config_param) + 1;
    *high = *(reinterpret_cast<uint16_t *>(caller) + 1);
    return 0;
}

// Slot 0x4b: kernel -> caller, the same code with the operands swapped, which
// is how the reference writes it too.
int sysGetOsdConfigParam(void *address) {
    auto *caller = static_cast<uint32_t *>(address);
    *caller = mergeFields(*caller, osd_config_param);

    auto *high = reinterpret_cast<uint16_t *>(caller) + 1;
    *high = *(reinterpret_cast<uint16_t *>(&osd_config_param) + 1);
    return 0;
}

// Slot 0x6f: a clamped byte copy out of the second block -- and a return value
// that has nothing to do with it.
//
// The clamp keeps `offset + size` inside the block rather than rejecting a
// caller that asked for too much, so an over-long request is shortened and an
// offset already past the end copies nothing. The comparison the reference
// makes is unsigned, so a negative `size` or `offset` fails it and is clamped
// rather than read as a negative count.
//
// The result is bits 47..44 of the display slots' own configuration word, or
// zero when that word's mode-select field is clear -- the same field SetGsCrt
// reads. It is unrelated to the copy and to every argument; SYS-15c says so,
// because no reading of the arguments would ever produce it.
int sysGetOsdConfigParam2(void *config, int32_t size, int32_t offset) {
    int32_t count = size;
    int32_t from = offset;
    if (static_cast<uint32_t>(count + from) > static_cast<uint32_t>(kParam2Size)) {
        if (from < kParam2Size) {
            count = kParam2Size - from;
        } else {
            from = kParam2Size;
            count = 0;
        }
    }

    auto *to = static_cast<uint8_t *>(config);
    for (int32_t i = 0; i < count; i++) {
        to[i] = osd_config_param2[from + i];
    }

    const uint64_t word = gs_mode_config;
    if (((word >> 6) & 7) == 0) {
        return 0;
    }
    return static_cast<int>((word >> 44) & 0xf);
}

}  // extern "C"
