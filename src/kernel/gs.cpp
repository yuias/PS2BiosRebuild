// The GS display slots: 0x02 (SetGsCrt) and 0x73 (SetVSyncFlag).
//
// docs/analysis/46-ee-syscalls-for-a-title.md §1 and §6, and
// docs/spec/05-ee-syscall-abi.md SYS-14. The other two GS slots, 0x70 and
// 0x71, are in `gs.S`: their value is a whole 64-bit register, which this
// target's ABI cannot return from a compiled function.
//
// 0x02 is served through its general dispatcher only. That is not a shortcut:
// the reference's own gates -- a persisted nibble, a GS revision test, a mode
// remembered from a previous call -- all read boot-configuration state that a
// freshly reset machine has left at zero, and zero sends the reference down
// the general dispatcher too (analysis 46 §1a). What is *not* served is
// everything that dispatcher then reaches beyond NTSC and PAL; SYS-14a says so.

#include <stdint.h>

namespace {

// GS privileged registers. Every one of them is 64 bits wide and is written
// with a single `sd`, which is why the values below are `uint64_t` and not
// pairs of words -- a 32-bit store to half of one is not a slower write of the
// same thing, it is a different bus transaction.
constexpr uintptr_t kSmode1 = 0x12000010;
constexpr uintptr_t kSmode2 = 0x12000020;
constexpr uintptr_t kSrfsh = 0x12000030;
constexpr uintptr_t kSynch1 = 0x12000040;
constexpr uintptr_t kSynch2 = 0x12000050;
constexpr uintptr_t kSynchv = 0x12000060;

// The two modes this serves, by the numbers the argument uses: NTSC is 2 and
// PAL is 3 (analysis 46 §1). Neither is `interlace`, which is a separate
// argument, so the constants below are the same for both fields of it.
constexpr int32_t kModeNtsc = 2;
constexpr int32_t kModePal = 3;

// One timing set per mode, as the general dispatcher writes them with the
// persisted configuration word at rest. `synchv_alternate` is the value the
// same block picks instead when that word says so (analysis 46 §1e); the test
// is in `synchvFor` below.
struct CrtTiming {
    uint64_t smode1;
    uint64_t synch1;
    uint64_t synch2;
    uint64_t synchv;
    uint64_t synchv_alternate;
};

constexpr CrtTiming kNtsc = {
    0x0000000740834504ull,
    0x0007f5b61f06f040ull,
    0x000000000033a4d8ull,
    0x00c7800601a01801ull,
    0x00c7800601a01802ull,
};

constexpr CrtTiming kPal = {
    0x0000000740836504ull,
    0x0007f5c21fc83030ull,
    0x00000000003484bcull,
    0x00a9000502101401ull,
    0x00a9000502101404ull,
};

// SRFSH is the one register both modes and both field settings agree on.
constexpr uint64_t kSrfshValue = 8;

// Where the persisted word's own bits land. Bit 0 of it is ORed into SMODE1 at
// bit 25; bits 8..6 and bit 40 together choose SYNCHV in the non-interlaced
// blocks. The 4-bit field at 47..44 is not SetGsCrt's -- slot 0x6f returns it
// (analysis 46 §5) -- and is named here because the two share this word.
constexpr unsigned kSmode1BitShift = 25;
constexpr unsigned kModeSelectShift = 6;
constexpr uint64_t kModeSelectMask = 7;
constexpr unsigned kSynchvAlternateBit = 40;

// SMODE1's SINT (bit 17): set in the first write of the sequence and cleared
// by the last, which is the same value with this bit off (analysis 46 §1e:
// `0x...40834504` first, `0x...40814504` last, on both interlace paths).
// The clear is not optional: PCSX2 raises no vblank interrupt on either CPU
// while SINT is set, so leaving it up stops every vsync-driven wait a title
// has -- on the IOP, the pad driver's polling among them.
constexpr uint64_t kSmode1Sint = 1ull << 17;

// The progressive path also carries bit 1 of the configuration word at bit
// 36 (VHP) in both SMODE1 writes (`0x8000c490..0x8000c4b0`); the interlaced
// path carries bit 0 at bit 25 only.
constexpr unsigned kSmode1VhpShift = 36;

void writeRegister(uintptr_t address, uint64_t value) {
    *reinterpret_cast<volatile uint64_t *>(address) = value;
}

// The reference sign-extends each of the three arguments out of the low half
// of its 32-bit slot before using it, so a caller that left junk above bit 15
// is treated the same way it is on hardware.
[[nodiscard]] constexpr int32_t asHalfword(int32_t value) {
    return static_cast<int16_t>(value);
}

[[nodiscard]] uint64_t synchvFor(const CrtTiming &timing, uint64_t config) {
    const bool selected = ((config >> kModeSelectShift) & kModeSelectMask) != 0;
    const bool alternate = (config >> kSynchvAlternateBit) & 1;
    return selected && alternate ? timing.synchv_alternate : timing.synchv;
}

// The seven writes the dispatcher makes, in the order it makes them. Their
// order is part of the contract: SMODE1 goes out with SINT set before the sync
// timings, SRFSH after them, and SMODE1 again with SINT cleared to close the
// sequence -- the GS is being told to change its own raster while this runs.
void program(const CrtTiming &timing, int32_t interlace, int32_t field,
             uint64_t config) {
    uint64_t smode1 = timing.smode1 | ((config & 1) << kSmode1BitShift);
    if (interlace == 0) {
        smode1 |= ((config >> 1) & 1) << kSmode1VhpShift;
    }
    // Interlaced: INT set, FFMD carrying the caller's `field`. Progressive
    // writes a literal zero, which the reference does with `sd $zero`.
    const uint64_t smode2 =
        interlace != 0 ? (static_cast<uint64_t>(field & 1) << 1) | 1 : 0;

    writeRegister(kSmode1, smode1);
    writeRegister(kSynch1, timing.synch1);
    writeRegister(kSynch2, timing.synch2);
    writeRegister(kSynchv, synchvFor(timing, config));
    writeRegister(kSmode2, smode2);
    writeRegister(kSrfsh, kSrfshValue);
    writeRegister(kSmode1, smode1 & ~kSmode1Sint);
}

}  // namespace

