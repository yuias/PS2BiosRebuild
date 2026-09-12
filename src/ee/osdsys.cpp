// OSDSYS: the program the boot ends by running.
//
// docs/spec/04-ee-kernel.md EE-9c: the default boot runs `rom0:OSDSYS` with a
// single argument selecting the browser. This is that program's place in the
// image and its entry contract.
//
// There is no menu here. The retail one's artwork and fonts are material
// `docs/clean-room-policy.md` puts out of bounds, and what stands in their
// place so far is one line of text -- which build this is -- drawn by
// `display.cpp` on a freely-licensed font. A menu needs pad input and the
// configuration record before it is worth more than that.
//
// The job the rest of the boot actually depends on is the other one: `docs/analysis/41` §4 established that reading
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

#include "build_stamp.h"
#include "console.hpp"
#include "config.hpp"
#include "pad.hpp"
#include "display.hpp"
#include "sifclient.hpp"

namespace {

using namespace ps2::sifclient;
using namespace ps2::console;

// Which build this is, in one string, so the console line and the screen
// cannot drift apart. The version is the project's, not ROMVER's (that field
// is the archive's own, spec/01 ARC-7a); the commit comes from the build-time
// stamp, with a trailing `+` when the tree was not the commit it names.
constexpr char kBanner[] =
    "PS2BiosRebuild " PS2_BUILD_VERSION " (" PS2_BUILD_STAMP ")";

// --- FILEIO's RPC (docs/analysis/43 §3, §4; spec/06 IOP-9) ------------------

constexpr uint32_t kFileioServer = 0x80000001;

// CDVDFSV's init service (docs/analysis/42 §5a): `sceCdInit(mode)`, mode 0
// being "initialise and wait for the drive". The retail OSDSYS calls it
// before it touches the disc (`27` finds the call's own banner string in
// the payload), and the wait is what carries it across a drive still
// spinning up: the driver's own open path never polls for readiness (`42`
// §5c), and PCSX2 reports the drive busy for several seconds after a boot
// while it detects the disc. Without this the open runs out of retries
// before the drive is ready and a present disc reads as none.
constexpr uint32_t kCdvdInitServer = 0x80000592;
constexpr uint32_t kCdInitModeWait = 0;
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

// --- the configuration record, on the console and the screen ---------------

// One line's worth of text, built in place. The display holds the pointer
// rather than a copy (`display.hpp`), so these outlive `present`.
constexpr uint32_t kLineBytes = ps2::display::kColumns + 1;
char config_lines[3][kLineBytes];
char pad_line[kLineBytes];

char *appendText(char *at, const char *end, const char *text) {
    for (; *text != '\0' && at < end; text++) {
        *at++ = *text;
    }
    return at;
}

char *appendSigned(char *at, const char *end, int32_t value) {
    if (value < 0 && at < end) {
        *at++ = '-';
    }
    uint32_t magnitude = value < 0 ? -static_cast<uint32_t>(value) : value;
    char digits[12];
    uint32_t n = 0;
    do {
        digits[n++] = static_cast<char>('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0);
    for (uint32_t k = 0; k < n && at < end; k++) {
        *at++ = digits[n - 1 - k];
    }
    return at;
}

// A timezone is stored in minutes; showing it in minutes makes the reader do
// the arithmetic the field already implies.
char *appendOffset(char *at, const char *end, int32_t minutes) {
    at = appendText(at, end, minutes < 0 ? "UTC-" : "UTC+");
    const uint32_t magnitude =
        minutes < 0 ? -static_cast<uint32_t>(minutes) : minutes;
    at = appendSigned(at, end, static_cast<int32_t>(magnitude / 60));
    at = appendText(at, end, ":");
    const uint32_t rest = magnitude % 60;
    if (rest < 10 && at < end) {
        *at++ = '0';
    }
    return appendSigned(at, end, static_cast<int32_t>(rest));
}

void showConfig(const ps2::config::Record &record) {
    char *at = config_lines[0];
    const char *end = config_lines[0] + kLineBytes - 1;
    if (!record.read) {
        // Not the same as an unconfigured machine, and saying so is the whole
        // value of the line: one means no answer came back, the other means
        // one did and the machine has never been set up.
        at = appendText(at, end, "settings: no answer from the drive");
        *at = '\0';
        config_lines[1][0] = '\0';
        config_lines[2][0] = '\0';
    } else {
        at = appendText(at, end, "language: ");
        at = appendText(at, end, record.language_name != nullptr
                        ? record.language_name : "index out of range");
        // IOP-13g1: which of the two generations the record is, because a
        // language that looks wrong is almost always this and not the index.
        at = appendText(at, end,
                        record.wide_language ? " (5-bit)" : " (1-bit)");
        *at = '\0';

        at = config_lines[1];
        end = config_lines[1] + kLineBytes - 1;
        at = appendText(at, end, "clock: ");
        at = appendOffset(at, end, record.timezone_minutes);
        at = appendText(at, end, record.clock_12_hour ? ", 12-hour" : ", 24-hour");
        *at = '\0';

        at = config_lines[2];
        end = config_lines[2] + kLineBytes - 1;
        at = appendText(at, end, record.configured
                        ? "this machine has been set up"
                        : "this machine has never been set up");
        *at = '\0';
    }

    for (uint32_t row = 0; row < 3; row++) {
        print("# OSDSYS: ");
        print(config_lines[row]);
        print("\n");
        ps2::display::setLine(2 + row,
                              config_lines[row][0] != '\0' ? config_lines[row]
                                                          : nullptr);
    }
    ps2::display::present();
}

// Wait for the raster, read the controller, and say what changed. The line is
// rebuilt only on a change, because a controller held down would otherwise
// print sixty identical lines a second and bury everything above it.
[[noreturn]] void padLoop() {
    uint16_t shown = 0;
    bool ever = false;
    for (;;) {
        ps2::display::waitVsync();
        const ps2::pad::State state = ps2::pad::read();
        const uint16_t held = state.present ? state.buttons : 0;
        // While nothing is answering, keep reporting: the counter in the line
        // is the only thing that says whether records are arriving at all,
        // and it is worthless if the line is printed once.
        if (ever && held == shown && state.present) {
            continue;
        }
        if (!state.present && ever && (state.frame & 0x3f) != 0) {
            continue;                           // roughly once a second
        }
        shown = held;
        ever = true;

        char *at = pad_line;
        const char *end = pad_line + kLineBytes - 1;
        if (!state.present) {
            // Three different silences, and they need telling apart: no
            // record has landed at all (the frame counter never moves), one
            // lands every blank but the controller never answered (slot 0),
            // or the handshake is stuck partway through it (slot 5).
            at = appendText(at, end, "controller: no answer, frame ");
            at = appendSigned(at, end, static_cast<int32_t>(state.frame));
            at = appendText(at, end, ", slot state ");
            at = appendSigned(at, end, state.slot_state);
        } else if (held == 0) {
            at = appendText(at, end, "controller: nothing held");
        } else {
            at = appendText(at, end, "controller:");
            for (uint32_t bit = 0; bit < ps2::pad::kButtons; bit++) {
                if ((held & (1u << bit)) != 0) {
                    at = appendText(at, end, " ");
                    at = appendText(at, end, ps2::pad::kButtonNames[bit]);
                }
            }
        }
        *at = '\0';
        print("# OSDSYS: ");
        print(pad_line);
        print("\n");
        ps2::display::setLine(6, pad_line);
        ps2::display::present();
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

// The socket the reference's OSD leaves open on the kernel's DECI2 manager:
// the EE TTY protocol, never closed before a title is launched, so the
// title's own open of it answers -3 and the title gives up on its TTY
// (docs/analysis/52). The handler is never called on an image with no host.
int deci2Handler(int, int, void *) {
    return 0;
}
uint32_t deci2_ettyp[4] = {0x0210, 0, 0, 0};

[[noreturn]] void osdMain(int argc, const char *const *argv) {
    // Which build this is, first: the one line a disc-less boot ends on that
    // identifies the image.
    print("# OSDSYS: ");
    print(kBanner);
    print("\n");
    print("# OSDSYS: loaded from the archive and running. Argument: ");
    // Say which argument arrived, since that is what selects what an OSD would
    // show; EE-9c passes exactly one.
    if (argc > 0) {
        print(argv[0]);
        print("\n");
    }

    // The screen, before anything that can fail: with no disc in the drive
    // every step after this reports a refusal and stops, and the picture
    // should still be up when it does. The console keeps every line it had.
    ps2::display::begin();
    ps2::display::setLine(0, kBanner);
    ps2::display::present();

    deci2_ettyp[2] = reinterpret_cast<uintptr_t>(deci2Handler);
    (void)syscall<int32_t>(kSysDeci2Call, 1,
                           reinterpret_cast<uintptr_t>(deci2_ettyp));

    initRpc();
    if (bindRpc(kCdvdInitServer)) {
        auto *mode = reinterpret_cast<uint32_t *>(request);
        mode[0] = kCdInitModeWait;
        callRpc(0, request, sizeof(uint32_t), reply, sizeof reply);
    }

    // The machine's own settings, before the disc: they decide what an OSD
    // would draw, and reading them is the point at which this program stops
    // being a loader with a banner. Shown whatever the drive holds.
    showConfig(ps2::config::read());
    if (!bindRpc(kFileioServer)) {
        print("# OSDSYS: the IOP has no FILEIO to ask; nothing to boot from.\n");
        osdHalt();
    }

    const int32_t read = readWholeFile(kSystemCnf);
    if (read < 0) {
        print("# OSDSYS: cdrom0:\\SYSTEM.CNF refused, error ");
        printSigned(read);
        print(" -- no disc, or none this driver can read.\n");
        // Nothing to boot is where an OSD would show its menu instead, so
        // this is where the controller belongs: the loop below is the first
        // thing in this program that waits for a person.
        if (ps2::pad::begin()) {
            print("# OSDSYS: controller driver up on port 0.\n");
            padLoop();
        }
        print("# OSDSYS: no controller driver; nothing to wait for.\n");
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
