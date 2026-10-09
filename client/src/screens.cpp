#include "screens.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "bitmap_font.h"

namespace dualdeck::client {

void renderCenteredBitmapText(SDL_Renderer* renderer, const std::string& text, float y,
                               int pixelSize, SDL_Color color) {
    int width = measureBitmapText(text, pixelSize);
    float x = (static_cast<float>(kWindowWidth) - static_cast<float>(width)) / 2.0f;
    renderBitmapText(renderer, text, x, y, pixelSize, color);
}

// Bottom-left corner stamp, this client binary's own DUALDECK_VERSION
// (see main()'s netConfig.appVersion comment for where this comes from).
// Real user ask: the client auto-updates itself on every launch (see
// run-client.sh's auto-update block), but a failed auto-update (offline,
// GitHub unreachable, a partial download) silently falls back to
// whatever version was already installed, with no on-screen way to tell
// which one that actually is short of uninstalling and reinstalling from
// scratch. Drawn only on the picker screens (this function and
// renderDiscoveryList() below) rather than every screen -- these are the
// two screens guaranteed to be reached on every single launch regardless
// of whether a host is ever found, unlike e.g. renderHostControlScreen()
// or the in-session menu, which only show up after a connection already
// succeeded. Skipped entirely when empty (a from-source dev build run
// directly, not via run-client.sh -- see the same DUALDECK_VERSION
// comment) rather than drawing a misleading blank stamp.
void renderClientVersionStamp(SDL_Renderer* renderer, const std::string& clientVersion) {
    if (clientVersion.empty()) return;
    renderBitmapText(renderer, clientVersion, 20.0f, static_cast<float>(kWindowHeight) - 34.0f, 2,
                      SDL_Color{90, 90, 96, 255});
}

// Real user request, 2026-08-26: "some sort of way to show the user what
// resolution is being streamed, what fps, codec, etc as a debug overlay
// for the client." Top-left corner stack, one line per stat -- kept as a
// free function taking exactly what it reads (mirrors renderConnecting()/
// renderHostControlScreen() etc.'s own style) rather than a main()-local
// lambda, since it needs nothing from the render loop's own local state
// beyond `net` itself and the locally configured video quality (for the
// QUALITY line, which reads back the client's own *requested* setting --
// see ClientSettings::videoQuality's own comment -- not anything NetClient
// tracks, since a session's actual negotiated quality byte isn't read back
// from the wire anywhere on this side once Hello is sent).
//
// Only ever called while net.isConnected() (see its call site) -- every
// NetClient getter used here already has its own documented default/last-
// known-value behavior for "not connected yet," but showing a stale
// RES/FPS/CODEC stack while reconnecting would be actively misleading
// rather than merely uninformative.
//
// All-caps ASCII only (see bitmap_font.h for the supported glyphs).
void renderDebugOverlay(SDL_Renderer* renderer, NetClient& net, int requestedVideoQuality) {
    constexpr int kPixelSize = 2;
    constexpr float kLineHeight = 16.0f;
    constexpr float kX = 20.0f;
    float y = 20.0f;
    const SDL_Color color{140, 220, 140, 255};

    auto line = [&](const std::string& text) {
        renderBitmapText(renderer, text, kX, y, kPixelSize, color);
        y += kLineHeight;
    };

    char buf[64];

    std::snprintf(buf, sizeof(buf), "RES: %uX%u", static_cast<unsigned>(net.hostNativeWidth()),
                  static_cast<unsigned>(net.hostNativeHeight()));
    line(buf);

    std::snprintf(buf, sizeof(buf), "FPS: %u", static_cast<unsigned>(net.receivedFps()));
    line(buf);

    const VideoCodec codec = net.negotiatedVideoCodec();
    line(std::string("CODEC: ") +
         (codec == VideoCodec::PyroWave ? "PYROWAVE" : codec == VideoCodec::H264 ? "H264" : "JPEG"));

    // Shows the raw requested quality number (0 = AUTO) rather than
    // settingsMenuItems()' preset names.
    std::string qualityLabel;
    if (requestedVideoQuality == 0) {
        qualityLabel = "AUTO";
    } else {
        std::snprintf(buf, sizeof(buf), "%d", requestedVideoQuality);
        qualityLabel = buf;
    }
    line("QUALITY: " + qualityLabel);

    // Real user report, 2026-08-26: "latency says 0ms and decode is
    // 1-2ms" looked suspicious enough to doubt the whole overlay -- but
    // dividing by 1000 to show whole milliseconds was throwing away the
    // one piece of information that would have settled it: a real,
    // sub-millisecond LAN latency (e.g. 400us) integer-divides to "0MS"
    // identically to lastLatencyMicros()'s own genuine "couldn't measure
    // this frame at all" fallback (nowWallUs < captureTimestampUs, host/
    // client clocks not synchronized -- see that getter's own comment),
    // which really is exactly 0, not just small. Showing raw
    // microseconds instead makes the two cases visually distinct on
    // their own: "LATENCY: 412US" is obviously a real measurement,
    // while a value that's *always* exactly "LATENCY: 0US" across many
    // frames (not just occasionally, which real jitter alone could
    // explain) is the actual signal that this session's latency number
    // specifically can't be trusted, without needing a separate N/A
    // state to say so.
    std::snprintf(buf, sizeof(buf), "LATENCY: %uUS", static_cast<unsigned>(net.lastLatencyMicros()));
    line(buf);

    std::snprintf(buf, sizeof(buf), "DECODE: %uUS", static_cast<unsigned>(net.lastDecodeMicros()));
    line(buf);

    std::snprintf(buf, sizeof(buf), "FRAME: %u.%uKB",
                  static_cast<unsigned>(net.lastFrameCompressedBytes() / 1024),
                  static_cast<unsigned>((net.lastFrameCompressedBytes() % 1024) * 10 / 1024));
    line(buf);
}

void renderDiscoverySearching(SDL_Renderer* renderer, const std::string& clientVersion) {
    SDL_SetRenderDrawColor(renderer, 20, 20, 24, 255);
    SDL_RenderClear(renderer);
    renderCenteredBitmapText(renderer, "SEARCHING FOR HOST...",
                              static_cast<float>(kWindowHeight) / 2.0f - 20.0f, 4,
                              SDL_Color{200, 200, 200, 255});
    renderCenteredBitmapText(renderer, "MAKE SURE A DUALDECK HOST IS RUNNING ON THIS NETWORK",
                              static_cast<float>(kWindowHeight) / 2.0f + 40.0f, 2,
                              SDL_Color{140, 140, 140, 255});
    renderCenteredBitmapText(renderer, kMenuComboHint, static_cast<float>(kWindowHeight) - 80.0f, 2,
                              SDL_Color{140, 140, 140, 255});
    renderClientVersionStamp(renderer, clientVersion);
    SDL_RenderPresent(renderer);
}

void renderConnecting(SDL_Renderer* renderer, const std::string& hostAddress) {
    SDL_SetRenderDrawColor(renderer, 20, 20, 24, 255);
    SDL_RenderClear(renderer);
    renderCenteredBitmapText(renderer, "CONNECTING TO " + hostAddress + "...",
                              static_cast<float>(kWindowHeight) / 2.0f - 20.0f, 4,
                              SDL_Color{220, 200, 80, 255});
    renderCenteredBitmapText(renderer, "PLEASE WAIT WHILE THE HOST RESPONDS",
                              static_cast<float>(kWindowHeight) / 2.0f + 40.0f, 2,
                              SDL_Color{140, 140, 140, 255});
    renderCenteredBitmapText(renderer, kMenuComboHint, static_cast<float>(kWindowHeight) - 80.0f, 2,
                              SDL_Color{140, 140, 140, 255});
    SDL_RenderPresent(renderer);
}

// GitHub issue #4 Phase E: shown in place of the video texture whenever
// net.hostMode() == HostMode::HostControl -- there is no video to show
// (HostControlAdapter::getLatestFrame() always returns false, see
// host/remote-server/src/host_control_adapter.cpp), but ControllerState
// packets are still being sent every frame exactly as in Emulation mode
// (the render loop below doesn't gate that on mode), so a controller
// plugged into a real host will already be navigating that host's own UI
// via HostControlAdapter's virtual gamepad while this screen is up.
void renderHostControlScreen(SDL_Renderer* renderer, const std::string& identity) {
    SDL_SetRenderDrawColor(renderer, 20, 24, 20, 255);
    SDL_RenderClear(renderer);
    renderCenteredBitmapText(renderer, "HOST CONTROL", static_cast<float>(kWindowHeight) / 2.0f - 60.0f, 4,
                              SDL_Color{120, 210, 150, 255});
    renderCenteredBitmapText(renderer, "USE YOUR CONTROLLER TO NAVIGATE THE HOST",
                              static_cast<float>(kWindowHeight) / 2.0f, 2, SDL_Color{200, 200, 200, 255});
    renderCenteredBitmapText(renderer, "AN EMULATION SESSION WILL APPEAR HERE AUTOMATICALLY WHEN ONE STARTS",
                              static_cast<float>(kWindowHeight) / 2.0f + 34.0f, 2, SDL_Color{140, 140, 140, 255});
    if (!identity.empty()) {
        renderCenteredBitmapText(renderer, identity, static_cast<float>(kWindowHeight) / 2.0f + 80.0f, 2,
                                  SDL_Color{150, 170, 210, 255});
    }
    renderCenteredBitmapText(renderer, kMenuComboHint, static_cast<float>(kWindowHeight) - 80.0f, 2,
                              SDL_Color{140, 140, 140, 255});
    SDL_RenderPresent(renderer);
}

void renderDiscoveryList(SDL_Renderer* renderer, const std::vector<DiscoveredHost>& hosts,
                          int selectedIndex, const std::string& clientVersion) {
    SDL_SetRenderDrawColor(renderer, 20, 20, 24, 255);
    SDL_RenderClear(renderer);
    renderCenteredBitmapText(renderer, "SELECT A HOST", 60.0f, 4, SDL_Color{220, 220, 220, 255});

    // Two lines per host (GitHub issue #28: "Update the Client
    // host-selection UI to display the host, emulated system, and
    // actual emulator"): the host name/address as before, plus a second
    // line naming the emulated system and adapter reported in its
    // DiscoveryResponse, e.g. "NINTENDO DS - MELONDS" ("-" stands in for
    // issue #28's middle dot, which the bitmap font has no glyph for),
    // matching the connected-session menu's identity line (see
    // identityLine() in main()).
    constexpr float kRowHeight = 84.0f;
    constexpr int kPixelSize = 3;
    constexpr int kIdentityPixelSize = 2;
    constexpr float kStartY = 180.0f;

    for (size_t i = 0; i < hosts.size(); ++i) {
        float rowY = kStartY + static_cast<float>(i) * kRowHeight;
        std::string addressAndPort = hosts[i].address + ":" + std::to_string(hosts[i].controlPort);
        std::string label = hosts[i].hostName.empty()
                                 ? addressAndPort
                                 : hosts[i].hostName + "  (" + addressAndPort + ")";
        std::string identity = hosts[i].system.systemName;
        if (!hosts[i].adapter.adapterName.empty()) {
            identity += (identity.empty() ? "" : " - ") + hosts[i].adapter.adapterName;
        }
        bool selected = static_cast<int>(i) == selectedIndex;
        SDL_Color color = selected ? SDL_Color{90, 200, 120, 255} : SDL_Color{200, 200, 200, 255};
        SDL_Color identityColor = selected ? SDL_Color{150, 210, 170, 255} : SDL_Color{140, 140, 140, 255};

        if (selected) {
            int width = std::max(measureBitmapText(label, kPixelSize),
                                  measureBitmapText(identity, kIdentityPixelSize));
            float x = (static_cast<float>(kWindowWidth) - static_cast<float>(width)) / 2.0f;
            SDL_FRect highlight{x - 20.0f, rowY - 8.0f, static_cast<float>(width) + 40.0f,
                                 static_cast<float>(kFontGlyphHeight * kPixelSize) +
                                     static_cast<float>(kFontGlyphHeight * kIdentityPixelSize) + 26.0f};
            SDL_SetRenderDrawColor(renderer, 50, 70, 55, 255);
            SDL_RenderFillRect(renderer, &highlight);
        }
        renderCenteredBitmapText(renderer, label, rowY, kPixelSize, color);
        if (!identity.empty()) {
            renderCenteredBitmapText(renderer, identity, rowY + static_cast<float>(kFontGlyphHeight * kPixelSize) + 10.0f,
                                      kIdentityPixelSize, identityColor);
        }
    }

    renderCenteredBitmapText(renderer, "D-PAD TO MOVE, A TO SELECT",
                              static_cast<float>(kWindowHeight) - 100.0f, 2,
                              SDL_Color{140, 140, 140, 255});
    renderCenteredBitmapText(renderer, kMenuComboHint, static_cast<float>(kWindowHeight) - 60.0f, 2,
                              SDL_Color{140, 140, 140, 255});
    renderClientVersionStamp(renderer, clientVersion);
    SDL_RenderPresent(renderer);
}

void renderPauseMenu(SDL_Renderer* renderer, const std::vector<std::string>& items, int selectedIndex,
                     const std::string& title, const std::string& statusLine, float micLevel,
                     const std::string& subtitle) {
    SDL_SetRenderDrawColor(renderer, 20, 20, 24, 220);
    SDL_RenderClear(renderer);
    renderCenteredBitmapText(renderer, title, 100.0f, 4, SDL_Color{220, 220, 220, 255});
    // Active emulated system/adapter (GitHub issue #28), e.g.
    // "NINTENDO DS - MELONDS" -- shown right under the title in a
    // neutral color, distinct from statusLine below (which is reserved
    // for error/warning text in red).
    if (!subtitle.empty()) {
        renderCenteredBitmapText(renderer, subtitle, 148.0f, 2, SDL_Color{150, 170, 210, 255});
    }

    constexpr float kRowHeight = 70.0f;
    constexpr int kPixelSize = 4;
    float startY = static_cast<float>(kWindowHeight) / 2.0f -
                    (static_cast<float>(items.size()) * kRowHeight) / 2.0f;

    for (size_t i = 0; i < items.size(); ++i) {
        float rowY = startY + static_cast<float>(i) * kRowHeight;
        bool selected = static_cast<int>(i) == selectedIndex;
        SDL_Color color = selected ? SDL_Color{90, 200, 120, 255} : SDL_Color{200, 200, 200, 255};

        if (selected) {
            int width = measureBitmapText(items[i], kPixelSize);
            float x = (static_cast<float>(kWindowWidth) - static_cast<float>(width)) / 2.0f;
            SDL_FRect highlight{x - 24.0f, rowY - 10.0f, static_cast<float>(width) + 48.0f,
                                 static_cast<float>(kFontGlyphHeight * kPixelSize) + 20.0f};
            SDL_SetRenderDrawColor(renderer, 50, 70, 55, 255);
            SDL_RenderFillRect(renderer, &highlight);
        }
        renderCenteredBitmapText(renderer, items[i], rowY, kPixelSize, color);
    }

    // Live microphone input-level meter (GitHub issue #2), shown only on
    // screens that pass a real level (>= 0) -- the settings screen while
    // the host supports mic input. Drawn below the menu rows regardless
    // of mute state, so muting is visibly distinct from "no signal at
    // all" (the bar keeps moving with real input; only the host stops
    // receiving it).
    if (micLevel >= 0.0f) {
        float meterY = startY + static_cast<float>(items.size()) * kRowHeight + 30.0f;
        constexpr float kMeterWidth = 420.0f;
        constexpr float kMeterHeight = 28.0f;
        float meterX = (static_cast<float>(kWindowWidth) - kMeterWidth) / 2.0f;

        renderCenteredBitmapText(renderer, "MIC LEVEL", meterY - 34.0f, 2, SDL_Color{140, 140, 140, 255});

        SDL_FRect meterBg{meterX, meterY, kMeterWidth, kMeterHeight};
        SDL_SetRenderDrawColor(renderer, 45, 45, 50, 255);
        SDL_RenderFillRect(renderer, &meterBg);

        float clampedLevel = std::clamp(micLevel, 0.0f, 1.0f);
        SDL_FRect meterFill{meterX, meterY, kMeterWidth * clampedLevel, kMeterHeight};
        SDL_Color fillColor = clampedLevel > 0.9f ? SDL_Color{220, 90, 90, 255} : SDL_Color{90, 200, 120, 255};
        SDL_SetRenderDrawColor(renderer, fillColor.r, fillColor.g, fillColor.b, fillColor.a);
        SDL_RenderFillRect(renderer, &meterFill);

        SDL_SetRenderDrawColor(renderer, 140, 140, 140, 255);
        SDL_RenderRect(renderer, &meterBg);
    }

    if (!statusLine.empty()) {
        renderCenteredBitmapText(renderer, statusLine, static_cast<float>(kWindowHeight) - 125.0f, 2,
                                  SDL_Color{220, 90, 90, 255});
    }
    renderCenteredBitmapText(renderer, "D-PAD TO MOVE, A TO SELECT, B TO GO BACK",
                              static_cast<float>(kWindowHeight) - 80.0f, 2,
                              SDL_Color{140, 140, 140, 255});
    SDL_RenderPresent(renderer);
}

} // namespace dualdeck::client
