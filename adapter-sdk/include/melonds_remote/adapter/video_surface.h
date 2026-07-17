#pragma once

// Generic video surface description (GitHub issue #28's "Emulator
// Adapter Contract": "One or more video surfaces", and "Generic
// protocol model" -> "Video surfaces": replace the fixed single DS
// framebuffer assumption with a list of described surfaces). Part of
// the Phase 1 contract groundwork -- see
// docs/adr/0001-host-service-and-adapter-architecture.md. Not yet used
// by the live client/host wire protocol (still the fixed 256x192 DS
// bottom screen, protocol.h's VideoFrame) -- that migration is later
// work, see the ADR's "What this milestone does not do" section.

#include <cstdint>
#include <string>

namespace melonds_remote::adapter {

// Semantic role a surface plays, so client-side layout code can pick a
// sensible default (e.g. "host shows top/TV, client shows bottom/
// GamePad") without knowing about a specific emulated system.
enum class SurfaceRole {
    Top,       // DS/3DS top screen
    Bottom,    // DS/3DS bottom (usually touch) screen
    Tv,        // Wii U TV output
    GamePad,   // Wii U GamePad screen
    Auxiliary, // anything not covered above (reserved for future adapters)
};

// Only one pixel format exists today (matching protocol.h's existing
// VideoFrame payload, BGRA8888 -- see docs/protocol.md's "Video
// payload" section for the exact in-memory byte order and why). Kept as
// an enum, not hardcoded, so a future adapter needing a different
// format doesn't require changing this struct's shape.
enum class PixelFormat {
    Bgra8888,
};

enum class Orientation {
    Landscape,
    Portrait,
};

struct VideoSurfaceDescriptor {
    // Stable within a session, e.g. "top", "bottom", "tv", "gamepad".
    // Not free text for matching purposes -- code may compare this
    // across frames of the same session to route a SurfaceFrame (see
    // adapter_contract.h) to the right destination.
    std::string surfaceId;
    SurfaceRole role = SurfaceRole::Bottom;
    uint16_t width = 0;
    uint16_t height = 0;
    PixelFormat pixelFormat = PixelFormat::Bgra8888;
    Orientation orientation = Orientation::Landscape;

    // Touch support and coordinate range (GitHub issue #28: "Touch
    // surfaces and coordinate ranges"). touchRangeX/Y are the maximum
    // valid coordinate (inclusive), matching protocol.h's existing
    // kTouchMaxX/kTouchMaxY convention -- only meaningful when
    // touchSupported is true.
    bool touchSupported = false;
    uint16_t touchRangeX = 0;
    uint16_t touchRangeY = 0;

    // Whether a session is considered failed if this surface never
    // registers a frame (issue #28: "required, optional, locally
    // displayed, remotely displayed, or selectable").
    bool required = true;
    bool locallyDisplayed = false;  // shown on the host's own screen
    bool remotelyDisplayed = false; // streamed to the DualDeck client
    bool selectable = false;        // user can choose to switch to/away from this surface

    uint16_t nominalFps = 60;
    uint16_t maxFps = 60;
};

} // namespace melonds_remote::adapter
