#pragma once

// First-run setup wizard (GitHub issue #19). Split out of main.cpp; see
// setup_wizard.cpp for the individual steps.

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>

#include "net_client.h"

namespace dualdeck::client {

enum class WizardOutcome {
    // Reached the end. outNetConfig holds the host the wizard connected
    // to, so the caller can go straight to it -- a host entered by
    // address may never show up in the discovery list.
    Completed,
    // B on the welcome screen: carry on to the normal host picker.
    Skipped,
    // Window closed, or EXIT chosen from the host picker's menu.
    Quit,
};

// Orchestrates the whole wizard as an explicit step state machine.
WizardOutcome runSetupWizard(SDL_Window* window, SDL_Renderer* renderer, SDL_Texture* texture, SDL_Gamepad*& gamepad,
                    uint16_t discoveryPort, NetClientConfig baseNetConfig,
                    const std::string& discoveryStorePath, NetClientConfig& outNetConfig);

} // namespace dualdeck::client
