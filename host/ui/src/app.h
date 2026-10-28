#pragma once

// The host UI's state and drawing: whatever menu or dialog the script
// last asked for, which entry has focus, and how input moves it. Kept
// apart from main.cpp's window/event plumbing so the screenshot mode can
// draw the exact same frames into an offscreen surface.

#include <SDL3/SDL.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "draw.h"
#include "protocol.h"
#include "text.h"

namespace dualdeck::hostui {

inline constexpr float kLayoutWidth = 1280.0f;
inline constexpr float kLayoutHeight = 800.0f;

class App {
public:
    using ReplyFn = std::function<void(const std::string&)>;

    App(TextRenderer& text, ReplyFn reply);

    // Applies one parsed request. Returns false for quit.
    bool apply(const Request& request);
    void press(Press press);
    // Mouse in layout coordinates.
    void pointerMoved(float x, float y);
    void pointerClicked(float x, float y);
    void scroll(float amount);
    // Window closed: answers whatever is pending the way "back" would.
    void closeRequested();

    // Advances animations by dt seconds (0 snaps them to rest).
    void update(float dt);
    void render(SDL_Renderer* renderer);

private:
    struct Hit {
        Rect rect;
        int index;
    };

    void replyOnce(const std::string& text);
    void moveFocus(Press press);
    Rect focusTarget() const;

    void renderBackground(SDL_Renderer* r);
    void renderHeader(SDL_Renderer* r);
    void renderStatus(SDL_Renderer* r, float x, float y);
    void renderHome(SDL_Renderer* r);
    void renderCards(SDL_Renderer* r);
    void renderList(SDL_Renderer* r);
    void renderRow(SDL_Renderer* r, const MenuItem& item, Rect rect, bool focused);
    void renderTag(SDL_Renderer* r, const MenuItem& item, float right, float centerY);
    void renderDialog(SDL_Renderer* r);
    void renderHints(SDL_Renderer* r);
    void renderFocusRing(SDL_Renderer* r);

    TextRenderer& text_;
    ReplyFn reply_;

    std::string version_;
    std::optional<Menu> menu_;
    std::optional<Dialog> dialog_;
    bool awaitingReply_ = false;
    int focus_ = 0;
    // Remembered list position when focus jumps to the home hero.
    int homeListFocus_ = 1;
    float listScroll_ = 0.0f;
    float listScrollTarget_ = 0.0f;
    int dialogFocus_ = 0;
    float textScroll_ = 0.0f;
    float textMaxScroll_ = 0.0f;
    float clock_ = 0.0f;
    float dialogFade_ = 0.0f;

    // Hit areas rebuilt every frame from layout.
    std::vector<Hit> itemHits_;
    std::vector<Hit> buttonHits_;
    std::vector<Rect> itemRects_;
    std::vector<Rect> buttonRects_;
    Rect focusRect_{};
    bool focusRectValid_ = false;
    float focusRadius_ = 14.0f;
};

} // namespace dualdeck::hostui
