#include "draw.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace dualdeck::hostui {

namespace {

constexpr float kPi = 3.14159265358979f;

// Width of the soft edge, in layout units. Roughly one screen pixel at
// the Deck's native size; good enough at other sizes too.
constexpr float kFeather = 1.0f;

SDL_FColor toF(SDL_Color c, float alphaScale = 1.0f) {
    return SDL_FColor{static_cast<float>(c.r) / 255.0f, static_cast<float>(c.g) / 255.0f,
                      static_cast<float>(c.b) / 255.0f, static_cast<float>(c.a) / 255.0f * alphaScale};
}

SDL_FColor lerp(SDL_FColor a, SDL_FColor b, float t) {
    return SDL_FColor{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

// A rounded rectangle's outline as corner centers plus unit directions,
// so the same outline can be pushed in or out by changing the radius.
struct Spoke {
    float cx, cy, dx, dy;
};

std::vector<Spoke> roundRectSpokes(Rect rect, float radius) {
    radius = std::clamp(radius, 0.0f, std::min(rect.w, rect.h) / 2.0f);
    const int seg = radius < 4.0f ? 2 : radius < 12.0f ? 5 : 8;
    const float corners[4][3] = {
        {rect.x + rect.w - radius, rect.y + radius, -kPi / 2.0f},
        {rect.x + rect.w - radius, rect.y + rect.h - radius, 0.0f},
        {rect.x + radius, rect.y + rect.h - radius, kPi / 2.0f},
        {rect.x + radius, rect.y + radius, kPi},
    };
    std::vector<Spoke> spokes;
    spokes.reserve(static_cast<size_t>(4 * (seg + 1)));
    for (const auto& c : corners) {
        for (int i = 0; i <= seg; ++i) {
            const float a = c[2] + (kPi / 2.0f) * static_cast<float>(i) / static_cast<float>(seg);
            spokes.push_back({c[0], c[1], std::cos(a), std::sin(a)});
        }
    }
    return spokes;
}

SDL_Vertex vert(float x, float y, SDL_FColor c) {
    return SDL_Vertex{SDL_FPoint{x, y}, c, SDL_FPoint{0, 0}};
}

// Fills the band between two radii of the same outline. Colors per edge.
void band(std::vector<SDL_Vertex>& v, std::vector<int>& idx, const std::vector<Spoke>& spokes, float rIn, float rOut,
          SDL_FColor cIn, SDL_FColor cOut) {
    const int base = static_cast<int>(v.size());
    const int n = static_cast<int>(spokes.size());
    rIn = std::max(rIn, 0.0f);
    for (const Spoke& s : spokes) {
        v.push_back(vert(s.cx + s.dx * rIn, s.cy + s.dy * rIn, cIn));
        v.push_back(vert(s.cx + s.dx * rOut, s.cy + s.dy * rOut, cOut));
    }
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        const int a = base + i * 2, b = base + i * 2 + 1, c = base + j * 2, d = base + j * 2 + 1;
        idx.insert(idx.end(), {a, b, c, b, d, c});
    }
}

void submit(SDL_Renderer* r, const std::vector<SDL_Vertex>& v, const std::vector<int>& idx) {
    if (v.empty()) return;
    SDL_RenderGeometry(r, nullptr, v.data(), static_cast<int>(v.size()), idx.data(), static_cast<int>(idx.size()));
}

} // namespace

void fillRoundRectGradient(SDL_Renderer* r, Rect rect, float radius, SDL_Color top, SDL_Color bottom) {
    if (rect.w <= 0 || rect.h <= 0) return;
    radius = std::clamp(radius, 0.0f, std::min(rect.w, rect.h) / 2.0f);
    const std::vector<Spoke> spokes = roundRectSpokes(rect, radius);
    const SDL_FColor ct = toF(top), cb = toF(bottom);
    const auto colorAt = [&](float y) { return lerp(ct, cb, std::clamp((y - rect.y) / rect.h, 0.0f, 1.0f)); };

    std::vector<SDL_Vertex> v;
    std::vector<int> idx;
    const float inner = radius - kFeather / 2.0f;
    v.push_back(vert(rect.x + rect.w / 2.0f, rect.y + rect.h / 2.0f, colorAt(rect.y + rect.h / 2.0f)));
    for (const Spoke& s : spokes) {
        const float y = s.cy + s.dy * inner;
        v.push_back(vert(s.cx + s.dx * inner, y, colorAt(y)));
    }
    const int n = static_cast<int>(spokes.size());
    for (int i = 0; i < n; ++i) idx.insert(idx.end(), {0, 1 + i, 1 + (i + 1) % n});
    // Feathered edge.
    const int base = static_cast<int>(v.size());
    const float outer = radius + kFeather / 2.0f;
    for (const Spoke& s : spokes) {
        const float yi = s.cy + s.dy * inner;
        SDL_FColor c = colorAt(yi);
        v.push_back(vert(s.cx + s.dx * inner, yi, c));
        c.a = 0.0f;
        v.push_back(vert(s.cx + s.dx * outer, s.cy + s.dy * outer, c));
    }
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        const int a = base + i * 2, b = base + i * 2 + 1, c = base + j * 2, d = base + j * 2 + 1;
        idx.insert(idx.end(), {a, b, c, b, d, c});
    }
    submit(r, v, idx);
}

