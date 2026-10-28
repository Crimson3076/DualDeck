#include "text.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <utility>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#endif
#include "stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "embedded_fonts.h"

namespace dualdeck::hostui {

namespace {

// Decodes one UTF-8 code point and advances i; malformed bytes come out
// as U+FFFD so a stray byte never stops the rest of the line drawing.
uint32_t nextCodepoint(std::string_view s, size_t& i) {
    const auto byte = [&](size_t k) { return static_cast<uint32_t>(static_cast<unsigned char>(s[k])); };
    const uint32_t c = byte(i);
    if (c < 0x80) {
        ++i;
        return c;
    }
    int extra = 0;
    uint32_t cp = 0;
    if ((c & 0xE0) == 0xC0) {
        extra = 1;
        cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        extra = 2;
        cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        extra = 3;
        cp = c & 0x07;
    } else {
        ++i;
        return 0xFFFD;
    }
    if (i + static_cast<size_t>(extra) >= s.size()) {
        i = s.size();
        return 0xFFFD;
    }
    for (int k = 1; k <= extra; ++k) {
        const uint32_t b = byte(i + static_cast<size_t>(k));
        if ((b & 0xC0) != 0x80) {
            i += static_cast<size_t>(k);
            return 0xFFFD;
        }
        cp = (cp << 6) | (b & 0x3F);
    }
    i += static_cast<size_t>(extra) + 1;
    return cp;
}

struct Font {
    stbtt_fontinfo info{};
    int ascent = 0;
    int descent = 0;
    int lineGap = 0;
};

struct CachedText {
    SDL_Texture* texture = nullptr;
    float width = 0.0f; // pixels
    float height = 0.0f;
};

} // namespace

struct TextRenderer::Impl {
    Font fonts[4];
    float scale = 1.0f;
    SDL_Renderer* renderer = nullptr;
    std::unordered_map<std::string, CachedText> cache;

    Font& font(Face face) { return fonts[static_cast<int>(face)]; }

    // The glyph for cp in f, or in whichever other bundled font has it
    // (the body face lacks some symbols, e.g. systemd's status dot).
    std::pair<Font*, int> resolve(Font& f, uint32_t cp) {
        const int glyph = stbtt_FindGlyphIndex(&f.info, static_cast<int>(cp));
        if (glyph != 0 || cp < 0x80) return {&f, glyph};
        for (Font& other : fonts) {
            const int g = stbtt_FindGlyphIndex(&other.info, static_cast<int>(cp));
            if (g != 0) return {&other, g};
        }
        // Round dots none of them have, as a bullet.
        if (cp == 0x25CF || cp == 0x25CB || cp == 0x2B24) return resolve(f, 0x2022);
        return {&f, glyph};
    }

    void clearCache() {
        for (auto& [key, entry] : cache) SDL_DestroyTexture(entry.texture);
        cache.clear();
    }

    // Width in pixels at pixel size px, walking the same advances the
    // rasterizer uses so measuring and drawing always agree.
    float layoutWidth(Font& f, float px, std::string_view text, float trackingPx) {
        float x = 0.0f;
        int prev = 0;
        Font* prevFont = nullptr;
        size_t i = 0;
        bool first = true;
        while (i < text.size()) {
            const auto [gf, glyph] = resolve(f, nextCodepoint(text, i));
            const float s = stbtt_ScaleForMappingEmToPixels(&gf->info, px);
            if (!first) x += trackingPx;
            if (prev && prevFont == gf) x += static_cast<float>(stbtt_GetGlyphKernAdvance(&gf->info, prev, glyph)) * s;
            int advance = 0, lsb = 0;
            stbtt_GetGlyphHMetrics(&gf->info, glyph, &advance, &lsb);
            x += static_cast<float>(advance) * s;
            prev = glyph;
            prevFont = gf;
            first = false;
        }
        return x;
    }

