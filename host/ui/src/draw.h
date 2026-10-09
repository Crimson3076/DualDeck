#pragma once

// Shape primitives the host UI is drawn from: rounded rectangles (solid,
// vertical gradient, outline), thick lines and arcs, and the small line
// icons and device drawings the menus use. Everything goes through
// SDL_RenderGeometry so it stays smooth at any window size.

#include <SDL3/SDL.h>

#include <string>

namespace dualdeck::hostui {

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

void fillRoundRect(SDL_Renderer* r, Rect rect, float radius, SDL_Color color);
void fillRoundRectGradient(SDL_Renderer* r, Rect rect, float radius, SDL_Color top, SDL_Color bottom);
void strokeRoundRect(SDL_Renderer* r, Rect rect, float radius, float thickness, SDL_Color color);
void fillCircle(SDL_Renderer* r, float cx, float cy, float radius, SDL_Color color);
void thickLine(SDL_Renderer* r, float x0, float y0, float x1, float y1, float thickness, SDL_Color color);
// Angles in radians, 0 = +x, clockwise on screen.
void arc(SDL_Renderer* r, float cx, float cy, float radius, float from, float to, float thickness, SDL_Color color);
// A circular wash of color fading to nothing at radius.
void radialGlow(SDL_Renderer* r, float cx, float cy, float radiusX, float radiusY, SDL_Color center);
// A soft halo around rect, for the focused element.
void glow(SDL_Renderer* r, Rect rect, float radius, float spread, SDL_Color color);

// 24x24-unit line icons, scaled to size, by name (the protocol's icon
// field): monitor, plus, refresh, gamepad, list, gear, trash, power,
// branch, info, download. Unknown names draw nothing.
void drawIcon(SDL_Renderer* r, const std::string& name, float x, float y, float size, SDL_Color color);
// The larger console drawings on system cards: ds, 3ds, wiiu, desktop,
// custom. outline is the frame color, fill the solid screen.
void drawDevice(SDL_Renderer* r, const std::string& name, float cx, float cy, SDL_Color outline, SDL_Color fill);
// The DualDeck mark: two stacked screens, the lower one solid.
void drawMark(SDL_Renderer* r, float x, float y, float height, SDL_Color color);

} // namespace dualdeck::hostui