void fillRoundRect(SDL_Renderer* r, Rect rect, float radius, SDL_Color color) {
    fillRoundRectGradient(r, rect, radius, color, color);
}

void strokeRoundRect(SDL_Renderer* r, Rect rect, float radius, float thickness, SDL_Color color) {
    if (rect.w <= 0 || rect.h <= 0) return;
    radius = std::clamp(radius, 0.0f, std::min(rect.w, rect.h) / 2.0f);
    const std::vector<Spoke> spokes = roundRectSpokes(rect, radius);
    const SDL_FColor c = toF(color);
    SDL_FColor clear = c;
    clear.a = 0.0f;
    std::vector<SDL_Vertex> v;
    std::vector<int> idx;
    const float outer = radius;
    const float inner = radius - thickness;
    const float h = kFeather / 2.0f;
    band(v, idx, spokes, inner + h, outer - h, c, c);
    band(v, idx, spokes, outer - h, outer + h, c, clear);
    band(v, idx, spokes, inner - h, inner + h, clear, c);
    submit(r, v, idx);
}

void fillCircle(SDL_Renderer* r, float cx, float cy, float radius, SDL_Color color) {
    fillRoundRect(r, Rect{cx - radius, cy - radius, radius * 2.0f, radius * 2.0f}, radius, color);
}

void thickLine(SDL_Renderer* r, float x0, float y0, float x1, float y1, float thickness, SDL_Color color) {
    const float dx = x1 - x0, dy = y1 - y0;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len <= 0.0f) return;
    const float nx = -dy / len, ny = dx / len;
    const float ex = dx / len * thickness / 2.0f, ey = dy / len * thickness / 2.0f; // round-ish caps
    const SDL_FColor c = toF(color);
    SDL_FColor clear = c;
    clear.a = 0.0f;
    std::vector<SDL_Vertex> v;
    std::vector<int> idx;
    const float hw = thickness / 2.0f;
    const float offsets[4] = {-hw - kFeather / 2.0f, -hw + kFeather / 2.0f, hw - kFeather / 2.0f,
                              hw + kFeather / 2.0f};
    const SDL_FColor colors[4] = {clear, c, c, clear};
    for (int k = 0; k < 4; ++k) {
        v.push_back(vert(x0 - ex * 0.5f + nx * offsets[k], y0 - ey * 0.5f + ny * offsets[k], colors[k]));
        v.push_back(vert(x1 + ex * 0.5f + nx * offsets[k], y1 + ey * 0.5f + ny * offsets[k], colors[k]));
    }
    for (int k = 0; k < 3; ++k) {
        const int a = k * 2, b = k * 2 + 1, cc = (k + 1) * 2, d = (k + 1) * 2 + 1;
        idx.insert(idx.end(), {a, b, cc, b, d, cc});
    }
    submit(r, v, idx);
}

