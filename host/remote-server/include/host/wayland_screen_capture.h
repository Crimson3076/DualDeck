#pragma once

// Real Wayland counterpart to X11ScreenCapture (see that header's own
// comment for the full "why this feature exists" context -- real user
// request, 2026-08-03, "add an option to the client's host control to
// mirror the screen"). Both of the user's actual test machines turned
// out to be Wayland sessions, confirmed real-hardware after the X11-only
// first cut showed a uniform grey (XWayland's empty compositing root,
// not a crash -- see docs/known-limitations.md's 2026-08-03 entry on
// that finding), so this is the follow-up: the only Wayland capture
// approach that's portable across both GNOME/Mutter and KDE/KWin (the
// user's two actual desktop environments) is the xdg-desktop-portal
// ScreenCast interface over PipeWire -- wlr-screencopy-unstable-v1
// would be simpler (no permission dialog, far less code) but only
// exists on wlroots-based compositors (Sway, Hyprland, gamescope), not
// either of the desktops actually being tested against here.
//
// Same PIMPL rationale as X11ScreenCapture: this header must stay
// includable, and this class safely inert, even in a build with no
// DBus/PipeWire dev headers at all. See host/remote-server/CMakeLists.txt's
// DBUS1_FOUND/PIPEWIRE_FOUND guard and this class's own .cpp file's
// DUALDECK_HAVE_WAYLAND_SCREEN_CAPTURE-gated real-vs-stub split.
//
// Unlike X11ScreenCapture (one synchronous XShmGetImage call per
// capture()), this class owns a persistent background thread running a
// PipeWire event loop for its whole lifetime -- PipeWire delivers
// frames asynchronously via callback, not on demand, so capture()
// just reads out whatever the background thread most recently
// received under a mutex. Real, unavoidable operational detail worth
// stating plainly, not hidden: the very first time a given host
// activates this feature, the portal shows an interactive one-time
// permission prompt *on the host's own screen* (a real GNOME/KDE
// system dialog, not anything this project draws) -- that's the
// portal's actual security boundary working as designed, not a bug.
// Requested with persist_mode so that grant is remembered and no
// further prompts appear on later launches.

#include <cstdint>
#include <memory>
#include <vector>

namespace melonds_remote::host {

class WaylandScreenCapture {
public:
    WaylandScreenCapture();
    ~WaylandScreenCapture();

    WaylandScreenCapture(const WaylandScreenCapture&) = delete;
    WaylandScreenCapture& operator=(const WaylandScreenCapture&) = delete;

    // False if this build has no DBus/PipeWire support compiled in, the
    // session bus isn't reachable, the portal's ScreenCast interface
    // isn't implemented on this host (no backend installed/running for
    // the current desktop), the user denied or dismissed the one-time
    // permission prompt, or the resulting PipeWire stream never
    // reaches a negotiated format within the constructor's bounded
    // wait. capture() is a safe no-op in every such case, matching
    // X11ScreenCapture's/HostControlAdapter::isDeviceReady()'s
    // "degrade gracefully, log once, never crash" convention.
    bool isReady() const;

    // The negotiated stream's dimensions -- unlike X11ScreenCapture's
    // fixed root-window size, this is only known once PipeWire actually
    // negotiates a format with the portal-provided source, so it's 0/0
    // until the first real frame's format is known (see capture()'s own
    // comment -- practically this resolves during the constructor's
    // bounded wait, before isReady() would ever report true).
    void nativeSize(uint16_t& outWidth, uint16_t& outHeight) const;

    // Fills outFrame with the most recently received frame, converted
    // to raw, tightly-packed BGRA8888 (matching kFrameBytesPerPixel/
    // frame_source.h's documented format -- PipeWire negotiates
    // whatever format the compositor's capture source actually
    // produces, most commonly BGRx; converted here rather than assumed
    // to already match). Never blocks -- reads out of a mutex-protected
    // "latest frame" slot the background PipeWire thread keeps updated,
    // the same "latest-frame-wins" contract every other frame source in
    // this project follows. Returns false (outFrame left untouched) if
    // no frame has been received yet.
    bool capture(std::vector<uint8_t>& outFrame, uint16_t& outWidth, uint16_t& outHeight);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace melonds_remote::host
