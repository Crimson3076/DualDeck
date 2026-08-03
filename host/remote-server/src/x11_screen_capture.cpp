#include "host/x11_screen_capture.h"

#include <cstdio>

#if defined(DUALDECK_HAVE_X11_SCREEN_CAPTURE)
#include <sys/ipc.h>
#include <sys/shm.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xfixes.h>

#include <cstring>
#endif

namespace melonds_remote::host {

#if defined(DUALDECK_HAVE_X11_SCREEN_CAPTURE)

struct X11ScreenCapture::Impl {
    Display* display = nullptr;
    Window root = 0;
    XShmSegmentInfo shmInfo{};
    XImage* image = nullptr;
    uint16_t width = 0;
    uint16_t height = 0;
    bool ready = false;
#if defined(DUALDECK_HAVE_X11_XFIXES_CURSOR)
    // See capture()'s own comment -- checked once at construction
    // (Xfixes is essentially universal, but this still degrades
    // gracefully to "capture works, cursor just isn't drawn" rather
    // than failing the whole feature if some unusual X server lacks
    // it, matching this class's existing XShm-availability handling).
    bool haveXfixes = false;
#endif

    // Tears down whatever setup got as far as succeeding, in reverse
    // order -- every intermediate failure branch in the constructor
    // below already unwinds itself explicitly instead of relying on
    // this (setup order matters: XShmAttach() must not be called
    // without a valid shmat() result, etc.), so this destructor only
    // ever has real work to do after a fully successful construction.
    ~Impl() {
        if (image) {
            if (display) ::XShmDetach(display, &shmInfo);
            XDestroyImage(image);
        }
        if (shmInfo.shmaddr != nullptr && shmInfo.shmaddr != reinterpret_cast<char*>(-1)) {
            ::shmdt(shmInfo.shmaddr);
        }
        if (display) ::XCloseDisplay(display);
    }
};

X11ScreenCapture::X11ScreenCapture() : impl_(std::make_unique<Impl>()) {
    impl_->display = ::XOpenDisplay(nullptr);
    if (!impl_->display) {
        // Expected and silent-by-design on a Wayland-only session with
        // no XWayland, or no DISPLAY set (e.g. a genuinely headless
        // host) -- the constructor's caller (HostControlAdapter) is the
        // one that logs, conditioned on whether mirroring was actually
        // requested via its own env var, so this class stays quiet here.
        return;
    }

    int shmMajor = 0;
    int shmMinor = 0;
    Bool pixmaps = False;
    if (!::XShmQueryVersion(impl_->display, &shmMajor, &shmMinor, &pixmaps)) {
        std::fprintf(stderr, "X11ScreenCapture: XShm extension not available -- screen mirroring disabled\n");
        ::XCloseDisplay(impl_->display);
        impl_->display = nullptr;
        return;
    }

    int screen = DefaultScreen(impl_->display);
    impl_->root = RootWindow(impl_->display, screen);
    int width = DisplayWidth(impl_->display, screen);
    int height = DisplayHeight(impl_->display, screen);
    if (width <= 0 || height <= 0) {
        std::fprintf(stderr,
                      "X11ScreenCapture: root window reported non-positive size -- screen mirroring disabled\n");
        ::XCloseDisplay(impl_->display);
        impl_->display = nullptr;
        return;
    }

    Visual* visual = DefaultVisual(impl_->display, screen);
    int depth = DefaultDepth(impl_->display, screen);
    impl_->image = ::XShmCreateImage(impl_->display, visual, static_cast<unsigned int>(depth), ZPixmap, nullptr,
                                      &impl_->shmInfo, static_cast<unsigned int>(width),
                                      static_cast<unsigned int>(height));
    if (!impl_->image) {
        std::fprintf(stderr, "X11ScreenCapture: XShmCreateImage failed -- screen mirroring disabled\n");
        ::XCloseDisplay(impl_->display);
        impl_->display = nullptr;
        return;
    }

    impl_->shmInfo.shmid =
        ::shmget(IPC_PRIVATE, static_cast<size_t>(impl_->image->bytes_per_line) * static_cast<size_t>(height),
                 IPC_CREAT | 0600);
    if (impl_->shmInfo.shmid < 0) {
        std::fprintf(stderr, "X11ScreenCapture: shmget failed -- screen mirroring disabled\n");
        XDestroyImage(impl_->image);
        impl_->image = nullptr;
        ::XCloseDisplay(impl_->display);
        impl_->display = nullptr;
        return;
    }
    impl_->shmInfo.shmaddr = impl_->image->data = static_cast<char*>(::shmat(impl_->shmInfo.shmid, nullptr, 0));
    // Marked for removal immediately -- the standard XShm idiom: the
    // segment stays valid for this process's own attachment (detached
    // in ~Impl(), or automatically on process exit) even after this
    // call, it just stops being something a *new* process could attach
    // to, so nothing leaks even if this process is killed uncleanly.
    ::shmctl(impl_->shmInfo.shmid, IPC_RMID, nullptr);
    if (impl_->shmInfo.shmaddr == reinterpret_cast<char*>(-1)) {
        std::fprintf(stderr, "X11ScreenCapture: shmat failed -- screen mirroring disabled\n");
        impl_->shmInfo.shmaddr = nullptr;
        XDestroyImage(impl_->image);
        impl_->image = nullptr;
        ::XCloseDisplay(impl_->display);
        impl_->display = nullptr;
        return;
    }
    impl_->shmInfo.readOnly = False;
    if (!::XShmAttach(impl_->display, &impl_->shmInfo)) {
        std::fprintf(stderr, "X11ScreenCapture: XShmAttach failed -- screen mirroring disabled\n");
        ::shmdt(impl_->shmInfo.shmaddr);
        impl_->shmInfo.shmaddr = nullptr;
        XDestroyImage(impl_->image);
        impl_->image = nullptr;
        ::XCloseDisplay(impl_->display);
        impl_->display = nullptr;
        return;
    }

    // Real, checked assumption, not guessed: this project's raw-frame
    // convention (frame_source.h's kFrameBytesPerPixel) is BGRA8888.
    // XShmCreateImage() with ZPixmap at a TrueColor/DirectColor 24/32-bit
    // depth visual -- by a wide margin the standard case for every
    // modern X11 desktop, and the only case this class supports -- lays
    // out each pixel in memory as B,G,R,X on a little-endian host,
    // which is byte-for-byte BGRA8888 (X is unused padding, harmless
    // the same way DS/3DS frame sources' own unused alpha byte already
    // is). Checked against the actual mask values here rather than
    // assumed outright, disabling this feature rather than silently
    // emitting corrupted color if some genuinely unusual visual (e.g. a
    // 16-bit desktop) is in use.
    bool maskOk = impl_->image->red_mask == 0xFF0000 && impl_->image->green_mask == 0x00FF00 &&
                  impl_->image->blue_mask == 0x0000FF && impl_->image->bits_per_pixel == 32;
    if (!maskOk) {
        std::fprintf(stderr,
                      "X11ScreenCapture: unexpected visual pixel layout (red=0x%lx green=0x%lx blue=0x%lx "
                      "bpp=%d) -- this host's X server isn't the standard 24/32-bit TrueColor case this "
                      "capture path supports, screen mirroring disabled\n",
                      impl_->image->red_mask, impl_->image->green_mask, impl_->image->blue_mask,
                      impl_->image->bits_per_pixel);
        ::XShmDetach(impl_->display, &impl_->shmInfo);
        ::shmdt(impl_->shmInfo.shmaddr);
        impl_->shmInfo.shmaddr = nullptr;
        XDestroyImage(impl_->image);
        impl_->image = nullptr;
        ::XCloseDisplay(impl_->display);
        impl_->display = nullptr;
        return;
    }

    impl_->width = static_cast<uint16_t>(width);
    impl_->height = static_cast<uint16_t>(height);
    impl_->ready = true;

#if defined(DUALDECK_HAVE_X11_XFIXES_CURSOR)
    int xfixesEventBase = 0;
    int xfixesErrorBase = 0;
    impl_->haveXfixes = ::XFixesQueryExtension(impl_->display, &xfixesEventBase, &xfixesErrorBase);
    if (!impl_->haveXfixes) {
        std::fprintf(stderr,
                      "X11ScreenCapture: Xfixes extension not available -- screen mirroring will work but "
                      "the mouse cursor won't be visible in it\n");
    }
#endif
}

X11ScreenCapture::~X11ScreenCapture() = default;

bool X11ScreenCapture::isReady() const { return impl_->ready; }

void X11ScreenCapture::nativeSize(uint16_t& outWidth, uint16_t& outHeight) const {
    outWidth = impl_->ready ? impl_->width : 0;
    outHeight = impl_->ready ? impl_->height : 0;
}

#if defined(DUALDECK_HAVE_X11_XFIXES_CURSOR)
// Composites the current XFixes cursor image onto a captured BGRA8888
// frame buffer, in place -- see this class's own comment on why
// XShmGetImage() alone never includes the cursor at all.
// XFixesCursorImage::pixels stores each pixel as a 32-bit premultiplied-
// alpha ARGB value (0xAARRGGBB) in the low 32 bits of each `unsigned
// long` array entry (matching the RENDER extension's own pixel format
// convention, per Xfixes's protocol spec) -- any upper bits present on
// a 64-bit `unsigned long` are unused/zero. `cursor->x`/`cursor->y` is
// where the cursor's hotspot currently is on screen;
// `cursor->xhot`/`yhot` is the hotspot's own offset within the cursor
// image, so the image's top-left screen position is
// (x - xhot, y - yhot). Clips against the frame's own bounds since the
// cursor can legitimately hang partway off any edge of the screen.
// Premultiplied-alpha "over" compositing needs no clamping: for a
// correctly formed cursor image, src's own channels are already <= its
// alpha, so src + dst*(1-a) can't exceed 255.
void compositeXfixesCursor(uint8_t* frame, int frameWidth, int frameHeight, const XFixesCursorImage* cursor) {
    const int originX = cursor->x - cursor->xhot;
    const int originY = cursor->y - cursor->yhot;
    for (int cy = 0; cy < cursor->height; ++cy) {
        const int screenY = originY + cy;
        if (screenY < 0 || screenY >= frameHeight) continue;
        for (int cx = 0; cx < cursor->width; ++cx) {
            const int screenX = originX + cx;
            if (screenX < 0 || screenX >= frameWidth) continue;
            unsigned long packed = cursor->pixels[static_cast<size_t>(cy) * cursor->width + cx];
            uint8_t a = static_cast<uint8_t>((packed >> 24) & 0xFF);
            if (a == 0) continue; // fully transparent, nothing to blend
            uint8_t r = static_cast<uint8_t>((packed >> 16) & 0xFF);
            uint8_t g = static_cast<uint8_t>((packed >> 8) & 0xFF);
            uint8_t b = static_cast<uint8_t>(packed & 0xFF);
            uint8_t* dst = frame + (static_cast<size_t>(screenY) * frameWidth + screenX) * 4;
            const int inv = 255 - a;
            dst[0] = static_cast<uint8_t>(b + (dst[0] * inv) / 255);
            dst[1] = static_cast<uint8_t>(g + (dst[1] * inv) / 255);
            dst[2] = static_cast<uint8_t>(r + (dst[2] * inv) / 255);
        }
    }
}
#endif

bool X11ScreenCapture::capture(std::vector<uint8_t>& outFrame, uint16_t& outWidth, uint16_t& outHeight) {
    if (!impl_->ready) return false;
    if (!::XShmGetImage(impl_->display, impl_->root, impl_->image, 0, 0, AllPlanes)) {
        return false;
    }

    // Copied row by row into a tightly-packed width*4 buffer rather
    // than a single bulk memcpy of the whole image: XImage::bytes_per_line
    // is whatever stride the X server actually used, which for a
    // 32-bit ZPixmap is virtually always exactly width*4 (no padding
    // needed at 4-byte pixels) but isn't guaranteed to be -- and
    // net_server.cpp's compressFrameBgraToJpeg() calls tjCompress2()
    // with pitch=0 ("tightly packed"), so a wider server-side stride
    // would silently skew every row after the first if copied verbatim.
    const size_t rowBytes = static_cast<size_t>(impl_->width) * 4;
    const size_t stride = static_cast<size_t>(impl_->image->bytes_per_line);
    outFrame.resize(rowBytes * impl_->height);
    for (int y = 0; y < impl_->height; ++y) {
        std::memcpy(outFrame.data() + static_cast<size_t>(y) * rowBytes,
                    impl_->image->data + static_cast<size_t>(y) * stride, rowBytes);
    }

#if defined(DUALDECK_HAVE_X11_XFIXES_CURSOR)
    if (impl_->haveXfixes) {
        XFixesCursorImage* cursor = ::XFixesGetCursorImage(impl_->display);
        if (cursor) {
            compositeXfixesCursor(outFrame.data(), impl_->width, impl_->height, cursor);
            ::XFree(cursor);
        }
    }
#endif

    outWidth = impl_->width;
    outHeight = impl_->height;
    return true;
}

#else // !DUALDECK_HAVE_X11_SCREEN_CAPTURE

struct X11ScreenCapture::Impl {};

X11ScreenCapture::X11ScreenCapture() : impl_(std::make_unique<Impl>()) {}
X11ScreenCapture::~X11ScreenCapture() = default;
bool X11ScreenCapture::isReady() const { return false; }
void X11ScreenCapture::nativeSize(uint16_t& outWidth, uint16_t& outHeight) const {
    outWidth = 0;
    outHeight = 0;
}
bool X11ScreenCapture::capture(std::vector<uint8_t>&, uint16_t&, uint16_t&) { return false; }

#endif

} // namespace melonds_remote::host
