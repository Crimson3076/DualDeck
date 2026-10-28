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
#include "address_entry.h"
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

enum class WizardStepResult { Advance, Back, Skip, Exit };
enum class WizardConnectionMethod { Auto, Manual };
enum class WizardConnectResult { Connected, Back, Exit };
enum class WizardVideoResult { Passed, Reconnect, Exit };
enum class WizardSimpleResult { Passed, Back, Exit };

// Five numbered steps after the welcome screen: controller, touch, how
// to connect (and the host list or address entry it leads to),
// connecting, and video.
constexpr int kWizardStepCount = 5;
// The wizard's screens have nothing that needs more than ~60Hz, and
// without a cap each loop spins a whole CPU core (see host_picker.cpp's
// kPickerFrameIntervalMs).
constexpr int kWizardFrameIntervalMs = 16;
// How long L3+R3 must be held to skip the controller test, where every
// face button is itself under test and can't be used to skip.
constexpr uint64_t kSkipHoldUs = 1'000'000;
// Keyboard input: Enter/Escape/arrow keys only count while no
// gamepad is connected (keyboard testing in Desktop Mode). Steam Input's
// default template for a non-Steam shortcut synthesizes Escape for B and
// Start (see the same gate in main()'s inner loop), which would otherwise
// fire a second Back after the real B press -- e.g. backing out two
// screens -- or end the controller test while B is being tested. Typed
// text and Backspace on the address screen still always work.

std::string stepLabel(int step) {
    return "STEP " + std::to_string(step) + " OF " + std::to_string(kWizardStepCount);
}

// Title, step counter and background shared by every wizard screen.
// Doesn't present: callers draw their body and footer on top.
void beginWizardScreen(SDL_Renderer* renderer, const std::string& title, int step) {
    SDL_SetRenderDrawColor(renderer, 20, 20, 24, 255);
    SDL_RenderClear(renderer);
    renderCenteredBitmapText(renderer, title, 60.0f, 4, SDL_Color{220, 220, 220, 255});
    if (step > 0) {
        renderCenteredBitmapText(renderer, stepLabel(step), 108.0f, 2, SDL_Color{110, 150, 200, 255});
    }
}

void renderWizardMessage(SDL_Renderer* renderer, const std::string& title, int step,
                          const std::vector<std::string>& lines, const std::vector<ButtonHint>& hints) {
    beginWizardScreen(renderer, title, step);
    float y = 220.0f;
    for (const auto& line : lines) {
        renderCenteredBitmapText(renderer, line, y, 2, SDL_Color{200, 200, 200, 255});
        y += 36.0f;
    }
    renderButtonHints(renderer, hints);
    SDL_RenderPresent(renderer);
}

void renderWizardMenu(SDL_Renderer* renderer, const std::string& title, int step,
                       const std::vector<std::string>& items, const std::vector<std::string>& descriptions,
                       int selectedIndex) {
    beginWizardScreen(renderer, title, step);

    constexpr float kRowHeight = 90.0f;
    constexpr int kPixelSize = 3;
    const float startY = static_cast<float>(kWindowHeight) / 2.0f -
                         (static_cast<float>(items.size()) * kRowHeight) / 2.0f;

    for (size_t i = 0; i < items.size(); ++i) {
        const float rowY = startY + static_cast<float>(i) * kRowHeight;
        const bool selected = static_cast<int>(i) == selectedIndex;
        const SDL_Color color = selected ? SDL_Color{90, 200, 120, 255} : SDL_Color{200, 200, 200, 255};
        const std::string& description = i < descriptions.size() ? descriptions[i] : std::string();

        if (selected) {
            const int width = std::max(measureBitmapText(items[i], kPixelSize), measureBitmapText(description, 2));
            const float x = (static_cast<float>(kWindowWidth) - static_cast<float>(width)) / 2.0f;
            SDL_FRect highlight{x - 24.0f, rowY - 10.0f, static_cast<float>(width) + 48.0f, 66.0f};
            SDL_SetRenderDrawColor(renderer, 50, 70, 55, 255);
            SDL_RenderFillRect(renderer, &highlight);
        }
        renderCenteredBitmapText(renderer, items[i], rowY, kPixelSize, color);
        if (!description.empty()) {
            renderCenteredBitmapText(renderer, description, rowY + 32.0f, 2,
                                      selected ? SDL_Color{150, 210, 170, 255} : SDL_Color{130, 130, 135, 255});
        }
    }

    renderButtonHints(renderer, defaultMenuHints());
    SDL_RenderPresent(renderer);
}

