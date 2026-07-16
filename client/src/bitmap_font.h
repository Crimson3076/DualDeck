#pragma once

// A minimal, self-contained 5x7 pixel font -- just enough characters to
// render a host discovery list (hostnames, IP addresses, port numbers)
// on screen without adding an external font/text-rendering dependency
// (SDL3_ttf, FreeType, etc.), matching the "no external dependencies"
// approach already used for the pairing-code entry screen. This matters
// specifically because Steam Deck Gaming Mode has no visible terminal --
// printing discovered host names to stdout, as a "simplest possible"
// approach might otherwise do, isn't actually usable there.
//
// Deliberately small: space, 0-9, A-Z, and '.', '-', ':' (enough for
// hostnames, IPv4 addresses, and "host:port" strings). Any unsupported
// character (lowercase is upper-cased first; anything else falls
// through) renders as a blank cell rather than failing.

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <string>

namespace melonds_remote::client {

constexpr int kFontGlyphWidth = 5;
constexpr int kFontGlyphHeight = 7;

// Renders `text` starting at (x, y) in `color`, each glyph pixel drawn as
// a `pixelSize`-by-`pixelSize` filled rect, `pixelSize` apart within a
// glyph and with a 1-pixel-cell gap between characters. Returns the
// total rendered width in pixels (useful for centering).
int renderBitmapText(SDL_Renderer* renderer, const std::string& text, float x, float y,
                      int pixelSize, SDL_Color color);

// Just the width a call to renderBitmapText() with these arguments would
// occupy, without actually drawing anything (for layout/centering).
int measureBitmapText(const std::string& text, int pixelSize);

} // namespace melonds_remote::client
