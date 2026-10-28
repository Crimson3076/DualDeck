#pragma once

// Anti-aliased text for the host UI, rasterized with stb_truetype from
// the fonts compiled into the binary (host/ui/fonts/, both SIL OFL):
// Chakra Petch for headings and labels, Atkinson Hyperlegible for
// everything meant to be read. Each distinct string is rendered once
// into a white texture and tinted at draw time.

#include <SDL3/SDL.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dualdeck::hostui {

enum class Face { Display, DisplaySemi, Body, BodyBold };

class TextRenderer {
public:
    TextRenderer();
    ~TextRenderer();
    TextRenderer(const TextRenderer&) = delete;
    TextRenderer& operator=(const TextRenderer&) = delete;

    bool init();
    // Output pixels per layout unit, so text is rasterized at the size
    // it lands on screen instead of being scaled (and blurred).
    void setScale(SDL_Renderer* renderer, float scale);

    float measure(Face face, float size, std::string_view text, float tracking = 0.0f);
    float lineHeight(Face face, float size);
    // y is the top of the line box.
    void draw(SDL_Renderer* renderer, Face face, float size, std::string_view text, float x, float y,
              SDL_Color color, float tracking = 0.0f);
    std::vector<std::string> wrap(Face face, float size, std::string_view text, float maxWidth);
    // Returns the height used.
    float drawWrapped(SDL_Renderer* renderer, Face face, float size, std::string_view text, float x, float y,
                      float maxWidth, SDL_Color color, float lineSpacing = 1.3f, int maxLines = 0);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dualdeck::hostui