WizardStepResult wizardWelcome(SDL_Renderer* renderer, SDL_Gamepad*& gamepad) {
    while (true) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (handleGamepadHotplug(event, gamepad)) continue;
            switch (event.type) {
                case SDL_EVENT_QUIT:
                    return WizardStepResult::Exit;
                case SDL_EVENT_KEY_DOWN:
                    if (gamepad) break; // see "Keyboard input" above
                    if (event.key.key == SDLK_RETURN) return WizardStepResult::Advance;
                    if (event.key.key == SDLK_ESCAPE) return WizardStepResult::Skip;
                    break;
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                    if (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) return WizardStepResult::Advance;
                    if (event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) return WizardStepResult::Skip;
                    break;
                default:
                    break;
            }
        }

        renderWizardMessage(renderer, "WELCOME TO DUALDECK", 0,
                             {"LET'S GET YOU PLAYING. THIS SETUP CHECKS YOUR", "CONTROLS, FINDS YOUR HOST PC AND TESTS",
                              "THE VIDEO STREAM. IT TAKES ABOUT A MINUTE.", "",
                              "YOU CAN RUN IT AGAIN LATER FROM MENU > SETTINGS."},
                             {{"A", "START"}, {"B", "SKIP SETUP"}});
        SDL_Delay(kWizardFrameIntervalMs);
    }
}

