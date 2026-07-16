// melonDS Remote -- Steam Deck client.
//
// What this does today:
//  - Opens a 1280x800 window (Steam Deck panel resolution)
//  - On every launch, scans the LAN for available melonds-remote hosts
//    and shows a gamepad/keyboard-navigable selection screen (spec
//    section 8.1's discovery, adapted per user request: always show the
//    picker rather than silently reconnecting to whichever host was used
//    last, so switching to a different HTPC is always one screen away)
//  - Connects to the chosen melonds-remote-server host and displays
//    whatever bottom-screen frames it sends, aspect-correct-fit inside
//    the window
//  - Reads the first connected gamepad and maps it to DS buttons per
//    SPEC.md section 7.3
//  - Reads touchscreen (finger) events, maps them through
//    melonds_remote::computeAspectFitRect / mapPointToDSCoords, and
//    ignores touches outside the rendered DS rectangle
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
#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "bitmap_font.h"
#include "device_identity.h"
#include "discovery_client.h"
#include "discovery_store.h"
#include "melonds_remote/protocol.h"
#include "melonds_remote/touch_mapping.h"
#include "net_client.h"

using namespace melonds_remote;
using namespace melonds_remote::client;

namespace {

constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 800;
constexpr int kDSWidth = 256;
constexpr int kDSHeight = 192;

// Matches host::NetServerConfig::discoveryPort's default (net_server.h).
constexpr uint16_t kDefaultDiscoveryPort = 8763;
// Each discoverHosts() call blocks for this long; while no host has
// answered yet, the discovery screen loops calling it again rather than
// giving up, so this is "how often does the searching screen get a
// chance to notice SDL_EVENT_QUIT", not a total search timeout.
constexpr int kDiscoveryScanMs = 1200;

// Deliberate-hold duration for the Start+Select "open menu" chord, shared
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

// Shown on the discovery and connecting screens (spec request: tell the
// user how to open the menu up front, not only once it's already open --
// the pause menu's own "START+SELECT TO CLOSE" hint doesn't help someone
// who doesn't know to open it in the first place).
constexpr const char* kMenuComboHint = "HOLD START + SELECT TO OPEN THE MENU";

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

uint16_t buildButtonsFromGamepad(SDL_Gamepad* gamepad) {
    if (!gamepad) return 0;

    uint16_t buttons = 0;
    for (const auto& mapping : kButtonMappings) {
        if (SDL_GetGamepadButton(gamepad, mapping.sdlButton)) {
            buttons |= mapping.dsBit;
        }
    }

    // Optional: left stick as an alternate D-pad (spec section 7.3).
    int16_t leftX = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
    int16_t leftY = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY);
    if (leftX > kStickDeadzone) buttons |= DSButton_Right;
    if (leftX < -kStickDeadzone) buttons |= DSButton_Left;
    if (leftY > kStickDeadzone) buttons |= DSButton_Down;
    if (leftY < -kStickDeadzone) buttons |= DSButton_Up;

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
    renderCenteredBitmapText(renderer, "MAKE SURE A MELONDS REMOTE HOST IS RUNNING ON THIS NETWORK",
                              static_cast<float>(kWindowHeight) / 2.0f + 40.0f, 2,
                              SDL_Color{140, 140, 140, 255});
    renderCenteredBitmapText(renderer, kMenuComboHint, static_cast<float>(kWindowHeight) - 80.0f, 2,
                              SDL_Color{140, 140, 140, 255});
    SDL_RenderPresent(renderer);
}