    CachedText* get(Face face, float size, std::string_view text, float tracking) {
        const float px = std::round(size * scale * 2.0f) / 2.0f;
        const float trackingPx = tracking * scale;
        std::string key;
        key.reserve(text.size() + 24);
        key += static_cast<char>('0' + static_cast<int>(face));
        key += std::to_string(static_cast<int>(px * 2.0f));
        key += ':';
        key += std::to_string(static_cast<int>(trackingPx * 10.0f));
        key += ':';
        key.append(text);
        if (auto it = cache.find(key); it != cache.end()) return &it->second;
        if (cache.size() > 600) clearCache();

        Font& f = font(face);
        const float s = stbtt_ScaleForMappingEmToPixels(&f.info, px);
        const int width = std::max(1, static_cast<int>(std::ceil(layoutWidth(f, px, text, trackingPx))) + 2);
        const float baseline = std::ceil(static_cast<float>(f.ascent) * s);
        const int height = std::max(1, static_cast<int>(std::ceil(static_cast<float>(f.ascent - f.descent) * s)) + 1);
        std::vector<uint8_t> coverage(static_cast<size_t>(width) * static_cast<size_t>(height), 0);

        float x = 1.0f;
        int prev = 0;
        Font* prevFont = nullptr;
        size_t i = 0;
        bool first = true;
        std::vector<uint8_t> glyphBuf;
        while (i < text.size()) {
            const auto [gf, glyph] = resolve(f, nextCodepoint(text, i));
            const float gs = stbtt_ScaleForMappingEmToPixels(&gf->info, px);
            if (!first) x += trackingPx;
            if (prev && prevFont == gf) x += static_cast<float>(stbtt_GetGlyphKernAdvance(&gf->info, prev, glyph)) * gs;
            int advance = 0, lsb = 0;
            stbtt_GetGlyphHMetrics(&gf->info, glyph, &advance, &lsb);
            const float shiftX = x - std::floor(x);
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            stbtt_GetGlyphBitmapBoxSubpixel(&gf->info, glyph, gs, gs, shiftX, 0.0f, &x0, &y0, &x1, &y1);
            const int gw = x1 - x0;
            const int gh = y1 - y0;
            if (gw > 0 && gh > 0) {
                glyphBuf.assign(static_cast<size_t>(gw) * static_cast<size_t>(gh), 0);
                stbtt_MakeGlyphBitmapSubpixel(&gf->info, glyphBuf.data(), gw, gh, gw, gs, gs, shiftX, 0.0f, glyph);
                const int ox = static_cast<int>(std::floor(x)) + x0;
                const int oy = static_cast<int>(baseline) + y0;
                for (int gy = 0; gy < gh; ++gy) {
                    const int ty = oy + gy;
                    if (ty < 0 || ty >= height) continue;
                    for (int gx = 0; gx < gw; ++gx) {
                        const int tx = ox + gx;
                        if (tx < 0 || tx >= width) continue;
                        uint8_t& dst = coverage[static_cast<size_t>(ty) * static_cast<size_t>(width) +
                                                static_cast<size_t>(tx)];
                        const int sum = dst + glyphBuf[static_cast<size_t>(gy) * static_cast<size_t>(gw) +
                                                       static_cast<size_t>(gx)];
                        dst = static_cast<uint8_t>(std::min(255, sum));
                    }
                }
            }
            x += static_cast<float>(advance) * gs;
            prev = glyph;
            prevFont = gf;
            first = false;
        }

        SDL_Surface* surface = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_ARGB8888);
        if (!surface) return nullptr;
        for (int y = 0; y < height; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(surface->pixels) +
                                                    static_cast<ptrdiff_t>(y) * surface->pitch);
            for (int xx = 0; xx < width; ++xx) {
                const uint32_t a = coverage[static_cast<size_t>(y) * static_cast<size_t>(width) +
                                            static_cast<size_t>(xx)];
                row[xx] = (a << 24) | 0x00FFFFFFu;
            }
        }
        SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
        SDL_DestroySurface(surface);
        if (!texture) return nullptr;
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
        auto [it, inserted] =
            cache.emplace(std::move(key), CachedText{texture, static_cast<float>(width), static_cast<float>(height)});
        return &it->second;
    }
};

TextRenderer::TextRenderer() : impl_(std::make_unique<Impl>()) {}

TextRenderer::~TextRenderer() {
    impl_->clearCache();
}

