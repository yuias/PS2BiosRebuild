// Getting pixels onto the screen, from the EE, with nothing rasterised.
//
// Two halves. The privileged GS registers below 0x12001000 say what the CRT
// does and which part of GS memory it reads; they are ordinary uncached
// 64-bit stores and the kernel already maps them (spec/04 EE-12, TLB entry
// 11). Pixels are the other half, and GS memory is not addressable from the
// EE at all -- the only way in is a transfer through the GIF.
//
// The transfer is a host-to-local image transfer rather than a textured
// sprite. A static screen redrawn on a button press gains nothing from the
// drawing environment a sprite needs (FRAME, ZBUF, XYOFFSET, SCISSOR, TEST,
// TEX0), and it fails legibly: a wrong BITBLTBUF puts recognisable garbage
// somewhere in GS memory, where a wrong TEX0 draws a blank quad and says
// nothing about why.
//
// It goes out on DMA channel 2 rather than as stores into the GIF FIFO at
// 0x10006000, which would be simpler, because the FIFO wants a whole
// quadword per write and this target's instruction set has no store that
// wide. Building the packet in memory needs only ordinary word stores.

#include "display.hpp"

#include "osd_font.h"
#include "sifclient.hpp"

namespace ps2::display {
namespace {

using ps2::sifclient::syscall;

// --- registers -------------------------------------------------------------

constexpr uintptr_t kPmode = 0x12000000;
constexpr uintptr_t kDispfb2 = 0x12000090;
constexpr uintptr_t kDisplay2 = 0x120000A0;

// The GS's own status word, which sits in the second page of privileged
// registers rather than the first. Bit 3 latches at each vertical blank and
// is cleared by writing a one back to it; the read side is 64 bits wide like
// every other register here, but only the low half carries the flags.
constexpr uintptr_t kGsCsr = 0x12001000;
constexpr uint32_t kCsrVsync = 1u << 3;

// The DMAC and the GIF, through the uncached alias the kernel's own interrupt
// code reaches the DMAC by.
constexpr uintptr_t kGifCtrl = 0xB0003000;
constexpr uintptr_t kD2Chcr = 0xB000A000;
constexpr uintptr_t kD2Madr = 0xB000A010;
constexpr uintptr_t kD2Qwc = 0xB000A020;
constexpr uintptr_t kDCtrl = 0xB000E000;

constexpr uint32_t kGifCtrlReset = 1;
constexpr uint32_t kDmacEnable = 1;
// Channel start, from memory to the peripheral, normal mode.
constexpr uint32_t kChcrStart = 0x00000101;
constexpr uint32_t kChcrStr = 1u << 8;

// Slot 0x02 (spec/05 SYS-14a), and the arguments the reference passes: NTSC,
// interlaced, and a field argument of 1, which is what puts FFMD up.
constexpr int kSysSetGsCrt = 0x02;
constexpr int32_t kInterlaced = 1;
constexpr int32_t kModeNtsc = 2;
constexpr int32_t kFieldFrame = 1;

// --- the picture -----------------------------------------------------------

// Where the framebuffer starts, in the 8 KiB units DISPFB counts, and how
// wide it is in the 64-pixel units both DISPFB and BITBLTBUF count.
constexpr uint32_t kFrameBase = 0;
constexpr uint32_t kFrameWidth = kWidth / 64;
constexpr uint32_t kPsmct32 = 0;

// PSMCT32 packs a pixel as one word, red in the low byte. Alpha is not read
// by the display here -- PMODE's MMOD takes the blend factor from ALP -- but
// a framebuffer of zero-alpha pixels is a trap for anything drawn later, so
// it is written at the full-scale value the GS uses, which is 0x80.
[[nodiscard]] constexpr uint32_t rgb(uint32_t red, uint32_t green, uint32_t blue) {
    return red | (green << 8) | (blue << 16) | (0x80u << 24);
}

constexpr uint32_t kBackground = rgb(0x10, 0x14, 0x2C);
constexpr uint32_t kForeground = rgb(0xC8, 0xCC, 0xD8);

// How many framebuffer lines one transfer carries. The band is reused for the
// clear and for each line of text, so it has to be at least a glyph tall and
// divide the framebuffer's height: 16 does both, in fourteen transfers.
constexpr uint32_t kBandLines = 16;
static_assert(kHeight % kBandLines == 0);
static_assert(kBandLines >= ps2::font::kGlyphHeight);

// Each source pixel becomes two, so a cell is twice the glyph's width.
constexpr uint32_t kCellWidth = ps2::font::kGlyphWidth * 2;

// The inset, in framebuffer pixels; the vertical one is half what it looks
// like on the raster, which shows every buffer line twice. These two leave
// exactly 48 columns and 16 rows with an equal margin on the far side.
constexpr uint32_t kMarginX = 32;
constexpr uint32_t kMarginY = 8;
static_assert(kColumns == (kWidth - 2 * kMarginX) / kCellWidth);
static_assert(kRows == (kHeight - 2 * kMarginY) / ps2::font::kGlyphHeight);

// A character row's top line in the framebuffer, and whether it has any lines
// inside a given band. 13 does not divide 16, so a row can straddle two.
constexpr uint32_t rowTop(uint32_t row) {
    return kMarginY + row * ps2::font::kGlyphHeight;
}

[[nodiscard]] constexpr bool rowTouchesBand(uint32_t row, uint32_t top) {
    return rowTop(row) < top + kBandLines &&
           rowTop(row) + ps2::font::kGlyphHeight > top;
}

// --- the packet ------------------------------------------------------------

// GIF register addresses, as a PACKED A+D descriptor names them.
constexpr uint64_t kRegBitbltbuf = 0x50;
constexpr uint64_t kRegTrxpos = 0x51;
constexpr uint64_t kRegTrxreg = 0x52;
constexpr uint64_t kRegTrxdir = 0x53;

constexpr uint64_t kTagEop = 1ull << 15;
constexpr uint64_t kTagFlagPacked = 0ull << 58;
constexpr uint64_t kTagFlagImage = 2ull << 58;
constexpr uint64_t kTagOneRegister = 1ull << 60;
// The one register a PACKED tag descends through here: A+D, which reads each
// quadword as a value and the address to put it at.
constexpr uint64_t kRegsAddressData = 0x0E;

// One quadword, as the two 64-bit halves the EE can actually store.
struct Quadword {
    uint64_t low;
    uint64_t high;
};

// Header and pixels in one block, so a single transfer carries both and the
// GIF never sees a gap between an image tag and the image.
struct Packet {
    Quadword tag;                     // four A+D pairs
    Quadword bitbltbuf;
    Quadword trxpos;
    Quadword trxreg;
    Quadword trxdir;
    Quadword image_tag;
    uint32_t pixels[kWidth * kBandLines];
};

constexpr uint32_t kHeaderQuadwords = 6;
constexpr uint32_t kImageQuadwords = kWidth * kBandLines * sizeof(uint32_t) / 16;

// NLOOP is fifteen bits and the channel's quadword count is sixteen.
static_assert(kImageQuadwords < (1u << 15));
static_assert(kHeaderQuadwords + kImageQuadwords < (1u << 16));

alignas(16) Packet packet;

// The packet is a DMA source and the data cache is write-back, so composing it
// through the cached mapping would leave the DMAC reading whatever was in
// memory before. Everything below writes it through the uncached alias
// instead, which is also how `sifclient` hands the IOP a destination.
template <typename T>
[[nodiscard]] T *uncached(T *address) {
    return reinterpret_cast<T *>(
        (reinterpret_cast<uintptr_t>(address) & 0x1FFFFFFF) | 0xA0000000);
}

[[nodiscard]] uint32_t physical(const void *address) {
    return reinterpret_cast<uintptr_t>(address) & 0x1FFFFFFF;
}

void writeRegister(uintptr_t address, uint64_t value) {
    *reinterpret_cast<volatile uint64_t *>(address) = value;
}

void writeWord(uintptr_t address, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(address) = value;
}

[[nodiscard]] uint32_t readWord(uintptr_t address) {
    return *reinterpret_cast<volatile uint32_t *>(address);
}

// Send the band at `top`. The wait afterwards is bounded rather than a bare
// poll: the channel finishes in microseconds on hardware, and a simulator
// that models the register but not the transfer would otherwise never let the
// boot past this line.
constexpr uint32_t kTransferAttempts = 100000;

void sendBand(uint32_t top) {
    Packet *p = uncached(&packet);

    p->tag.low = 4 | kTagEop | kTagFlagPacked | kTagOneRegister;
    p->tag.high = kRegsAddressData;

    p->bitbltbuf.low = (static_cast<uint64_t>(kFrameBase) << 32) |
                       (static_cast<uint64_t>(kFrameWidth) << 48) |
                       (static_cast<uint64_t>(kPsmct32) << 56);
    p->bitbltbuf.high = kRegBitbltbuf;

    p->trxpos.low = static_cast<uint64_t>(top) << 48;
    p->trxpos.high = kRegTrxpos;

    p->trxreg.low = kWidth | (static_cast<uint64_t>(kBandLines) << 32);
    p->trxreg.high = kRegTrxreg;

    p->trxdir.low = 0;                                   // host to GS memory
    p->trxdir.high = kRegTrxdir;

    p->image_tag.low = kImageQuadwords | kTagEop | kTagFlagImage;
    p->image_tag.high = 0;

    writeWord(kD2Madr, physical(&packet));
    writeWord(kD2Qwc, kHeaderQuadwords + kImageQuadwords);
    writeWord(kD2Chcr, kChcrStart);
    for (uint32_t spin = 0; spin < kTransferAttempts; spin++) {
        if ((readWord(kD2Chcr) & kChcrStr) == 0) {
            return;
        }
    }
}

void fillBand(uint32_t colour) {
    uint32_t *pixels = uncached(packet.pixels);
    for (uint32_t i = 0; i < kWidth * kBandLines; i++) {
        pixels[i] = colour;
    }
}

// Paint one character into the band. `top` is the band's first framebuffer
// line, so a row straddling two bands draws its upper part in one and its
// lower part in the other and the clipping below is what makes that work.
void blitGlyph(char character, uint32_t column, uint32_t row, uint32_t top) {
    const auto code = static_cast<uint8_t>(character);
    if (code < ps2::font::kFirstGlyph ||
        code >= ps2::font::kFirstGlyph + ps2::font::kGlyphCount) {
        return;
    }
    const uint32_t glyph = (code - ps2::font::kFirstGlyph) * ps2::font::kGlyphHeight;
    uint32_t *pixels = uncached(packet.pixels);

    for (uint32_t line = 0; line < ps2::font::kGlyphHeight; line++) {
        const uint32_t y = rowTop(row) + line;
        if (y < top || y >= top + kBandLines) {
            continue;
        }
        const uint8_t bits = ps2::font::kGlyphRows[glyph + line];
        if (bits == 0) {
            continue;
        }
        uint32_t *out =
            pixels + (y - top) * kWidth + kMarginX + column * kCellWidth;
        for (uint32_t bit = 0; bit < ps2::font::kGlyphWidth; bit++) {
            if ((bits & (0x80u >> bit)) != 0) {
                out[bit * 2] = kForeground;
                out[bit * 2 + 1] = kForeground;
            }
        }
    }
}

// The text on screen, by row. Held rather than drawn as it arrives, because
// a band is composed from every row that touches it and sent once.
const char *lines[kRows];

}  // namespace

void begin() {
    (void)syscall<int32_t>(kSysSetGsCrt, kInterlaced, kModeNtsc, kFieldFrame);

    // Read circuit 2 alone, which is the one the reference enables; with
    // circuit 1 off there is nothing for the blend fields to mix, and they
    // are carried over unchanged rather than zeroed so the register reads the
    // way a trace of the console does.
    writeRegister(kPmode, 0x66);
    writeRegister(kDispfb2, kFrameBase | (static_cast<uint64_t>(kFrameWidth) << 9) |
                                (static_cast<uint64_t>(kPsmct32) << 15));
    // DX/DY place the picture on the raster and MAGH stretches each pixel over
    // four of the clock's units; DW and DH are the displayed size minus one.
    // These are the console's own numbers, not a derivation.
    writeRegister(kDisplay2, 636ull | (50ull << 12) | (3ull << 23) |
                                 (2559ull << 32) | (447ull << 44));

    writeWord(kGifCtrl, kGifCtrlReset);
    writeWord(kDCtrl, kDmacEnable);

    // Not left to the loader: whether a program's zero-initialised data
    // arrives zeroed is the loader's contract, and this one does not state it.
    for (auto &line : lines) {
        line = nullptr;
    }
    present();
}

void setLine(uint32_t row, const char *text) {
    if (row < kRows) {
        lines[row] = text;
    }
}

// A ceiling on the wait rather than a bare spin, for the same reason
// `sendBand` has one: a frame is 1/60 s and a machine that never raises the
// flag must not take the program with it. The count is far longer than a
// frame at any plausible clock, so a real blank is never missed.
constexpr uint32_t kVsyncAttempts = 40000000;

void present() {
    for (uint32_t top = 0; top < kHeight; top += kBandLines) {
        fillBand(kBackground);
        for (uint32_t row = 0; row < kRows; row++) {
            const char *text = lines[row];
            if (text == nullptr || !rowTouchesBand(row, top)) {
                continue;
            }
            for (uint32_t column = 0; column < kColumns && text[column] != '\0';
                 column++) {
                blitGlyph(text[column], column, row, top);
            }
        }
        sendBand(top);
    }
}

void waitVsync() {
    // Clear first, then wait for the next one to arrive. Waiting on the bit
    // as found would return immediately on a blank that happened while the
    // caller was drawing, which is a frame of pacing lost every time.
    writeRegister(kGsCsr, kCsrVsync);
    for (uint32_t attempt = 0; attempt < kVsyncAttempts; attempt++) {
        if ((readWord(kGsCsr) & kCsrVsync) != 0) {
            return;
        }
    }
}

}  // namespace ps2::display