void arc(SDL_Renderer* r, float cx, float cy, float radius, float from, float to, float thickness, SDL_Color color) {
    const int seg = std::max(6, static_cast<int>(std::abs(to - from) * radius / 3.0f));
    const SDL_FColor c = toF(color);
    SDL_FColor clear = c;
    clear.a = 0.0f;
    const float hw = thickness / 2.0f;
    const float radii[4] = {radius - hw - kFeather / 2.0f, radius - hw + kFeather / 2.0f,
                            radius + hw - kFeather / 2.0f, radius + hw + kFeather / 2.0f};
    const SDL_FColor colors[4] = {clear, c, c, clear};
    std::vector<SDL_Vertex> v;
    std::vector<int> idx;
    for (int i = 0; i <= seg; ++i) {
        const float a = from + (to - from) * static_cast<float>(i) / static_cast<float>(seg);
        for (int k = 0; k < 4; ++k) {
            v.push_back(vert(cx + std::cos(a) * radii[k], cy + std::sin(a) * radii[k], colors[k]));
        }
    }
    for (int i = 0; i < seg; ++i) {
        for (int k = 0; k < 3; ++k) {
            const int a = i * 4 + k, b = i * 4 + k + 1, cc = (i + 1) * 4 + k, d = (i + 1) * 4 + k + 1;
            idx.insert(idx.end(), {a, b, cc, b, d, cc});
        }
    }
    submit(r, v, idx);
}

void radialGlow(SDL_Renderer* r, float cx, float cy, float radiusX, float radiusY, SDL_Color center) {
    const int seg = 64;
    const int rings = 6;
    const SDL_FColor c = toF(center);
    std::vector<SDL_Vertex> v;
    std::vector<int> idx;
    v.push_back(vert(cx, cy, c));
    // Rings at eased radii so the falloff looks smooth, not banded.
    for (int k = 1; k <= rings; ++k) {
        const float t = static_cast<float>(k) / static_cast<float>(rings);
        SDL_FColor ck = c;
        ck.a = c.a * (1.0f - t) * (1.0f - t);
        for (int i = 0; i < seg; ++i) {
            const float a = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(seg);
            v.push_back(vert(cx + std::cos(a) * radiusX * t, cy + std::sin(a) * radiusY * t, ck));
        }
    }
    for (int i = 0; i < seg; ++i) idx.insert(idx.end(), {0, 1 + i, 1 + (i + 1) % seg});
    for (int k = 0; k < rings - 1; ++k) {
        const int inner = 1 + k * seg, outer = 1 + (k + 1) * seg;
        for (int i = 0; i < seg; ++i) {
            const int j = (i + 1) % seg;
            idx.insert(idx.end(), {inner + i, outer + i, inner + j, outer + i, outer + j, inner + j});
        }
    }
    submit(r, v, idx);
}

void glow(SDL_Renderer* r, Rect rect, float radius, float spread, SDL_Color color) {
    const int steps = 10;
    const float step = spread / static_cast<float>(steps);
    for (int i = 0; i < steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const float grow = step * static_cast<float>(i);
        SDL_Color c = color;
        c.a = static_cast<Uint8>(static_cast<float>(color.a) * (1.0f - t) * (1.0f - t));
        strokeRoundRect(r, Rect{rect.x - grow - step, rect.y - grow - step, rect.w + 2 * (grow + step),
                                rect.h + 2 * (grow + step)},
                        radius + grow + step, step + 0.5f, c);
    }
}

