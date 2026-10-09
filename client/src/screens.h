#pragma once

// Shared layout constants and the bitmap-font screens the client draws
// outside of an active video stream: the host picker's searching/list
// screens, "connecting", Host Control, the pause/settings menu, and the
// in-session debug overlay. Split out of main.cpp so the stream loop,
// host picker and setup wizard can share them.

#include <SDL3/SDL.h>

#include <string>
#include <vector>

#include "discovery_client.h"
#include "net_client.h"

namespace dualdeck::client {

inline constexpr int kWindowWidth = 1280;
inline constexpr int kWindowHeight = 800;
inline constexpr int kDSWidth = 256;
inline constexpr int kDSHeight = 192;

// Shown on the discovery and connecting screens (spec request: tell the
// user how to open the menu up front, not only once it's already open --
// the pause menu's own "L3+R3 TO CLOSE" hint doesn't help someone
// who doesn't know to open it in the first place).
inline constexpr const char* kMenuComboHint = "HOLD BOTH STICKS IN (L3+R3) TO OPEN THE MENU";

void renderCenteredBitmapText(SDL_Renderer* renderer, const std::string& text, float y,
                               int pixelSize, SDL_Color color);
void renderClientVersionStamp(SDL_Renderer* renderer, const std::string& clientVersion);
void renderDebugOverlay(SDL_Renderer* renderer, NetClient& net, int requestedVideoQuality);
void renderDiscoverySearching(SDL_Renderer* renderer, const std::string& clientVersion);
void renderConnecting(SDL_Renderer* renderer, const std::string& hostAddress);
void renderHostControlScreen(SDL_Renderer* renderer, const std::string& identity);
void renderDiscoveryList(SDL_Renderer* renderer, const std::vector<DiscoveredHost>& hosts,
                          int selectedIndex, const std::string& clientVersion);
// One "[BUTTON] ACTION" entry in a screen's footer.
struct ButtonHint {
    std::string button;
    std::string action;
};
// Draws the footer row of button hints, centered near the bottom edge.
void renderButtonHints(SDL_Renderer* renderer, const std::vector<ButtonHint>& hints);
// D-PAD MOVE, A SELECT, B BACK.
const std::vector<ButtonHint>& defaultMenuHints();

// A ring of dots with one lit dot that circles, so a screen that's
// waiting on the network visibly isn't frozen. Driven by the clock.
void renderSpinner(SDL_Renderer* renderer, float centerX, float centerY);

void renderPauseMenu(SDL_Renderer* renderer, const std::vector<std::string>& items, int selectedIndex,
                     const std::string& title = "MENU", const std::string& statusLine = "",
                     float micLevel = -1.0f, const std::string& subtitle = "",
                     const std::vector<ButtonHint>& hints = defaultMenuHints());

} // namespace dualdeck::client
