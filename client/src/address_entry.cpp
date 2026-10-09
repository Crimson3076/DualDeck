#include "address_entry.h"

#include "bitmap_font.h"
#include "gamepad_input.h"
#include "screens.h"

namespace dualdeck::client {

namespace {
constexpr int kFrameIntervalMs = 16;
} // namespace

std::optional<std::string> runAddressEntry(SDL_Renderer* renderer, SDL_Window* window, SDL_Gamepad*& gamepad,
                                            const std::string& initialText, const std::string& subtitle) {
    // 4 rows of 3 keys, then a full-width CONNECT row.
    const char* const keys[4][3] = {
        {"1", "2", "3"}, {"4", "5", "6"}, {"7", "8", "9"}, {".", "0", "DEL"}};
    constexpr int kKeyRows = 4;
    constexpr int kKeyCols = 3;
    constexpr int kConnectRow = kKeyRows;
    constexpr size_t kMaxLength = 63;

    std::string text = initialText;
    int row = 0;
    int col = 0;
    MenuStickState stick;
    SDL_StartTextInput(window);

    auto finish = [&](std::optional<std::string> result) {
        SDL_StopTextInput(window);
        return result;
    };
    auto deleteLast = [&]() {
        if (!text.empty()) text.pop_back();
    };
    auto pressKey = [&]() -> bool {
        if (row == kConnectRow) return !text.empty();
        const std::string key = keys[row][col];
        if (key == "DEL") {
            deleteLast();
        } else if (text.size() < kMaxLength) {
            text += key;
        }
        return false;
    };

    while (true) {
        MenuAction action = MenuAction::None;
        bool connect = false;
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (handleGamepadHotplug(event, gamepad)) continue;
            switch (event.type) {
                case SDL_EVENT_QUIT:
                    return finish(std::nullopt);
                case SDL_EVENT_TEXT_INPUT:
                    for (const char* c = event.text.text; *c && text.size() < kMaxLength; ++c) {
                        if (*c != ' ') text += *c;
                    }
                    break;
                case SDL_EVENT_KEY_DOWN:
                    if (event.key.key == SDLK_BACKSPACE) {
                        deleteLast();
                        break;
                    }
                    if (gamepad) break; // see the note in address_entry.h
                    if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) {
                        connect = !text.empty();
                    } else if (event.key.key == SDLK_ESCAPE) {
                        return finish(std::nullopt);
                    }
                    break;
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                    if (event.gbutton.button == SDL_GAMEPAD_BUTTON_WEST) {
                        deleteLast();
                    } else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_START) {
                        connect = !text.empty();
                    } else {
                        action = menuActionForButton(event.gbutton.button);
                    }
                    break;
                default:
                    break;
            }
        }
        if (action == MenuAction::None) action = pollMenuStick(gamepad, stick, SDL_GetTicksNS() / 1000);

        switch (action) {
            case MenuAction::Up: row = (row + kKeyRows) % (kKeyRows + 1); break;
            case MenuAction::Down: row = (row + 1) % (kKeyRows + 1); break;
            case MenuAction::Left: col = (col + kKeyCols - 1) % kKeyCols; break;
            case MenuAction::Right: col = (col + 1) % kKeyCols; break;
            case MenuAction::Select: connect = pressKey(); break;
            case MenuAction::Back: return finish(std::nullopt);
            case MenuAction::None: break;
        }
        if (connect) return finish(text);

        SDL_SetRenderDrawColor(renderer, 20, 20, 24, 255);
        SDL_RenderClear(renderer);
        renderCenteredBitmapText(renderer, "ENTER YOUR HOST'S ADDRESS", 60.0f, 4, SDL_Color{220, 220, 220, 255});
        if (!subtitle.empty()) {
            renderCenteredBitmapText(renderer, subtitle, 108.0f, 2, SDL_Color{110, 150, 200, 255});
        }

        // Text field.
        constexpr float kFieldWidth = 560.0f;
        constexpr float kFieldY = 150.0f;
        const float fieldX = (static_cast<float>(kWindowWidth) - kFieldWidth) / 2.0f;
        SDL_FRect field{fieldX, kFieldY, kFieldWidth, 56.0f};
        SDL_SetRenderDrawColor(renderer, 36, 36, 42, 255);
        SDL_RenderFillRect(renderer, &field);
        SDL_SetRenderDrawColor(renderer, 110, 150, 200, 255);
        SDL_RenderRect(renderer, &field);
        const bool cursorOn = (SDL_GetTicks() / 500) % 2 == 0;
        if (text.empty()) {
            renderCenteredBitmapText(renderer, "E.G. 192.168.1.20", kFieldY + 18.0f, 3, SDL_Color{90, 90, 96, 255});
        } else {
            renderCenteredBitmapText(renderer, text + (cursorOn ? "_" : " "), kFieldY + 18.0f, 3,
                                      SDL_Color{230, 230, 230, 255});
        }

        // Key grid.
        constexpr float kKeyWidth = 140.0f;
        constexpr float kKeyHeight = 58.0f;
        constexpr float kGap = 12.0f;
        constexpr float kGridY = 240.0f;
        const float gridWidth = kKeyCols * kKeyWidth + (kKeyCols - 1) * kGap;
        const float gridX = (static_cast<float>(kWindowWidth) - gridWidth) / 2.0f;
        auto drawKey = [&](float x, float y, float w, const std::string& label, bool selected) {
            SDL_FRect box{x, y, w, kKeyHeight};
            if (selected) {
                SDL_SetRenderDrawColor(renderer, 50, 70, 55, 255);
            } else {
                SDL_SetRenderDrawColor(renderer, 36, 36, 42, 255);
            }
            SDL_RenderFillRect(renderer, &box);
            SDL_SetRenderDrawColor(renderer, selected ? 90 : 70, selected ? 200 : 70, selected ? 120 : 78, 255);
            SDL_RenderRect(renderer, &box);
            const SDL_Color color = selected ? SDL_Color{90, 200, 120, 255} : SDL_Color{200, 200, 200, 255};
            const float labelWidth = static_cast<float>(measureBitmapText(label, 3));
            renderBitmapText(renderer, label, x + (w - labelWidth) / 2.0f,
                             y + (kKeyHeight - static_cast<float>(kFontGlyphHeight * 3)) / 2.0f, 3, color);
        };
        for (int r = 0; r < kKeyRows; ++r) {
            for (int c = 0; c < kKeyCols; ++c) {
                drawKey(gridX + static_cast<float>(c) * (kKeyWidth + kGap),
                        kGridY + static_cast<float>(r) * (kKeyHeight + kGap), kKeyWidth, keys[r][c],
                        row == r && col == c);
            }
        }
        drawKey(gridX, kGridY + kKeyRows * (kKeyHeight + kGap), gridWidth, "CONNECT", row == kConnectRow);

        renderButtonHints(renderer, {{"A", "PRESS"}, {"X", "DELETE"}, {"START", "CONNECT"}, {"B", "BACK"}});
        SDL_RenderPresent(renderer);
        SDL_Delay(kFrameIntervalMs);
    }
}

} // namespace dualdeck::client