void drawIcon(SDL_Renderer* r, const std::string& name, float x, float y, float size, SDL_Color color) {
    const float u = size / 24.0f;
    const float t = 2.0f * u;
    const auto L = [&](float ax, float ay, float bx, float by) { thickLine(r, x + ax * u, y + ay * u, x + bx * u, y + by * u, t, color); };
    const auto box = [&](float ax, float ay, float w, float h, float rad) {
        strokeRoundRect(r, Rect{x + ax * u, y + ay * u, w * u, h * u}, rad * u, t, color);
    };
    const auto circ = [&](float cx, float cy, float rad) { arc(r, x + cx * u, y + cy * u, rad * u, 0.0f, 2.0f * kPi, t, color); };

    if (name == "monitor") {
        box(3, 4, 18, 12, 2);
        L(8, 20, 16, 20);
        L(12, 16, 12, 20);
    } else if (name == "plus") {
        circ(12, 12, 9);
        L(12, 8, 12, 16);
        L(8, 12, 16, 12);
    } else if (name == "refresh") {
        arc(r, x + 12 * u, y + 12 * u, 8 * u, 0.34f, 5.5f, t, color);
        L(17.7f, 6.3f, 21.5f, 10.0f);
        L(21.5f, 4.5f, 21.5f, 10.0f);
        L(21.5f, 10.0f, 16.0f, 10.0f);
    } else if (name == "download") {
        L(12, 4, 12, 15);
        L(7, 10, 12, 15);
        L(17, 10, 12, 15);
        L(5, 20, 19, 20);
    } else if (name == "gamepad") {
        box(2, 7, 20, 11, 5.5f);
        L(7, 10.5f, 7, 14.5f);
        L(5, 12.5f, 9, 12.5f);
        fillCircle(r, x + 16 * u, y + 11.5f * u, 1.3f * u, color);
        fillCircle(r, x + 18 * u, y + 13.5f * u, 1.3f * u, color);
    } else if (name == "list") {
        L(4, 6, 20, 6);
        L(4, 12, 20, 12);
        L(4, 18, 14, 18);
    } else if (name == "gear") {
        circ(12, 12, 3.5f);
        for (int i = 0; i < 8; ++i) {
            const float a = static_cast<float>(i) * kPi / 4.0f;
            L(12 + std::cos(a) * 6.5f, 12 + std::sin(a) * 6.5f, 12 + std::cos(a) * 9.5f, 12 + std::sin(a) * 9.5f);
        }
    } else if (name == "trash") {
        L(4, 7, 20, 7);
        L(9, 7, 9, 4);
        L(9, 4, 15, 4);
        L(15, 4, 15, 7);
        L(6, 7, 7, 20);
        L(7, 20, 17, 20);
        L(17, 20, 18, 7);
    } else if (name == "power") {
        arc(r, x + 12 * u, y + 13 * u, 8 * u, -kPi * 0.3f, kPi * 1.3f, t, color);
        L(12, 3, 12, 12);
    } else if (name == "branch") {
        circ(7, 5, 2);
        circ(7, 19, 2);
        circ(17, 7, 2);
        L(7, 7, 7, 17);
        arc(r, x + 12 * u, y + 9 * u, 5 * u, 0.0f, kPi * 0.6f, t, color);
    } else if (name == "info") {
        circ(12, 12, 9);
        L(12, 11, 12, 17);
        fillCircle(r, x + 12 * u, y + 7.5f * u, 1.3f * u, color);
    }
}

void drawDevice(SDL_Renderer* r, const std::string& name, float cx, float cy, SDL_Color outline, SDL_Color fill) {
    const float t = 3.0f;
    const auto frame = [&](float x, float y, float w, float h, float rad) {
        strokeRoundRect(r, Rect{cx + x, cy + y, w, h}, rad, t, outline);
    };
    const auto solid = [&](float x, float y, float w, float h, float rad) {
        fillRoundRect(r, Rect{cx + x, cy + y, w, h}, rad, fill);
    };
    if (name == "ds") {
        frame(-36, -60, 72, 52, 6);
        solid(-36, 8, 72, 52, 6);
    } else if (name == "3ds") {
        frame(-55, -60, 110, 56, 6);
        solid(-41, 10, 82, 50, 6);
    } else if (name == "wiiu") {
        frame(-70, -43, 140, 86, 18);
        solid(-36, -25, 72, 50, 4);
    } else if (name == "desktop") {
        frame(-65, -55, 130, 80, 6);
        solid(-15, 33, 30, 22, 3);
    } else if (name == "custom") {
        solid(-50, -12, 100, 24, 12);
        solid(-12, -50, 24, 100, 12);
    } else {
        frame(-50, -40, 100, 80, 8);
    }
}

void drawMark(SDL_Renderer* r, float x, float y, float height, SDL_Color color) {
    const float w = height * 0.85f;
    const float screen = height * 0.43f;
    const float t = std::max(2.0f, height * 0.075f);
    strokeRoundRect(r, Rect{x, y, w, screen}, height * 0.1f, t, color);
    fillRoundRect(r, Rect{x, y + height - screen, w, screen}, height * 0.1f, color);
}

} // namespace dualdeck::hostui
