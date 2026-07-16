// melonDS Remote -- Steam Deck client.
//
// What this does today:
//  - Opens a 1280x800 window (Steam Deck panel resolution)
//  - Connects to a melonds-remote-server host and displays whatever
//    bottom-screen frames it sends, aspect-correct-fit inside the window
//  - Reads the first connected gamepad and maps it to DS buttons per
//    SPEC.md section 7.3
//  - Reads touchscreen (finger) events, maps them through
//    melonds_remote::computeAspectFitRect / mapPointToDSCoords, and
//    ignores touches outside the rendered DS rectangle
//  - Sends a full ControllerState packet at a fixed ~120Hz rate
//    regardless of whether anything changed (spec section 6.3)
//  - Logs connection/controller/touch/frame events to stdout as the
//    "debug overlay" for this milestone (an on-screen overlay is future
//    work, see docs/architecture.md "Known gaps")
//  - Implements the pairing-code flow (spec section 13): if the host
//    rejects a handshake with PairingRequired, shows a 6-digit code-entry
//    screen (SDL text input, so Steam's on-screen keyboard drives it in
//    Gaming Mode) instead of blindly retrying, and persists the token the
//    host issues on success so future runs reconnect silently.

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <optional>
#include <thread>
#include <vector>

#include "bitmap_font.h"
#include "discovery_client.h"
#include "discovery_store.h"
#include "melonds_remote/protocol.h"
#include "melonds_remote/touch_mapping.h"
#include "net_client.h"
#include "pairing_store.h"

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

// Renders 6 boxes centered on screen: outlined for not-yet-entered
// digits, filled for entered ones. Deliberately doesn't render the actual
// digit glyphs (no font/text-rendering dependency in this client) --
// SDL_StartTextInput() drives Steam's on-screen keyboard in Gaming Mode,
// which shows the typed characters itself; this is just a progress
// indicator of how many of the 6 digits have been entered so far.
void renderPairingCodeEntry(SDL_Renderer* renderer, size_t digitsEntered) {
    constexpr int kBoxCount = 6;
    constexpr float kBoxSize = 64.0f;
    constexpr float kBoxGap = 16.0f;
    constexpr float kTotalWidth = kBoxCount * kBoxSize + (kBoxCount - 1) * kBoxGap;

    SDL_SetRenderDrawColor(renderer, 20, 20, 24, 255);
    SDL_RenderClear(renderer);

    float startX = (static_cast<float>(kWindowWidth) - kTotalWidth) / 2.0f;
    float y = (static_cast<float>(kWindowHeight) - kBoxSize) / 2.0f;

    for (int i = 0; i < kBoxCount; ++i) {
        SDL_FRect box{startX + static_cast<float>(i) * (kBoxSize + kBoxGap), y, kBoxSize, kBoxSize};
        if (static_cast<size_t>(i) < digitsEntered) {
            SDL_SetRenderDrawColor(renderer, 90, 200, 120, 255);
            SDL_RenderFillRect(renderer, &box);
        } else {
            SDL_SetRenderDrawColor(renderer, 200, 200, 200, 255);
            SDL_RenderRect(renderer, &box);
        }
    }

    SDL_RenderPresent(renderer);
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
        std::string label = hosts[i].hostName.empty()
                                 ? hosts[i].address
                                 : hosts[i].hostName + "  (" + hosts[i].address + ")";
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
                              static_cast<float>(kWindowHeight) - 80.0f, 2,
                              SDL_Color{140, 140, 140, 255});
    SDL_RenderPresent(renderer);
}

