// The controller, as an EE program reads it.
//
// docs/spec/06-iop-kernel.md IOP-14. The driver on the other processor is not
// asked for buttons: a program opens a port once, gives the driver an address,
// and from then on a record arrives in its own memory once per vertical blank.
// This is the reading half -- loading the two modules, the open, and picking
// apart the record that lands.
//
// Header over a real translation unit, for the reason `sifclient.hpp` gives:
// only EE programs use it, and each links its own copy.

#pragma once

#include <stdint.h>

namespace ps2::pad {

// IOP-14e's bit order, low byte first. A bit is **clear** while its button is
// held; `State::buttons` below has already been inverted, so these read the
// natural way round.
inline constexpr uint16_t kSelect   = 1u << 0;
inline constexpr uint16_t kL3       = 1u << 1;
inline constexpr uint16_t kR3       = 1u << 2;
inline constexpr uint16_t kStart    = 1u << 3;
inline constexpr uint16_t kUp       = 1u << 4;
inline constexpr uint16_t kRight    = 1u << 5;
inline constexpr uint16_t kDown     = 1u << 6;
inline constexpr uint16_t kLeft     = 1u << 7;
inline constexpr uint16_t kL2       = 1u << 8;
inline constexpr uint16_t kR2       = 1u << 9;
inline constexpr uint16_t kL1       = 1u << 10;
inline constexpr uint16_t kR1       = 1u << 11;
inline constexpr uint16_t kTriangle = 1u << 12;
inline constexpr uint16_t kCircle   = 1u << 13;
inline constexpr uint16_t kCross    = 1u << 14;
inline constexpr uint16_t kSquare   = 1u << 15;

inline constexpr uint32_t kButtons = 16;
extern const char *const kButtonNames[kButtons];

struct State {
    // False when no record has arrived yet, or the one that did says this
    // frame's reply was not good. Every field below is then meaningless --
    // which is not the same as "no buttons held" and must not be shown as one.
    bool present;

    uint16_t buttons;               // already inverted: a set bit is held
    uint8_t id;                     // IOP-14g +0x09; 0x41 is a digital pad
    uint8_t slot_state;             // IOP-14g +0x04
    uint32_t frame;                 // the counter of the half this came from
};

// Load the two IOP modules and open port 0 slot 0. False if either module
// refuses to load or the driver refuses the open; the caller then has no
// controller and `read` will keep answering `present == false`.
[[nodiscard]] bool begin();

// The newest record the driver has pushed. Cheap: it reads two counters and
// one record out of uncached memory, so a caller may call it every frame.
[[nodiscard]] State read();

}  // namespace ps2::pad
