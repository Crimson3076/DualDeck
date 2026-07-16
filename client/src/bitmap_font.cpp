#include "bitmap_font.h"

#include <cctype>
#include <unordered_map>

namespace melonds_remote::client {

namespace {

using Glyph = std::array<uint8_t, kFontGlyphHeight>;

// Each row's low 5 bits are pixels, MSB-of-the-5 (bit 4) leftmost.
const std::unordered_map<char, Glyph>& glyphTable() {
    static const std::unordered_map<char, Glyph> table = {
        {' ', {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000}},
        {'0', {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110}},
        {'1', {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
        {'2', {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111}},
        {'3', {0b11111, 0b00010, 0b00100, 0b00010, 0b00001, 0b10001, 0b01110}},
        {'4', {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010}},
        {'5', {0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110}},
        {'6', {0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110}},
        {'7', {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000}},
        {'8', {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110}},
        {'9', {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100}},
        {'A', {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
        {'B', {0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110}},
        {'C', {0b01111, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b01111}},
        {'D', {0b11100, 0b10010, 0b10001, 0b10001, 0b10001, 0b10010, 0b11100}},
        {'E', {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111}},
        {'F', {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000}},
        {'G', {0b01111, 0b10000, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111}},
        {'H', {0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
        {'I', {0b01110, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
        {'J', {0b00001, 0b00001, 0b00001, 0b00001, 0b10001, 0b10001, 0b01110}},
        {'K', {0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001}},
        {'L', {0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111}},
        {'M', {0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001}},
        {'N', {0b10001, 0b11001, 0b10101, 0b10101, 0b10011, 0b10001, 0b10001}},
        {'O', {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
        {'P', {0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000}},
        {'Q', {0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101}},
        {'R', {0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001}},
        {'S', {0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110}},
        {'T', {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100}},
        {'U', {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
        {'V', {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100}},
        {'W', {0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b10101, 0b01010}},
        {'X', {0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001}},
        {'Y', {0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100}},
        {'Z', {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111}},
        {'.', {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b01100, 0b01100}},
        {'-', {0b00000, 0b00000, 0b00000, 0b11111, 0b00000, 0b00000, 0b00000}},
        {':', {0b00000, 0b01100, 0b01100, 0b00000, 0b01100, 0b01100, 0b00000}},
    };
    return table;
}

const Glyph& glyphFor(char c) {
    char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const auto& table = glyphTable();
    auto it = table.find(upper);
    if (it != table.end()) return it->second;
    static const Glyph blank{0, 0, 0, 0, 0, 0, 0};
    return blank;
}

} // namespace

int measureBitmapText(const std::string& text, int pixelSize) {
    if (text.empty()) return 0;
    int glyphWidth = kFontGlyphWidth * pixelSize;
    int gap = pixelSize;
    return static_cast<int>(text.size()) * glyphWidth +
           (static_cast<int>(text.size()) - 1) * gap;
}

int renderBitmapText(SDL_Renderer* renderer, const std::string& text, float x, float y,
                      int pixelSize, SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);

    float cursorX = x;
    int gap = pixelSize;

    for (char c : text) {
        const Glyph& glyph = glyphFor(c);
        for (int row = 0; row < kFontGlyphHeight; ++row) {
            uint8_t bits = glyph[static_cast<size_t>(row)];
            for (int col = 0; col < kFontGlyphWidth; ++col) {
                bool on = (bits >> (kFontGlyphWidth - 1 - col)) & 1;
                if (!on) continue;
                SDL_FRect pixel{cursorX + static_cast<float>(col * pixelSize),
                                 y + static_cast<float>(row * pixelSize),
                                 static_cast<float>(pixelSize), static_cast<float>(pixelSize)};
                SDL_RenderFillRect(renderer, &pixel);
            }
        }
        cursorX += static_cast<float>(kFontGlyphWidth * pixelSize + gap);
    }

    return static_cast<int>(cursorX - x) - gap;
}

} // namespace melonds_remote::client
