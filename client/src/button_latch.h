#pragma once

// Keeps a button that closed a client menu from reaching the game.
//
// Menu presses are handled on SDL's BUTTON_DOWN event, but gameplay
// input is polled every frame. So when B (or A on RESUME, or the
// L3+R3 chord) closes a menu, the very next frame polls that button as
// still held and sends it to the host: pressing B to leave Settings
// also pressed B in the game. block() is called with whatever is held
// the moment a menu closes; filter() then hides those buttons until
// each one is released, after which it passes through normally again.
//
// Plain bit masks, no SDL, so client/tests can cover it directly.

#include <cstdint>

namespace dualdeck::client {

template <typename Mask>
struct ButtonReleaseLatch {
    Mask blocked = 0;

    void block(Mask held) { blocked = static_cast<Mask>(blocked | held); }

    // Drops buttons from `blocked` once released, and returns `held`
    // without the ones still blocked.
    Mask filter(Mask held) {
        blocked = static_cast<Mask>(blocked & held);
        return static_cast<Mask>(held & ~blocked);
    }
};

} // namespace dualdeck::client
