// Replacing the running program: EE-9.
//
// docs/spec/04-ee-kernel.md EE-9 and docs/analysis/41. The kernel does not
// load a program itself. Slot 0x06 stages the archive's `EELOAD` -- raw code
// for the fixed address 0x00082000 -- and enters it with the name to load
// and the caller's arguments; EELOAD has the IOP put the ELF in memory and
// comes back through slot 0x07 with its entry and gp, which is what enters
// the program with the launcher's registers (spec/05 SYS-8d). Slot 0x7B is
// 0x06 with the path pinned to `rom0:OSDSYS`, and the boot is an EELOAD
// entered with no arguments at all, whose default is that same program.
//
// `rom0:` is the IOP's (EE-9d); `EELOAD`'s own bytes come across the SIF by
// the kernel's file command, a window at a time.

#include <stdint.h>

namespace {

// A content request answers at most this much (the IOP's payload buffer), so a
// file is fetched a window at a time.
constexpr uint32_t kWindow = 0x4000;
constexpr uint32_t kVerbSize = 0;
constexpr uint32_t kVerbContent = 1;

// EE-9a, docs/analysis/41 §1: where EELOAD lives, and the memory a fresh
// load clears before it (§2: from there to the end of memory; here to the
// end of the 2 MiB a program's own loader zero-fills beyond anyway, since
// the simulators that gate the boot pay per word -- docs/implementation.md).
constexpr uintptr_t kEeloadAddress = 0x00082000;
constexpr uintptr_t kClearEnd = 0x00200000;

// `<algorithm>` needs a standard library this target has no build of, and
// `-fno-builtin` means the loops below stay loops rather than becoming calls to
// a `memcpy` the image does not link.
constexpr void copyBytes(uint8_t *to, const uint8_t *from, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        to[i] = from[i];
    }
}

}  // namespace

extern "C" {

// sif.cpp. BOOT-11: one exchange across the bus. Request kVerbSize asks how
// large the file is and fills `sif_reply`; kVerbContent sends the window
// [offset, offset + length) of its bytes to `destination`.
void *sifExchange(const char *name, uint32_t verb, void *destination,
                  uint32_t offset, uint32_t length);
void print(const char *text) asm("_print");

int sysLoadProgram(const char *path, int argc, char **argv)
    asm("_sys_load_program");
int sysLoadOsd(int argc, char **argv) asm("_sys_load_osd");
int sysExecPs2(uint32_t entry, uint32_t gp, int argc, char **argv)
    asm("_sys_exec_ps2");
int bootDefault();

// thread.cpp: the record 0x3C hands the arguments over from (SYS-8d), the
// teardown a replacement brings, and the program's own thread (SYS-10k).
void setProgramArguments(uint32_t argc, const char *packed);
void terminateOtherThreads();
uint32_t startProgramThread(uint32_t gp, uint32_t entry);

alignas(16) uint32_t sif_reply[4];
alignas(16) uint8_t bounce[kWindow];         // a window bound for an unaligned address

}  // extern "C"

