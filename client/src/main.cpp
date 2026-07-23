// DualDeck -- Steam Deck client.
//
// What this does today:
//  - Opens a 1280x800 window (Steam Deck panel resolution)
//  - On every launch, scans the LAN for available DualDeck hosts
//    and shows a gamepad/keyboard-navigable selection screen (spec
//    section 8.1's discovery, adapted per user request: always show the
//    picker rather than silently reconnecting to whichever host was used
//    last, so switching to a different HTPC is always one screen away)
//  - Connects to the chosen dualdeck-host-service host and displays
//    whatever bottom-screen frames it sends, aspect-correct-fit inside
//    the window
//  - Reads the first connected gamepad and maps it to DS buttons per
//    SPEC.md section 7.3
//  - Reads touchscreen (finger) events, and left-click/drag mouse events
//    (GitHub issue #23 -- e.g. a Steam Deck trackpad configured as a
//    mouse via Steam Input's "Trackpad" binding, an alternative to an
//    actual touchscreen), maps both through
//    melonds_remote::computeAspectFitRect / mapPointToDSCoords, and
//    ignores touches/clicks outside the rendered DS rectangle
//  - Sends a full ControllerState packet at a fixed ~120Hz rate
//    regardless of whether anything changed (spec section 6.3)
//  - Logs connection/controller/touch/frame events to stderr as the
//    "debug overlay" for this milestone (an on-screen overlay is future
//    work, see docs/architecture.md "Known gaps") -- stderr specifically,
//    not stdout, since stdout is fully buffered once redirected to a file
//    (the common case for troubleshooting), which can delay or lose these
//    messages entirely for a while; stderr isn't.
//  - Implements device-approval authentication (spec section 13,
//    replacing an earlier 6-digit-code-entry screen that required typing
//    on the client -- unworkable since Steam Input doesn't reliably bring
//    up a virtual keyboard in Gaming Mode, see docs/known-limitations.md):
//    sends a persistent, self-generated device identity on every Hello; a
//    human at the host approves or denies it, no typing anywhere.

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "bitmap_font.h"
#include "client_settings.h"
#include "device_identity.h"
#include "discovery_client.h"
#include "discovery_store.h"
#include "mic_capture.h"
#include "melonds_remote/protocol.h"
#include "melonds_remote/touch_mapping.h"
#include "net_client.h"
#include "wizard_state.h"

using namespace melonds_remote;
using namespace melonds_remote::client;

namespace {

constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 800;
constexpr int kDSWidth = 256;
constexpr int kDSHeight = 192;

// Matches host::NetServerConfig::discoveryPort's default (net_server.h).
constexpr uint16_t kDefaultDiscoveryPort = 8763;
// Each discoverHosts() call blocks for this long. discoverAndSelectHost()
// runs it on a background thread, repeatedly, for as long as the picker
// screen is shown -- not a total search timeout, just how often the
// on-screen host list refreshes. Originally run inline on the render/
// input thread itself, which meant SDL_PollEvent() (and therefore every
// button press, not just DPAD/SOUTH on the picker) went unserviced for
// this entire duration on every single scan -- reported as buttons
// "sometimes doing nothing" (GitHub issue #21, reopened after the first
// pass only added a "connecting" screen for the *post-selection* wait,
// not this pre-selection one).
constexpr int kDiscoveryScanMs = 1200;
// discoverAndSelectHost()'s own loop no longer blocks on network I/O (see
// kDiscoveryScanMs's comment), so without an explicit cap it would poll
// events and re-render as fast as the platform allows -- measured at a
// full CPU core pinned to 100% under Xvfb software rendering, since
// nothing else paces it. ~60Hz is plenty for a picker/menu screen with
// no low-latency requirement of its own.
constexpr int kPickerFrameIntervalMs = 16;

// Deliberate-hold duration for the L3+R3 "open menu" chord, shared
// by discoverAndSelectHost() and main()'s inner loop so every screen uses
// the same chord (GitHub issues #8, #9: the discovery/host-selection
// screen previously had no exit control at all, despite already showing
// the "HOLD START + SELECT" hint -- the hint just wasn't backed by any
// actual handling there). Requires a sustained hold rather than firing
// the instant both buttons are seen down in the same polled frame -- real
// Steam Deck hardware testing showed a single button press (Start, or B)
// opening the menu, most likely via Steam Input's default binding
// template synthesizing a keyboard Escape for individual buttons (see the
// gating on !gamepad in the KEY_DOWN handlers below); requiring a
// sustained two-button hold is defense in depth against any single
// spurious button/synthesized-input report on top of that fix.
//
// Originally Start+Select, changed to the left/right stick clicks (L3+R3)
// because Start+Select is also Steam Input's own default chord for
// switching between the "gamepad" and "desktop" action sets on Steam
// Deck -- holding it was being intercepted before this app ever saw a
// sustained press, rather than opening this menu. L3/R3 aren't mapped to
// any DS button (the DS has no analog sticks), so they're free, and
// unlike Start+Select they're not a Steam Deck system chord.
constexpr uint64_t kMenuChordHoldUs = 350'000; // 350ms deliberate hold

// DS button bit <- SDL gamepad button, per SPEC.md section 7.3.
struct ButtonMapping {
    SDL_GamepadButton sdlButton;
    uint16_t dsBit;
};

const ButtonMapping kButtonMappings[] = {
    {SDL_GAMEPAD_BUTTON_SOUTH, DSButton_A},
    {SDL_GAMEPAD_BUTTON_EAST, DSButton_B},
    {SDL_GAMEPAD_BUTTON_WEST, DSButton_X},
    {SDL_GAMEPAD_BUTTON_NORTH, DSButton_Y},
    {SDL_GAMEPAD_BUTTON_DPAD_UP, DSButton_Up},
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, DSButton_Down},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, DSButton_Left},
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, DSButton_Right},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, DSButton_L},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, DSButton_R},
    {SDL_GAMEPAD_BUTTON_START, DSButton_Start},
    {SDL_GAMEPAD_BUTTON_BACK, DSButton_Select}, // "View" on Steam Deck
};

constexpr int16_t kStickDeadzone = 8000;

// Plain `-axis` overflows int16_t's range at the negative extreme
// (-(-32768) doesn't fit in 16 bits) -- clamps to the wire's own
// documented -32768..32767 range instead of relying on wraparound.
int16_t negateStickAxis(int16_t axis) {
    return axis == INT16_MIN ? INT16_MAX : static_cast<int16_t>(-axis);
}

// Shown on the discovery and connecting screens (spec request: tell the
// user how to open the menu up front, not only once it's already open --
// the pause menu's own "L3+R3 TO CLOSE" hint doesn't help someone
// who doesn't know to open it in the first place).
constexpr const char* kMenuComboHint = "HOLD BOTH STICKS IN (L3+R3) TO OPEN THE MENU";

// Wall-clock (epoch) microseconds, for the wire ControllerState.clientTimestampUs
// field specifically. Deliberately not SDL_GetTicksNS() (which is time since
// SDL_Init(), not comparable across processes/machines) -- the host uses this
// to estimate one-way input latency, which only makes sense against a shared
// time base (spec section 8.5). This assumes client and host clocks are
// reasonably synced (e.g. via NTP), same as the LAN latency targets in the
// spec already assume.
uint64_t wallClockNowUs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<microseconds>(system_clock::now().time_since_epoch()).count());
}

// `stickEmulatesDpad` (spec section 7.3's "left stick as an alternate
// D-pad") defaults to on since it's harmless local visual feedback for
// the setup wizard's controller test, the only other caller. The real
// per-frame gameplay call site below passes false for a 3DS/Azahar
// session: that stick has a genuine analog Circle Pad of its own (sent
// separately, see buildControllerState()), and folding the same stick
// tilt into the physical D-Pad's digital bits too would fire both at
// once for one physical motion -- confirmed via AzaharAdapter.cpp's
// registerInputEngine(), which binds the D-Pad's four native buttons to
// GenericButton_Dpad* independently of the Circle Pad's own analog
// engine binding, so a game watching either one would see it.
uint16_t buildButtonsFromGamepad(SDL_Gamepad* gamepad, bool stickEmulatesDpad = true) {
    if (!gamepad) return 0;

    uint16_t buttons = 0;
    for (const auto& mapping : kButtonMappings) {
        if (SDL_GetGamepadButton(gamepad, mapping.sdlButton)) {
            buttons |= mapping.dsBit;
        }
    }

    if (stickEmulatesDpad) {
        int16_t leftX = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
        int16_t leftY = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY);
        if (leftX > kStickDeadzone) buttons |= DSButton_Right;
        if (leftX < -kStickDeadzone) buttons |= DSButton_Left;
        if (leftY > kStickDeadzone) buttons |= DSButton_Down;
        if (leftY < -kStickDeadzone) buttons |= DSButton_Up;
    }

    return buttons;
}

void renderCenteredBitmapText(SDL_Renderer* renderer, const std::string& text, float y,
                               int pixelSize, SDL_Color color) {
    int width = measureBitmapText(text, pixelSize);
    float x = (static_cast<float>(kWindowWidth) - static_cast<float>(width)) / 2.0f;
    renderBitmapText(renderer, text, x, y, pixelSize, color);
}

void renderDiscoverySearching(SDL_Renderer* renderer) {
    SDL_SetRenderDrawColor(renderer, 20, 20, 24, 255);
    SDL_RenderClear(renderer);
    renderCenteredBitmapText(renderer, "SEARCHING FOR HOST...",
                              static_cast<float>(kWindowHeight) / 2.0f - 20.0f, 4,
                              SDL_Color{200, 200, 200, 255});
    renderCenteredBitmapText(renderer, "MAKE SURE A DUALDECK HOST IS RUNNING ON THIS NETWORK",
                              static_cast<float>(kWindowHeight) / 2.0f + 40.0f, 2,
                              SDL_Color{140, 140, 140, 255});
    renderCenteredBitmapText(renderer, kMenuComboHint, static_cast<float>(kWindowHeight) - 80.0f, 2,
                              SDL_Color{140, 140, 140, 255});
    SDL_RenderPresent(renderer);
}

void renderConnecting(SDL_Renderer* renderer, const std::string& hostAddress) {
    SDL_SetRenderDrawColor(renderer, 20, 20, 24, 255);
    SDL_RenderClear(renderer);
    renderCenteredBitmapText(renderer, "CONNECTING TO " + hostAddress + "...",
                              static_cast<float>(kWindowHeight) / 2.0f - 20.0f, 4,
                              SDL_Color{220, 200, 80, 255});
    renderCenteredBitmapText(renderer, "PLEASE WAIT WHILE THE HOST RESPONDS",
                              static_cast<float>(kWindowHeight) / 2.0f + 40.0f, 2,
                              SDL_Color{140, 140, 140, 255});
    renderCenteredBitmapText(renderer, kMenuComboHint, static_cast<float>(kWindowHeight) - 80.0f, 2,
                              SDL_Color{140, 140, 140, 255});
    SDL_RenderPresent(renderer);
}

// GitHub issue #4 Phase E: shown in place of the video texture whenever
// net.hostMode() == HostMode::HostControl -- there is no video to show
// (HostControlAdapter::getLatestFrame() always returns false, see
// host/remote-server/src/host_control_adapter.cpp), but ControllerState
// packets are still being sent every frame exactly as in Emulation mode
// (the render loop below doesn't gate that on mode), so a controller
// plugged into a real host will already be navigating that host's own UI
// via HostControlAdapter's virtual gamepad while this screen is up.
void renderHostControlScreen(SDL_Renderer* renderer, const std::string& identity) {
    SDL_SetRenderDrawColor(renderer, 20, 24, 20, 255);
    SDL_RenderClear(renderer);
    renderCenteredBitmapText(renderer, "HOST CONTROL", static_cast<float>(kWindowHeight) / 2.0f - 60.0f, 4,
                              SDL_Color{120, 210, 150, 255});
    renderCenteredBitmapText(renderer, "USE YOUR CONTROLLER TO NAVIGATE THE HOST",
                              static_cast<float>(kWindowHeight) / 2.0f, 2, SDL_Color{200, 200, 200, 255});
    renderCenteredBitmapText(renderer, "AN EMULATION SESSION WILL APPEAR HERE AUTOMATICALLY WHEN ONE STARTS",
                              static_cast<float>(kWindowHeight) / 2.0f + 34.0f, 2, SDL_Color{140, 140, 140, 255});
    if (!identity.empty()) {
        renderCenteredBitmapText(renderer, identity, static_cast<float>(kWindowHeight) / 2.0f + 80.0f, 2,
                                  SDL_Color{150, 170, 210, 255});
    }
    renderCenteredBitmapText(renderer, kMenuComboHint, static_cast<float>(kWindowHeight) - 80.0f, 2,
                              SDL_Color{140, 140, 140, 255});
    SDL_RenderPresent(renderer);
}