void renderDiscoveryList(SDL_Renderer* renderer, const std::vector<DiscoveredHost>& hosts,
                          int selectedIndex) {
    SDL_SetRenderDrawColor(renderer, 20, 20, 24, 255);
    SDL_RenderClear(renderer);
    renderCenteredBitmapText(renderer, "SELECT A HOST", 60.0f, 4, SDL_Color{220, 220, 220, 255});

    constexpr float kRowHeight = 60.0f;
    constexpr int kPixelSize = 3;
    constexpr float kStartY = 180.0f;

    for (size_t i = 0; i < hosts.size(); ++i) {
        float rowY = kStartY + static_cast<float>(i) * kRowHeight;
        std::string addressAndPort = hosts[i].address + ":" + std::to_string(hosts[i].controlPort);
        std::string label = hosts[i].hostName.empty()
                                 ? addressAndPort
                                 : hosts[i].hostName + "  (" + addressAndPort + ")";
        bool selected = static_cast<int>(i) == selectedIndex;
        SDL_Color color = selected ? SDL_Color{90, 200, 120, 255} : SDL_Color{200, 200, 200, 255};

        if (selected) {
            int width = measureBitmapText(label, kPixelSize);
            float x = (static_cast<float>(kWindowWidth) - static_cast<float>(width)) / 2.0f;
            SDL_FRect highlight{x - 20.0f, rowY - 8.0f, static_cast<float>(width) + 40.0f,
                                 static_cast<float>(kFontGlyphHeight * kPixelSize) + 16.0f};
            SDL_SetRenderDrawColor(renderer, 50, 70, 55, 255);
            SDL_RenderFillRect(renderer, &highlight);
        }
        renderCenteredBitmapText(renderer, label, rowY, kPixelSize, color);
    }

    renderCenteredBitmapText(renderer, "D-PAD TO MOVE, A TO SELECT",
                              static_cast<float>(kWindowHeight) - 100.0f, 2,
                              SDL_Color{140, 140, 140, 255});
    renderCenteredBitmapText(renderer, kMenuComboHint, static_cast<float>(kWindowHeight) - 60.0f, 2,
                              SDL_Color{140, 140, 140, 255});
    SDL_RenderPresent(renderer);
}

void renderPauseMenu(SDL_Renderer* renderer, const std::vector<std::string>& items, int selectedIndex) {
    SDL_SetRenderDrawColor(renderer, 20, 20, 24, 220);
    SDL_RenderClear(renderer);
    renderCenteredBitmapText(renderer, "MENU", 100.0f, 4, SDL_Color{220, 220, 220, 255});

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

    renderCenteredBitmapText(renderer, "D-PAD TO MOVE, A TO SELECT, START+SELECT TO CLOSE",
                              static_cast<float>(kWindowHeight) - 80.0f, 2,
                              SDL_Color{140, 140, 140, 255});
    SDL_RenderPresent(renderer);
}

// Runs on every launch (spec request: "each time the client is booted, it
// should show this screen in the event I want to connect to another
// client"): scans the LAN for melonds-remote hosts and always lets the
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
// Returns std::nullopt if the user closed the window before a host was
// chosen (SDL_EVENT_QUIT), or chose EXIT from the Start+Select menu below;
// main() treats either as "cancel the whole run", not "connect anyway."
std::optional<DiscoveredHost> discoverAndSelectHost(SDL_Renderer* renderer, SDL_Gamepad*& gamepad,
                                                     uint16_t discoveryPort,
                                                     const std::string& lastHostAddress) {
    std::vector<DiscoveredHost> hosts;
    int selectedIndex = 0;

    // Start+Select "open menu" chord, offering an EXIT control -- this
    // screen previously had none at all (GitHub issues #8, #9), despite
    // already showing the "HOLD START + SELECT" hint via kMenuComboHint.
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

        // Held Start+Select toggles the menu -- see kMenuChordHoldUs's
        // declaration for why a deliberate hold is required.
        bool menuChordHeld = gamepad && SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_START) &&
                             SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_BACK);
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
            continue;
        }

        if (hosts.empty()) {
            renderDiscoverySearching(renderer);
        } else {
            renderDiscoveryList(renderer, hosts, selectedIndex);
        }

        std::string selectedAddress = hosts.empty() ? "" : hosts[static_cast<size_t>(selectedIndex)].address;

        std::vector<DiscoveredHost> freshHosts = discoverHosts(discoveryPort, kDiscoveryScanMs);
        std::sort(freshHosts.begin(), freshHosts.end(),
                  [](const DiscoveredHost& a, const DiscoveredHost& b) { return a.address < b.address; });
        hosts = std::move(freshHosts);

        if (hosts.empty()) {
            selectedIndex = 0;
            continue;
        }

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
}

} // namespace

