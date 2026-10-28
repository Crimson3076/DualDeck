#pragma once

#include <SDL3/SDL.h>

#include <optional>
#include <string>

namespace dualdeck::client {

// Address entry that works with only a controller: an on-screen number
// pad driven by the D-pad, since Steam's virtual keyboard doesn't
// reliably come up for this app in Gaming Mode (the same limitation that
// retired the old pairing-code screen -- see docs/history.md). A
// physical or Bluetooth keyboard can still type straight in, including
// host names (Enter and Escape count only with no gamepad connected,
// since Steam Input synthesizes Escape for B and Start -- see
// setup_wizard.cpp). Used by the setup wizard and the host picker. `subtitle`
// is drawn under the title (e.g. the wizard's step counter). Returns
// std::nullopt if the user backed out.
std::optional<std::string> runAddressEntry(SDL_Renderer* renderer, SDL_Window* window, SDL_Gamepad*& gamepad,
                                            const std::string& initialText, const std::string& subtitle = "");

} // namespace dualdeck::client