void renderDiscoveryList(SDL_Renderer* renderer, const std::vector<DiscoveredHost>& hosts,
                          int selectedIndex) {
    SDL_SetRenderDrawColor(renderer, 20, 20, 24, 255);
    SDL_RenderClear(renderer);
    renderCenteredBitmapText(renderer, "SELECT A HOST", 60.0f, 4, SDL_Color{220, 220, 220, 255});

    // Two lines per host (GitHub issue #28: "Update the Client
    // host-selection UI to display the host, emulated system, and
    // actual emulator"): the host name/address as before, plus a second
    // line naming the emulated system and adapter reported in its
    // DiscoveryResponse, e.g. "NINTENDO DS - MELONDS" -- the bitmap font
    // has no glyph for the middle-dot separator used in issue #28's own
    // example text (see bitmap_font.cpp's supported character set), so
    // "-" stands in for it here, matching the connected-session menu's
    // identity line (see identityLine() in main()).
    constexpr float kRowHeight = 84.0f;
    constexpr int kPixelSize = 3;
    constexpr int kIdentityPixelSize = 2;
    constexpr float kStartY = 180.0f;

    for (size_t i = 0; i < hosts.size(); ++i) {
        float rowY = kStartY + static_cast<float>(i) * kRowHeight;
        std::string addressAndPort = hosts[i].address + ":" + std::to_string(hosts[i].controlPort);
        std::string label = hosts[i].hostName.empty()
                                 ? addressAndPort
                                 : hosts[i].hostName + "  (" + addressAndPort + ")";
        std::string identity = hosts[i].system.systemName;
        if (!hosts[i].adapter.adapterName.empty()) {
            identity += (identity.empty() ? "" : " - ") + hosts[i].adapter.adapterName;
        }
        bool selected = static_cast<int>(i) == selectedIndex;
        SDL_Color color = selected ? SDL_Color{90, 200, 120, 255} : SDL_Color{200, 200, 200, 255};
        SDL_Color identityColor = selected ? SDL_Color{150, 210, 170, 255} : SDL_Color{140, 140, 140, 255};

        if (selected) {
            int width = std::max(measureBitmapText(label, kPixelSize),
                                  measureBitmapText(identity, kIdentityPixelSize));
            float x = (static_cast<float>(kWindowWidth) - static_cast<float>(width)) / 2.0f;
            SDL_FRect highlight{x - 20.0f, rowY - 8.0f, static_cast<float>(width) + 40.0f,
                                 static_cast<float>(kFontGlyphHeight * kPixelSize) +
                                     static_cast<float>(kFontGlyphHeight * kIdentityPixelSize) + 26.0f};
            SDL_SetRenderDrawColor(renderer, 50, 70, 55, 255);
            SDL_RenderFillRect(renderer, &highlight);
        }
        renderCenteredBitmapText(renderer, label, rowY, kPixelSize, color);
        if (!identity.empty()) {
            renderCenteredBitmapText(renderer, identity, rowY + static_cast<float>(kFontGlyphHeight * kPixelSize) + 10.0f,
                                      kIdentityPixelSize, identityColor);
        }
    }

    renderCenteredBitmapText(renderer, "D-PAD TO MOVE, A TO SELECT",
                              static_cast<float>(kWindowHeight) - 100.0f, 2,
                              SDL_Color{140, 140, 140, 255});
    renderCenteredBitmapText(renderer, kMenuComboHint, static_cast<float>(kWindowHeight) - 60.0f, 2,
                              SDL_Color{140, 140, 140, 255});
    SDL_RenderPresent(renderer);
}

void renderPauseMenu(SDL_Renderer* renderer, const std::vector<std::string>& items, int selectedIndex,
                     const std::string& title = "MENU", const std::string& statusLine = "",
                     float micLevel = -1.0f, const std::string& subtitle = "") {
    SDL_SetRenderDrawColor(renderer, 20, 20, 24, 220);
    SDL_RenderClear(renderer);
    renderCenteredBitmapText(renderer, title, 100.0f, 4, SDL_Color{220, 220, 220, 255});
    // Active emulated system/adapter (GitHub issue #28), e.g.
    // "NINTENDO DS - MELONDS" -- shown right under the title in a
    // neutral color, distinct from statusLine below (which is reserved
    // for error/warning text in red).
    if (!subtitle.empty()) {
        renderCenteredBitmapText(renderer, subtitle, 148.0f, 2, SDL_Color{150, 170, 210, 255});
    }

    constexpr float kRowHeight = 70.0f;
    constexpr int kPixelSize = 4;
    float startY = static_cast<float>(kWindowHeight) / 2.0f -
                    (static_cast<float>(items.size()) * kRowHeight) / 2.0f;

    for (size_t i = 0; i < items.size(); ++i) {
        float rowY = startY + static_cast<float>(i) * kRowHeight;
        bool selected = static_cast<int>(i) == selectedIndex;
        SDL_Color color = selected ? SDL_Color{90, 200, 120, 255} : SDL_Color{200, 200, 200, 255};

        if (selected) {
            int width = measureBitmapText(items[i], kPixelSize);
            float x = (static_cast<float>(kWindowWidth) - static_cast<float>(width)) / 2.0f;
            SDL_FRect highlight{x - 24.0f, rowY - 10.0f, static_cast<float>(width) + 48.0f,
                                 static_cast<float>(kFontGlyphHeight * kPixelSize) + 20.0f};
            SDL_SetRenderDrawColor(renderer, 50, 70, 55, 255);
            SDL_RenderFillRect(renderer, &highlight);
        }
        renderCenteredBitmapText(renderer, items[i], rowY, kPixelSize, color);
    }

    // Live microphone input-level meter (GitHub issue #2), shown only on
    // screens that pass a real level (>= 0) -- the settings screen while
    // the host supports mic input. Drawn below the menu rows regardless
    // of mute state, so muting is visibly distinct from "no signal at
    // all" (the bar keeps moving with real input; only the host stops
    // receiving it).
    if (micLevel >= 0.0f) {
        float meterY = startY + static_cast<float>(items.size()) * kRowHeight + 30.0f;
        constexpr float kMeterWidth = 420.0f;
        constexpr float kMeterHeight = 28.0f;
        float meterX = (static_cast<float>(kWindowWidth) - kMeterWidth) / 2.0f;

        renderCenteredBitmapText(renderer, "MIC LEVEL", meterY - 34.0f, 2, SDL_Color{140, 140, 140, 255});

        SDL_FRect meterBg{meterX, meterY, kMeterWidth, kMeterHeight};
        SDL_SetRenderDrawColor(renderer, 45, 45, 50, 255);
        SDL_RenderFillRect(renderer, &meterBg);

        float clampedLevel = std::clamp(micLevel, 0.0f, 1.0f);
        SDL_FRect meterFill{meterX, meterY, kMeterWidth * clampedLevel, kMeterHeight};
        SDL_Color fillColor = clampedLevel > 0.9f ? SDL_Color{220, 90, 90, 255} : SDL_Color{90, 200, 120, 255};
        SDL_SetRenderDrawColor(renderer, fillColor.r, fillColor.g, fillColor.b, fillColor.a);
        SDL_RenderFillRect(renderer, &meterFill);

        SDL_SetRenderDrawColor(renderer, 140, 140, 140, 255);
        SDL_RenderRect(renderer, &meterBg);
    }

    if (!statusLine.empty()) {
        renderCenteredBitmapText(renderer, statusLine, static_cast<float>(kWindowHeight) - 125.0f, 2,
                                  SDL_Color{220, 90, 90, 255});
    }
    renderCenteredBitmapText(renderer, "D-PAD TO MOVE, A TO SELECT, B TO GO BACK",
                              static_cast<float>(kWindowHeight) - 80.0f, 2,
                              SDL_Color{140, 140, 140, 255});
    SDL_RenderPresent(renderer);
}

// Runs on every launch (spec request: "each time the client is booted, it
// should show this screen in the event I want to connect to another
// client"): scans the LAN for DualDeck hosts and always lets the
// user pick one via gamepad D-pad/South or keyboard arrows/Enter -- never
// auto-connects silently, even when only one host answers, so switching
// to a different HTPC is always available, not just when there happens
// to be more than one. The previously-picked host (see discovery_store.h)
// is pre-highlighted as the default selection for a quick one-button
// reconnect, but the user can always navigate to a different one instead.
//
// Keeps rescanning while the list is shown (not just once up front), so
// a host that finishes booting a few seconds late still shows up without
// restarting the client. Selection is preserved across rescans by
// address, and the list is sorted for a stable display order (discovery
// itself doesn't guarantee reply order is consistent scan to scan).
//
// The scan itself runs on its own background thread (GitHub issue #21,
// reopened) -- discoverHosts() blocks for kDiscoveryScanMs, and running
// that inline on this function's own loop, as a first pass of this fix
// did, meant SDL_PollEvent() (and every button press with it) went
// unserviced for the whole 1.2s of every single rescan, not just the
// initial one, making the picker feel randomly unresponsive depending on
// exactly when a press landed relative to the blocking call. This
// thread does nothing but scan-and-publish; the loop below always polls
// events and renders every frame regardless of scan timing.
//
// Returns std::nullopt if the user closed the window before a host was
// chosen (SDL_EVENT_QUIT), or chose EXIT from the L3+R3 menu below;
// main() treats either as "cancel the whole run", not "connect anyway."
std::optional<DiscoveredHost> discoverAndSelectHost(SDL_Renderer* renderer, SDL_Gamepad*& gamepad,
                                                     uint16_t discoveryPort,
                                                     const std::string& lastHostAddress) {
    std::vector<DiscoveredHost> hosts;
    int selectedIndex = 0;

    std::mutex scanMutex;
    std::vector<DiscoveredHost> latestScan;
    std::atomic<bool> scanStop{false};
    std::thread scanThread([&]() {
        while (!scanStop.load()) {
            std::vector<DiscoveredHost> fresh = discoverHosts(discoveryPort, kDiscoveryScanMs, &scanStop);
            std::sort(fresh.begin(), fresh.end(),
                      [](const DiscoveredHost& a, const DiscoveredHost& b) { return a.address < b.address; });
            std::lock_guard<std::mutex> lock(scanMutex);
            latestScan = std::move(fresh);
        }
    });
    // Guarantees scanThread is stopped and joined on every return path
    // below (including SDL_EVENT_QUIT and the menu's EXIT) without
    // needing a matching stop+join before each one individually.
    struct ScanThreadGuard {
        std::atomic<bool>& stop;
        std::thread& thread;
        ~ScanThreadGuard() {
            stop = true;
            if (thread.joinable()) thread.join();
        }
    } scanThreadGuard{scanStop, scanThread};

    // L3+R3 "open menu" chord, offering an EXIT control -- this
    // screen previously had none at all (GitHub issues #8, #9), despite
    // already showing the menu-combo hint via kMenuComboHint.
    // Same deliberate-hold pattern and menu-navigation conventions as
    // main()'s inner loop (see kMenuChordHoldUs's declaration for why).
    // No "CHANGE HOST"/"RESUME SEARCH" distinction is needed here beyond
    // RESUME (close the menu, keep searching) since there's nothing else
    // to navigate to from this screen.
    const std::vector<std::string> menuItems = {"RESUME", "EXIT"};
    bool menuActive = false;
    int menuSelectedIndex = 0;
    uint64_t menuChordSinceUs = 0;
    bool menuChordFired = false;

    while (true) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_EVENT_QUIT:
                    return std::nullopt;
                case SDL_EVENT_GAMEPAD_ADDED:
                    if (!gamepad) {
                        gamepad = SDL_OpenGamepad(event.gdevice.which);
                    }
                    break;
                case SDL_EVENT_GAMEPAD_REMOVED:
                    if (gamepad && SDL_GetGamepadID(gamepad) == event.gdevice.which) {
                        SDL_CloseGamepad(gamepad);
                        gamepad = nullptr;
                    }
                    break;
                case SDL_EVENT_KEY_DOWN: {
                    int menuCount = static_cast<int>(menuItems.size());
                    // Only honored with no gamepad connected (Desktop
                    // Mode/keyboard testing convenience) -- see the
                    // matching gate in main()'s inner loop for why.
                    if (!gamepad && event.key.key == SDLK_ESCAPE) {
                        menuActive = !menuActive;
                        menuSelectedIndex = 0;
                    } else if (menuActive && event.key.key == SDLK_UP) {
                        menuSelectedIndex = (menuSelectedIndex + menuCount - 1) % menuCount;
                    } else if (menuActive && event.key.key == SDLK_DOWN) {
                        menuSelectedIndex = (menuSelectedIndex + 1) % menuCount;
                    } else if (menuActive && event.key.key == SDLK_RETURN) {
                        if (menuItems[static_cast<size_t>(menuSelectedIndex)] == "EXIT") return std::nullopt;
                        menuActive = false; // RESUME
                    } else if (!menuActive && !hosts.empty()) {
                        int count = static_cast<int>(hosts.size());
                        if (event.key.key == SDLK_UP) {
                            selectedIndex = (selectedIndex + count - 1) % count;
                        } else if (event.key.key == SDLK_DOWN) {
                            selectedIndex = (selectedIndex + 1) % count;
                        } else if (event.key.key == SDLK_RETURN) {
                            return hosts[static_cast<size_t>(selectedIndex)];
                        }
                    }
                    break;
                }
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                    if (menuActive) {
                        int menuCount = static_cast<int>(menuItems.size());
                        if (event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP) {
                            menuSelectedIndex = (menuSelectedIndex + menuCount - 1) % menuCount;
                        } else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
                            menuSelectedIndex = (menuSelectedIndex + 1) % menuCount;
                        } else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) {
                            if (menuItems[static_cast<size_t>(menuSelectedIndex)] == "EXIT") return std::nullopt;
                            menuActive = false; // RESUME
                        } else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) {
                            menuActive = false; // back/cancel, no action taken
                        }
                    } else if (!hosts.empty()) {
                        int count = static_cast<int>(hosts.size());
                        if (event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP) {
                            selectedIndex = (selectedIndex + count - 1) % count;
                        } else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
                            selectedIndex = (selectedIndex + 1) % count;
                        } else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) {
                            return hosts[static_cast<size_t>(selectedIndex)];
                        }
                    }
                    break;
                default:
                    break;
            }
        }

        // Held L3+R3 toggles the menu -- see kMenuChordHoldUs's
        // declaration for why a deliberate hold is required (and why L3+R3
        // rather than Start+Select).
        bool menuChordHeld = gamepad && SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_STICK) &&
                             SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
        uint64_t nowForChordUs = SDL_GetTicksNS() / 1000;
        if (menuChordHeld) {
            if (menuChordSinceUs == 0) menuChordSinceUs = nowForChordUs;
            if (!menuChordFired && nowForChordUs - menuChordSinceUs >= kMenuChordHoldUs) {
                menuActive = !menuActive;
                menuSelectedIndex = 0;
                menuChordFired = true;
            }
        } else {
            menuChordSinceUs = 0;
            menuChordFired = false;
        }

        if (menuActive) {
            renderPauseMenu(renderer, menuItems, menuSelectedIndex);
            SDL_Delay(kPickerFrameIntervalMs);
            continue;
        }

        if (hosts.empty()) {
            renderDiscoverySearching(renderer);
        } else {
            renderDiscoveryList(renderer, hosts, selectedIndex);
        }

        // Pull whatever the background scan thread has published so far --
        // never blocks on network I/O itself, just a quick copy under a
        // mutex, so this loop iterates at normal frame rate regardless of
        // scan timing (see the function-level comment above for why this
        // used to run discoverHosts() inline here instead).
        std::string selectedAddress = hosts.empty() ? "" : hosts[static_cast<size_t>(selectedIndex)].address;
        {
            std::lock_guard<std::mutex> lock(scanMutex);
            hosts = latestScan;
        }

        if (hosts.empty()) {
            selectedIndex = 0;
        } else {
            auto it = std::find_if(hosts.begin(), hosts.end(),
                                    [&](const DiscoveredHost& h) { return h.address == selectedAddress; });
            if (it != hosts.end()) {
                selectedIndex = static_cast<int>(it - hosts.begin());
            } else {
                auto lastIt = std::find_if(hosts.begin(), hosts.end(), [&](const DiscoveredHost& h) {
                    return h.address == lastHostAddress;
                });
                selectedIndex = lastIt != hosts.end() ? static_cast<int>(lastIt - hosts.begin()) : 0;
            }
        }

        SDL_Delay(kPickerFrameIntervalMs);
    }
}