// Runs when no --host was given on the command line: scans the LAN for
// melonds-remote hosts and, if more than one answers, lets the user pick
// via gamepad D-pad/South or keyboard arrows/Enter. Auto-selects without
// prompting when exactly one host answers, or when a previously-picked
// host (see discovery_store.h) is among the ones that answered this time
// -- the list is only shown when there's a genuine choice to make (spec
// request: "user selectable in the event there is more than 1 IP in the
// household running a server").
//
// Returns std::nullopt if the user closed the window before a host was
// chosen (SDL_EVENT_QUIT); main() treats that as "cancel the whole run",
// not "connect anyway."
std::optional<DiscoveredHost> discoverAndSelectHost(SDL_Renderer* renderer, SDL_Gamepad*& gamepad,
                                                     uint16_t discoveryPort,
                                                     const std::string& lastHostAddress) {
    std::vector<DiscoveredHost> hosts;
    int selectedIndex = 0;
    bool listActive = false;

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
                case SDL_EVENT_KEY_DOWN:
                    if (listActive && !hosts.empty()) {
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
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                    if (listActive && !hosts.empty()) {
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

        if (!listActive) {
            renderDiscoverySearching(renderer);
            hosts = discoverHosts(discoveryPort, kDiscoveryScanMs);
            if (!hosts.empty()) {
                if (hosts.size() == 1) {
                    return hosts.front();
                }
                if (!lastHostAddress.empty()) {
                    auto it = std::find_if(hosts.begin(), hosts.end(), [&](const DiscoveredHost& h) {
                        return h.address == lastHostAddress;
                    });
                    if (it != hosts.end()) {
                        return *it;
                    }
                }
                listActive = true;
                selectedIndex = 0;
            }
        } else {
            renderDiscoveryList(renderer, hosts, selectedIndex);
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    NetClientConfig netConfig;
    bool authTokenExplicit = false; // --auth-token given: skip pairing entirely (CI/scripting use)
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

    const std::string pairingStorePath = defaultPairingStorePath();
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

    // BGRA8888 matches the wire format documented in docs/protocol.md and
    // melonDS's own software-renderer output, so no per-frame color
    // conversion is needed on the client.
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_BGRA8888,
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
        std::printf("[input] opened gamepad: %s\n", gamepad ? SDL_GetGamepadName(gamepad) : "?");
    }
    if (gamepadIds) SDL_free(gamepadIds);

    // LAN discovery (spec section 8.1): no --host means the user wants us
    // to find one rather than type an IP into a Steam Deck. Skipped
    // entirely when --host/a positional address was given, matching the
    // existing scripted/CI use (run-client.sh, --auth-token flows).
    if (!hostExplicit) {
        std::string lastHost = loadLastHost(discoveryStorePath).value_or("");
        auto selected = discoverAndSelectHost(renderer, gamepad, discoveryPort, lastHost);
        if (!selected) {
            std::printf("[discovery] cancelled before a host was chosen -- exiting\n");
            if (gamepad) SDL_CloseGamepad(gamepad);
            SDL_DestroyTexture(texture);
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 0;
        }
        netConfig.hostAddress = selected->address;
        netConfig.controlPort = selected->controlPort;
        netConfig.inputPort = selected->inputPort;
        netConfig.videoPort = selected->videoPort;
        saveLastHost(discoveryStorePath, netConfig.hostAddress);
        std::printf("[discovery] selected host \"%s\" at %s\n", selected->hostName.c_str(),
                    netConfig.hostAddress.c_str());
    }

    // Pairing mode (spec section 13): unless the caller gave an explicit
    // static --auth-token, use whatever pairing token we've previously
    // been issued for this host, if any -- silent reconnect instead of
    // prompting for a 6-digit code again.
    if (!authTokenExplicit) {
        if (auto stored = loadPairingToken(pairingStorePath, netConfig.hostAddress)) {
            netConfig.authToken = *stored;
            std::printf("[pairing] using previously-paired token for %s\n", netConfig.hostAddress.c_str());
        }
    }

    // Set once a handshake comes back PairingRequired: the reconnect
    // thread below stops retrying (a stale/missing token won't magically
    // start working) and the render loop shows the code-entry screen
    // instead, until the user finishes entering a code.
    std::atomic<bool> awaitingPairingCode{false};

    auto reportPairingRequired = [&]() {
        awaitingPairingCode = true;
        std::printf("[pairing] host requires a pairing code -- look at the host's screen/log "
                    "for a 6-digit code and enter it here\n");
    };

    NetClient net(netConfig);
    if (net.connect()) {
        std::printf("[net] connected to %s (session %u)\n", netConfig.hostAddress.c_str(),
                     net.sessionId());
    } else if (net.lastRejectReason() == HelloRejectReason::PairingRequired) {
        reportPairingRequired();
    } else {
        std::fprintf(stderr,
                      "[net] failed to connect to %s -- will keep retrying in the "
                      "background, showing a local test pattern meanwhile\n",
                      netConfig.hostAddress.c_str());
    }

    // Auto-reconnect (spec section 7.2): connect() does several blocking
    // socket calls, so retries run on their own thread rather than
    // stalling the render/input loop below. Backoff caps at 5s so a
    // permanently-unreachable host doesn't spin the CPU.
    std::atomic<bool> shuttingDown{false};
    std::thread reconnectThread([&]() {
        uint32_t backoffMs = 1000;
        constexpr uint32_t kMaxBackoffMs = 5000;
        while (!shuttingDown.load()) {
            if (!net.isConnected() && !awaitingPairingCode.load()) {
                std::printf("[net] attempting to (re)connect to %s...\n",
                            netConfig.hostAddress.c_str());
                if (net.connect()) {
                    std::printf("[net] reconnected (session %u)\n", net.sessionId());
                    backoffMs = 1000;
                } else if (net.lastRejectReason() == HelloRejectReason::PairingRequired) {
                    reportPairingRequired();
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

    bool pairingUIActive = false;
    std::string enteredCode;

    bool running = true;
    while (running) {
        bool wantPairingUI = awaitingPairingCode.load();
        if (wantPairingUI && !pairingUIActive) {
            SDL_StartTextInput(window); // drives Steam's on-screen keyboard in Gaming Mode
            enteredCode.clear();
            SDL_SetWindowTitle(window, "melonDS Remote -- enter pairing code shown on host");
            pairingUIActive = true;
        } else if (!wantPairingUI && pairingUIActive) {
            SDL_StopTextInput(window);
            SDL_SetWindowTitle(window, "melonDS Remote");
            pairingUIActive = false;
        }

        RenderRect dsRect = computeAspectFitRect(kWindowWidth, kWindowHeight);

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_EVENT_QUIT:
                    running = false;
                    break;
                case SDL_EVENT_GAMEPAD_ADDED:
                    if (!gamepad) {
                        gamepad = SDL_OpenGamepad(event.gdevice.which);
                        std::printf("[input] gamepad connected\n");
                    }
                    break;
                case SDL_EVENT_GAMEPAD_REMOVED:
                    if (gamepad && SDL_GetGamepadID(gamepad) == event.gdevice.which) {
                        SDL_CloseGamepad(gamepad);
                        gamepad = nullptr;
                        std::printf("[input] gamepad disconnected\n");
                    }
                    break;
                case SDL_EVENT_FINGER_DOWN:
                case SDL_EVENT_FINGER_MOTION: {
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
                case SDL_EVENT_TEXT_INPUT:
                    if (pairingUIActive) {
                        for (const char* p = event.text.text; *p; ++p) {
                            if (*p >= '0' && *p <= '9' && enteredCode.size() < 6) {
                                enteredCode.push_back(*p);
                            }
                        }
                    }
                    break;
                case SDL_EVENT_KEY_DOWN:
                    if (pairingUIActive && event.key.down && event.key.key == SDLK_BACKSPACE &&
                        !enteredCode.empty()) {
                        enteredCode.pop_back();
                    }
                    break;
                default:
                    break;
            }
        }

        if (pairingUIActive && enteredCode.size() == 6) {
            std::printf("[pairing] submitting entered code...\n");
            net.setAuthToken(enteredCode);
            if (net.connect()) {
                std::string newToken = net.lastPairingToken();
                if (!newToken.empty()) {
                    savePairingToken(pairingStorePath, netConfig.hostAddress, newToken);
                    std::printf("[pairing] paired successfully -- token saved, future runs won't "
                                "need a code\n");
                }
                awaitingPairingCode = false;
            } else {
                std::fprintf(stderr,
                              "[pairing] code rejected -- double check the code currently shown "
                              "on the host and try again\n");
                enteredCode.clear();
            }
        }

        if (pairingUIActive) {
            renderPairingCodeEntry(renderer, enteredCode.size());
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

        SDL_RenderPresent(renderer);
    }

    shuttingDown = true;
    reconnectThread.join();
    net.disconnect();
    if (gamepad) SDL_CloseGamepad(gamepad);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
