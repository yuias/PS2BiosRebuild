// OSDSYS: the program the boot ends by running.
//
// docs/spec/04-ee-kernel.md EE-9c: the default boot runs `rom0:OSDSYS` with a
// single argument selecting the browser. This is that program's place in the
// image and its entry contract; what an on-screen menu needs -- a display, a
// font, artwork -- is material `docs/clean-room-policy.md` puts out of bounds
// and would be built from freely-licensed sources when there is a screen to
// put it on.
//
// What it does have is the one job the retail OSDSYS does that the rest of the
// boot depends on: `docs/analysis/41` §4 established that reading
// `cdrom0:\SYSTEM.CNF;1` and honouring its `BOOT2=` line belongs here and not
// to EELOAD, whose own contract is only "load the path you were handed". So
// this asks the IOP's FILEIO for that file over the SIF, takes the path out of
// it, and hands it back to syscall 0x06 -- which stages EELOAD again, this
// time pointed at the disc. With no disc in the drive every step of that fails
// cleanly and the program says which one and stops, which is what the boot
// gates with no disc attached expect to see.
//
// Unlike an IOP module this is a plain executable: it is linked at the address
// it runs from and the kernel's loader places it there (spec/02 notes the
// archive's four ET_EXEC files).

#include <stdint.h>

#include "console.hpp"
#include "sifclient.hpp"