// First-run setup wizard (GitHub issue #19): a linear sequence of screens
// that walks a new user through connecting to a host and confirming
// video/controller/touch all work, instead of dropping them straight onto
// the discovery screen with no explanation. Runs once automatically (see
// wizard_state.h) and is reachable again afterward from the Settings screen
// in main()'s pause menu.
//
// Deliberately out of scope, documented in docs/known-limitations.md
// rather than silently skipped: audio/microphone testing (this project has
// no audio feature at all yet), host-side UI changes (would require
// patching real melonDS Qt source, a separate undertaking), distinguishing
// a denied device from one still awaiting approval (the protocol has no
// wire signal for "denied" -- see docs/protocol.md), and real Steam Deck
// LCD/OLED hardware verification (not possible from this sandbox).

enum class WizardStepResult { Advance, Back, Exit };
enum class WizardConnectionMethod { Auto, Manual };
enum class WizardConnectResult { Connected, Back, Exit };
enum class WizardVideoResult { Passed, Reconnect, Exit };
enum class WizardSimpleResult { Passed, Back, Exit };

void renderWizardMessage(SDL_Renderer* renderer, const std::string& title,
                          const std::vector<std::string>& lines, const std::string& hint) {
    SDL_SetRenderDrawColor(renderer, 20, 20, 24, 255);
    SDL_RenderClear(renderer);
    renderCenteredBitmapText(renderer, title, 90.0f, 4, SDL_Color{220, 220, 220, 255});

    float y = 220.0f;
    for (const auto& line : lines) {
        renderCenteredBitmapText(renderer, line, y, 2, SDL_Color{200, 200, 200, 255});
        y += 36.0f;
    }

    if (!hint.empty()) {
        renderCenteredBitmapText(renderer, hint, static_cast<float>(kWindowHeight) - 60.0f, 2,
                                  SDL_Color{140, 140, 140, 255});
    }
    SDL_RenderPresent(renderer);
}

void renderWizardMenu(SDL_Renderer* renderer, const std::string& title,
                       const std::vector<std::string>& items, int selectedIndex,
                       const std::string& hint) {
    SDL_SetRenderDrawColor(renderer, 20, 20, 24, 255);
    SDL_RenderClear(renderer);
    renderCenteredBitmapText(renderer, title, 100.0f, 4, SDL_Color{220, 220, 220, 255});

    constexpr float kRowHeight = 70.0f;
    constexpr int kPixelSize = 3;
    float startY = static_cast<float>(kWindowHeight) / 2.0f -
                   (static_cast<float>(items.size()) * kRowHeight) / 2.0f;

    for (size_t i = 0; i < items.size(); ++i) {
        float rowY = startY + static_cast<float>(i) * kRowHeight;
        bool selected = static_cast<int>(i) == selectedIndex;
        SDL_Color color = selected ? SDL_Color{90, 200, 120, 255} : SDL_Color{200, 200, 200, 255};

        if (selected) {
            int width = measureBitmapText(items[i], kPixelSize);
            float x = (static_cast<float>(kWindowWidth) - static_cast<float>(width)) / 2.0f;
            SDL_FRect highlight{x - 20.0f, rowY - 8.0f, static_cast<float>(width) + 40.0f,
                                 static_cast<float>(kFontGlyphHeight * kPixelSize) + 16.0f};
            SDL_SetRenderDrawColor(renderer, 50, 70, 55, 255);
            SDL_RenderFillRect(renderer, &highlight);
        }
        renderCenteredBitmapText(renderer, items[i], rowY, kPixelSize, color);
    }

    if (!hint.empty()) {
        renderCenteredBitmapText(renderer, hint, static_cast<float>(kWindowHeight) - 60.0f, 2,
                                  SDL_Color{140, 140, 140, 255});
    }
    SDL_RenderPresent(renderer);
}

WizardStepResult wizardWelcome(SDL_Renderer* renderer, SDL_Gamepad*& gamepad) {
    while (true) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_EVENT_QUIT:
                    return WizardStepResult::Exit;
                case SDL_EVENT_GAMEPAD_ADDED:
                    if (!gamepad) gamepad = SDL_OpenGamepad(event.gdevice.which);
                    break;
                case SDL_EVENT_GAMEPAD_REMOVED:
                    if (gamepad && SDL_GetGamepadID(gamepad) == event.gdevice.which) {
                        SDL_CloseGamepad(gamepad);
                        gamepad = nullptr;
                    }
                    break;
                case SDL_EVENT_KEY_DOWN:
                    if (event.key.key == SDLK_RETURN) return WizardStepResult::Advance;
                    if (event.key.key == SDLK_ESCAPE) return WizardStepResult::Exit;
                    break;
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                    if (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) return WizardStepResult::Advance;
                    if (event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) return WizardStepResult::Exit;
                    break;
                default:
                    break;
            }
        }

        renderWizardMessage(renderer, "SETUP WIZARD",
                             {"THIS WILL HELP YOU CONNECT TO A HOST",
                              "AND TEST VIDEO CONTROLLER AND TOUCH",
                              "IT TAKES ABOUT A MINUTE"},
                             "A TO CONTINUE - B TO EXIT");
    }
}

WizardStepResult wizardChooseMethod(SDL_Renderer* renderer, SDL_Gamepad*& gamepad,
                                     WizardConnectionMethod& outMethod) {
    const std::vector<std::string> items = {"AUTO DISCOVER HOST", "ENTER HOST ADDRESS MANUALLY"};
    int selectedIndex = 0;

    while (true) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            int count = static_cast<int>(items.size());
            switch (event.type) {
                case SDL_EVENT_QUIT:
                    return WizardStepResult::Exit;
                case SDL_EVENT_GAMEPAD_ADDED:
                    if (!gamepad) gamepad = SDL_OpenGamepad(event.gdevice.which);
                    break;
                case SDL_EVENT_GAMEPAD_REMOVED:
                    if (gamepad && SDL_GetGamepadID(gamepad) == event.gdevice.which) {
                        SDL_CloseGamepad(gamepad);
                        gamepad = nullptr;
                    }
                    break;
                case SDL_EVENT_KEY_DOWN:
                    if (event.key.key == SDLK_UP) {
                        selectedIndex = (selectedIndex + count - 1) % count;
                    } else if (event.key.key == SDLK_DOWN) {
                        selectedIndex = (selectedIndex + 1) % count;
                    } else if (event.key.key == SDLK_RETURN) {
                        outMethod = selectedIndex == 0 ? WizardConnectionMethod::Auto
                                                        : WizardConnectionMethod::Manual;
                        return WizardStepResult::Advance;
                    } else if (event.key.key == SDLK_ESCAPE) {
                        return WizardStepResult::Back;
                    }
                    break;
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                    if (event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP) {
                        selectedIndex = (selectedIndex + count - 1) % count;
                    } else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
                        selectedIndex = (selectedIndex + 1) % count;
                    } else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) {
                        outMethod = selectedIndex == 0 ? WizardConnectionMethod::Auto
                                                        : WizardConnectionMethod::Manual;
                        return WizardStepResult::Advance;
                    } else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) {
                        return WizardStepResult::Back;
                    }
                    break;
                default:
                    break;
            }
        }

        renderWizardMenu(renderer, "HOW DO YOU WANT TO CONNECT", items, selectedIndex,
                          "D-PAD TO MOVE - A TO SELECT - B TO GO BACK");
    }
}

// Steam Input doesn't reliably bring up a virtual keyboard in Gaming Mode
// (the same limitation that killed the old 6-digit pairing-code entry
// screen -- see docs/known-limitations.md); this screen works fine with a
// physical/Bluetooth keyboard but a Gaming-Mode user with only a
// controller may have no way to type here. Documented, not solved.
// Returns std::nullopt if the user cancelled (Escape/window close) --
// runSetupWizard() treats that as "go back", not "exit the wizard".
std::optional<std::string> wizardManualEntry(SDL_Renderer* renderer, SDL_Window* window,
                                              SDL_Gamepad*& gamepad) {
    std::string text;
    std::optional<std::string> result;
    SDL_StartTextInput(window);

    while (!result) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_EVENT_QUIT:
                    SDL_StopTextInput(window);
                    return std::nullopt;
                case SDL_EVENT_GAMEPAD_ADDED:
                    if (!gamepad) gamepad = SDL_OpenGamepad(event.gdevice.which);
                    break;
                case SDL_EVENT_GAMEPAD_REMOVED:
                    if (gamepad && SDL_GetGamepadID(gamepad) == event.gdevice.which) {
                        SDL_CloseGamepad(gamepad);
                        gamepad = nullptr;
                    }
                    break;
                case SDL_EVENT_TEXT_INPUT:
                    text += event.text.text;
                    break;
                case SDL_EVENT_KEY_DOWN:
                    if (event.key.key == SDLK_BACKSPACE && !text.empty()) {
                        text.pop_back();
                    } else if (event.key.key == SDLK_RETURN && !text.empty()) {
                        result = text;
                    } else if (event.key.key == SDLK_ESCAPE) {
                        SDL_StopTextInput(window);
                        return std::nullopt;
                    }
                    break;
                default:
                    break;
            }
        }

        if (result) break;

        renderWizardMessage(renderer, "ENTER HOST ADDRESS",
                             {text.empty() ? "TYPE THE HOSTS IP ADDRESS" : text,
                              "USES DEFAULT PORTS 8760 8761 8762",
                              "A KEYBOARD IS NEEDED FOR THIS SCREEN"},
                             "ENTER TO CONNECT - ESCAPE TO GO BACK");
    }

    SDL_StopTextInput(window);
    return result;
}

// connect() blocks on several socket calls, so retries run on their own
// thread (mirroring main()'s own reconnectThread below) rather than
// freezing this screen. Mirrors the ApprovalRequired/status handling of
// main()'s inner loop, but distinguishes every HelloRejectReason value
// instead of collapsing them, since a first-time user has no other way
// to learn why a connection attempt is failing.
WizardConnectResult wizardConnectAndApprove(SDL_Renderer* renderer, SDL_Gamepad*& gamepad, NetClient& net,
                                             const std::string& hostAddress) {
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> everAttempted{false};
    std::thread connectThread([&]() {
        uint32_t backoffMs = 500;
        constexpr uint32_t kMaxBackoffMs = 3000;
        while (!stopRequested.load() && !net.isConnected()) {
            net.connect();
            everAttempted = true;
            if (net.isConnected()) break;
            for (uint32_t waited = 0; waited < backoffMs && !stopRequested.load(); waited += 100) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            backoffMs = std::min(backoffMs * 2, kMaxBackoffMs);
        }
    });

    WizardConnectResult outcome = WizardConnectResult::Back;
    while (true) {
        bool done = false;
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_EVENT_QUIT:
                    outcome = WizardConnectResult::Exit;
                    done = true;
                    break;
                case SDL_EVENT_GAMEPAD_ADDED:
                    if (!gamepad) gamepad = SDL_OpenGamepad(event.gdevice.which);
                    break;
                case SDL_EVENT_GAMEPAD_REMOVED:
                    if (gamepad && SDL_GetGamepadID(gamepad) == event.gdevice.which) {
                        SDL_CloseGamepad(gamepad);
                        gamepad = nullptr;
                    }
                    break;
                case SDL_EVENT_KEY_DOWN:
                    if (event.key.key == SDLK_ESCAPE) {
                        outcome = WizardConnectResult::Back;
                        done = true;
                    }
                    break;
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                    if (event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) {
                        outcome = WizardConnectResult::Back;
                        done = true;
                    }
                    break;
                default:
                    break;
            }
        }

        if (!done && net.isConnected()) {
            outcome = WizardConnectResult::Connected;
            done = true;
        }

        if (done) break;

        std::string status;
        if (!everAttempted.load()) {
            status = "CONNECTING TO " + hostAddress;
        } else {
            switch (net.lastRejectReason()) {
                case HelloRejectReason::ApprovalRequired:
                    status = "WAITING FOR APPROVAL ON THE HOST";
                    break;
                case HelloRejectReason::ProtocolVersionMismatch:
                    status = "PROTOCOL VERSION MISMATCH - UPDATE THE APP OR HOST";
                    break;
                case HelloRejectReason::AuthenticationFailed:
                    status = "AUTHENTICATION FAILED";
                    break;
                case HelloRejectReason::HostBusy:
                    status = "HOST IS BUSY - RETRYING";
                    break;
                case HelloRejectReason::AppVersionMismatch:
                    status = "VERSION MISMATCH - HOST IS " + net.hostAppVersion() + ", UPDATE TO MATCH";
                    break;
                case HelloRejectReason::None:
                default:
                    status = "HOST UNREACHABLE AT " + hostAddress;
                    break;
            }
        }

        renderWizardMessage(renderer, "CONNECTING",
                             {status,
                              "IF WAITING FOR APPROVAL CHECK THE HOST SCREEN",
                              "AND APPROVE THIS DEVICE THERE"},
                             "B TO GO BACK");
    }

    stopRequested = true;
    connectThread.join();
    return outcome;
}

