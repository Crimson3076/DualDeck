#pragma once

// Real user request, 2026-08-03: "I want to add an option to the
// client's host control to mirror the screen, as I cannot access the TV
// I am testing on currently." Host Control mode has never sent any
// video (see HostControlAdapter::getLatestFrame()'s original comment:
// "there is no emulated screen while no emulator is running") -- true,
// but not the whole story once the ask became "show me the host's
// actual desktop/Big Picture," not an emulated framebuffer. This is a
// real, deliberately narrow first version (see docs/known-limitations.md's
// 2026-08-03 entry, and the user's own explicit "quick and good enough"
// choice over investing in a smoother capture path right away): X11
// only, via XShm -- not Wayland. Capturing an arbitrary compositor's
// output under Wayland needs either the wlr-screencopy protocol or the
// xdg-desktop-portal ScreenCast/PipeWire API, both substantially more
// code and neither verifiable without real Wayland hardware in hand.
// Detects and degrades cleanly (isReady() == false) rather than
// guessing at an unverified environment.
//
// PIMPL'd deliberately: this header must stay includable (and this
// class constructible/safely inert) even in a build where X11 dev
// headers aren't available at all -- see host/remote-server/
// CMakeLists.txt's X11_FOUND guard and this class's own .cpp file,
// which compiles a real implementation when DUALDECK_HAVE_X11_SCREEN_CAPTURE
// is defined and an always-not-ready stub otherwise. No X11 type ever
// appears in this header, so host_control_adapter.h/.cpp (and anything
// else that includes this) never needs to know or care whether this
// particular build actually has X11 support compiled in.

#include <cstdint>
#include <memory>
#include <vector>

namespace melonds_remote::host {

class X11ScreenCapture {
public:
    X11ScreenCapture();
    ~X11ScreenCapture();

    X11ScreenCapture(const X11ScreenCapture&) = delete;
    X11ScreenCapture& operator=(const X11ScreenCapture&) = delete;

    // False if this build has no X11 support compiled in, there's no
    // reachable X11 display (e.g. a Wayland-only session with no
    // XWayland, or no DISPLAY set at all), the XShm extension isn't
    // available, or this host's default visual isn't the standard
    // 24/32-bit TrueColor case this class supports -- capture() is a
    // safe no-op in every such case, matching this codebase's "degrade
    // gracefully, log once, never crash" convention (see
    // HostControlAdapter::isDeviceReady()'s comment for the same
    // pattern applied to /dev/uinput).
    bool isReady() const;

    // The fixed dimensions capture() will report every time (the root
    // window's own size, queried once at construction) -- 0/0 if
    // !isReady().
    void nativeSize(uint16_t& outWidth, uint16_t& outHeight) const;

    // Fills outFrame with the current root window's pixels as raw,
    // tightly-packed BGRA8888 (matching kFrameBytesPerPixel/
    // frame_source.h's documented format -- the unused byte per pixel
    // is harmless padding, same as every other frame source in this
    // project), outWidth/outHeight with nativeSize()'s dimensions.
    // Never blocks beyond a single synchronous XShmGetImage round-trip
    // (typically low-single-digit milliseconds) -- the caller
    // (HostControlAdapter::getLatestFrame()) is responsible for rate-
    // limiting how often this is actually invoked; this class has no
    // timing policy of its own. Returns false (outFrame left
    // untouched) on any capture failure; never throws.
    bool capture(std::vector<uint8_t>& outFrame, uint16_t& outWidth, uint16_t& outHeight);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace melonds_remote::host