WizardStepResult wizardChooseMethod(SDL_Renderer* renderer, SDL_Gamepad*& gamepad,
                                     WizardConnectionMethod& outMethod) {
    const std::vector<std::string> items = {"FIND HOSTS ON MY NETWORK", "ENTER AN IP ADDRESS"};
    const std::vector<std::string> descriptions = {"RECOMMENDED. LISTS EVERY DUALDECK HOST THAT ANSWERS",
                                                   "FOR WHEN THE HOST DOESN'T SHOW UP IN THE LIST"};
    int selectedIndex = outMethod == WizardConnectionMethod::Manual ? 1 : 0;
    MenuStickState stick;

    while (true) {
        MenuAction action = MenuAction::None;
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (handleGamepadHotplug(event, gamepad)) continue;
            switch (event.type) {
                case SDL_EVENT_QUIT:
                    return WizardStepResult::Exit;
                case SDL_EVENT_KEY_DOWN:
                    if (gamepad) break; // see "Keyboard input" above
                    action = event.key.key == SDLK_ESCAPE ? MenuAction::Back : menuActionForKey(event.key.key);
                    break;
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                    action = menuActionForButton(event.gbutton.button);
                    break;
                default:
                    break;
            }
        }
        if (action == MenuAction::None) action = pollMenuStick(gamepad, stick, SDL_GetTicksNS() / 1000);

        const int count = static_cast<int>(items.size());
        if (action == MenuAction::Up) selectedIndex = (selectedIndex + count - 1) % count;
        if (action == MenuAction::Down) selectedIndex = (selectedIndex + 1) % count;
        if (action == MenuAction::Back) return WizardStepResult::Back;
        if (action == MenuAction::Select) {
            outMethod = selectedIndex == 0 ? WizardConnectionMethod::Auto : WizardConnectionMethod::Manual;
            return WizardStepResult::Advance;
        }

        renderWizardMenu(renderer, "HOW SHOULD WE FIND YOUR HOST?", 3, items, descriptions, selectedIndex);
        SDL_Delay(kWizardFrameIntervalMs);
    }
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
            if (handleGamepadHotplug(event, gamepad)) continue;
            switch (event.type) {
                case SDL_EVENT_QUIT:
                    outcome = WizardConnectResult::Exit;
                    done = true;
                    break;
                case SDL_EVENT_KEY_DOWN:
                    if (gamepad) break; // see "Keyboard input" above
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

        // A headline in color plus a line or two on what to do about it.
        std::string headline = "CONNECTING TO " + hostAddress;
        std::vector<std::string> detail = {"WAITING FOR THE HOST TO ANSWER"};
        SDL_Color headlineColor{220, 200, 80, 255};
        if (everAttempted.load()) {
            switch (net.lastRejectReason()) {
                case HelloRejectReason::ApprovalRequired:
                    headline = "APPROVE THIS DEVICE ON THE HOST";
                    detail = {"A POPUP ON THE HOST PC ASKS WHETHER TO ALLOW THIS DEVICE.",
                              "CHOOSE ALLOW THERE. THIS SCREEN CONTINUES BY ITSELF."};
                    headlineColor = SDL_Color{110, 170, 230, 255};
                    break;
                case HelloRejectReason::ProtocolVersionMismatch:
                    // The host's version is always available here
                    // (NetServer sets HelloAckPayload::appVersion even on a
                    // rejected handshake -- see net_server.cpp). Pair with
                    // the discovery screen's version stamp for this
                    // client's own.
                    headline = "VERSION MISMATCH";
                    detail = {"THE HOST IS RUNNING " + net.hostAppVersion() + ".",
                              "UPDATE THIS DECK AND THE HOST TO THE SAME VERSION."};
                    headlineColor = SDL_Color{220, 90, 90, 255};
                    break;
                case HelloRejectReason::AuthenticationFailed:
                    headline = "THE HOST REJECTED THIS DEVICE";
                    detail = {"CHECK THE HOST'S APPROVED DEVICES, THEN TRY AGAIN."};
                    headlineColor = SDL_Color{220, 90, 90, 255};
                    break;
                case HelloRejectReason::HostBusy:
                    headline = "THE HOST IS BUSY";
                    detail = {"ANOTHER DEVICE IS CONNECTED. RETRYING..."};
                    break;
                case HelloRejectReason::AppVersionMismatch:
                    headline = "VERSION MISMATCH";
                    detail = {"THE HOST IS RUNNING " + net.hostAppVersion() + ".",
                              "UPDATE THIS DECK AND THE HOST TO THE SAME VERSION."};
                    headlineColor = SDL_Color{220, 90, 90, 255};
                    break;
                case HelloRejectReason::AppVersionMismatchUpdateTriggered:
                    // Real user request, 2026-08-03: the host already
                    // recognized this device as approved and kicked off
                    // its own update in the background (see
                    // NetServerConfig::selfUpdateCommand's comment) --
                    // the reconnect loop just keeps retrying until it
                    // comes back on a matching version.
                    headline = "THE HOST IS UPDATING ITSELF";
                    detail = {"FROM " + net.hostAppVersion() + ". THIS RECONNECTS WHEN IT'S DONE."};
                    break;
                case HelloRejectReason::None:
                default:
                    headline = "CAN'T REACH " + hostAddress;
                    detail = {"MAKE SURE THE HOST PC IS ON, DUALDECK HOST IS RUNNING,",
                              "AND BOTH ARE ON THE SAME NETWORK. RETRYING..."};
                    break;
            }
        }

        beginWizardScreen(renderer, "CONNECTING", 4);
        renderSpinner(renderer, static_cast<float>(kWindowWidth) / 2.0f, 250.0f);
        renderCenteredBitmapText(renderer, headline, 330.0f, 3, headlineColor);
        float y = 390.0f;
        for (const auto& line : detail) {
            renderCenteredBitmapText(renderer, line, y, 2, SDL_Color{200, 200, 200, 255});
            y += 34.0f;
        }
        renderButtonHints(renderer, {{"B", "BACK"}});
        SDL_RenderPresent(renderer);
        SDL_Delay(kWizardFrameIntervalMs);
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
            if (handleGamepadHotplug(event, gamepad)) continue;
            switch (event.type) {
                case SDL_EVENT_QUIT:
                    return WizardVideoResult::Exit;
                // A finishes either way: a host in Host Control mode, or
                // with no game open yet, has no video to show, and that
                // shouldn't trap someone in setup.
                case SDL_EVENT_KEY_DOWN:
                    if (gamepad) break; // see "Keyboard input" above
                    if (event.key.key == SDLK_RETURN) return WizardVideoResult::Passed;
                    if (event.key.key == SDLK_ESCAPE) return WizardVideoResult::Reconnect;
                    break;
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                    if (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) return WizardVideoResult::Passed;
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

        // Text sits on a dark band so it stays readable over the video.
        SDL_FRect band{0.0f, 0.0f, static_cast<float>(kWindowWidth), 96.0f};
        SDL_SetRenderDrawColor(renderer, 20, 20, 24, 255);
        SDL_RenderFillRect(renderer, &band);
        renderBitmapText(renderer, stepLabel(5), 20.0f, 20.0f, 2, SDL_Color{110, 150, 200, 255});
        if (!everSawFrame) {
            renderCenteredBitmapText(renderer, "CONNECTED. WAITING FOR VIDEO...", 24.0f, 3,
                                      SDL_Color{220, 200, 80, 255});
            renderCenteredBitmapText(renderer, "OPEN A GAME ON THE HOST TO SEE IT HERE, OR FINISH WITHOUT IT", 62.0f,
                                      2, SDL_Color{200, 200, 200, 255});
        } else {
            renderCenteredBitmapText(renderer, "VIDEO IS WORKING", 24.0f, 3, SDL_Color{90, 200, 120, 255});
            renderCenteredBitmapText(renderer, "IF YOU CAN SEE THE GAME, YOU'RE READY", 62.0f, 2,
                                      SDL_Color{200, 200, 200, 255});
        }
        SDL_FRect footer{0.0f, static_cast<float>(kWindowHeight) - 84.0f, static_cast<float>(kWindowWidth), 84.0f};
        SDL_SetRenderDrawColor(renderer, 20, 20, 24, 255);
        SDL_RenderFillRect(renderer, &footer);
        renderButtonHints(renderer, {{"A", everSawFrame ? "FINISH" : "FINISH WITHOUT VIDEO"},
                                     {"B", "RECONNECT"}});
        SDL_RenderPresent(renderer);
        SDL_Delay(kWizardFrameIntervalMs);
    }
}