int main(int argc, char** argv) {
    NetClientConfig netConfig;
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
        } else if (!arg.empty() && arg[0] != '-') {
            // Positional host address, for scripts/run-client.sh's
            // `melonds-remote-client 127.0.0.1` convenience form.
            netConfig.hostAddress = arg;
            hostExplicit = true;
        } else {
            std::fprintf(stderr, "unrecognized argument: %s\n", arg.c_str());
            return 1;
        }
    }

    const std::string discoveryStorePath = defaultLastHostStorePath();

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("melonDS Remote", kWindowWidth, kWindowHeight,
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

    // Start+Select "open menu" chord state -- see kMenuChordHoldUs's
    // declaration above for why a deliberate hold is required.
    // menuChordSinceUs == 0 means "not currently held"; menuChordFired
    // prevents re-triggering on every frame for as long as the hold
    // continues, only resetting once the chord is released.
    uint64_t menuChordSinceUs = 0;
    bool menuChordFired = false;

    // Outer loop lets "Change Host" (from the in-app menu below) return to
    // the discovery/selection screen without exiting the whole process --
    // everything from discovery through the connected render loop reruns
    // per host. Only reachable when !hostExplicit (an explicit --host/
    // positional address has nothing to fall back to, so that menu entry
    // is hidden in that case -- see menuItems below).
    bool quitApp = false;
    while (!quitApp) {
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
            saveLastHost(discoveryStorePath, netConfig.hostAddress);
            std::fprintf(stderr, "[discovery] selected host \"%s\" at %s\n", selected->hostName.c_str(),
                        netConfig.hostAddress.c_str());
        }

        NetClient net(netConfig);
        if (net.connect()) {
            std::fprintf(stderr, "[net] connected to %s (session %u)\n", netConfig.hostAddress.c_str(),
                         net.sessionId());
        } else if (net.lastRejectReason() == HelloRejectReason::ApprovalRequired) {
            std::fprintf(stderr, "[net] awaiting approval on the host -- a human there needs to approve "
                        "this device once; will keep retrying automatically\n");
        } else {
            std::fprintf(stderr,
                          "[net] failed to connect to %s -- will keep retrying in the "
                          "background, showing a local test pattern meanwhile\n",
                          netConfig.hostAddress.c_str());
        }

        // Auto-reconnect (spec section 7.2): connect() does several blocking
        // socket calls, so retries run on their own thread rather than
        // stalling the render/input loop below. Backoff caps at 5s so a
        // permanently-unreachable (or not-yet-approved) host doesn't spin the
        // CPU. Unlike the old pairing-code flow, there is no reason to ever
        // pause these retries waiting on client-side user action -- approval
        // happens entirely on the host, so the same reconnect loop that
        // handles a temporarily-down host also naturally handles "not
        // approved yet" (it'll just start succeeding once approved).
        std::atomic<bool> shuttingDown{false};
        std::thread reconnectThread([&]() {
            uint32_t backoffMs = 1000;
            constexpr uint32_t kMaxBackoffMs = 5000;
            while (!shuttingDown.load()) {
                if (!net.isConnected()) {
                    std::fprintf(stderr, "[net] attempting to (re)connect to %s...\n",
                                netConfig.hostAddress.c_str());
                    if (net.connect()) {
                        std::fprintf(stderr, "[net] reconnected (session %u)\n", net.sessionId());
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
        bool touchActive = false;
        uint16_t touchX = 0;
        uint16_t touchY = 0;
        std::optional<int64_t> activeFingerId;

        std::vector<uint8_t> frame;
        std::vector<uint8_t> testPattern(static_cast<size_t>(kDSWidth) * kDSHeight * 4, 0x40);

        const uint64_t inputIntervalUs = 1'000'000 / 120; // spec section 6.3
        uint64_t lastInputSendUs = SDL_GetTicksNS() / 1000;

        // In-app menu (spec request: "no sort of menu to configure settings
        // or exit"): held Start+Select toggles it. "Change Host" is only
        // offered when discovery is in play at all -- an explicit --host
        // has no host list to go back to.
        std::vector<std::string> menuItems = {"RESUME"};
        if (!hostExplicit) menuItems.push_back("CHANGE HOST");
        menuItems.push_back("EXIT");
        bool menuActive = false;
        int menuSelectedIndex = 0;
        bool changeHostRequested = false;

        bool runningInner = true;
        while (runningInner) {
            RenderRect dsRect = computeAspectFitRect(kWindowWidth, kWindowHeight);

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
                        if (menuActive) break;
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
                            touchActive = false;
                            activeFingerId.reset();
                        }
                        break;
                    case SDL_EVENT_KEY_DOWN: {
                        int count = static_cast<int>(menuItems.size());
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
                        // ignored and only the real Start+Select gamepad
                        // chord can open the menu.
                        if (!gamepad && event.key.key == SDLK_ESCAPE) {
                            menuActive = !menuActive;
                            menuSelectedIndex = 0;
                        } else if (menuActive && event.key.key == SDLK_UP) {
                            menuSelectedIndex = (menuSelectedIndex + count - 1) % count;
                        } else if (menuActive && event.key.key == SDLK_DOWN) {
                            menuSelectedIndex = (menuSelectedIndex + 1) % count;
                        } else if (menuActive && event.key.key == SDLK_RETURN) {
                            const std::string& picked = menuItems[static_cast<size_t>(menuSelectedIndex)];
                            if (picked == "RESUME") {
                                menuActive = false;
                            } else if (picked == "CHANGE HOST") {
                                changeHostRequested = true;
                                runningInner = false;
                            } else if (picked == "EXIT") {
                                quitApp = true;
                                runningInner = false;
                            }
                        }
                        break;
                    }
                    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                        if (menuActive) {
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

            // Held Start+Select toggles the menu -- polled once per frame
            // (not event-driven) since it's a simultaneous-hold chord, not
            // a single button press. Must be held continuously for
            // kMenuChordHoldUs before it fires (see menuChordSinceUs's
            // declaration above for why), and won't fire again until both
            // buttons are released and re-pressed.
            bool menuChordHeld = gamepad && SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_START) &&
                                 SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_BACK);
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
                continue;
            }

            uint64_t nowUs = SDL_GetTicksNS() / 1000;
            if (nowUs - lastInputSendUs >= inputIntervalUs) {
                ControllerState state;
                state.sequence = sequence++;
                state.clientTimestampUs = wallClockNowUs();
                state.dsButtons = buildButtonsFromGamepad(gamepad);
                state.emulatorActions = 0;
                state.touchActive = touchActive ? 1 : 0;
                state.touchX = touchX;
                state.touchY = touchY;
                net.sendControllerState(state);
                lastInputSendUs = nowUs;
            }

            const uint8_t* pixels = testPattern.data();
            if (net.isConnected() && net.getLatestFrame(frame) &&
                frame.size() == testPattern.size()) {
                pixels = frame.data();
            }
            SDL_UpdateTexture(texture, nullptr, pixels, kDSWidth * 4);

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
            // ApprovalRequired specifically tells the user where to look --
            // there's nothing to do here, a human needs to approve on the host.
            if (!net.isConnected()) {
                std::string status = net.lastRejectReason() == HelloRejectReason::ApprovalRequired
                                          ? "WAITING FOR APPROVAL ON HOST " + netConfig.hostAddress + "..."
                                          : "CONNECTING TO " + netConfig.hostAddress + "...";
                renderCenteredBitmapText(renderer, status, 24.0f, 2, SDL_Color{220, 200, 80, 255});
                renderCenteredBitmapText(renderer, kMenuComboHint, 54.0f, 2, SDL_Color{140, 140, 140, 255});
            }

            SDL_RenderPresent(renderer);
        }

        shuttingDown = true;
        reconnectThread.join();
        net.disconnect();

        if (changeHostRequested) {
            std::fprintf(stderr, "[menu] changing host -- returning to discovery\n");
        }
    }

    if (gamepad) SDL_CloseGamepad(gamepad);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
