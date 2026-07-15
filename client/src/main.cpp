// melonDS Remote -- Steam Deck client (Phase 1 skeleton).
//
// NOTE ON BUILD STATUS: this file has been written and reviewed against
// the SDL3 API but has NOT been build-verified in the development
// sandbox used to write it, because no SDL3 package is installable there
// (see docs/building.md). Build and run this on a real Linux desktop or
// Steam Deck with SDL3 installed, and fix up any API mismatches against
// the SDL3 version actually available before relying on it.
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

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <optional>
#include <thread>
#include <vector>

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

} // namespace

int main(int argc, char** argv) {
    NetClientConfig netConfig;

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
        } else if (arg == "--auth-token") {
            netConfig.authToken = nextArg();
        } else if (arg == "--client-name") {
            netConfig.clientName = nextArg();
        } else if (!arg.empty() && arg[0] != '-') {
            // Positional host address, for scripts/run-client.sh's
            // `melonds-remote-client 127.0.0.1` convenience form.
            netConfig.hostAddress = arg;
        } else {
            std::fprintf(stderr, "unrecognized argument: %s\n", arg.c_str());
            return 1;
        }
    }

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

    NetClient net(netConfig);
    if (net.connect()) {
        std::printf("[net] connected to %s (session %u)\n", netConfig.hostAddress.c_str(),
                     net.sessionId());
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
            if (!net.isConnected()) {
                std::printf("[net] attempting to (re)connect to %s...\n",
                            netConfig.hostAddress.c_str());
                if (net.connect()) {
                    std::printf("[net] reconnected (session %u)\n", net.sessionId());
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

    bool running = true;
    while (running) {
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
                default:
                    break;
            }
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