WizardSimpleResult wizardControllerTest(SDL_Renderer* renderer, SDL_Gamepad*& gamepad) {
    constexpr uint16_t kAllButtonsMask = 0x0FFF;
    uint16_t everSeen = 0;
    uint64_t allSeenSinceUs = 0;
    uint64_t skipHeldSinceUs = 0;

    struct Label {
        uint16_t bit;
        const char* name;
    };
    const Label labels[] = {
        {DSButton_Up, "UP"},   {DSButton_Down, "DOWN"},   {DSButton_Left, "LEFT"},   {DSButton_Right, "RIGHT"},
        {DSButton_A, "A"},     {DSButton_B, "B"},         {DSButton_X, "X"},         {DSButton_Y, "Y"},
        {DSButton_L, "L"},     {DSButton_R, "R"},         {DSButton_Start, "START"}, {DSButton_Select, "SELECT"},
    };

    while (true) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (handleGamepadHotplug(event, gamepad)) continue;
            switch (event.type) {
                case SDL_EVENT_QUIT:
                    return WizardSimpleResult::Exit;
                case SDL_EVENT_KEY_DOWN:
                    if (gamepad) break; // see "Keyboard input" above
                    // Deliberately keyboard-only: every gamepad face
                    // button is itself under test here, so treating one as
                    // a menu action would make it impossible to confirm it
                    // reports correctly. Gamepad users skip with a held
                    // L3+R3 instead (below).
                    if (event.key.key == SDLK_ESCAPE) return WizardSimpleResult::Back;
                    if (event.key.key == SDLK_RETURN) return WizardSimpleResult::Passed;
                    break;
                default:
                    break;
            }
        }

        const uint16_t current = buildButtonsFromGamepad(gamepad);
        everSeen = static_cast<uint16_t>(everSeen | current);

        const uint64_t nowUs = SDL_GetTicksNS() / 1000;
        if ((everSeen & kAllButtonsMask) == kAllButtonsMask) {
            if (allSeenSinceUs == 0) allSeenSinceUs = nowUs;
            if (nowUs - allSeenSinceUs >= 1'000'000) return WizardSimpleResult::Passed;
        } else {
            allSeenSinceUs = 0;
        }

        const bool skipHeld = gamepad && SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_STICK) &&
                              SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
        if (skipHeld) {
            if (skipHeldSinceUs == 0) skipHeldSinceUs = nowUs;
            if (nowUs - skipHeldSinceUs >= kSkipHoldUs) return WizardSimpleResult::Passed;
        } else {
            skipHeldSinceUs = 0;
        }

        beginWizardScreen(renderer, "CONTROLLER TEST", 1);
        if (!gamepad) {
            renderCenteredBitmapText(renderer, "NO CONTROLLER FOUND. CONNECT ONE TO TEST IT.", 150.0f, 2,
                                      SDL_Color{220, 200, 80, 255});
        } else {
            const char* name = SDL_GetGamepadName(gamepad);
            renderCenteredBitmapText(renderer, std::string("PRESS EVERY BUTTON ON YOUR ") + (name ? name : "CONTROLLER"),
                                      150.0f, 2, SDL_Color{200, 200, 200, 255});
        }

        // Each button is a box: dim until it's been pressed once, green
        // after, and brighter while held right now.
        constexpr int kCols = 4;
        constexpr float kBoxWidth = 200.0f;
        constexpr float kBoxHeight = 72.0f;
        constexpr float kGap = 20.0f;
        const float gridWidth = kCols * kBoxWidth + (kCols - 1) * kGap;
        const float startX = (static_cast<float>(kWindowWidth) - gridWidth) / 2.0f;
        constexpr float kStartY = 210.0f;
        for (size_t i = 0; i < std::size(labels); ++i) {
            const int col = static_cast<int>(i) % kCols;
            const int row = static_cast<int>(i) / kCols;
            const float x = startX + static_cast<float>(col) * (kBoxWidth + kGap);
            const float y = kStartY + static_cast<float>(row) * (kBoxHeight + kGap);
            const bool seen = (everSeen & labels[i].bit) != 0;
            const bool held = (current & labels[i].bit) != 0;

            SDL_FRect box{x, y, kBoxWidth, kBoxHeight};
            if (held) {
                SDL_SetRenderDrawColor(renderer, 70, 140, 90, 255);
            } else if (seen) {
                SDL_SetRenderDrawColor(renderer, 40, 70, 50, 255);
            } else {
                SDL_SetRenderDrawColor(renderer, 34, 34, 40, 255);
            }
            SDL_RenderFillRect(renderer, &box);
            SDL_SetRenderDrawColor(renderer, seen ? 90 : 70, seen ? 200 : 70, seen ? 120 : 78, 255);
            SDL_RenderRect(renderer, &box);

            const SDL_Color color = seen ? SDL_Color{150, 230, 170, 255} : SDL_Color{130, 130, 136, 255};
            const float labelWidth = static_cast<float>(measureBitmapText(labels[i].name, 3));
            renderBitmapText(renderer, labels[i].name, x + (kBoxWidth - labelWidth) / 2.0f,
                             y + (kBoxHeight - static_cast<float>(kFontGlyphHeight * 3)) / 2.0f, 3, color);
        }

        int seenCount = 0;
        for (const auto& label : labels) seenCount += (everSeen & label.bit) != 0 ? 1 : 0;
        const std::string progress = seenCount == static_cast<int>(std::size(labels))
                                         ? "ALL BUTTONS WORK!"
                                         : std::to_string(seenCount) + " OF " + std::to_string(std::size(labels)) +
                                               " BUTTONS PRESSED";
        renderCenteredBitmapText(renderer, progress, 500.0f, 2,
                                  seenCount == static_cast<int>(std::size(labels)) ? SDL_Color{90, 200, 120, 255}
                                                                                   : SDL_Color{160, 160, 165, 255});

        if (skipHeldSinceUs != 0) {
            // Fills as L3+R3 is held, so the skip doesn't feel stuck.
            constexpr float kBarWidth = 300.0f;
            const float fraction = std::min(1.0f, static_cast<float>(nowUs - skipHeldSinceUs) /
                                                      static_cast<float>(kSkipHoldUs));
            const float barX = (static_cast<float>(kWindowWidth) - kBarWidth) / 2.0f;
            SDL_FRect bg{barX, 560.0f, kBarWidth, 12.0f};
            SDL_SetRenderDrawColor(renderer, 45, 45, 50, 255);
            SDL_RenderFillRect(renderer, &bg);
            SDL_FRect fill{barX, 560.0f, kBarWidth * fraction, 12.0f};
            SDL_SetRenderDrawColor(renderer, 110, 150, 200, 255);
            SDL_RenderFillRect(renderer, &fill);
        }

        renderButtonHints(renderer, {{"HOLD L3+R3", "SKIP THIS TEST"}});
        SDL_RenderPresent(renderer);
        SDL_Delay(kWizardFrameIntervalMs);
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
            if (handleGamepadHotplug(event, gamepad)) continue;
            switch (event.type) {
                case SDL_EVENT_QUIT:
                    return WizardSimpleResult::Exit;
                case SDL_EVENT_KEY_DOWN:
                    if (gamepad) break; // see "Keyboard input" above
                    if (event.key.key == SDLK_ESCAPE) return WizardSimpleResult::Back;
                    if (event.key.key == SDLK_RETURN) return WizardSimpleResult::Passed;
                    break;
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                    // No buttons are under test here, so A can skip (for
                    // a Deck docked to a screen with no touch) and B
                    // goes back.
                    if (event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) return WizardSimpleResult::Back;
                    if (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) return WizardSimpleResult::Passed;
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
        int hitCount = 0;
        for (const auto& target : targets) {
            allHit = allHit && target.hit;
            hitCount += target.hit ? 1 : 0;
        }

        uint64_t nowUs = SDL_GetTicksNS() / 1000;
        if (allHit) {
            if (allHitSinceUs == 0) allHitSinceUs = nowUs;
            if (nowUs - allHitSinceUs >= 1'000'000) return WizardSimpleResult::Passed;
        } else {
            allHitSinceUs = 0;
        }

        SDL_SetRenderDrawColor(renderer, 20, 20, 24, 255);
        SDL_RenderClear(renderer);

        SDL_SetRenderDrawColor(renderer, 60, 60, 68, 255);
        SDL_FRect dsOutline{static_cast<float>(dsRect.x), static_cast<float>(dsRect.y),
                             static_cast<float>(dsRect.width), static_cast<float>(dsRect.height)};
        SDL_RenderRect(renderer, &dsOutline);

        // Drawn inside the touch area (the DS screen fills the whole
        // window height), between the corner targets.
        renderCenteredBitmapText(renderer, "TOUCH TEST", 300.0f, 4, SDL_Color{220, 220, 220, 255});
        renderCenteredBitmapText(renderer, stepLabel(2), 348.0f, 2, SDL_Color{110, 150, 200, 255});
        renderCenteredBitmapText(renderer, allHit ? "TOUCH WORKS!" : "TAP EACH RED SQUARE", 400.0f, 2,
                                  allHit ? SDL_Color{90, 200, 120, 255} : SDL_Color{200, 200, 200, 255});
        renderCenteredBitmapText(renderer, std::to_string(hitCount) + " OF 4", 430.0f, 2,
                                  SDL_Color{160, 160, 165, 255});

        for (const auto& target : targets) {
            float tx = static_cast<float>(dsRect.x + target.fx * dsRect.width);
            float ty = static_cast<float>(dsRect.y + target.fy * dsRect.height);
            SDL_Color color = target.hit ? SDL_Color{90, 200, 120, 255} : SDL_Color{220, 80, 80, 255};
            SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
            SDL_FRect box{tx - 20.0f, ty - 20.0f, 40.0f, 40.0f};
            SDL_RenderFillRect(renderer, &box);
        }

        renderButtonHints(renderer, {{"A", "SKIP"}, {"B", "BACK"}});
        SDL_RenderPresent(renderer);
        SDL_Delay(kWizardFrameIntervalMs);
    }
}

