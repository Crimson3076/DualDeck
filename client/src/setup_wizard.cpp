#include "setup_wizard.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "bitmap_font.h"
#include "client_log.h"
#include "discovery_store.h"
#include "gamepad_input.h"
#include "host_picker.h"
#include "dualdeck/protocol.h"
#include "dualdeck/touch_mapping.h"
#include "screens.h"

namespace dualdeck::client {

namespace {

// First-run setup wizard (GitHub issue #19): a linear sequence of screens
// that walks a new user through connecting to a host and confirming
// video/controller/touch all work, instead of dropping them straight onto
// the discovery screen with no explanation. Runs once automatically (see
// wizard_state.h) and is reachable again afterward from the Settings screen
// in main()'s pause menu.
//
// Deliberately out of scope, documented in docs/history.md
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
                              "AND TEST VIDEO, CONTROLLER AND TOUCH",
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
// screen -- see docs/history.md); this screen works fine with a
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
                             {text.empty() ? "TYPE THE HOST'S IP ADDRESS" : text,
                              "USES DEFAULT PORTS 8760, 8761, 8762",
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
                    // Real user report: this message used to give no way to
                    // tell *which* side needed updating short of comparing
                    // installs by hand -- the host's version is always
                    // available here (NetServer sets HelloAckPayload::
                    // appVersion unconditionally, even on a rejected
                    // handshake -- see net_server.cpp), same as
                    // AppVersionMismatch's message below already shows it.
                    // Pair with the discovery screen's version stamp
                    // (renderClientVersionStamp()) for this client's own
                    // version.
                    status = "PROTOCOL VERSION MISMATCH - HOST IS " + net.hostAppVersion() +
                              " - UPDATE THE APP OR HOST";
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
                case HelloRejectReason::AppVersionMismatchUpdateTriggered:
                    // Real user request, 2026-08-03: the host already
                    // recognized this device as approved and kicked off
                    // its own update in the background (see
                    // NetServerConfig::selfUpdateCommand's comment) --
                    // the client's own reconnect loop just needs to keep
                    // retrying, same as any other transient rejection,
                    // until the host comes back on a matching version.
                    status = "HOST " + net.hostAppVersion() + " IS UPDATING ITSELF - RETRYING...";
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

} // namespace

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
                auto selected = discoverAndSelectHost(renderer, gamepad, discoveryPort, "", baseNetConfig.appVersion);
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

} // namespace dualdeck::client