bool TextRenderer::init() {
    const EmbeddedFont sources[4] = {kFontDisplayBold, kFontDisplaySemiBold, kFontBodyRegular, kFontBodyBold};
    for (int i = 0; i < 4; ++i) {
        Font& f = impl_->fonts[i];
        if (!stbtt_InitFont(&f.info, sources[i].data, stbtt_GetFontOffsetForIndex(sources[i].data, 0))) {
            return false;
        }
        stbtt_GetFontVMetrics(&f.info, &f.ascent, &f.descent, &f.lineGap);
    }
    return true;
}

void TextRenderer::setScale(SDL_Renderer* renderer, float scale) {
    if (renderer != impl_->renderer) {
        impl_->clearCache();
        impl_->renderer = renderer;
    }
    impl_->scale = scale > 0.0f ? scale : 1.0f;
}

float TextRenderer::measure(Face face, float size, std::string_view text, float tracking) {
    Font& f = impl_->font(face);
    const float px = size * impl_->scale;
    return impl_->layoutWidth(f, px, text, tracking * impl_->scale) / impl_->scale;
}

float TextRenderer::lineHeight(Face face, float size) {
    Font& f = impl_->font(face);
    const float s = stbtt_ScaleForMappingEmToPixels(&f.info, size);
    return static_cast<float>(f.ascent - f.descent) * s;
}

void TextRenderer::draw(SDL_Renderer* renderer, Face face, float size, std::string_view text, float x, float y,
                        SDL_Color color, float tracking) {
    if (text.empty()) return;
    CachedText* entry = impl_->get(face, size, text, tracking);
    if (!entry) return;
    SDL_SetTextureColorMod(entry->texture, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(entry->texture, color.a);
    const SDL_FRect dst{std::round(x * impl_->scale) / impl_->scale - 1.0f / impl_->scale,
                        std::round(y * impl_->scale) / impl_->scale, entry->width / impl_->scale,
                        entry->height / impl_->scale};
    SDL_RenderTexture(renderer, entry->texture, nullptr, &dst);
}

std::vector<std::string> TextRenderer::wrap(Face face, float size, std::string_view text, float maxWidth) {
    std::vector<std::string> lines;
    size_t paraStart = 0;
    while (paraStart <= text.size()) {
        size_t paraEnd = text.find('\n', paraStart);
        if (paraEnd == std::string_view::npos) paraEnd = text.size();
        std::string_view para = text.substr(paraStart, paraEnd - paraStart);
        // Keep a paragraph's indentation (command output lines up by it).
        const size_t indentEnd = std::min(para.find_first_not_of(' '), para.size());
        const std::string indent(para.substr(0, indentEnd));
        para.remove_prefix(indentEnd);
        std::string line = indent;
        size_t i = 0;
        while (i <= para.size()) {
            size_t sp = para.find(' ', i);
            if (sp == std::string_view::npos) sp = para.size();
            const std::string_view word = para.substr(i, sp - i);
            std::string candidate = line.size() == indent.size() ? line + std::string(word) : line + " " + std::string(word);
            if (line.size() > indent.size() && measure(face, size, candidate) > maxWidth) {
                lines.push_back(line);
                line = std::string(word);
            } else {
                line = std::move(candidate);
            }
            i = sp + 1;
        }
        lines.push_back(line);
        paraStart = paraEnd + 1;
    }
    return lines;
}

float TextRenderer::drawWrapped(SDL_Renderer* renderer, Face face, float size, std::string_view text, float x,
                                float y, float maxWidth, SDL_Color color, float lineSpacing, int maxLines) {
    std::vector<std::string> lines = wrap(face, size, text, maxWidth);
    if (maxLines > 0 && static_cast<int>(lines.size()) > maxLines) {
        lines.resize(static_cast<size_t>(maxLines));
        std::string& last = lines.back();
        while (!last.empty() && measure(face, size, last + "\xE2\x80\xA6") > maxWidth) last.pop_back();
        last += "\xE2\x80\xA6";
    }
    const float step = size * lineSpacing;
    for (size_t i = 0; i < lines.size(); ++i) {
        draw(renderer, face, size, lines[i], x, y + static_cast<float>(i) * step, color);
    }
    return static_cast<float>(lines.size()) * step;
}

} // namespace dualdeck::hostui