namespace {

constexpr char kEeloadName[] = "EELOAD";
constexpr char kRom0Osdsys[] = "rom0:OSDSYS";
constexpr char kNoEeload[] = "# no EELOAD: the archive has nothing by that name.\n";

// Fetch a whole file to `to`. The channel writes whole quadwords at a
// quadword-aligned address, so an aligned destination takes its windows
// straight from the bus -- the last of them may run up to fifteen bytes past
// the file -- and any other goes through the bounce buffer.
void fetchFile(const char *name, uint8_t *to, uint32_t size) {
    const bool direct = (reinterpret_cast<uintptr_t>(to) & 15) == 0;
    for (uint32_t done = 0; done < size; done += kWindow) {
        const uint32_t length = size - done < kWindow ? size - done : kWindow;
        if (direct) {
            sifExchange(name, kVerbContent, to + done, done, length);
        } else {
            sifExchange(name, kVerbContent, bounce, done, length);
            copyBytes(to + done, bounce, length);
        }
    }
}

// SYS-8d: the strings a program's 0x3C call hands over, packed with a NUL
// after each. The reference keeps them in a kernel buffer too.
constexpr uint32_t kArgumentBytes = 256;
constexpr uint32_t kArgumentsMax = 16;
char argument_strings[kArgumentBytes];

// Pack argv into the kernel's own buffer and leave it in the thread record.
// The caller's strings may be anywhere -- the reference copies rather than
// points -- and a list that does not fit is cut at the last string that does.
void storeArguments(int argc, const char *const *argv) {
    uint32_t used = 0;
    uint32_t stored = 0;
    for (int k = 0; k < argc; k++) {
        const char *at = argv[k];
        uint32_t length = 1;
        while (at[length - 1] != '\0') {
            length++;
        }
        if (used + length > kArgumentBytes) {
            break;
        }
        for (uint32_t i = 0; i < length; i++) {
            argument_strings[used + i] = at[i];
        }
        used += length;
        stored++;
    }
    setProgramArguments(stored, argument_strings);
}

// EPC is where the dispatcher's `eret` goes; `sync.p` is a `.word` because
// LLVM has no R5900 target to assemble it with (syscall.S says the same).
void setEpc(uint32_t address) {
    asm volatile("mtc0 %0, $14\n\t.word 0x0000040f" ::"r"(address));
}

// docs/analysis/41 §2, in its order: every other thread torn down, user
// memory cleared from EELOAD's address up, the file copied in, the
// arguments left for it, EPC pointed at it. Returns the address to enter,
// or 0 when the archive has no EELOAD.
[[nodiscard]] uint32_t stageEeload(int argc, const char *const *argv) {
    sifExchange(kEeloadName, kVerbSize, sif_reply, 0, 0);
    const uint32_t size = sif_reply[0];
    if (size == 0) {
        print(kNoEeload);
        return 0;
    }
    terminateOtherThreads();
    auto *clear = reinterpret_cast<uint32_t *>(kEeloadAddress);
    for (uintptr_t at = kEeloadAddress; at < kClearEnd; at += 4) {
        *clear++ = 0;
    }
    fetchFile(kEeloadName, reinterpret_cast<uint8_t *>(kEeloadAddress), size);
    storeArguments(argc, argv);
    setEpc(kEeloadAddress);
    return kEeloadAddress;
}

using Entry = void (*)(uint32_t entry, uint32_t gp, int argc, char **argv);

}  // namespace

extern "C" {

// Slot 0x06: load(path, argc, argv). EELOAD's own list is `{ "EELOAD", path,
// argv... }` (docs/analysis/41 §2). Comes back with -1 when there is no
// EELOAD; on success it comes back too, but not to its caller: the
// dispatcher restores the caller's frame around $v0 = the address and its
// `eret` lands there (EE-7g), which is how the reference enters it as well.
int sysLoadProgram(const char *path, int argc, char **argv) {
    const char *list[kArgumentsMax];
    uint32_t count = 0;
    list[count++] = kEeloadName;
    list[count++] = path;
    for (int k = 0; k < argc && count < kArgumentsMax; k++) {
        list[count++] = argv[k];
    }
    const uint32_t entry = stageEeload(static_cast<int>(count), list);
    return entry == 0 ? -1 : static_cast<int>(entry);
}

// Slot 0x7B: EE-9b, the same call with the path pinned. The reference is a
// four-instruction wrapper that shifts the arguments along; so is this.
int sysLoadOsd(int argc, char **argv) {
    return sysLoadProgram(kRom0Osdsys, argc, argv);
}

// Slot 0x07: exec(entry, gp, argc, argv) -- the program EELOAD has put in
// memory is entered, every other thread gone, with the caller's registers
// (SYS-8d). The reference re-initialises the hardware on the way
// ("Restart Without Memory Clear", docs/analysis/41 §2); this image has
// nothing of that state to re-initialise yet (docs/implementation.md).
int sysExecPs2(uint32_t entry, uint32_t gp, int argc, char **argv) {
    (void)gp;
    terminateOtherThreads();
    storeArguments(argc, argv);
    setEpc(entry);
    return static_cast<int>(entry);
}

// EE-9c: the boot ends by entering EELOAD with no arguments at all -- the
// call an emulator's hook expects first (docs/analysis/41 §3) -- on a thread
// of the program's own (SYS-10k). EELOAD's default is `rom0:OSDSYS` with the
// browser's argument. This is called from the kernel's own entry, outside any
// syscall, so there is no frame to come back through: it is entered directly.
int bootDefault() {
    const uint32_t entry = stageEeload(0, nullptr);
    if (entry == 0) {
        return -1;
    }
    startProgramThread(0, entry);
    storeArguments(0, nullptr);
    reinterpret_cast<Entry>(entry)(entry, 0, 0, nullptr);
    __builtin_unreachable();
}

}  // extern "C"