namespace {

using namespace ps2::sifclient;
using namespace ps2::console;

// --- FILEIO's RPC (docs/analysis/43 §3, §4; spec/06 IOP-9) ------------------

constexpr uint32_t kFileioServer = 0x80000001;
constexpr uint32_t kFnoOpen = 0;
constexpr uint32_t kFnoClose = 1;
constexpr uint32_t kFnoRead = 2;
constexpr int kFlagReadOnly = 1;                 // IOP_O_RDONLY [header]

// Every field order below is that operation's own: analysis §4 warns that
// FILEIO does not share one request layout across its fnos, and `open`'s path
// starting at +4 rather than +0 is exactly the case it warns about.
struct OpenRequest {
    uint32_t mode;
    char path[256];
};

struct CloseRequest {
    uint32_t fd;
};

struct ReadRequest {
    uint32_t fd;
    uint32_t dest_ee;
    uint32_t length;
};

// The server answers into a fixed four-word block whatever the operation, and
// `read`'s payload does not ride in it -- that goes straight to `dest_ee` by
// DMA, which is why the file buffer below is aligned and static rather than a
// local.
alignas(16) uint32_t reply[4];
alignas(16) uint8_t request[320];

// SYSTEM.CNF is a few short lines; a sector is more than the retail file needs
// and keeps the read to one call.
constexpr uint32_t kConfigBytes = 2048;
alignas(64) char config[kConfigBytes + 1];

[[nodiscard]] int32_t callFileio(uint32_t fno, uint32_t send_size) {
    callRpc(fno, request, send_size, reply, sizeof reply);
    return static_cast<int32_t>(reply[0]);
}

[[nodiscard]] uint32_t copyString(char *to, const char *from, uint32_t limit) {
    uint32_t n = 0;
    while (n + 1 < limit && from[n] != '\0') {
        to[n] = from[n];
        n++;
    }
    to[n] = '\0';
    return n;
}

// Read a whole file into `config`, NUL-terminated. Negative on any failure,
// which is the ordinary case with no disc in the drive.
[[nodiscard]] int32_t readWholeFile(const char *path) {
    auto *open_request = reinterpret_cast<OpenRequest *>(request);
    open_request->mode = kFlagReadOnly;
    const uint32_t length = copyString(open_request->path, path, sizeof open_request->path);
    const int32_t fd = callFileio(kFnoOpen, sizeof(uint32_t) + length + 1);
    if (fd < 0) {
        return fd;
    }

    auto *read_request = reinterpret_cast<ReadRequest *>(request);
    read_request->fd = static_cast<uint32_t>(fd);
    read_request->dest_ee = reinterpret_cast<uintptr_t>(config) & 0x1FFFFFFF;
    read_request->length = kConfigBytes;
    const int32_t got = callFileio(kFnoRead, sizeof(ReadRequest));

    auto *close_request = reinterpret_cast<CloseRequest *>(request);
    close_request->fd = static_cast<uint32_t>(fd);
    (void)callFileio(kFnoClose, sizeof(CloseRequest));

    if (got < 0) {
        return got;
    }
    config[got] = '\0';
    return got;
}

// --- SYSTEM.CNF (docs/analysis/43 §6) --------------------------------------

constexpr char kSystemCnf[] = "cdrom0:\\SYSTEM.CNF;1";
constexpr char kBoot2Key[] = "BOOT2";

[[nodiscard]] constexpr bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

[[nodiscard]] bool startsWith(const char *text, const char *prefix) {
    for (uint32_t k = 0; prefix[k] != '\0'; k++) {
        if (text[k] != prefix[k]) {
            return false;
        }
    }
    return true;
}

// `BOOT2 = cdrom0:\SLPS_259.18;1` -- the retail file spaces its `=` out and
// ends its lines with CRLF, so neither the key nor the value can be taken by
// position. Returns a pointer into `config`, NUL-terminated in place, or null.
[[nodiscard]] char *findBoot2(char *text) {
    for (char *line = text; *line != '\0';) {
        char *end = line;
        while (*end != '\0' && *end != '\n') {
            end++;
        }
        const bool last = *end == '\0';
        *end = '\0';

        char *at = line;
        while (isSpace(*at)) {
            at++;
        }
        if (startsWith(at, kBoot2Key)) {
            at += sizeof kBoot2Key - 1;
            while (isSpace(*at)) {
                at++;
            }
            if (*at == '=') {
                at++;
                while (isSpace(*at)) {
                    at++;
                }
                char *stop = at;
                while (*stop != '\0' && !isSpace(*stop)) {
                    stop++;
                }
                *stop = '\0';
                if (*at != '\0') {
                    return at;
                }
            }
        }
        if (last) {
            return nullptr;
        }
        line = end + 1;
    }
    return nullptr;
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

// The socket the reference's OSD leaves open on the kernel's DECI2 manager:
// the EE TTY protocol, never closed before a title is launched, so the
// title's own open of it answers -3 and the title gives up on its TTY
// (docs/analysis/52). The handler is never called on an image with no host.
int deci2Handler(int, int, void *) {
    return 0;
}
uint32_t deci2_ettyp[4] = {0x0210, 0, 0, 0};

[[noreturn]] void osdMain(int argc, const char *const *argv) {
    print("# OSDSYS: loaded from the archive and running. Argument: ");
    // Say which argument arrived, since that is what selects what an OSD would
    // show; EE-9c passes exactly one.
    if (argc > 0) {
        print(argv[0]);
        print("\n");
    }

    deci2_ettyp[2] = reinterpret_cast<uintptr_t>(deci2Handler);
    (void)syscall<int32_t>(kSysDeci2Call, 1,
                           reinterpret_cast<uintptr_t>(deci2_ettyp));

    initRpc();
    if (!bindRpc(kFileioServer)) {
        print("# OSDSYS: the IOP has no FILEIO to ask; nothing to boot from.\n");
        osdHalt();
    }

    const int32_t read = readWholeFile(kSystemCnf);
    if (read < 0) {
        print("# OSDSYS: cdrom0:\\SYSTEM.CNF refused, error ");
        printSigned(read);
        print(" -- no disc, or none this driver can read.\n");
        osdHalt();
    }

    char *boot2 = findBoot2(config);
    if (boot2 == nullptr) {
        print("# OSDSYS: SYSTEM.CNF has no BOOT2 line.\n");
        osdHalt();
    }

    print("# OSDSYS: BOOT2 = ");
    print(boot2);
    print("\n");

    // EE-9a: slot 0x06 stages EELOAD again with this path, and does not come
    // back -- the dispatcher's own `eret` lands in EELOAD (spec/05 SYS-6a's
    // shape). A negative answer means the archive has no EELOAD to stage.
    (void)syscall(kSysLoadProgram, reinterpret_cast<uintptr_t>(boot2), 0, 0);
    print("# OSDSYS: syscall 0x06 refused the disc's path.\n");
    osdHalt();
}

}  // extern "C"