WizardStepResult wizardDone(SDL_Renderer* renderer, SDL_Gamepad*& gamepad) {
    while (true) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (handleGamepadHotplug(event, gamepad)) continue;
            switch (event.type) {
                case SDL_EVENT_QUIT:
                    return WizardStepResult::Exit;
                case SDL_EVENT_KEY_DOWN:
                    if (gamepad) break; // see "Keyboard input" above
                    if (event.key.key == SDLK_RETURN) return WizardStepResult::Advance;
                    break;
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                    if (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) return WizardStepResult::Advance;
                    break;
                default:
                    break;
            }
        }

        renderWizardMessage(renderer, "YOU'RE ALL SET", 0,
                             {"DUALDECK WILL CONNECT TO THIS HOST NOW.", "",
                              "HOLD BOTH STICKS IN (L3+R3) ANY TIME TO OPEN THE MENU,",
                              "CHANGE HOSTS, ADJUST SETTINGS OR EXIT."},
                             {{"A", "START PLAYING"}});
        SDL_Delay(kWizardFrameIntervalMs);
    }
}

} // namespace

// Orchestrates the whole wizard as an explicit step state machine (see
// setup_wizard.h for what each outcome means).
//
// B on the host picker goes back a step (its backRequested flag); any
// other std::nullopt from it (window closed, EXIT from its L3+R3 menu)
// quits, the same as everywhere else it's used.
WizardOutcome runSetupWizard(SDL_Window* window, SDL_Renderer* renderer, SDL_Texture* texture, SDL_Gamepad*& gamepad,
                    uint16_t discoveryPort, NetClientConfig baseNetConfig,
                    const std::string& discoveryStorePath, NetClientConfig& outNetConfig) {
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
                if (r == WizardStepResult::Exit) return WizardOutcome::Quit;
                if (r == WizardStepResult::Skip) return WizardOutcome::Skipped;
                step = Step::ControllerTest;
                break;
            }
            case Step::ControllerTest: {
                auto r = wizardControllerTest(renderer, gamepad);
                if (r == WizardSimpleResult::Exit) return WizardOutcome::Quit;
                if (r == WizardSimpleResult::Back) {
                    step = Step::Welcome;
                    break;
                }
                step = Step::TouchTest;
                break;
            }
            case Step::TouchTest: {
                auto r = wizardTouchTest(renderer, gamepad);
                if (r == WizardSimpleResult::Exit) return WizardOutcome::Quit;
                if (r == WizardSimpleResult::Back) {
                    step = Step::ControllerTest;
                    break;
                }
                step = Step::ChooseMethod;
                break;
            }
            case Step::ChooseMethod: {
                auto r = wizardChooseMethod(renderer, gamepad, method);
                if (r == WizardStepResult::Exit) return WizardOutcome::Quit;
                if (r == WizardStepResult::Back) {
                    step = Step::TouchTest;
                    break;
                }
                step = method == WizardConnectionMethod::Auto ? Step::FindHost : Step::ManualEntry;
                break;
            }
            case Step::ManualEntry: {
                auto address = runAddressEntry(renderer, window, gamepad,
                                               loadLastHost(discoveryStorePath).value_or(""), stepLabel(3));
                if (!address) {
                    step = Step::ChooseMethod;
                    break;
                }
                netConfig.hostAddress = *address;
                saveLastHost(discoveryStorePath, netConfig.hostAddress);
                netConfig.controlPort = baseNetConfig.controlPort;
                netConfig.inputPort = baseNetConfig.inputPort;
                netConfig.videoPort = baseNetConfig.videoPort;
                netConfig.audioPort = baseNetConfig.audioPort;
                step = Step::Connect;
                break;
            }
            case Step::FindHost: {
                bool back = false;
                auto selected = discoverAndSelectHost(renderer, gamepad, discoveryPort,
                                                      loadLastHost(discoveryStorePath).value_or(""),
                                                      baseNetConfig.appVersion, &back);
                if (back) {
                    step = Step::ChooseMethod;
                    break;
                }
                if (!selected) return WizardOutcome::Quit;
                netConfig.hostAddress = selected->address;
                netConfig.controlPort = selected->controlPort;
                netConfig.inputPort = selected->inputPort;
                netConfig.videoPort = selected->videoPort;
                netConfig.audioPort = selected->audioPort;
                saveLastHost(discoveryStorePath, netConfig.hostAddress);
                step = Step::Connect;
                break;
            }
            case Step::Connect: {
                net = std::make_unique<NetClient>(netConfig);
                auto r = wizardConnectAndApprove(renderer, gamepad, *net, netConfig.hostAddress);
                if (r == WizardConnectResult::Exit) return WizardOutcome::Quit;
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
                if (r == WizardVideoResult::Exit) return WizardOutcome::Quit;
                if (r == WizardVideoResult::Reconnect) {
                    net->disconnect();
                    net.reset();
                    step = method == WizardConnectionMethod::Auto ? Step::FindHost : Step::ManualEntry;
                    break;
                }
                step = Step::Done;
                break;
            }
            case Step::Done: {
                if (net) {
                    net->disconnect();
                    net.reset();
                }
                if (wizardDone(renderer, gamepad) == WizardStepResult::Exit) return WizardOutcome::Quit;
                outNetConfig = netConfig;
                return WizardOutcome::Completed;
            }
        }
    }
}

} // namespace dualdeck::client
