#pragma once

// The LAN host picker shown on every launch. Split out of main.cpp; see
// discoverAndSelectHost()'s comment in host_picker.cpp.

#include <SDL3/SDL.h>

#include <cstdint>
#include <optional>
#include <string>

#include "discovery_client.h"

namespace melonds_remote::client {

// Returns std::nullopt if the user closed the window or chose EXIT from
// the L3+R3 menu; callers treat that as "cancel the whole run".
std::optional<DiscoveredHost> discoverAndSelectHost(SDL_Renderer* renderer, SDL_Gamepad*& gamepad,
                                                     uint16_t discoveryPort,
                                                     const std::string& lastHostAddress,
                                                     const std::string& clientVersion);

} // namespace melonds_remote::client
