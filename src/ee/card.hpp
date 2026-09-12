// The memory card, as an EE program reads it.
//
// docs/spec/06-iop-kernel.md IOP-15. The card lives on the other processor
// and is reached through one service: ask what is in a slot, then ask for the
// entries of its root directory. Both answers arrive in this program's own
// memory by transfer, not as the reply to the call, so every read of them
// goes through the uncached alias.
//
// Header over a real translation unit, for the reason `sifclient.hpp` gives.

#pragma once

#include <stdint.h>

namespace ps2::card {

// How many root entries this program is willing to hold at once. A card's
// root is one save directory per title, so this is a screenful, not a card.
inline constexpr uint32_t kEntries = 8;
inline constexpr uint32_t kNameBytes = 32;

struct Entry {
    char name[kNameBytes + 1];
    uint32_t length;                    // for a directory, its entry count
    uint16_t mode;
    bool directory;
};

struct State {
    // False when the service never answered: no such service on the other
    // processor, or the driver refused. Distinct from `present` being false,
    // which is an answer.
    bool asked;

    bool present;                       // something is in the slot
    bool formatted;                     // ... and this program can read it
    int32_t code;                       // the driver's own answer

    uint32_t entries;                   // how many of `entry` are filled
    Entry entry[kEntries];
};

// Load the two IOP modules and bind the service. False if either module
// refuses to load or the service never appears.
[[nodiscard]] bool begin();

// Ask about slot 0 of port 0, and list its root if it has one.
[[nodiscard]] State read();

}  // namespace ps2::card
