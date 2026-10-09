#pragma once

// First-run setup wizard (GitHub issue #19). Split out of main.cpp; see
// setup_wizard.cpp for the individual steps.

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>

#include "net_client.h"

namespace dualdeck::client {

// Orchestrates the whole wizard as an explicit step state machine. Returns
// true if the user reached the end (Done), false if they exited entirely
// (window close, or Exit/B from the very first screen) -- callers decide
// separately whether "false" means quit the whole app (first automatic
// run) or just fall through to the normal discovery screen (re-invoked
bool runSetupWizard(SDL_Window* window, SDL_Renderer* renderer, SDL_Texture* texture, SDL_Gamepad*& gamepad,
                    uint16_t discoveryPort, NetClientConfig baseNetConfig,
                    const std::string& discoveryStorePath);

} // namespace dualdeck::client