WizardVideoResult wizardVideoTest(SDL_Renderer* renderer, SDL_Texture* texture, SDL_Gamepad*& gamepad,
                                   NetClient& net) {
    std::vector<uint8_t> frame;
    bool everSawFrame = false;

    while (true) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_EVENT_QUIT:
                    return WizardVideoResult::Exit;
                case SDL_EVENT_GAMEPAD_ADDED:
                    if (!gamepad) gamepad = SDL_OpenGamepad(event.gdevice.which);
                    break;
                case SDL_EVENT_GAMEPAD_REMOVED:
                    if (gamepad && SDL_GetGamepadID(gamepad) == event.gdevice.which) {
                        SDL_CloseGamepad(gamepad);
                        gamepad = nullptr;
                    }
                    break;
                case SDL_EVENT_KEY_DOWN:
                    if (event.key.key == SDLK_RETURN && everSawFrame) return WizardVideoResult::Passed;
                    if (event.key.key == SDLK_ESCAPE) return WizardVideoResult::Reconnect;
                    break;
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                    if (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH && everSawFrame) {
                        return WizardVideoResult::Passed;
                    }
                    if (event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) return WizardVideoResult::Reconnect;
                    break;
                default:
                    break;
            }
        }

        if (!net.isConnected()) return WizardVideoResult::Reconnect;

        if (net.getLatestFrame(frame) &&
            frame.size() == static_cast<size_t>(kDSWidth) * kDSHeight * 4) {
            everSawFrame = true;
            SDL_UpdateTexture(texture, nullptr, frame.data(), kDSWidth * 4);
        }

        RenderRect dsRect = computeAspectFitRect(kWindowWidth, kWindowHeight);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        SDL_FRect dst{static_cast<float>(dsRect.x), static_cast<float>(dsRect.y),
                      static_cast<float>(dsRect.width), static_cast<float>(dsRect.height)};
        SDL_RenderTexture(renderer, texture, nullptr, &dst);

        if (!everSawFrame) {
            renderCenteredBitmapText(renderer, "NO VIDEO YET", 24.0f, 3, SDL_Color{220, 200, 80, 255});
            renderCenteredBitmapText(renderer, "OPEN A ROM ON THE HOST IF THE SCREEN STAYS BLANK", 60.0f, 2,
                                      SDL_Color{200, 200, 200, 255});
        } else {
            renderCenteredBitmapText(renderer, "VIDEO IS WORKING", 24.0f, 3, SDL_Color{90, 200, 120, 255});
            renderCenteredBitmapText(renderer, "A TO CONTINUE", 60.0f, 2, SDL_Color{200, 200, 200, 255});
        }
        renderCenteredBitmapText(renderer, "B TO GO BACK AND RECONNECT",
                                  static_cast<float>(kWindowHeight) - 60.0f, 2, SDL_Color{140, 140, 140, 255});
        SDL_RenderPresent(renderer);
    }
}

WizardSimpleResult wizardControllerTest(SDL_Renderer* renderer, SDL_Gamepad*& gamepad) {
    constexpr uint16_t kAllButtonsMask = 0x0FFF;
    uint16_t everSeen = 0;
    uint64_t allSeenSinceUs = 0;

    struct Label {
        uint16_t bit;
        const char* name;
    };
    const Label labels[] = {
        {DSButton_A, "A"},        {DSButton_B, "B"},         {DSButton_X, "X"},      {DSButton_Y, "Y"},
        {DSButton_Up, "UP"},      {DSButton_Down, "DOWN"},   {DSButton_Left, "LEFT"}, {DSButton_Right, "RIGHT"},
        {DSButton_L, "L"},        {DSButton_R, "R"},         {DSButton_Start, "START"}, {DSButton_Select, "SELECT"},
    };

    while (true) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_EVENT_QUIT:
                    return WizardSimpleResult::Exit;
                case SDL_EVENT_GAMEPAD_ADDED:
                    if (!gamepad) gamepad = SDL_OpenGamepad(event.gdevice.which);
                    break;
                case SDL_EVENT_GAMEPAD_REMOVED:
                    if (gamepad && SDL_GetGamepadID(gamepad) == event.gdevice.which) {
                        SDL_CloseGamepad(gamepad);
                        gamepad = nullptr;
                    }
                    break;
                case SDL_EVENT_KEY_DOWN:
                    // Deliberately keyboard-only: gamepad South/East are
                    // themselves under test here (A and B), so treating
                    // either as a menu action would make it impossible to
                    // confirm they report correctly.
                    if (event.key.key == SDLK_ESCAPE) return WizardSimpleResult::Back;
                    break;
                default:
                    break;
            }
        }

        uint16_t current = buildButtonsFromGamepad(gamepad);
        everSeen = static_cast<uint16_t>(everSeen | current);

        uint64_t nowUs = SDL_GetTicksNS() / 1000;
        if ((everSeen & kAllButtonsMask) == kAllButtonsMask) {
            if (allSeenSinceUs == 0) allSeenSinceUs = nowUs;
            if (nowUs - allSeenSinceUs >= 1'000'000) return WizardSimpleResult::Passed;
        } else {
            allSeenSinceUs = 0;
        }

        SDL_SetRenderDrawColor(renderer, 20, 20, 24, 255);
        SDL_RenderClear(renderer);
        renderCenteredBitmapText(renderer, "CONTROLLER TEST", 60.0f, 4, SDL_Color{220, 220, 220, 255});
        renderCenteredBitmapText(renderer, "PRESS EVERY BUTTON AND DIRECTION", 110.0f, 2,
                                  SDL_Color{200, 200, 200, 255});

        constexpr int kCols = 4;
        constexpr float kColWidth = 220.0f;
        constexpr float kRowHeight = 60.0f;
        float gridWidth = static_cast<float>(kCols) * kColWidth;
        float startX = (static_cast<float>(kWindowWidth) - gridWidth) / 2.0f;
        float startY = 220.0f;
        for (size_t i = 0; i < std::size(labels); ++i) {
            int col = static_cast<int>(i) % kCols;
            int row = static_cast<int>(i) / kCols;
            float x = startX + static_cast<float>(col) * kColWidth;
            float y = startY + static_cast<float>(row) * kRowHeight;
            bool seen = (everSeen & labels[i].bit) != 0;
            SDL_Color color = seen ? SDL_Color{90, 200, 120, 255} : SDL_Color{90, 90, 96, 255};
            renderBitmapText(renderer, labels[i].name, x, y, 3, color);
        }

        renderCenteredBitmapText(renderer, "PRESS ALL 12 BUTTONS TO CONTINUE - ESCAPE TO GO BACK",
                                  static_cast<float>(kWindowHeight) - 60.0f, 2, SDL_Color{140, 140, 140, 255});
        SDL_RenderPresent(renderer);
    }
}

WizardSimpleResult wizardTouchTest(SDL_Renderer* renderer, SDL_Gamepad*& gamepad) {
    struct Target {
        double fx;
        double fy;
        bool hit;
    };
    Target targets[] = {
        {0.15, 0.15, false},
        {0.85, 0.15, false},
        {0.15, 0.85, false},
        {0.85, 0.85, false},
    };
    constexpr double kHitRadius = 40.0;
    uint64_t allHitSinceUs = 0;
    // Left-button drag state for mouse-click touch (GitHub issue #23) --
    // see the identical pattern's comment in main()'s connected loop.
    bool mouseDown = false;

    while (true) {
        RenderRect dsRect = computeAspectFitRect(kWindowWidth, kWindowHeight);

        auto checkHit = [&](double px, double py) {
            if (!mapPointToDSCoords(px, py, dsRect)) return;
            for (auto& target : targets) {
                double tx = dsRect.x + target.fx * dsRect.width;
                double ty = dsRect.y + target.fy * dsRect.height;
                if (std::hypot(px - tx, py - ty) <= kHitRadius) {
                    target.hit = true;
                }
            }
        };

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_EVENT_QUIT:
                    return WizardSimpleResult::Exit;
                case SDL_EVENT_GAMEPAD_ADDED:
                    if (!gamepad) gamepad = SDL_OpenGamepad(event.gdevice.which);
                    break;
                case SDL_EVENT_GAMEPAD_REMOVED:
                    if (gamepad && SDL_GetGamepadID(gamepad) == event.gdevice.which) {
                        SDL_CloseGamepad(gamepad);
                        gamepad = nullptr;
                    }
                    break;
                case SDL_EVENT_KEY_DOWN:
                    if (event.key.key == SDLK_ESCAPE) return WizardSimpleResult::Back;
                    break;
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                    if (event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) return WizardSimpleResult::Back;
                    break;
                case SDL_EVENT_FINGER_DOWN:
                case SDL_EVENT_FINGER_MOTION:
                    checkHit(static_cast<double>(event.tfinger.x) * kWindowWidth,
                             static_cast<double>(event.tfinger.y) * kWindowHeight);
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    if (event.button.which == SDL_TOUCH_MOUSEID || event.button.button != SDL_BUTTON_LEFT) {
                        break;
                    }
                    mouseDown = true;
                    checkHit(event.button.x, event.button.y);
                    break;
                case SDL_EVENT_MOUSE_MOTION:
                    if (!mouseDown || event.motion.which == SDL_TOUCH_MOUSEID) break;
                    checkHit(event.motion.x, event.motion.y);
                    break;
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    if (event.button.button == SDL_BUTTON_LEFT) mouseDown = false;
                    break;
                default:
                    break;
            }
        }

        bool allHit = true;
        for (const auto& target : targets) allHit = allHit && target.hit;

        uint64_t nowUs = SDL_GetTicksNS() / 1000;
        if (allHit) {
            if (allHitSinceUs == 0) allHitSinceUs = nowUs;
            if (nowUs - allHitSinceUs >= 1'000'000) return WizardSimpleResult::Passed;
        } else {
            allHitSinceUs = 0;
        }

        SDL_SetRenderDrawColor(renderer, 20, 20, 24, 255);
        SDL_RenderClear(renderer);
        renderCenteredBitmapText(renderer, "TOUCH TEST", 60.0f, 4, SDL_Color{220, 220, 220, 255});
        renderCenteredBitmapText(renderer, "TOUCH EACH OF THE FOUR CORNERS", 110.0f, 2,
                                  SDL_Color{200, 200, 200, 255});

        SDL_SetRenderDrawColor(renderer, 60, 60, 68, 255);
        SDL_FRect dsOutline{static_cast<float>(dsRect.x), static_cast<float>(dsRect.y),
                             static_cast<float>(dsRect.width), static_cast<float>(dsRect.height)};
        SDL_RenderRect(renderer, &dsOutline);

        for (const auto& target : targets) {
            float tx = static_cast<float>(dsRect.x + target.fx * dsRect.width);
            float ty = static_cast<float>(dsRect.y + target.fy * dsRect.height);
            SDL_Color color = target.hit ? SDL_Color{90, 200, 120, 255} : SDL_Color{220, 80, 80, 255};
            SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
            SDL_FRect box{tx - 16.0f, ty - 16.0f, 32.0f, 32.0f};
            SDL_RenderFillRect(renderer, &box);
        }

        renderCenteredBitmapText(renderer, "ESCAPE OR B TO GO BACK", static_cast<float>(kWindowHeight) - 60.0f,
                                  2, SDL_Color{140, 140, 140, 255});
        SDL_RenderPresent(renderer);
    }
}