extern "C" {

int sysSetGsCrt(int32_t interlace, int32_t mode, int32_t field)
    asm("_sys_set_gs_crt");
void sysSetVSyncFlag(uint32_t *flag, uint64_t *alarm) asm("_sys_set_vsync_flag");

// The boot-configuration doubleword the display slots share. The reference
// keeps it at a fixed kernel address that no caller names -- it is reached
// only through the syscalls that read it -- so this is a plain kernel variable
// and its address is not part of the interface. Nothing writes it yet: the
// path that persists a chosen mode is OSDSYS's, not the kernel's.
uint64_t gs_mode_config;

// Slot 0x73's two words. The reference keeps a counter beside them that its
// vsync-time bookkeeping increments; that machinery belongs to whichever
// interrupt path installs the VBLANK handler, not to this slot, and is not
// served yet -- so there is nothing here for a counter to count.
uint32_t *vsync_flag;
uint64_t *vsync_alarm;

// Slot 0x02. The zero it returns is arbitrary: the reference never assigns the
// result register on any path that programs the GS (analysis 46 §1a), so the
// ABI wants *a* value there and contracts none. Reading this one as a status
// would be answering a question the interface does not ask.
int sysSetGsCrt(int32_t interlace, int32_t mode, int32_t field) {
    const int32_t which = asHalfword(mode);
    const int32_t lace = asHalfword(interlace);
    const int32_t parity = asHalfword(field);

    if (which == kModeNtsc) {
        program(kNtsc, lace, parity, gs_mode_config);
    } else if (which == kModePal) {
        program(kPal, lace, parity, gs_mode_config);
    }
    // SYS-14a: every other mode is left alone rather than guessed at.
    return 0;
}

// Slot 0x73: store the two pointers, and nothing else. No validation, no
// dereference, no hardware. Whatever installs the VBLANK handler is what reads
// them back; until something does, this is the whole syscall.
void sysSetVSyncFlag(uint32_t *flag, uint64_t *alarm) {
    vsync_flag = flag;
    vsync_alarm = alarm;
}

}  // extern "C"
