// EELOAD: the stub that replaces the running program with another.
//
// docs/spec/04-ee-kernel.md EE-9 and docs/analysis/41. The kernel's program
// loader (slot 0x06) does not load a program itself: it stages this file at
// 0x00082000 and enters it with the name to load, and this asks the IOP's
// LOADFILE over the SIF to put that ELF in memory (fno 1, spec/06 IOP-5f),
// then hands the entry and gp to slot 0x07, which enters the program with
// the launcher's own registers (spec/05 SYS-8d). Called with no arguments --
// the boot's first call, as an emulator's hook expects it (docs/analysis/41
// §3) -- it loads `rom0:OSDSYS` and gives it the browser's argument.
//
// The SIF client it asks over is `sifclient.hpp`'s, shared with whatever
// stands in the OSDSYS slot -- the program this one loads first.

#include <stdint.h>

#include "console.hpp"
#include "sifclient.hpp"

extern "C" {
alignas(16) uint8_t eeload_stack[0x4000];
alignas(16) uint32_t eeload_args[1 + 16 + 64];    // SYS-8b's block
[[noreturn]] void eeloadHalt();
[[noreturn]] void eeloadMain(int argc, char **argv);
}

namespace {

using namespace ps2::sifclient;
using namespace ps2::console;

// --- the loader (spec/06 IOP-5f's fno 1) -----------------------------------------

constexpr uint32_t kLoadfileServer = 0x80000006;
constexpr uint32_t kFunctionElfLoad = 1;
constexpr uint32_t kPathMax = 252;

// The request as the SDK's client lays it out [header]: `{ epc | result,
// gp, path[252], secname[252] }`; the answer overwrites the first words.
struct ElfLoadRequest {
    uint32_t epc;
    uint32_t gp;
    char path[kPathMax];
    char secname[kPathMax];
};
static_assert(sizeof(ElfLoadRequest) == 0x200);

alignas(16) ElfLoadRequest request;
alignas(16) uint32_t answer[4];

// The program's name when no one gives one. Kept as data, eight-byte
// aligned, so that an emulator's fast boot can write another name over it
// (docs/analysis/41 §3): the hook searches for this exact string.
alignas(8) char default_path[64] = "rom0:OSDSYS";
const char kBrowserArgument[] = "BootBrowser";    // EE-9c

void copyString(char *to, const char *from, uint32_t limit) {
    uint32_t k = 0;
    for (; k + 1 < limit && from[k] != '\0'; k++) {
        to[k] = from[k];
    }
    to[k] = '\0';
}

}  // namespace

extern "C" {

[[noreturn]] void eeloadHalt() {
    for (;;) {
    }
}

// argv = { "EELOAD", path, the program's own arguments... } from slot 0x06
// (docs/analysis/41 §2), or nothing at all from the boot.
[[noreturn]] void eeloadMain(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : default_path;
    int program_argc;
    char **program_argv;
    static char *browser[2] = {default_path, nullptr};
    if (argc > 2) {
        program_argc = argc - 2;
        program_argv = argv + 2;
    } else {
        browser[0] = const_cast<char *>(kBrowserArgument);
        program_argc = 1;
        program_argv = browser;
    }

    initRpc();
    bool bound = false;
    for (uint32_t attempt = 0; attempt < 0x1001 && !bound; attempt++) {
        bound = bindRpc(kLoadfileServer);
    }
    if (!bound) {
        print("# EELOAD: the IOP has no LOADFILE to ask.\n");
        eeloadHalt();
    }
    print("# EELOAD: loading ");
    print(path);
    print("\n");
    request.epc = 0;
    request.gp = 0;
    copyString(request.path, path, kPathMax);
    copyString(request.secname, "all", kPathMax);
    callRpc(kFunctionElfLoad, &request, sizeof(request), answer, sizeof(answer));
    if (static_cast<int32_t>(answer[0]) < 0) {
        print("# EELOAD: LOADFILE refused it, error ");
        printSigned(static_cast<int32_t>(answer[0]));
        print("\n");
        eeloadHalt();
    }
    print("# EELOAD: entry ");
    printHex(answer[0]);
    print(", gp ");
    printHex(answer[1]);
    print("\n");
    (void)syscall(kSysExecPs2, answer[0], answer[1],
                  static_cast<uint32_t>(program_argc), reinterpret_cast<uintptr_t>(program_argv));
    print("# EELOAD: ExecPS2 came back; nothing was entered.\n");
    eeloadHalt();
}

}  // extern "C"