// Orchestrates the whole wizard as an explicit step state machine. Returns
// true if the user reached the end (Done), false if they exited entirely
// (window close, or Exit/B from the very first screen) -- callers decide
// separately whether "false" means quit the whole app (first automatic
// run) or just fall through to the normal discovery screen (re-invoked
// from the pause menu).
//
// discoverAndSelectHost()'s std::nullopt already means "exit the whole
// run" everywhere else it's used (it conflates SDL_EVENT_QUIT with its own
// internal EXIT menu item), so the FindHost step below treats it the same
// way here for consistency, even though within the wizard's own step
// functions "cancel" more often means "go back" instead.
bool runSetupWizard(SDL_Window* window, SDL_Renderer* renderer, SDL_Texture* texture, SDL_Gamepad*& gamepad,
                    uint16_t discoveryPort, NetClientConfig baseNetConfig,
                    const std::string& discoveryStorePath) {
    // Order deliberately puts ControllerTest/TouchTest (both purely
    // local -- no NetClient involved at all, see their signatures)
    // before any network step, so a user can check their gamepad/touch
    // mapping without needing a host reachable, approved, and running a
    // ROM yet. Only Connect/VideoTest, right at the end, actually need
    // a live connection. Previously Connect+VideoTest came first, which
    // meant VideoTest's "NO VIDEO YET" state (correct on its own terms
    // -- it really does need a running ROM) blocked reaching the
    // controller test at all if the user just wanted to confirm their
    // gamepad worked before bothering to set up the host side.
    enum class Step { Welcome, ControllerTest, TouchTest, ChooseMethod, ManualEntry, FindHost, Connect,
                       VideoTest, Done };
    Step step = Step::Welcome;
    WizardConnectionMethod method = WizardConnectionMethod::Auto;
    NetClientConfig netConfig = baseNetConfig;
    std::unique_ptr<NetClient> net;

    while (true) {
        switch (step) {
            case Step::Welcome: {
                auto r = wizardWelcome(renderer, gamepad);
                if (r == WizardStepResult::Exit) return false;
                step = Step::ControllerTest;
                break;
            }
            case Step::ControllerTest: {
                auto r = wizardControllerTest(renderer, gamepad);
                if (r == WizardSimpleResult::Exit) return false;
                if (r == WizardSimpleResult::Back) {
                    step = Step::Welcome;
                    break;
                }
                step = Step::TouchTest;
                break;
            }
            case Step::TouchTest: {
                auto r = wizardTouchTest(renderer, gamepad);
                if (r == WizardSimpleResult::Exit) return false;
                if (r == WizardSimpleResult::Back) {
                    step = Step::ControllerTest;
                    break;
                }
                step = Step::ChooseMethod;
                break;
            }
            case Step::ChooseMethod: {
                auto r = wizardChooseMethod(renderer, gamepad, method);
                if (r == WizardStepResult::Exit) return false;
                if (r == WizardStepResult::Back) {
                    step = Step::TouchTest;
                    break;
                }
                step = method == WizardConnectionMethod::Auto ? Step::FindHost : Step::ManualEntry;
                break;
            }
            case Step::ManualEntry: {
                auto address = wizardManualEntry(renderer, window, gamepad);
                if (!address) {
                    step = Step::ChooseMethod;
                    break;
                }
                netConfig.hostAddress = *address;
                netConfig.controlPort = baseNetConfig.controlPort;
                netConfig.inputPort = baseNetConfig.inputPort;
                netConfig.videoPort = baseNetConfig.videoPort;
                step = Step::Connect;
                break;
            }
            case Step::FindHost: {
                auto selected = discoverAndSelectHost(renderer, gamepad, discoveryPort, "");
                if (!selected) return false;
                netConfig.hostAddress = selected->address;
                netConfig.controlPort = selected->controlPort;
                netConfig.inputPort = selected->inputPort;
                netConfig.videoPort = selected->videoPort;
                saveLastHost(discoveryStorePath, netConfig.hostAddress);
                step = Step::Connect;
                break;
            }
            case Step::Connect: {
                net = std::make_unique<NetClient>(netConfig);
                auto r = wizardConnectAndApprove(renderer, gamepad, *net, netConfig.hostAddress);
                if (r == WizardConnectResult::Exit) return false;
                if (r == WizardConnectResult::Back) {
                    net.reset();
                    step = method == WizardConnectionMethod::Auto ? Step::FindHost : Step::ManualEntry;
                    break;
                }
                step = Step::VideoTest;
                break;
            }
            case Step::VideoTest: {
                auto r = wizardVideoTest(renderer, texture, gamepad, *net);
                if (r == WizardVideoResult::Exit) return false;
                if (r == WizardVideoResult::Reconnect) {
                    net->disconnect();
                    net.reset();
                    step = method == WizardConnectionMethod::Auto ? Step::FindHost : Step::ManualEntry;
                    break;
                }
                step = Step::Done;
                break;
            }
            case Step::Done:
                if (net) net->disconnect();
                return true;
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    NetClientConfig netConfig;
    // Set from DUALDECK_VERSION (exported by run-client.sh, read from
    // the archive's VERSION file) rather than baked in at compile time
    // -- see net_server.h's NetServerConfig::appVersion and
    // protocol.h's HelloPayload::appVersion for what this is compared
    // against and why. This is this client binary's own env var, not
    // the one the patched melonDS/Azahar reads for the same purpose on
    // the host side (MELONDS_REMOTE_VERSION/AZAHAR_REMOTE_VERSION,
    // deliberately left alone -- see docs/known-limitations.md's
    // rebrand section for why those stay as-is). Empty (unset) disables
    // the version-mismatch check for this connection, e.g. for a
    // from-source dev build run directly, not via run-client.sh.
    if (const char* envVersion = std::getenv("DUALDECK_VERSION")) {
        netConfig.appVersion = envVersion;
    }
    bool authTokenExplicit = false; // --auth-token given: skip device-approval entirely (CI/scripting use)
    bool hostExplicit = false;      // --host/positional given: skip LAN discovery entirely
    uint16_t discoveryPort = kDefaultDiscoveryPort;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto nextArg = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", arg.c_str());
                std::exit(1);
            }
            return argv[++i];
        };

        if (arg == "--host") {
            netConfig.hostAddress = nextArg();
            hostExplicit = true;
        } else if (arg == "--auth-token") {
            netConfig.authToken = nextArg();
            authTokenExplicit = true;
        } else if (arg == "--client-name") {
            netConfig.clientName = nextArg();
        } else if (arg == "--discovery-port") {
            discoveryPort = static_cast<uint16_t>(std::stoi(nextArg()));
        } else if (arg == "--app-version") {
            netConfig.appVersion = nextArg(); // overrides DUALDECK_VERSION above
        } else if (!arg.empty() && arg[0] != '-') {
            // Positional host address, for scripts/run-client.sh's
            // `dualdeck-client 127.0.0.1` convenience form.
            netConfig.hostAddress = arg;
            hostExplicit = true;
        } else {
            std::fprintf(stderr, "unrecognized argument: %s\n", arg.c_str());
            return 1;
        }
    }

    const std::string discoveryStorePath = defaultLastHostStorePath();

    // SDL_INIT_AUDIO is required for MicCapture's SDL_OpenAudioDeviceStream
    // calls (GitHub issue #2) to succeed at all -- without it every open()
    // just fails silently, so the client would connect fine but never
    // actually stream microphone audio no matter what the user picks in
    // Settings.
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("DualDeck", kWindowWidth, kWindowHeight,
                                           SDL_WINDOW_FULLSCREEN);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Every UI/touch-hit-test coordinate in this file is computed against
    // the kWindowWidth/kWindowHeight constants (1280x800, Steam Deck's
    // exact panel resolution) -- SDL_CreateWindow's SDL_WINDOW_FULLSCREEN
    // flag actually fullscreens at the real display's native resolution
    // regardless of the size passed to it (confirmed in SDL3's own
    // SDL_video.h: "fullscreen window at desktop resolution"), so on any
    // other device -- an 1920x1080 ROG Ally, or simply a future display --
    // this file's own 1280x800-based rendering just occupied the top-left
    // 1280x800 pixels of a larger real backbuffer, uncentered and
    // unscaled. SDL_SetRenderLogicalPresentation makes SDL do the
    // scale-and-letterbox itself on every subsequent SDL_Render* call, so
    // none of this file's existing 1280x800 layout math needs to change --
    // it draws into a virtual 1280x800 canvas that SDL maps onto whatever
    // the real window/display size turns out to be. LETTERBOX (not
    // STRETCH) to preserve the UI's own aspect ratio rather than
    // distorting bitmap text; touch coordinates need no corresponding
    // fix since event.tfinger.x/y are already normalized 0..1 fractions
    // of the real window, independent of its actual pixel size.
    if (!SDL_SetRenderLogicalPresentation(renderer, kWindowWidth, kWindowHeight,
                                           SDL_LOGICAL_PRESENTATION_LETTERBOX)) {
        std::fprintf(stderr, "SDL_SetRenderLogicalPresentation failed: %s\n", SDL_GetError());
    }

    // The wire format (docs/protocol.md) and melonDS's own software-renderer
    // output are B,G,R,X bytes in memory -- SDL_PIXELFORMAT_BGRA32 is the
    // constant that actually means that. SDL_PIXELFORMAT_BGRA8888 (no "32")
    // is a *packed*-format name, not a byte-order-in-memory one: on a
    // little-endian machine it names a completely different byte order
    // (equivalent to ARGB8888's byte order) due to how SDL defines packed
    // formats as a bit layout read MSB-to-LSB of a 32-bit int, which is
    // reversed in memory on little-endian -- feeding real B,G,R,X bytes to
    // a texture declared BGRA8888 showed as flatly wrong colors (verified:
    // pure red bytes rendered as black). Do not "fix" this back to
    // BGRA8888 -- see SDL_pixels.h's SDL_PIXELFORMAT_BGRA32 definition.
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_BGRA32,
                                              SDL_TEXTUREACCESS_STREAMING, kDSWidth, kDSHeight);
    if (!texture) {
        std::fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    // Opaque video feed, not a translucent overlay -- SDL3 defaults a
    // texture's blend mode to SDL_BLENDMODE_BLEND whenever its pixel
    // format carries an alpha channel (SDL_PIXELFORMAT_BGRA32 does), so
    // without this, any frame byte whose alpha isn't exactly 0xFF gets
    // alpha-composited against the window's black background instead of
    // drawn as-is. Some Azahar 3D content (confirmed via renderer_vulkan.cpp:
    // ApplySecondLayerOpacity's constant-alpha blend pipeline, used by
    // DrawScreens for every screen blit) can leave a captured frame's alpha
    // channel at less than 0xFF -- invisible on the host's own window
    // (compositors generally treat a normal window surface as opaque
    // regardless of its alpha channel) but very visible here once streamed,
    // matching the "3D content renders too dark" reports. Forcing NONE here
    // makes the alpha byte inert, which is correct for every source (DS and
    // 3DS alike) since this texture only ever holds an already-composited
    // screen capture that was never meant to be translucent.
    if (!SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE)) {
        std::fprintf(stderr, "SDL_SetTextureBlendMode failed: %s\n", SDL_GetError());
        return 1;
    }
    // Starts at DS's native size (matching the texture just created above)
    // and is recreated at the connected host's own reported native
    // dimensions once a real HelloAck arrives -- see the "nowConnected &&
    // !wasConnected" block below. Not every host is DS-sized: AzaharAdapter
    // (3DS) reports 320x240, and a video frame that size used to always be
    // silently dropped since this texture, and every size check against
    // it, stayed hardcoded at kDSWidth/kDSHeight regardless of what the
    // host actually streamed.
    int textureWidth = kDSWidth;
    int textureHeight = kDSHeight;

    SDL_Gamepad* gamepad = nullptr;
    int gamepadCount = 0;
    SDL_JoystickID* gamepadIds = SDL_GetGamepads(&gamepadCount);
    if (gamepadIds && gamepadCount > 0) {
        gamepad = SDL_OpenGamepad(gamepadIds[0]);
        std::fprintf(stderr, "[input] opened gamepad: %s\n", gamepad ? SDL_GetGamepadName(gamepad) : "?");
    }
    if (gamepadIds) SDL_free(gamepadIds);

    // Device-approval authentication (spec section 13): unless the caller
    // gave an explicit static --auth-token, send this client's own
    // persistent device identity -- the same value used with every host,
    // every time. A human at the host approves or denies it once; there
    // is nothing to type or store per-host on the client side (see
    // device_identity.h and docs/protocol.md's "Authentication and
    // device approval" section). Loaded once, outside the reconnect-loop
    // below, since it doesn't depend on which host is chosen.
    if (!authTokenExplicit) {
        netConfig.authToken = loadOrCreateDeviceIdentity(defaultDeviceIdentityStorePath());
    }

    // First-run setup wizard (GitHub issue #19): runs once automatically,
    // then only reachable again from the Settings screen in the pause menu
    // below (see wizard_state.h). Skipped entirely for an explicit --host/
    // positional address, same reasoning as "CHANGE HOST" being hidden in
    // that case -- an explicit host address means scripted/CI use, not an
    // interactive first-time user.
    const std::string wizardStatePath = defaultWizardStatePath();
    bool runWizardNow = !hostExplicit && !isSetupComplete(wizardStatePath);
    const std::string clientSettingsPath = defaultClientSettingsPath();
    ClientSettings clientSettings = loadClientSettings(clientSettingsPath);

    // L3+R3 "open menu" chord state -- see kMenuChordHoldUs's
    // declaration above for why a deliberate hold is required.
    // menuChordSinceUs == 0 means "not currently held"; menuChordFired
    // prevents re-triggering on every frame for as long as the hold
    // continues, only resetting once the chord is released.
    uint64_t menuChordSinceUs = 0;
    bool menuChordFired = false;

    // Last known emulated-system/adapter identity (GitHub issue #28),
    // shown in the connected-session menu and on the disconnected/
    // reconnecting overlay. Primed from the discovery selection (so it's
    // available even before the first successful handshake) and kept
    // up to date from net.hostSystemIdentity()/hostAdapterIdentity()
    // once connected; deliberately never cleared on disconnect, so a
    // dropped connection's reconnect/error screen still shows "what was
    // I connected to" rather than going blank (issue #28: "preserve it
    // on reconnect/error screens where practical"). Reset naturally on
    // "Change Host" since a fresh discovery selection overwrites it.
    std::string sessionSystemName;
    std::string sessionAdapterName;
    // Machine-comparable systemId ("nds", "3ds", ...), tracked alongside
    // sessionSystemName/sessionAdapterName for the same reasons (primed
    // from discovery, refreshed from HelloAck) -- used to gate the
    // stick-as-alternate-d-pad convenience below to DS sessions only, see
    // its use site's comment.
    std::string sessionSystemId;
    auto identityLine = [&]() -> std::string {
        if (sessionSystemName.empty() && sessionAdapterName.empty()) return "";
        if (sessionAdapterName.empty()) return sessionSystemName;
        if (sessionSystemName.empty()) return sessionAdapterName;
        // The bitmap font (client/src/bitmap_font.cpp) only has glyphs
        // for space, 0-9, A-Z, '.', '-', ':' -- no multi-byte UTF-8
        // separator like the middle dot in issue #28's own example text,
        // so "-" stands in for it on this screen.
        return sessionSystemName + " - " + sessionAdapterName;
    };

    // Outer loop lets "Change Host" (from the in-app menu below) return to
    // the discovery/selection screen without exiting the whole process --
    // everything from discovery through the connected render loop reruns
    // per host. Only reachable when !hostExplicit (an explicit --host/
    // positional address has nothing to fall back to, so that menu entry
    // is hidden in that case -- see menuItems below).
    bool quitApp = false;
    while (!quitApp) {
        if (runWizardNow) {
            runWizardNow = false;
            bool wizardCompleted =
                runSetupWizard(window, renderer, texture, gamepad, discoveryPort, netConfig, discoveryStorePath);
            if (wizardCompleted) {
                markSetupComplete(wizardStatePath);
            } else {
                std::fprintf(stderr, "[wizard] cancelled during first run -- exiting\n");
                quitApp = true;
                break;
            }
            continue; // re-enter the loop: show the normal discovery screen next, same as any other launch
        }

        // LAN discovery (spec section 8.1): always shown unless --host/a
        // positional address was given, matching the existing scripted/CI
        // use (run-client.sh, --auth-token flows). Per user request, this
        // always runs -- even if only one host answers, or it's the same
        // one as last time -- rather than silently reconnecting, so a
        // different HTPC is always one screen away.
        if (!hostExplicit) {
            std::string lastHost = loadLastHost(discoveryStorePath).value_or("");
            auto selected = discoverAndSelectHost(renderer, gamepad, discoveryPort, lastHost);
            if (!selected) {
                std::fprintf(stderr, "[discovery] cancelled before a host was chosen -- exiting\n");
                quitApp = true;
                break;
            }
            netConfig.hostAddress = selected->address;
            netConfig.controlPort = selected->controlPort;
            netConfig.inputPort = selected->inputPort;
            netConfig.videoPort = selected->videoPort;
            netConfig.audioPort = selected->audioPort;
            // Primed from discovery so the identity line has something to
            // show during "CONNECTING..." -- refreshed from the real
            // HelloAck once connected (see the render loop below), which
            // is authoritative if it ever disagrees with what discovery
            // last reported.
            sessionSystemName = selected->system.systemName;
            sessionAdapterName = selected->adapter.adapterName;
            sessionSystemId = selected->system.systemId;
            // Acknowledge the button press before saving the selection or
            // starting any socket work. The previous synchronous connect
            // left the picker frozen until the host responded, making a
            // successful selection look ignored in Gaming Mode (GitHub
            // issue #21).
            renderConnecting(renderer, netConfig.hostAddress);
            saveLastHost(discoveryStorePath, netConfig.hostAddress);
            std::fprintf(stderr, "[discovery] selected host \"%s\" at %s\n", selected->hostName.c_str(),
                        netConfig.hostAddress.c_str());
        } else {
            // Explicit-host/scripted launches skip the picker but should
            // still expose the same connection state once the window opens.
            renderConnecting(renderer, netConfig.hostAddress);
        }

        // Read fresh every time a NetClient is (re)constructed, not just
        // once at startup, so picking a new VIDEO QUALITY in Settings and
        // then reconnecting (CHANGE HOST, or a dropped connection) takes
        // effect without needing to relaunch -- see settingsMenuItems()'s
        // cycleVideoQuality() below for how clientSettings.videoQuality
        // gets changed.
        netConfig.videoQuality = static_cast<uint8_t>(clientSettings.videoQuality);
        NetClient net(netConfig);

        // Auto-reconnect (spec section 7.2): connect() does several blocking
        // socket calls, so retries run on their own thread rather than
        // stalling the render/input loop below. Backoff caps at 5s so a
        // permanently-unreachable (or not-yet-approved) host doesn't spin the
        // CPU. Unlike the old pairing-code flow, there is no reason to ever
        // pause these retries waiting on client-side user action -- approval
        // happens entirely on the host, so the same reconnect loop that
        // handles a temporarily-down host also naturally handles "not
        // approved yet" (it'll just start succeeding once approved). This
        // thread owns every attempt, including the initial one; previously
        // main() made a synchronous attempt first and then immediately
        // handed the same disconnected client to this thread, causing a
        // duplicate attempt after an initial failure (GitHub issue #21).
        std::atomic<bool> shuttingDown{false};
        std::thread reconnectThread([&]() {
            uint32_t backoffMs = 1000;
            constexpr uint32_t kMaxBackoffMs = 5000;
            while (!shuttingDown.load()) {
                if (!net.isConnected()) {
                    std::fprintf(stderr, "[net] attempting to (re)connect to %s...\n",
                                netConfig.hostAddress.c_str());
                    if (net.connect()) {
                        std::fprintf(stderr, "[net] connected (session %u)\n", net.sessionId());
                        backoffMs = 1000;
                    } else {
                        backoffMs = std::min(backoffMs * 2, kMaxBackoffMs);
                    }
                }
                for (uint32_t waited = 0; waited < backoffMs && !shuttingDown.load(); waited += 100) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        });

        uint32_t sequence = 0;
        // See the identity-refresh block below (GitHub issue #28) for
        // why this exists: detects the disconnected->connected edge
        // rather than re-reading net.host*Identity() every frame.
        bool wasConnected = false;
        // Tracks the same edge for net.hostMode() (GitHub issue #4 Phase
        // E) -- a host can flip modes mid-session (an adapter connecting/
        // disconnecting, or a manual override), each transition carrying
        // a fresh identity via ModeChanged that the block below should
        // pick up the same way it already does for the initial connect.
        HostMode lastHostMode = HostMode::Emulation;
        bool touchActive = false;
        uint16_t touchX = 0;
        uint16_t touchY = 0;
        std::optional<int64_t> activeFingerId;
        // Mouse-click touch (GitHub issue #23): left click/drag maps to a
        // touch the same way a finger does, so a Steam Deck trackpad
        // configured as a mouse (Steam Input's default "Trackpad" binding,
        // or a real mouse in Desktop Mode) works as an alternative to an
        // actual touchscreen. Tracked separately from activeFingerId so
        // releasing one input source doesn't clear a touch still being
        // held by the other -- touchActive is the OR of both below.
        bool mouseTouchDown = false;

        std::vector<uint8_t> frame;
        // Matches texture's current dimensions (textureWidth/textureHeight),
        // whatever they are at this point -- correct as long as every
        // recreation of texture below also resizes this alongside it.
        std::vector<uint8_t> testPattern(static_cast<size_t>(textureWidth) * textureHeight * 4, 0x40);

        const uint64_t inputIntervalUs = 1'000'000 / 120; // spec section 6.3
        uint64_t lastInputSendUs = SDL_GetTicksNS() / 1000;

        // In-app menu (spec request: "no sort of menu to configure settings
        // or exit"): held L3+R3 toggles it. "Change Host" is only
        // offered when discovery is in play at all -- an explicit --host
        // has no host list to go back to.
        std::vector<std::string> menuItems = {"RESUME"};
        if (!hostExplicit) menuItems.push_back("CHANGE HOST");
        menuItems.push_back("SETTINGS");
        // "EXIT EMULATION" ends the game/app running on the HOST (GitHub
        // issue #25) -- distinct from "EXIT" below, which only quits this
        // client. Picking it opens a second-level confirm menu rather than
        // acting immediately, since either choice there is destructive on
        // a machine the user isn't necessarily looking at (per SPEC.md's
        // "Wii U GamePad" model, the host's own screen is showing the top
        // screen only while a client streams, and no one may be at the
        // host to see or undo a mistake) -- see exitEmulationItems below.
        menuItems.push_back("EXIT EMULATION");
        menuItems.push_back("EXIT");
        bool menuActive = false;
        int menuSelectedIndex = 0;
        bool settingsActive = false;
        int settingsSelectedIndex = 0;
        bool settingsSaveFailed = false;

        // Microphone capture (GitHub issue #2). Opened once the host's
        // HelloAck reports micSupported; reopened only when the user
        // picks a different device in Settings. Muting doesn't close it
        // -- see MicCapture::open()'s comment -- so the level meter
        // keeps reflecting real input while muted, distinct from "no
        // signal." Closed on disconnect (below) so a stale device isn't
        // left open across host switches. Declared before
        // settingsMenuItems/cycleMicDevice below since their `[&]`
        // lambdas can only capture names already in scope.
        melonds_remote::client::MicCapture micCapture;
        std::vector<int16_t> micPendingSamples;
        uint32_t micSequence = 0;
        float micLevel = 0.0f;

        // Labels the current ClientSettings::videoQuality value for the
        // settings menu -- a handful of named presets rather than a raw
        // number, matching this menu's cycle-through-fixed-choices style
        // (MICROPHONE:/AUTO UPDATE ON LAUNCH: above) instead of a
        // continuous slider this text UI has no widget for.
        auto videoQualityLabel = [](int quality) -> std::string {
            if (quality == 0) return "AUTO";
            if (quality <= 40) return "LOW (SLOWEST LINKS)";
            if (quality <= 65) return "MEDIUM";
            if (quality <= 85) return "HIGH";
            return "MAXIMUM (LARGEST)";
        };
        auto settingsMenuItems = [&]() {
            std::vector<std::string> items{
                std::string("AUTO UPDATE ON LAUNCH: ") +
                    (clientSettings.autoUpdateOnLaunch ? "ON" : "OFF"),
                std::string("VIDEO QUALITY: ") + videoQualityLabel(clientSettings.videoQuality),
            };
            if (!hostExplicit) items.push_back("RUN SETUP WIZARD");
            if (net.hostMicSupported()) {
                std::string micLabel =
                    clientSettings.micDeviceName.empty() ? "SYSTEM DEFAULT" : clientSettings.micDeviceName;
                items.push_back(std::string("MICROPHONE: ") + micLabel);
                items.push_back(std::string("MIC: ") + (clientSettings.micMuted ? "MUTED" : "ON"));
            }
            items.push_back("BACK");
            return items;
        };
        // Cycles clientSettings.videoQuality through a fixed set of
        // presets (see videoQualityLabel above), wrapping back to AUTO.
        // Only takes effect on the next connection (see netConfig.
        // videoQuality's own comment above, near NetClient's
        // construction) -- there's no packet type for changing an
        // already-connected session's compression quality.
        auto cycleVideoQuality = [&]() {
            static constexpr int kPresets[] = {0, 40, 65, 85, 100};
            constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));
            int currentIndex = 0;
            for (int i = 0; i < kPresetCount; ++i) {
                if (kPresets[i] == clientSettings.videoQuality) {
                    currentIndex = i;
                    break;
                }
            }
            clientSettings.videoQuality = kPresets[(currentIndex + 1) % kPresetCount];
            settingsSaveFailed = !saveClientSettings(clientSettingsPath, clientSettings);
        };
        // Cycles clientSettings.micDeviceName to the next enumerated
        // recording device (wrapping back to "SYSTEM DEFAULT"), saves,
        // and reopens capture on the new device. Shared by the keyboard
        // and gamepad settings handlers below, same as the inline
        // AUTO UPDATE ON LAUNCH toggle they already duplicate.
        auto cycleMicDevice = [&]() {
            auto devices = melonds_remote::client::listMicDevices();
            size_t currentIndex = 0;
            for (size_t i = 0; i < devices.size(); ++i) {
                if (devices[i].name == clientSettings.micDeviceName) {
                    currentIndex = i;
                    break;
                }
            }
            size_t nextIndex = (currentIndex + 1) % devices.size();
            // "SYSTEM DEFAULT" is stored as an empty name (see
            // ClientSettings::micDeviceName's comment), never the literal
            // label -- so a later SDL enumeration change can't strand a
            // saved setting that no longer matches anything.
            clientSettings.micDeviceName =
                devices[nextIndex].id == SDL_AUDIO_DEVICE_DEFAULT_RECORDING ? "" : devices[nextIndex].name;
            settingsSaveFailed = !saveClientSettings(clientSettingsPath, clientSettings);
            micCapture.open(clientSettings.micDeviceName);
        };
        bool exitEmulationConfirm = false;
        int exitEmulationSelectedIndex = 0;
        // A lambda re-evaluated at every use site (same pattern as
        // settingsMenuItems above), not a fixed const vector -- the
        // middle item names whichever adapter is actually connected
        // (e.g. "EXIT AZAHAR ENTIRELY"), which used to be hardcoded to
        // "EXIT MELONDS ENTIRELY" even when connected to a non-melonDS
        // adapter.
        auto exitEmulationItems = [&]() {
            std::string adapterUpper = sessionAdapterName;
            std::transform(adapterUpper.begin(), adapterUpper.end(), adapterUpper.begin(),
                            [](unsigned char c) { return std::toupper(c); });
            std::string middleItem =
                adapterUpper.empty() ? "EXIT EMULATION ENTIRELY" : "EXIT " + adapterUpper + " ENTIRELY";
            return std::vector<std::string>{"EXIT ROM", middleItem, "CANCEL"};
        };
        // Sent as ControllerState::emulatorActions for a fixed window after
        // confirming a choice above, not just one packet -- input goes over
        // UDP (spec section 6.3), so a single-packet one-shot action risks
        // silently doing nothing if that one packet is lost. Continuous
        // per-frame state (buttons, touch) doesn't have this problem since
        // it's resent every packet regardless; a momentary action like this
        // needs its own resend window. See pendingEmulatorActionMs below
        // for how long.
        uint16_t pendingEmulatorAction = 0;
        uint64_t pendingEmulatorActionUntilUs = 0;
        constexpr uint64_t kPendingEmulatorActionUs = 250'000; // 250ms of ~120Hz packets
        bool changeHostRequested = false;
        bool setupWizardRequested = false;

        bool runningInner = true;
        while (runningInner) {
            // Use the connected host's actual reported aspect ratio, not the
            // DS/3DS-only 4:3 default -- textureWidth/textureHeight are
            // updated to the real HelloAck-reported dimensions per host
            // (e.g. Cemu's 854x480 GamePad surface), so a fixed 4:3 here
            // would letterbox/stretch anything else incorrectly.
            RenderRect dsRect = computeAspectFitRect(
                kWindowWidth, kWindowHeight,
                static_cast<double>(textureWidth) / static_cast<double>(textureHeight));

            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                switch (event.type) {
                    case SDL_EVENT_QUIT:
                        runningInner = false;
                        quitApp = true;
                        break;
                    case SDL_EVENT_GAMEPAD_ADDED:
                        if (!gamepad) {
                            gamepad = SDL_OpenGamepad(event.gdevice.which);
                            std::fprintf(stderr, "[input] gamepad connected\n");
                        }
                        break;
                    case SDL_EVENT_GAMEPAD_REMOVED:
                        if (gamepad && SDL_GetGamepadID(gamepad) == event.gdevice.which) {
                            SDL_CloseGamepad(gamepad);
                            gamepad = nullptr;
                            std::fprintf(stderr, "[input] gamepad disconnected\n");
                        }
                        break;
                    case SDL_EVENT_FINGER_DOWN:
                    case SDL_EVENT_FINGER_MOTION: {
                        if (menuActive || settingsActive) break;
                        double px = static_cast<double>(event.tfinger.x) * kWindowWidth;
                        double py = static_cast<double>(event.tfinger.y) * kWindowHeight;
                        auto mapped = mapPointToDSCoords(px, py, dsRect);
                        if (mapped) {
                            touchActive = true;
                            touchX = mapped->first;
                            touchY = mapped->second;
                            activeFingerId = static_cast<int64_t>(event.tfinger.fingerID);
                        } else if (event.type == SDL_EVENT_FINGER_DOWN) {
                            // touch started outside the DS rectangle: ignored per spec 7.4
                        }
                        break;
                    }
                    case SDL_EVENT_FINGER_UP:
                        if (activeFingerId &&
                            *activeFingerId == static_cast<int64_t>(event.tfinger.fingerID)) {
                            // Only clears touchActive if a mouse-driven touch
                            // isn't also currently in progress -- see
                            // mouseTouchDown's declaration above.
                            touchActive = mouseTouchDown;
                            activeFingerId.reset();
                        }
                        break;
                    case SDL_EVENT_MOUSE_BUTTON_DOWN:
                        // which == SDL_TOUCH_MOUSEID means this button event
                        // was synthesized from a real touch SDL already
                        // delivered as SDL_EVENT_FINGER_DOWN above -- skip it
                        // here to avoid double-handling the same physical
                        // touch as two separate input sources.
                        if (menuActive || settingsActive || event.button.which == SDL_TOUCH_MOUSEID ||
                            event.button.button != SDL_BUTTON_LEFT) {
                            break;
                        }
                        if (auto mapped = mapPointToDSCoords(event.button.x, event.button.y, dsRect)) {
                            touchActive = true;
                            touchX = mapped->first;
                            touchY = mapped->second;
                            mouseTouchDown = true;
                        }
                        // else: click started outside the DS rectangle, same
                        // as an out-of-bounds finger touch above -- ignored.
                        break;
                    case SDL_EVENT_MOUSE_MOTION:
                        // Only a drag (button already down) counts as touch
                        // movement -- plain cursor motion with no button
                        // held isn't a touch, unlike SDL_EVENT_FINGER_MOTION
                        // (a touchscreen has no "hover" state to generate
                        // motion events for in the first place).
                        if (menuActive || settingsActive || !mouseTouchDown ||
                            event.motion.which == SDL_TOUCH_MOUSEID) {
                            break;
                        }
                        if (auto mapped = mapPointToDSCoords(event.motion.x, event.motion.y, dsRect)) {
                            touchX = mapped->first;
                            touchY = mapped->second;
                        }
                        // else: dragged outside the DS rectangle -- keeps the
                        // last in-bounds position, same as finger motion.
                        break;
                    case SDL_EVENT_MOUSE_BUTTON_UP:
                        if (event.button.which == SDL_TOUCH_MOUSEID || event.button.button != SDL_BUTTON_LEFT) {
                            break;
                        }
                        if (mouseTouchDown) {
                            // Only clears touchActive if a finger touch isn't
                            // also currently in progress.
                            touchActive = activeFingerId.has_value();
                            mouseTouchDown = false;
                        }
                        break;
                    case SDL_EVENT_KEY_DOWN: {
                        // Only honored with no gamepad connected (Desktop
                        // Mode/keyboard testing convenience). On real Steam
                        // Deck hardware a gamepad is always present, and
                        // Steam Input's default binding template for a
                        // newly-added non-Steam shortcut synthesizes a
                        // keyboard Escape for individual button presses
                        // (observed: B and Start both opened the menu on
                        // real hardware even though neither is bound to it
                        // alone in the gamepad chord below) -- gating this
                        // on !gamepad means those synthesized keys are
                        // ignored and only the real L3+R3 gamepad
                        // chord can open the menu.
                        if (!gamepad && event.key.key == SDLK_ESCAPE) {
                            if (settingsActive) {
                                settingsActive = false;
                                menuActive = true;
                            } else {
                                menuActive = !menuActive;
                                menuSelectedIndex = 0;
                                exitEmulationConfirm = false;
                            }
                        } else if (settingsActive && event.key.key == SDLK_UP) {
                            int count = static_cast<int>(settingsMenuItems().size());
                            settingsSelectedIndex = (settingsSelectedIndex + count - 1) % count;
                        } else if (settingsActive && event.key.key == SDLK_DOWN) {
                            int count = static_cast<int>(settingsMenuItems().size());
                            settingsSelectedIndex = (settingsSelectedIndex + 1) % count;
                        } else if (settingsActive && event.key.key == SDLK_RETURN) {
                            const std::string picked =
                                settingsMenuItems()[static_cast<size_t>(settingsSelectedIndex)];
                            if (picked.rfind("AUTO UPDATE ON LAUNCH:", 0) == 0) {
                                clientSettings.autoUpdateOnLaunch = !clientSettings.autoUpdateOnLaunch;
                                settingsSaveFailed = !saveClientSettings(clientSettingsPath, clientSettings);
                            } else if (picked.rfind("VIDEO QUALITY:", 0) == 0) {
                                cycleVideoQuality();
                            } else if (picked == "RUN SETUP WIZARD") {
                                setupWizardRequested = true;
                                runningInner = false;
                            } else if (picked.rfind("MICROPHONE:", 0) == 0) {
                                cycleMicDevice();
                            } else if (picked.rfind("MIC:", 0) == 0) {
                                clientSettings.micMuted = !clientSettings.micMuted;
                                settingsSaveFailed = !saveClientSettings(clientSettingsPath, clientSettings);
                            } else if (picked == "BACK") {
                                settingsActive = false;
                                menuActive = true;
                            }
                        } else if (menuActive && exitEmulationConfirm) {
                            int subCount = static_cast<int>(exitEmulationItems().size());
                            if (event.key.key == SDLK_UP) {
                                exitEmulationSelectedIndex = (exitEmulationSelectedIndex + subCount - 1) % subCount;
                            } else if (event.key.key == SDLK_DOWN) {
                                exitEmulationSelectedIndex = (exitEmulationSelectedIndex + 1) % subCount;
                            } else if (event.key.key == SDLK_RETURN) {
                                const std::string picked =
                                    exitEmulationItems()[static_cast<size_t>(exitEmulationSelectedIndex)];
                                if (picked == "CANCEL") {
                                    exitEmulationConfirm = false;
                                } else {
                                    pendingEmulatorAction = (picked == "EXIT ROM")
                                                                 ? EmulatorAction_QuitSession
                                                                 : EmulatorAction_QuitApplication;
                                    pendingEmulatorActionUntilUs =
                                        SDL_GetTicksNS() / 1000 + kPendingEmulatorActionUs;
                                    exitEmulationConfirm = false;
                                    menuActive = false;
                                }
                            }
                        } else if (menuActive && event.key.key == SDLK_UP) {
                            int count = static_cast<int>(menuItems.size());
                            menuSelectedIndex = (menuSelectedIndex + count - 1) % count;
                        } else if (menuActive && event.key.key == SDLK_DOWN) {
                            int count = static_cast<int>(menuItems.size());
                            menuSelectedIndex = (menuSelectedIndex + 1) % count;
                        } else if (menuActive && event.key.key == SDLK_RETURN) {
                            const std::string& picked = menuItems[static_cast<size_t>(menuSelectedIndex)];
                            if (picked == "RESUME") {
                                menuActive = false;
                            } else if (picked == "CHANGE HOST") {
                                changeHostRequested = true;
                                runningInner = false;
                            } else if (picked == "SETTINGS") {
                                menuActive = false;
                                settingsActive = true;
                                settingsSelectedIndex = 0;
                            } else if (picked == "EXIT EMULATION") {
                                exitEmulationConfirm = true;
                                exitEmulationSelectedIndex = 0;
                            } else if (picked == "EXIT") {
                                quitApp = true;
                                runningInner = false;
                            }
                        }
                        break;
                    }
                    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                        if (settingsActive) {
                            int count = static_cast<int>(settingsMenuItems().size());
                            if (event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP) {
                                settingsSelectedIndex = (settingsSelectedIndex + count - 1) % count;
                            } else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
                                settingsSelectedIndex = (settingsSelectedIndex + 1) % count;
                            } else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) {
                                const std::string picked =
                                    settingsMenuItems()[static_cast<size_t>(settingsSelectedIndex)];
                                if (picked.rfind("AUTO UPDATE ON LAUNCH:", 0) == 0) {
                                    clientSettings.autoUpdateOnLaunch = !clientSettings.autoUpdateOnLaunch;
                                    settingsSaveFailed = !saveClientSettings(clientSettingsPath, clientSettings);
                                } else if (picked.rfind("VIDEO QUALITY:", 0) == 0) {
                                    cycleVideoQuality();
                                } else if (picked == "RUN SETUP WIZARD") {
                                    setupWizardRequested = true;
                                    runningInner = false;
                                } else if (picked.rfind("MICROPHONE:", 0) == 0) {
                                    cycleMicDevice();
                                } else if (picked.rfind("MIC:", 0) == 0) {
                                    clientSettings.micMuted = !clientSettings.micMuted;
                                    settingsSaveFailed = !saveClientSettings(clientSettingsPath, clientSettings);
                                } else if (picked == "BACK") {
                                    settingsActive = false;
                                    menuActive = true;
                                }
                            } else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) {
                                settingsActive = false;
                                menuActive = true;
                            }
                        } else if (menuActive && exitEmulationConfirm) {
                            int subCount = static_cast<int>(exitEmulationItems().size());
                            if (event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP) {
                                exitEmulationSelectedIndex = (exitEmulationSelectedIndex + subCount - 1) % subCount;
                            } else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
                                exitEmulationSelectedIndex = (exitEmulationSelectedIndex + 1) % subCount;
                            } else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) {
                                const std::string picked =
                                    exitEmulationItems()[static_cast<size_t>(exitEmulationSelectedIndex)];
                                if (picked == "CANCEL") {
                                    exitEmulationConfirm = false;
                                } else {
                                    pendingEmulatorAction = (picked == "EXIT ROM")
                                                                 ? EmulatorAction_QuitSession
                                                                 : EmulatorAction_QuitApplication;
                                    pendingEmulatorActionUntilUs =
                                        SDL_GetTicksNS() / 1000 + kPendingEmulatorActionUs;
                                    exitEmulationConfirm = false;
                                    menuActive = false;
                                }
                            } else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) {
                                exitEmulationConfirm = false; // back to the main menu, no action taken
                            }
                        } else if (menuActive) {
                            int count = static_cast<int>(menuItems.size());
                            if (event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP) {
                                menuSelectedIndex = (menuSelectedIndex + count - 1) % count;
                            } else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
                                menuSelectedIndex = (menuSelectedIndex + 1) % count;
                            } else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) {
                                const std::string& picked = menuItems[static_cast<size_t>(menuSelectedIndex)];
                                if (picked == "RESUME") {
                                    menuActive = false;
                                } else if (picked == "CHANGE HOST") {
                                    changeHostRequested = true;
                                    runningInner = false;
                                } else if (picked == "SETTINGS") {
                                    menuActive = false;
                                    settingsActive = true;
                                    settingsSelectedIndex = 0;
                                } else if (picked == "EXIT EMULATION") {
                                    exitEmulationConfirm = true;
                                    exitEmulationSelectedIndex = 0;
                                } else if (picked == "EXIT") {
                                    quitApp = true;
                                    runningInner = false;
                                }
                            } else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) {
                                menuActive = false; // back/cancel, no action taken
                            }
                        }
                        break;
                    default:
                        break;
                }
            }

            // Held L3+R3 toggles the menu -- polled once per frame
            // (not event-driven) since it's a simultaneous-hold chord, not
            // a single button press. Must be held continuously for
            // kMenuChordHoldUs before it fires (see menuChordSinceUs's
            // declaration above for why), and won't fire again until both
            // buttons are released and re-pressed.
            bool menuChordHeld = gamepad && SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_STICK) &&
                                 SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
            uint64_t nowForChordUs = SDL_GetTicksNS() / 1000;
            if (menuChordHeld) {
                if (menuChordSinceUs == 0) menuChordSinceUs = nowForChordUs;
                if (!menuChordFired && nowForChordUs - menuChordSinceUs >= kMenuChordHoldUs) {
                    if (settingsActive) {
                        settingsActive = false;
                        menuActive = false;
                    } else {
                        menuActive = !menuActive;
                        menuSelectedIndex = 0;
                        exitEmulationConfirm = false;
                    }
                    menuChordFired = true;
                }
            } else {
                menuChordSinceUs = 0;
                menuChordFired = false;
            }

            // Refresh the identity line from the real HelloAck the moment
            // a (re)connect completes (GitHub issue #28) -- authoritative
            // over whatever discovery guessed beforehand, in the rare
            // case a host's active adapter changed between the discovery
            // scan and this connection. Only on the disconnected->connected
            // edge (or a mode transition, issue #4 Phase E -- a
            // ModeChanged packet carries its own fresh identity, e.g.
            // switching from a real adapter's identity to
            // kHostControlSystemIdentity/kHostControlAdapterIdentity or
            // back), not every frame, since hostSystemIdentity()/
            // hostAdapterIdentity() each take a mutex; sessionSystemName/
            // sessionAdapterName are deliberately left alone otherwise
            // (see their declaration above for why they survive a drop).
            bool nowConnected = net.isConnected();
            HostMode nowHostMode = net.hostMode();
            if (nowConnected && (!wasConnected || nowHostMode != lastHostMode)) {
                SystemIdentity hostSystem = net.hostSystemIdentity();
                AdapterIdentity hostAdapter = net.hostAdapterIdentity();
                if (!hostSystem.systemName.empty()) sessionSystemName = hostSystem.systemName;
                if (!hostAdapter.adapterName.empty()) sessionAdapterName = hostAdapter.adapterName;
                if (!hostSystem.systemId.empty()) sessionSystemId = hostSystem.systemId;
                if (nowConnected && wasConnected) {
                    // A mode transition mid-session, not the initial
                    // connect (which reconnectThread already logs) --
                    // worth its own line for the same "Gaming Mode has no
                    // visible terminal, stdout is what we've got" reason
                    // as every other status line in this loop.
                    std::fprintf(stderr, "[net] host mode changed to %s\n",
                                  nowHostMode == HostMode::HostControl ? "HOST CONTROL" : "EMULATION");
                }

                // Recreate the video texture (and its matching test-pattern
                // filler) at whatever native size this HelloAck actually
                // reported, if it differs from what's currently allocated --
                // e.g. connecting to an Azahar/3DS host (320x240) after a
                // melonDS/DS one (256x192), or vice versa. Same edge as the
                // identity refresh above, for the same reason: this is a
                // real (if occasional) resize, not per-frame work.
                int newWidth = net.hostNativeWidth();
                int newHeight = net.hostNativeHeight();
                if (newWidth != textureWidth || newHeight != textureHeight) {
                    SDL_DestroyTexture(texture);
                    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_BGRA32,
                                                 SDL_TEXTUREACCESS_STREAMING, newWidth, newHeight);
                    if (!texture) {
                        // Extremely unlikely (the original creation at
                        // startup already proved the renderer can make
                        // textures at all) -- fall back to the old
                        // dimensions rather than leaving texture null and
                        // crashing the next SDL_UpdateTexture/RenderTexture
                        // call below.
                        std::fprintf(stderr,
                                      "SDL_CreateTexture failed while resizing for new host "
                                      "dimensions: %s\n",
                                      SDL_GetError());
                        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_BGRA32,
                                                     SDL_TEXTUREACCESS_STREAMING, textureWidth, textureHeight);
                    } else {
                        textureWidth = newWidth;
                        textureHeight = newHeight;
                        testPattern.assign(static_cast<size_t>(textureWidth) * textureHeight * 4, 0x40);
                        std::fprintf(stderr, "[video] texture resized to %dx%d for this host\n", textureWidth,
                                      textureHeight);
                    }
                    // Every fresh texture needs the same opaque blend mode
                    // as the startup one above (see its comment) -- SDL3
                    // resets blend mode to its own per-format default on
                    // each new SDL_CreateTexture call, it isn't inherited
                    // from the destroyed texture.
                    if (texture && !SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE)) {
                        std::fprintf(stderr, "SDL_SetTextureBlendMode failed: %s\n", SDL_GetError());
                    }
                }
            }
            wasConnected = nowConnected;
            lastHostMode = nowHostMode;

            // Microphone capture/send (GitHub issue #2): runs every frame
            // regardless of which screen is showing -- the host keeps the
            // session running while this client-local menu overlay is up,
            // so audio shouldn't pause just because the settings screen
            // does. Opens lazily once the host's HelloAck confirms
            // support; closes immediately on disconnect so a stale device
            // isn't left open across a host switch or timeout, matching
            // issue #2's "stop capture ... when the session disconnects."
            if (net.isConnected() && net.hostMicSupported() && !micCapture.isOpen()) {
                if (!micCapture.open(clientSettings.micDeviceName)) {
                    std::fprintf(stderr, "[mic] failed to open capture device \"%s\": %s\n",
                                clientSettings.micDeviceName.c_str(), SDL_GetError());
                }
            }
            if (!net.isConnected() && micCapture.isOpen()) {
                micCapture.close();
                micPendingSamples.clear();
                micLevel = 0.0f;
            }
            if (micCapture.isOpen()) {
                micLevel = micCapture.pollSamples(micPendingSamples);
                while (micPendingSamples.size() >= kMicAudioSamplesPerPacket) {
                    // Muting still polls/levels above (the meter should
                    // reflect real input while muted, not read as "no
                    // signal") -- only the actual send to the host is
                    // skipped, per issue #2's "muting prevents microphone
                    // samples from reaching melonDS."
                    if (!clientSettings.micMuted) {
                        MicAudioFramePayload micFrame;
                        micFrame.sequence = micSequence++;
                        micFrame.clientTimestampUs = wallClockNowUs();
                        micFrame.numSamples = kMicAudioSamplesPerPacket;
                        micFrame.samples.assign(micPendingSamples.begin(),
                                                 micPendingSamples.begin() + kMicAudioSamplesPerPacket);
                        net.sendMicAudioFrame(micFrame);
                    }
                    micPendingSamples.erase(micPendingSamples.begin(),
                                             micPendingSamples.begin() + kMicAudioSamplesPerPacket);
                }
            }

            if (settingsActive) {
                renderPauseMenu(renderer, settingsMenuItems(), settingsSelectedIndex, "SETTINGS",
                                settingsSaveFailed ? "COULD NOT SAVE SETTINGS" : "",
                                net.hostMicSupported() ? micLevel : -1.0f);
                continue;
            }

            if (menuActive) {
                if (exitEmulationConfirm) {
                    renderPauseMenu(renderer, exitEmulationItems(), exitEmulationSelectedIndex, "EXIT EMULATION");
                } else {
                    renderPauseMenu(renderer, menuItems, menuSelectedIndex, "MENU", "", -1.0f, identityLine());
                }
                continue;
            }

            uint64_t nowUs = SDL_GetTicksNS() / 1000;
            if (nowUs - lastInputSendUs >= inputIntervalUs) {
                ControllerState state;
                state.sequence = sequence++;
                state.clientTimestampUs = wallClockNowUs();
                // Stick-as-alternate-D-pad (spec 7.3) only makes sense for
                // a system with no analog stick of its own -- DS is the
                // only one of those. Every other system's stick is sent
                // as real analog data below instead (never both at once
                // for one physical motion -- see buildButtonsFromGamepad's
                // comment); an explicit allow-list rather than excluding
                // "3ds" specifically, so a future system (e.g. Wii U,
                // whose GamePad has two real analog sticks already)
                // doesn't inherit DS's convenience by accident.
                state.dsButtons = buildButtonsFromGamepad(gamepad, sessionSystemId == "nds");

                // Real analog stick data (protocol.h's leftStickX/Y,
                // rightStickX/Y) -- always sent regardless of session
                // system; host/adapter_bridge.cpp forwards it straight
                // into GenericInputState for whichever adapter is
                // connected, and an adapter with no analog input (DS)
                // simply never reads it. SDL's gamepad Y axis is positive
                // = down; negated here to match the positive = up
                // convention src/core/hle/service/hid/hid.cpp's
                // GetStickDirectionState() uses for the 3DS circle pad
                // (confirmed by reading it, not assumed) -- X needs no
                // flip since positive = right agrees on both sides.
                if (gamepad) {
                    state.leftStickX = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
                    state.leftStickY = negateStickAxis(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY));
                    state.rightStickX = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
                    state.rightStickY = negateStickAxis(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY));
                }
                // See pendingEmulatorAction's declaration above for why this
                // resends for a window instead of just the one packet that
                // set it.
                if (pendingEmulatorActionUntilUs != 0 && nowUs < pendingEmulatorActionUntilUs) {
                    state.emulatorActions = pendingEmulatorAction;
                } else {
                    state.emulatorActions = 0;
                    pendingEmulatorAction = 0;
                    pendingEmulatorActionUntilUs = 0;
                }
                state.touchActive = touchActive ? 1 : 0;
                state.touchX = touchX;
                state.touchY = touchY;
                net.sendControllerState(state);
                lastInputSendUs = nowUs;
            }

            // GitHub issue #4 Phase E: while the host is in HostControl
            // mode there is no video to show (see renderHostControlScreen()'s
            // comment) -- ControllerState was just sent above unconditionally,
            // same as in Emulation mode, so a real host's HostControlAdapter
            // is already receiving input; only the on-screen presentation
            // differs. Falls through to the normal texture path the instant
            // nowHostMode flips back to Emulation (e.g. an adapter connects).
            if (nowConnected && nowHostMode == HostMode::HostControl) {
                renderHostControlScreen(renderer, identityLine());
                continue;
            }

            const uint8_t* pixels = testPattern.data();
            if (net.isConnected() && net.getLatestFrame(frame) &&
                frame.size() == testPattern.size()) {
                pixels = frame.data();
            }
            SDL_UpdateTexture(texture, nullptr, pixels, textureWidth * 4);

            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer);

            SDL_FRect dst{static_cast<float>(dsRect.x), static_cast<float>(dsRect.y),
                          static_cast<float>(dsRect.width), static_cast<float>(dsRect.height)};
            SDL_RenderTexture(renderer, texture, nullptr, &dst);

            // Otherwise a failed/retrying connection just looks like a stuck
            // dark test-pattern screen with no indication anything is even
            // trying -- the actual retry attempts only show up in stdout,
            // which Gaming Mode has no visible terminal for (same reasoning
            // as the discovery screen using the bitmap font). Distinguishing
            // ApprovalRequired/AppVersionMismatch specifically tell the user
            // where to look/what to do -- there's nothing to do for
            // ApprovalRequired but wait (a human needs to approve on the
            // host), while AppVersionMismatch means retrying forever won't
            // help until one side is updated to match the other.
            if (!net.isConnected()) {
                std::string status;
                switch (net.lastRejectReason()) {
                    case HelloRejectReason::ApprovalRequired:
                        status = "WAITING FOR APPROVAL ON HOST " + netConfig.hostAddress + "...";
                        break;
                    case HelloRejectReason::AppVersionMismatch:
                        status = "VERSION MISMATCH WITH " + netConfig.hostAddress + " (HOST IS " +
                                  net.hostAppVersion() + ") - UPDATE TO MATCH";
                        break;
                    case HelloRejectReason::ProtocolVersionMismatch:
                        status = "PROTOCOL VERSION MISMATCH WITH " + netConfig.hostAddress +
                                  " - UPDATE THE APP OR HOST";
                        break;
                    // The host requires a shared secret for this mode (3DS/
                    // host-control) and this client's --auth-token doesn't
                    // match it (or wasn't given one at all) -- retrying
                    // forever won't help, unlike a genuine network hang,
                    // which this would otherwise look identical to (GitHub
                    // issue: "stuck on connecting" turned out to be this).
                    case HelloRejectReason::AuthenticationFailed:
                        status = "AUTHENTICATION FAILED WITH " + netConfig.hostAddress +
                                  " - CHECK YOUR --auth-token";
                        break;
                    case HelloRejectReason::HostBusy:
                        status = "HOST " + netConfig.hostAddress + " IS BUSY - RETRYING...";
                        break;
                    default:
                        status = "CONNECTING TO " + netConfig.hostAddress + "...";
                        break;
                }
                renderCenteredBitmapText(renderer, status, 24.0f, 2, SDL_Color{220, 200, 80, 255});
                // Last known emulated system/adapter (GitHub issue #28),
                // if any is known yet -- lets the user confirm which
                // session they're waiting to get back to while
                // reconnecting, without waiting for a fresh handshake.
                std::string lastIdentity = identityLine();
                if (!lastIdentity.empty()) {
                    renderCenteredBitmapText(renderer, lastIdentity, 44.0f, 2, SDL_Color{150, 170, 210, 255});
                }
                renderCenteredBitmapText(renderer, kMenuComboHint, 64.0f, 2, SDL_Color{140, 140, 140, 255});
            }

            SDL_RenderPresent(renderer);
        }

        shuttingDown = true;
        reconnectThread.join();
        net.disconnect();

        if (changeHostRequested) {
            std::fprintf(stderr, "[menu] changing host -- returning to discovery\n");
        }

        if (setupWizardRequested) {
            std::fprintf(stderr, "[menu] launching setup wizard\n");
            bool wizardCompleted = runSetupWizard(window, renderer, texture, gamepad, discoveryPort, netConfig,
                                                   discoveryStorePath);
            // Unlike the automatic first-run case, a cancelled re-invocation
            // from this menu should not quit the whole app -- just fall
            // through to the normal discovery screen on the next iteration,
            // same as "CHANGE HOST".
            if (wizardCompleted) markSetupComplete(wizardStatePath);
        }
    }

    if (gamepad) SDL_CloseGamepad(gamepad);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
