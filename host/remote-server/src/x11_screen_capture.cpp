#include "host/x11_screen_capture.h"

#include <cstdio>

#if defined(DUALDECK_HAVE_X11_SCREEN_CAPTURE)
#include <sys/ipc.h>
#include <sys/shm.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>

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
}

X11ScreenCapture::~X11ScreenCapture() = default;

bool X11ScreenCapture::isReady() const { return impl_->ready; }

void X11ScreenCapture::nativeSize(uint16_t& outWidth, uint16_t& outHeight) const {
    outWidth = impl_->ready ? impl_->width : 0;
    outHeight = impl_->ready ? impl_->height : 0;
}

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
