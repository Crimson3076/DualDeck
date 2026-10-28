// dualdeck-host-ui: the full-screen DualDeck Host window. Started by
// packaging/host/dualdeck-host.sh, which sends it menus and dialogs over
// stdin and reads the user's choices from stdout (see protocol.h). It
// knows nothing about what the entries do, so the script stays the one
// place the host's actions live.
//
//   dualdeck-host-ui [--fullscreen]
//   dualdeck-host-ui --screenshot out.png [--scale 1.5] < requests
//     Draws the requests' final state into a PNG without opening a
//     window (needs no display; used by CI and for reviewing changes).

#include <SDL3/SDL.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "app.h"
#include "protocol.h"
#include "text.h"

using namespace dualdeck::hostui;

namespace {

void writeReply(const std::string& text) {
    std::fputs(text.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

int runScreenshot(const char* path, float scale) {
    const int w = static_cast<int>(std::lround(kLayoutWidth * scale));
    const int h = static_cast<int>(std::lround(kLayoutHeight * scale));
    SDL_Surface* surface = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_XRGB8888);
    SDL_Renderer* renderer = surface ? SDL_CreateSoftwareRenderer(surface) : nullptr;
    if (!renderer) {
        std::fprintf(stderr, "dualdeck-host-ui: offscreen renderer failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetRenderScale(renderer, scale, scale);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    TextRenderer text;
    if (!text.init()) {
        std::fprintf(stderr, "dualdeck-host-ui: couldn't load the built-in fonts\n");
        return 1;
    }
    text.setScale(renderer, scale);
    App app(text, writeReply);
    RequestParser parser;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (auto request = parser.feed(line)) {
            if (!app.apply(*request)) break;
            // Lay out after each request so presses see real positions.
            app.update(0.0f);
        }
    }
    app.update(0.0f);
    app.render(renderer);
    SDL_RenderPresent(renderer);
    const bool ok = SDL_SavePNG(surface, path);
    if (!ok) std::fprintf(stderr, "dualdeck-host-ui: saving %s failed: %s\n", path, SDL_GetError());
    SDL_DestroyRenderer(renderer);
    SDL_DestroySurface(surface);
    return ok ? 0 : 1;
}

// Requests read on a background thread, handed to the event loop.
struct Inbox {
    std::mutex mutex;
    std::deque<std::string> lines;
    std::atomic<bool> closed{false};
    Uint32 wakeEvent = 0;
};

void readStdin(Inbox* inbox) {
    std::string line;
    while (std::getline(std::cin, line)) {
        {
            std::lock_guard<std::mutex> lock(inbox->mutex);
            inbox->lines.push_back(line);
        }
        SDL_Event ev{};
        ev.type = inbox->wakeEvent;
        SDL_PushEvent(&ev);
    }
    inbox->closed = true;
    SDL_Event ev{};
    ev.type = inbox->wakeEvent;
    SDL_PushEvent(&ev);
}

bool wantsFullscreen(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--fullscreen") == 0) return true;
        if (std::strcmp(argv[i], "--windowed") == 0) return false;
    }
    // Gaming Mode runs everything under gamescope, where a window should
    // fill the screen like a game does.
    const char* desktop = std::getenv("XDG_CURRENT_DESKTOP");
    return std::getenv("GAMESCOPE_WAYLAND_DISPLAY") != nullptr ||
           (desktop && std::strstr(desktop, "gamescope") != nullptr) || std::getenv("SteamGamepadUI") != nullptr;
}

// Held-direction repeat, so holding the D-pad or stick scrolls.
struct Repeater {
    int direction = -1; // index into Press values, -1 = none
    Uint64 nextAt = 0;
};

} // namespace

int main(int argc, char** argv) {
    const char* screenshot = nullptr;
    float scale = 1.0f;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) screenshot = argv[++i];
        else if (std::strcmp(argv[i], "--scale") == 0 && i + 1 < argc) scale = std::strtof(argv[++i], nullptr);
    }
    if (screenshot) {
        if (!SDL_Init(0)) {
            std::fprintf(stderr, "dualdeck-host-ui: SDL_Init failed: %s\n", SDL_GetError());
            return 1;
        }
        const int rc = runScreenshot(screenshot, scale > 0.1f ? scale : 1.0f);
        SDL_Quit();
        return rc;
    }

    SDL_SetAppMetadata("DualDeck Host", nullptr, "io.github.crimson3076.dualdeck.host");
    SDL_SetHint(SDL_HINT_VIDEO_WAYLAND_ALLOW_LIBDECOR, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {
        // The script falls back to its kdialog/terminal menus when this
        // never answers "ready".
        std::fprintf(stderr, "dualdeck-host-ui: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (wantsFullscreen(argc, argv)) flags |= SDL_WINDOW_FULLSCREEN;
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer("DualDeck Host", static_cast<int>(kLayoutWidth), static_cast<int>(kLayoutHeight),
                                     flags, &window, &renderer)) {
        std::fprintf(stderr, "dualdeck-host-ui: couldn't open a window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);
    SDL_SetRenderLogicalPresentation(renderer, static_cast<int>(kLayoutWidth), static_cast<int>(kLayoutHeight),
                                     SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    TextRenderer text;
    if (!text.init()) {
        std::fprintf(stderr, "dualdeck-host-ui: couldn't load the built-in fonts\n");
        return 1;
    }
    App app(text, writeReply);
    RequestParser parser;

    Inbox inbox;
    inbox.wakeEvent = SDL_RegisterEvents(1);
    std::thread reader(readStdin, &inbox);
    reader.detach();

    SDL_Gamepad* gamepad = nullptr;
    // Steam Input turns the Deck's A/B into Return/Escape key presses as
    // well as gamepad buttons; drop a key that echoes a button just pressed.
    Uint64 lastPadAccept = 0, lastPadBack = 0;
    Repeater repeat;
    Uint64 lastTick = SDL_GetTicks();
    bool running = true;

    const auto pressDir = [&](Press p, bool held) {
        app.press(p);
        if (held) {
            repeat.direction = static_cast<int>(p);
            repeat.nextAt = SDL_GetTicks() + 380;
        }
    };

    while (running) {
        SDL_Event ev;
        if (SDL_WaitEventTimeout(&ev, 16)) {
            do {
                switch (ev.type) {
                case SDL_EVENT_QUIT:
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                    app.closeRequested();
                    running = false;
                    break;
                case SDL_EVENT_GAMEPAD_ADDED:
                    if (!gamepad) gamepad = SDL_OpenGamepad(ev.gdevice.which);
                    break;
                case SDL_EVENT_GAMEPAD_REMOVED:
                    if (gamepad && SDL_GetGamepadID(gamepad) == ev.gdevice.which) {
                        SDL_CloseGamepad(gamepad);
                        gamepad = nullptr;
                    }
                    break;
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                    switch (ev.gbutton.button) {
                    case SDL_GAMEPAD_BUTTON_SOUTH:
                        lastPadAccept = SDL_GetTicks();
                        app.press(Press::Accept);
                        break;
                    case SDL_GAMEPAD_BUTTON_EAST:
                        lastPadBack = SDL_GetTicks();
                        app.press(Press::Back);
                        break;
                    case SDL_GAMEPAD_BUTTON_DPAD_UP: pressDir(Press::Up, true); break;
                    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: pressDir(Press::Down, true); break;
                    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: pressDir(Press::Left, true); break;
                    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: pressDir(Press::Right, true); break;
                    default: break;
                    }
                    break;
                case SDL_EVENT_GAMEPAD_BUTTON_UP:
                    if (ev.gbutton.button >= SDL_GAMEPAD_BUTTON_DPAD_UP &&
                        ev.gbutton.button <= SDL_GAMEPAD_BUTTON_DPAD_RIGHT) {
                        repeat.direction = -1;
                    }
                    break;
                case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
                    // Left stick acts like the D-pad.
                    static int stickDir = -1;
                    const float v = static_cast<float>(ev.gaxis.value) / 32767.0f;
                    int dir = stickDir;
                    if (ev.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
                        dir = v < -0.6f ? static_cast<int>(Press::Up)
                              : v > 0.6f ? static_cast<int>(Press::Down)
                              : (stickDir == static_cast<int>(Press::Up) || stickDir == static_cast<int>(Press::Down))
                                  ? -1
                                  : stickDir;
                    } else if (ev.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) {
                        dir = v < -0.6f ? static_cast<int>(Press::Left)
                              : v > 0.6f ? static_cast<int>(Press::Right)
                              : (stickDir == static_cast<int>(Press::Left) || stickDir == static_cast<int>(Press::Right))
                                  ? -1
                                  : stickDir;
                    } else {
                        break;
                    }
                    if (dir != stickDir) {
                        stickDir = dir;
                        if (dir >= 0) {
                            pressDir(static_cast<Press>(dir), true);
                        } else {
                            repeat.direction = -1;
                        }
                    }
                    break;
                }
                case SDL_EVENT_KEY_DOWN: {
                    const Uint64 now = SDL_GetTicks();
                    switch (ev.key.key) {
                    case SDLK_UP: app.press(Press::Up); break;
                    case SDLK_DOWN: app.press(Press::Down); break;
                    case SDLK_LEFT: app.press(Press::Left); break;
                    case SDLK_RIGHT: app.press(Press::Right); break;
                    case SDLK_TAB: app.press((ev.key.mod & SDL_KMOD_SHIFT) ? Press::Up : Press::Down); break;
                    case SDLK_RETURN:
                    case SDLK_KP_ENTER:
                    case SDLK_SPACE:
                        if (!ev.key.repeat && now - lastPadAccept > 250) app.press(Press::Accept);
                        break;
                    case SDLK_ESCAPE:
                    case SDLK_BACKSPACE:
                        if (!ev.key.repeat && now - lastPadBack > 250) app.press(Press::Back);
                        break;
                    case SDLK_F11:
                        SDL_SetWindowFullscreen(window, (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) == 0);
                        break;
                    default: break;
                    }
                    break;
                }
                case SDL_EVENT_MOUSE_MOTION:
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                case SDL_EVENT_MOUSE_WHEEL: {
                    SDL_ConvertEventToRenderCoordinates(renderer, &ev);
                    if (ev.type == SDL_EVENT_MOUSE_MOTION) {
                        app.pointerMoved(ev.motion.x, ev.motion.y);
                    } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                        if (ev.button.button == SDL_BUTTON_LEFT) app.pointerClicked(ev.button.x, ev.button.y);
                        if (ev.button.button == SDL_BUTTON_RIGHT) app.press(Press::Back);
                    } else {
                        app.scroll(ev.wheel.y);
                    }
                    break;
                }
                default:
                    if (ev.type == inbox.wakeEvent) {
                        std::deque<std::string> lines;
                        {
                            std::lock_guard<std::mutex> lock(inbox.mutex);
                            lines.swap(inbox.lines);
                        }
                        for (const std::string& line : lines) {
                            if (auto request = parser.feed(line)) {
                                if (!app.apply(*request)) running = false;
                            }
                        }
                        // The script went away: nothing left to show.
                        if (inbox.closed) running = false;
                    }
                    break;
                }
            } while (running && SDL_PollEvent(&ev));
        }

        const Uint64 now = SDL_GetTicks();
        if (repeat.direction >= 0 && now >= repeat.nextAt) {
            app.press(static_cast<Press>(repeat.direction));
            repeat.nextAt = now + 110;
        }

        int outW = 0, outH = 0;
        SDL_GetRenderOutputSize(renderer, &outW, &outH);
        const float s = std::min(static_cast<float>(outW) / kLayoutWidth, static_cast<float>(outH) / kLayoutHeight);
        text.setScale(renderer, s);
        app.update(static_cast<float>(now - lastTick) / 1000.0f);
        lastTick = now;
        SDL_SetRenderDrawColor(renderer, 14, 19, 27, 255);
        SDL_RenderClear(renderer);
        app.render(renderer);
        SDL_RenderPresent(renderer);
    }

    if (gamepad) SDL_CloseGamepad(gamepad);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    // The reader thread may still be blocked on stdin; exiting the
    // process ends it.
    std::fflush(stdout);
    std::_Exit(0);
}
