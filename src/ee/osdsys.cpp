// OSDSYS: the program the boot ends by running.
//
// docs/spec/04-ee-kernel.md EE-9c: the default boot runs `rom0:OSDSYS` with a
// single argument selecting the browser. This is that program's place in the
// image and its entry contract; what an on-screen menu needs -- a display, a
// font, artwork -- is material `docs/clean-room-policy.md` puts out of bounds
// and would be built from freely-licensed sources when there is a screen to
// put it on.
//
// Unlike an IOP module this is a plain executable: it is linked at the address
// it runs from and the kernel's loader places it there (spec/02 notes the
// archive's four ET_EXEC files).

#include <stdint.h>

namespace {

constexpr uintptr_t kSioIsr = 0xB000F130;
constexpr uintptr_t kSioTx = 0xB000F180;
constexpr uint32_t kSioTxReady = 0x8000;

[[nodiscard]] uint32_t readWord(uintptr_t address) {
    return *reinterpret_cast<volatile uint32_t *>(address);
}

void writeByte(uintptr_t address, uint8_t value) {
    *reinterpret_cast<volatile uint8_t *>(address) = value;
}

// The EE's serial console, which is the only output this program has: a byte
// goes out once the ready bit clears.
void print(const char *text) {
    for (const char *at = text; *at != '\0'; at++) {
        while ((readWord(kSioIsr) & kSioTxReady) != 0) {
        }
        writeByte(kSioTx, static_cast<uint8_t>(*at));
    }
}

}  // namespace

extern "C" {

// `osdsys.S` hands these to slot 0x3C: the stack this program runs on, and the
// block its arguments come back in (spec/05 SYS-8b: argc, sixteen argv words,
// then the strings).
alignas(16) uint8_t osd_stack[0x1000];
alignas(16) uint32_t osd_args[(4 + 16 * 4 + 256) / 4];

// The root 0x3C is given: where a `main` that returned would go.
[[noreturn]] void osdHalt() {
    for (;;) {
    }
}

[[noreturn]] void osdMain(int argc, const char *const *argv) {
    print("# OSDSYS: loaded from the archive and running. Argument: ");
    // Say which argument arrived, since that is what selects what an OSD would
    // show; EE-9c passes exactly one.
    if (argc > 0) {
        print(argv[0]);
        print("\n");
    }
    osdHalt();
}

}  // extern "C"
