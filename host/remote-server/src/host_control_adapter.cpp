#include "host/host_control_adapter.h"

#include <fcntl.h>
#include <linux/uinput.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace melonds_remote::host {

// Fallback downscale target when no client has reported a real display
// size yet (targetDisplayWidth_/Height_ still 0) -- the Steam Deck LCD's
// native resolution, this project's primary target device. Only used as
// a starting point until the first real Hello handshake arrives; see
// HostControlAdapter::setTargetDisplaySize().
constexpr uint16_t kFallbackTargetDisplayWidth = 1280;
constexpr uint16_t kFallbackTargetDisplayHeight = 800;

// Downscales one BGRA8888 frame via box-filter (area-average) resampling
// -- real user report, 2026-08-03: streaming a 4096x2160 desktop
// mirror to a Steam Deck-sized display was consistently sluggish even
// though no single stage (host CPU, network, reported encode latency)
// was individually saturated; the user's own investigation concluded
// the full native desktop resolution was being captured, compressed,
// transmitted, and decoded every frame for zero visual benefit, since
// the client can't show more detail than its own display anyway. Box-
// filter (rather than nearest-neighbor) because this is desktop content
// -- fine text/UI edges alias visibly under simple point-sampling at
// the ~3x+ reduction ratios this feature targets (e.g. 4096x2160 ->
// ~1280x675). Deliberately hand-written rather than pulling in a new
// scaling library (libyuv/swscale) for what's currently this one call
// site -- O(srcW*srcH) total work, the same order as the memcpy this
// replaces, negligible next to JPEG compression's own cost.
void downscaleBgra8888(const uint8_t* src, int srcWidth, int srcHeight, std::vector<uint8_t>& dst, int dstWidth,
                        int dstHeight) {
    dst.resize(static_cast<size_t>(dstWidth) * dstHeight * 4);
    for (int dy = 0; dy < dstHeight; ++dy) {
        int srcY0 = dy * srcHeight / dstHeight;
        int srcY1 = std::max(srcY0 + 1, (dy + 1) * srcHeight / dstHeight);
        for (int dx = 0; dx < dstWidth; ++dx) {
            int srcX0 = dx * srcWidth / dstWidth;
            int srcX1 = std::max(srcX0 + 1, (dx + 1) * srcWidth / dstWidth);
            uint32_t sums[4] = {0, 0, 0, 0};
            int count = 0;
            for (int sy = srcY0; sy < srcY1; ++sy) {
                const uint8_t* row = src + static_cast<size_t>(sy) * srcWidth * 4;
                for (int sx = srcX0; sx < srcX1; ++sx) {
                    const uint8_t* px = row + static_cast<size_t>(sx) * 4;
                    sums[0] += px[0];
                    sums[1] += px[1];
                    sums[2] += px[2];
                    sums[3] += px[3];
                    ++count;
                }
            }
            uint8_t* outPx = dst.data() + (static_cast<size_t>(dy) * dstWidth + dx) * 4;
            outPx[0] = static_cast<uint8_t>(sums[0] / count);
            outPx[1] = static_cast<uint8_t>(sums[1] / count);
            outPx[2] = static_cast<uint8_t>(sums[2] / count);
            outPx[3] = static_cast<uint8_t>(sums[3] / count);
        }
    }
}

// Computes the largest size that fits within (maxWidth, maxHeight)
// while preserving (srcWidth, srcHeight)'s aspect ratio, never
// exceeding the source size (this is a downscale-only fit -- upscaling
// a capture before JPEG compression would waste work for zero benefit,
// since the client's own render path already scales-to-fit whatever
// size frame actually arrives).
void fitDownscaleTarget(int srcWidth, int srcHeight, int maxWidth, int maxHeight, int& outWidth, int& outHeight) {
    double scale = std::min({1.0, static_cast<double>(maxWidth) / srcWidth, static_cast<double>(maxHeight) / srcHeight});
    outWidth = std::max(1, static_cast<int>(srcWidth * scale));
    outHeight = std::max(1, static_cast<int>(srcHeight * scale));
}

// Plain `-axis` overflows int16_t's range at the negative extreme
// (-(-32768) doesn't fit in 16 bits) -- same clamp-to-range technique as
// client/src/main.cpp's own negateStickAxis(), duplicated here rather than
// shared since the two live in entirely separate binaries/build targets.
int16_t negateStickAxis(int16_t axis) {
    return axis == INT16_MIN ? INT16_MAX : static_cast<int16_t>(-axis);
}

HostControlGamepadState translateControllerState(const ControllerState& state) {
    HostControlGamepadState out;
    // Nintendo label -> PHYSICAL position, because the other side of this
    // is uinput: BTN_SOUTH really is the bottom button, and desktop/Steam
    // UI treats it as "confirm". The DS puts B at the bottom and A on the
    // right, the mirror of the Xbox arrangement uinput names describe --
    // so a label-for-label copy here (A -> south) lands confirm on the
    // wrong physical button.
    //
    // Deliberately NOT symmetric with adapter_bridge.cpp's
    // dsButtonsToGenericButtons(), which keeps A -> South and is correct
    // to do so: its GenericButton_* values are consumed by the emulator
    // adapters, which map straight back (South -> A, see the Azahar
    // patch's button table), making that path a label round-trip. This
    // path has no such round-trip -- it ends at a real virtual gamepad,
    // where position is the only thing that means anything.
    //
    // Companion to the 2026-08-04 client-side fix in client/src/main.cpp:
    // once the client began sending the DS button that matches the
    // physical position pressed, leaving this table alone would have
    // silently moved Host Control's confirm button one position clockwise.
    out.south = (state.dsButtons & DSButton_B) != 0;
    out.east = (state.dsButtons & DSButton_A) != 0;
    out.west = (state.dsButtons & DSButton_Y) != 0;
    out.north = (state.dsButtons & DSButton_X) != 0;
    out.tl = (state.dsButtons & DSButton_L) != 0;
    out.tr = (state.dsButtons & DSButton_R) != 0;
    out.start = (state.dsButtons & DSButton_Start) != 0;
    out.select = (state.dsButtons & DSButton_Select) != 0;

    if (state.dsButtons & DSButton_Left) out.hatX = -1;
    else if (state.dsButtons & DSButton_Right) out.hatX = 1;
    if (state.dsButtons & DSButton_Up) out.hatY = -1;
    else if (state.dsButtons & DSButton_Down) out.hatY = 1;

    out.leftStickX = state.leftStickX;
    // Real user report (Steam Controller Tester), 2026-08-03: "L joystick
    // reads down when going up." client/src/main.cpp's own
    // negateStickAxis() call on leftStickY already flips SDL's raw
    // positive-is-down convention to positive-is-up before this ever hits
    // the wire, specifically to match the 3DS circle pad's
    // GetStickDirectionState() convention (see that call site's own
    // comment, confirmed by reading Azahar's hid.cpp directly) --
    // correct for an emulated analog stick, but this ABS_Y axis feeds a
    // literal uinput virtual Xbox-360-style gamepad, where the universal
    // joystick/evdev/SDL convention is the opposite: positive Y = down.
    // Passing the wire's already-inverted value straight through (as the
    // old code here did, under the "both use the same centered-at-0
    // range, so no rescaling is needed" assumption that's true for X but
    // not for Y's sign) silently double-applies the flip for Host Control
    // mode specifically, inverting every up/down motion Steam's own
    // controller tester displays. Negated back here, undoing the wire's
    // 3DS-oriented flip so this virtual gamepad's Y axis matches standard
    // convention like every other field in this function already does.
    out.leftStickY = negateStickAxis(state.leftStickY);
    // Same Y-sign reasoning as leftStickY above applies identically to the
    // right stick -- client/src/main.cpp negates rightStickY the same way
    // it negates leftStickY, for the same 3DS-circle-pad-convention reason.
    out.rightStickX = state.rightStickX;
    out.rightStickY = negateStickAxis(state.rightStickY);
    out.leftTrigger = state.leftTrigger;
    out.rightTrigger = state.rightTrigger;
    out.thumbL = (state.extraButtons & ExtraButton_ThumbLeft) != 0;
    out.thumbR = (state.extraButtons & ExtraButton_ThumbRight) != 0;
    return out;
}

HostControlMouseState translateMouseState(const ControllerState& state) {
    HostControlMouseState out;
    out.dx = state.mouseDeltaX;
    out.dy = state.mouseDeltaY;
    out.leftDown = (state.mouseButtons & MouseButton_Left) != 0;
    out.rightDown = (state.mouseButtons & MouseButton_Right) != 0;
    return out;
}

namespace {

constexpr int kAbsRangeMin = -32768;
constexpr int kAbsRangeMax = 32767;

// Steam Controller touchpad experiment (see host_control_adapter.h's
// isTouchpadReady() comment for the full honest caveat) -- arbitrary
// position range, needs real-hardware feel-tuning the same way the
// client's own kTouchpadMouseSensitivity does.
constexpr int32_t kTouchpadPosMin = 0;
constexpr int32_t kTouchpadPosMax = 1024;
constexpr int32_t kTouchpadPosCenter = (kTouchpadPosMin + kTouchpadPosMax) / 2;
// Wire deltas (ControllerState::mouseDeltaX/Y) are the same "screen-
// independent relative pixels" unit the EV_REL mouse device already
// consumes unscaled -- this rescales that into the much smaller
// [kTouchpadPosMin, kTouchpadPosMax] touchpad-surface range so a typical
// swipe doesn't instantly pin to an edge.
constexpr float kTouchpadPositionScale = 0.5f;
// Valve's own wired Steam Controller USB IDs -- see the constructor's own
// comment on why this identity is opt-in and what it realistically can't
// achieve. 0x1142 (the wireless dongle's ID) is an alternative worth
// trying on real hardware if 0x1102 doesn't get picked up any better.
constexpr uint16_t kSteamControllerVendor = 0x28de;
constexpr uint16_t kSteamControllerProduct = 0x1102;

// (HostControlGamepadState field pointer, uinput BTN_* code) pairs --
// iterated by both device setup and emitState() so the two can never
// drift out of sync with each other.
struct ButtonMapping {
    bool HostControlGamepadState::*field;
    int code;
};

// Real user report (Steam Controller Tester), 2026-08-03: "X and Y are
// reversed during host control mode." Root cause: <linux/input-event-
// codes.h> defines BTN_X as a plain alias for BTN_NORTH (0x133) and
// BTN_Y as a plain alias for BTN_WEST (0x134) -- confirmed by reading
// /usr/include/linux/input-event-codes.h directly, not assumed. A
// Linux input device's button *index* (what SDL -- and Steam's
// controller tester, which reads through SDL -- actually keys
// gamecontrollerdb.txt's `x:bN`/`y:bN` entries on, for a device
// identified as an Xbox 360 pad) is assigned in ascending raw-code
// order, not UI_SET_KEYBIT call order: BTN_NORTH (0x133) sorts before
// BTN_WEST (0x134). The old code below sent DSButton_X's presses as
// code BTN_WEST (0x134, the higher index) and DSButton_Y's as BTN_NORTH
// (0x133, the lower index) -- backwards from gamecontrollerdb's
// expectation that the lower index is "x", so Steam displayed X presses
// as Y and vice versa. Fixed by using the `BTN_X`/`BTN_Y` aliases
// directly (same numeric codes, 0x133/0x134) on the fields whose wire
// meaning they actually match, so the lower raw code now belongs to the
// X field, matching gamecontrollerdb's assumption.
constexpr ButtonMapping kButtonMappings[] = {
    {&HostControlGamepadState::south, BTN_SOUTH}, {&HostControlGamepadState::east, BTN_EAST},
    {&HostControlGamepadState::west, BTN_X},       {&HostControlGamepadState::north, BTN_Y},
    {&HostControlGamepadState::tl, BTN_TL},       {&HostControlGamepadState::tr, BTN_TR},
    {&HostControlGamepadState::start, BTN_START}, {&HostControlGamepadState::select, BTN_SELECT},
    // Real user report (Steam Controller Tester), 2026-08-03: "same with
    // stick clicking" (not registering at all) -- there was no uinput
    // capability/button mapping for these before this fix.
    {&HostControlGamepadState::thumbL, BTN_THUMBL}, {&HostControlGamepadState::thumbR, BTN_THUMBR},
};

bool writeEvent(int fd, uint16_t type, uint16_t code, int32_t value) {
    input_event ev{};
    ev.type = type;
    ev.code = code;
    ev.value = value;
    return ::write(fd, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev));
}

} // namespace

HostControlAdapter::HostControlAdapter() {
    // Real user request, 2026-08-01 -- see isTouchpadReady()'s comment in
    // the header for the full honest caveat on what this can and can't
    // actually achieve. Read once here, not re-checked per-tick, matching
    // this codebase's existing "env vars are read at construction" style
    // (e.g. MELONDS_REMOTE_ENABLE is only ever read at startup, never
    // polled).
    const char* touchpadEnv = std::getenv("DUALDECK_HOSTCONTROL_STEAM_TOUCHPAD");
    touchpadEnabled_ = touchpadEnv != nullptr && touchpadEnv[0] != '\0' && std::strcmp(touchpadEnv, "0") != 0;

    // Screen-mirror experiment, same opt-in-env-var-read-once style as
    // touchpadEnabled_ above -- see getLatestFrame()'s header comment
    // for the full design. Neither capture backend is constructed (and
    // neither attempts an X11/D-Bus connection) unless this is actually
    // set, so a host that never opts in never even tries to reach
    // either.
    //
    // Real user report, 2026-08-03 (Bazzite, a confirmed Wayland
    // session): journalctl showed "screen-mirror capture ready (X11)"
    // and the client showed solid black -- X11ScreenCapture's isReady()
    // only checks "can I connect to a display and does XShm work,"
    // which XWayland (the X11-compatibility layer every real Wayland
    // session still runs, for legacy X11 app support) happily answers
    // yes to, even though its own root window has no real desktop
    // content composited into it at all -- true content lives in the
    // Wayland compositor, invisible to X11's view entirely. This was
    // already suspected (see the previous version of this comment,
    // which chose to try X11 first anyway purely for cost reasons) but
    // never actually fixed -- X11 kept "succeeding" every time, so the
    // Wayland portal fallback below was never reached in practice on a
    // Wayland session. Fixed by checking WAYLAND_DISPLAY first: a real
    // Wayland session has this set (XWayland's presence is irrelevant to
    // it), so this now tries WaylandScreenCapture *first* in that case,
    // falling back to X11ScreenCapture only if the portal path doesn't
    // pan out -- inverting the previous unconditional "X11 first, it's
    // cheaper" priority specifically when there's a real Wayland session
    // to prefer instead. A real X11-only desktop (WAYLAND_DISPLAY unset)
    // keeps the original X11-first behavior: cheap, synchronous, no
    // portal permission prompt.
    const char* mirrorEnv = std::getenv("DUALDECK_HOSTCONTROL_MIRROR_SCREEN");
    mirrorEnabled_ = mirrorEnv != nullptr && mirrorEnv[0] != '\0' && std::strcmp(mirrorEnv, "0") != 0;
    if (mirrorEnabled_) {
        const char* waylandDisplayEnv = std::getenv("WAYLAND_DISPLAY");
        bool preferWayland = waylandDisplayEnv != nullptr && waylandDisplayEnv[0] != '\0';
        if (preferWayland) {
            std::fprintf(stderr,
                          "HostControlAdapter: WAYLAND_DISPLAY is set -- trying the Wayland portal + "
                          "PipeWire capture path first (X11 would only ever see XWayland's own empty "
                          "compositing root here, not real desktop content).\n");
            mirrorWaylandCapture_ = std::make_unique<WaylandScreenCapture>();
            if (!mirrorWaylandCapture_->isReady()) {
                std::fprintf(stderr,
                              "HostControlAdapter: Wayland portal screen-mirror capture unavailable -- "
                              "falling back to X11 (may only capture XWayland's empty root on this "
                              "session, not real content).\n");
                mirrorX11Capture_ = std::make_unique<X11ScreenCapture>();
                if (!mirrorX11Capture_->isReady()) {
                    std::fprintf(stderr,
                                  "HostControlAdapter: X11 screen-mirror capture also unavailable -- "
                                  "screen mirroring stays disabled; everything else continues to work "
                                  "normally.\n");
                }
            }
        } else {
            mirrorX11Capture_ = std::make_unique<X11ScreenCapture>();
            if (mirrorX11Capture_->isReady()) {
                std::fprintf(stderr, "HostControlAdapter: screen-mirror capture ready (X11)\n");
            } else {
                std::fprintf(stderr,
                              "HostControlAdapter: DUALDECK_HOSTCONTROL_MIRROR_SCREEN was set but no usable "
                              "X11 display/XShm was found -- trying the Wayland portal + PipeWire path "
                              "instead.\n");
                mirrorWaylandCapture_ = std::make_unique<WaylandScreenCapture>();
                if (!mirrorWaylandCapture_->isReady()) {
                    std::fprintf(stderr,
                                  "HostControlAdapter: Wayland portal screen-mirror capture also unavailable "
                                  "-- screen mirroring stays disabled; everything else continues to work "
                                  "normally.\n");
                }
            }
        }

        // Optional override of the default ~5fps capture rate (a full
        // desktop capture is real work, not worth repeating at
        // videoLoop()'s up-to-60fps poll rate for a feature whose whole
        // point is periodic menu/setup visibility -- see
        // getLatestFrame()'s comment). Clamped the same way other
        // capture-fps env vars in this project are (e.g. the Azahar/Cemu
        // patches' *_REMOTE_CAPTURE_FPS) -- an out-of-range or
        // unparseable value falls back to the default rather than
        // producing a nonsensical interval.
        const char* fpsEnv = std::getenv("DUALDECK_HOSTCONTROL_MIRROR_FPS");
        if (fpsEnv != nullptr && fpsEnv[0] != '\0') {
            char* end = nullptr;
            long fps = std::strtol(fpsEnv, &end, 10);
            if (end != fpsEnv && *end == '\0' && fps >= 1 && fps <= 30) {
                mirrorCaptureInterval_ = std::chrono::milliseconds(1000 / fps);
            }
        }
    }

    uinputFd_ = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (uinputFd_ < 0) {
        std::fprintf(stderr,
                      "HostControlAdapter: could not open /dev/uinput (%s) -- host-control mode's "
                      "virtual gamepad is disabled (needs the uinput kernel module loaded and write "
                      "access to /dev/uinput, e.g. via the 'input' group or a udev rule); everything "
                      "else continues to work normally.\n",
                      std::strerror(errno));
        return;
    }

    ::ioctl(uinputFd_, UI_SET_EVBIT, EV_KEY);
    for (const auto& mapping : kButtonMappings) {
        ::ioctl(uinputFd_, UI_SET_KEYBIT, mapping.code);
    }

    ::ioctl(uinputFd_, UI_SET_EVBIT, EV_ABS);
    ::ioctl(uinputFd_, UI_SET_ABSBIT, ABS_X);
    ::ioctl(uinputFd_, UI_SET_ABSBIT, ABS_Y);
    ::ioctl(uinputFd_, UI_SET_ABSBIT, ABS_HAT0X);
    ::ioctl(uinputFd_, UI_SET_ABSBIT, ABS_HAT0Y);
    // Right stick + analog triggers -- see HostControlGamepadState's own
    // comment (real user report, 2026-08-03: neither existed before this).
    ::ioctl(uinputFd_, UI_SET_ABSBIT, ABS_RX);
    ::ioctl(uinputFd_, UI_SET_ABSBIT, ABS_RY);
    ::ioctl(uinputFd_, UI_SET_ABSBIT, ABS_Z);
    ::ioctl(uinputFd_, UI_SET_ABSBIT, ABS_RZ);

    // Steam Controller touchpad experiment, opt-in only -- these
    // capability bits are deliberately NOT part of the absOk chain below
    // that gates the whole device's UI_DEV_CREATE: a failure here (or the
    // env var simply being unset) must disable only touchpadEnabled_,
    // never the proven gamepad/stick paths. See touchpadCapsOk below.
    bool touchpadCapsOk = true;
    if (touchpadEnabled_) {
        ::ioctl(uinputFd_, UI_SET_ABSBIT, ABS_MT_SLOT);
        ::ioctl(uinputFd_, UI_SET_ABSBIT, ABS_MT_POSITION_X);
        ::ioctl(uinputFd_, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
        ::ioctl(uinputFd_, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
        ::ioctl(uinputFd_, UI_SET_KEYBIT, BTN_TOUCH);
        ::ioctl(uinputFd_, UI_SET_KEYBIT, BTN_LEFT);
        touchpadCapsOk = ::ioctl(uinputFd_, UI_SET_PROPBIT, INPUT_PROP_BUTTONPAD) >= 0;
    }

    uinput_setup usetup{};
    usetup.id.bustype = BUS_USB;
    if (touchpadEnabled_) {
        // See isTouchpadReady()'s comment in the header -- this identity
        // swap is the whole point of the experiment (Steam's controller
        // driver keys off vendor/product ID), but it cannot by itself make
        // a uinput/evdev device visible to the HIDAPI-based driver that
        // actually reads real Steam Controller touchpads.
        usetup.id.vendor = kSteamControllerVendor;
        usetup.id.product = kSteamControllerProduct;
    } else {
        // Reuses the real Xbox 360 controller's USB vendor/product IDs --
        // the same convention several other open-source virtual-gamepad
        // projects use -- so desktop environments/Steam Input recognize
        // this device's button layout correctly out of the box, rather
        // than falling back to a generic/unrecognized-gamepad heuristic.
        usetup.id.vendor = 0x045e;
        usetup.id.product = 0x028e;
    }
    std::strncpy(usetup.name, "DualDeck Host Control", sizeof(usetup.name) - 1);

    if (::ioctl(uinputFd_, UI_DEV_SETUP, &usetup) < 0) {
        std::fprintf(stderr, "HostControlAdapter: UI_DEV_SETUP failed (%s)\n", std::strerror(errno));
        ::close(uinputFd_);
        uinputFd_ = -1;
        return;
    }

    auto setupAbs = [this](uint16_t code) {
        uinput_abs_setup absSetup{};
        absSetup.code = code;
        absSetup.absinfo.minimum = kAbsRangeMin;
        absSetup.absinfo.maximum = kAbsRangeMax;
        return ::ioctl(uinputFd_, UI_ABS_SETUP, &absSetup) >= 0;
    };
    // The hat axes use a much smaller conventional range (-1/0/1) than
    // the analog stick's full int16_t sweep -- set those two separately
    // rather than folding into the lambda above.
    uinput_abs_setup hatSetup{};
    bool absOk = setupAbs(ABS_X) && setupAbs(ABS_Y) && setupAbs(ABS_RX) && setupAbs(ABS_RY);
    hatSetup.code = ABS_HAT0X;
    hatSetup.absinfo.minimum = -1;
    hatSetup.absinfo.maximum = 1;
    absOk = absOk && ::ioctl(uinputFd_, UI_ABS_SETUP, &hatSetup) >= 0;
    hatSetup.code = ABS_HAT0Y;
    absOk = absOk && ::ioctl(uinputFd_, UI_ABS_SETUP, &hatSetup) >= 0;

    // Analog triggers use uinput's conventional 0..255 range (matching a
    // real Xbox 360 pad's ABS_Z/ABS_RZ), not the sticks' signed
    // centered-at-0 range -- reuses hatSetup's storage, just with
    // different bounds, rather than a third near-identical local.
    hatSetup.code = ABS_Z;
    hatSetup.absinfo.minimum = 0;
    hatSetup.absinfo.maximum = 255;
    absOk = absOk && ::ioctl(uinputFd_, UI_ABS_SETUP, &hatSetup) >= 0;
    hatSetup.code = ABS_RZ;
    absOk = absOk && ::ioctl(uinputFd_, UI_ABS_SETUP, &hatSetup) >= 0;

    // Deliberately NOT folded into absOk above -- see touchpadCapsOk's own
    // comment where the KEYBIT/PROPBIT calls happened. A failure here only
    // ever disables touchpadEnabled_ after UI_DEV_CREATE succeeds below.
    if (touchpadEnabled_ && touchpadCapsOk) {
        uinput_abs_setup mtSetup{};
        mtSetup.code = ABS_MT_POSITION_X;
        mtSetup.absinfo.minimum = kTouchpadPosMin;
        mtSetup.absinfo.maximum = kTouchpadPosMax;
        touchpadCapsOk = ::ioctl(uinputFd_, UI_ABS_SETUP, &mtSetup) >= 0;
        mtSetup.code = ABS_MT_POSITION_Y;
        touchpadCapsOk = touchpadCapsOk && ::ioctl(uinputFd_, UI_ABS_SETUP, &mtSetup) >= 0;
        mtSetup.code = ABS_MT_TRACKING_ID;
        mtSetup.absinfo.minimum = -1;
        mtSetup.absinfo.maximum = 65535;
        touchpadCapsOk = touchpadCapsOk && ::ioctl(uinputFd_, UI_ABS_SETUP, &mtSetup) >= 0;
        // Single slot (0..0) -- the wire protocol carries one aggregate
        // delta stream, not per-finger identity; see touchpadX_/touchpadY_'s
        // own header comment.
        mtSetup.code = ABS_MT_SLOT;
        mtSetup.absinfo.minimum = 0;
        mtSetup.absinfo.maximum = 0;
        touchpadCapsOk = touchpadCapsOk && ::ioctl(uinputFd_, UI_ABS_SETUP, &mtSetup) >= 0;
    }

    if (!absOk || ::ioctl(uinputFd_, UI_DEV_CREATE) < 0) {
        std::fprintf(stderr, "HostControlAdapter: failed to create the virtual gamepad device (%s)\n",
                      std::strerror(errno));
        ::close(uinputFd_);
        uinputFd_ = -1;
        return;
    }

    if (touchpadEnabled_ && !touchpadCapsOk) {
        std::fprintf(stderr,
                      "HostControlAdapter: DUALDECK_HOSTCONTROL_STEAM_TOUCHPAD was set but setting up "
                      "the touchpad capability bits failed -- continuing with the virtual gamepad's "
                      "existing button/stick/mouse behavior only.\n");
        touchpadEnabled_ = false;
    }

    touchpadX_ = kTouchpadPosCenter;
    touchpadY_ = kTouchpadPosCenter;

    std::fprintf(stderr, "HostControlAdapter: virtual gamepad ready%s\n",
                 touchpadEnabled_ ? " (Steam Controller touchpad experiment enabled)" : "");

    mouseUinputFd_ = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (mouseUinputFd_ < 0) {
        std::fprintf(stderr,
                      "HostControlAdapter: could not open /dev/uinput for the virtual mouse (%s) -- "
                      "touchpad-as-mouse control is disabled; the virtual gamepad above still works.\n",
                      std::strerror(errno));
        return;
    }

    ::ioctl(mouseUinputFd_, UI_SET_EVBIT, EV_REL);
    ::ioctl(mouseUinputFd_, UI_SET_RELBIT, REL_X);
    ::ioctl(mouseUinputFd_, UI_SET_RELBIT, REL_Y);
    ::ioctl(mouseUinputFd_, UI_SET_EVBIT, EV_KEY);
    ::ioctl(mouseUinputFd_, UI_SET_KEYBIT, BTN_LEFT);
    ::ioctl(mouseUinputFd_, UI_SET_KEYBIT, BTN_RIGHT);

    uinput_setup mouseSetup{};
    mouseSetup.id.bustype = BUS_VIRTUAL;
    // No real device's vendor/product ID to match here (unlike the
    // gamepad's Xbox 360 reuse above) -- EV_REL + REL_X/REL_Y + BTN_LEFT/
    // BTN_RIGHT alone is what makes evdev/libinput classify this as a
    // mouse; the exact IDs are only ever shown in a device list, never
    // used for capability detection.
    mouseSetup.id.vendor = 0x0001;
    mouseSetup.id.product = 0x0001;
    std::strncpy(mouseSetup.name, "DualDeck Host Control Mouse", sizeof(mouseSetup.name) - 1);

    if (::ioctl(mouseUinputFd_, UI_DEV_SETUP, &mouseSetup) < 0 ||
        ::ioctl(mouseUinputFd_, UI_DEV_CREATE) < 0) {
        std::fprintf(stderr, "HostControlAdapter: failed to create the virtual mouse device (%s)\n",
                      std::strerror(errno));
        ::close(mouseUinputFd_);
        mouseUinputFd_ = -1;
        return;
    }

    std::fprintf(stderr, "HostControlAdapter: virtual mouse ready\n");
}

HostControlAdapter::~HostControlAdapter() {
    if (uinputFd_ >= 0) {
        ::ioctl(uinputFd_, UI_DEV_DESTROY);
        ::close(uinputFd_);
    }
    if (mouseUinputFd_ >= 0) {
        ::ioctl(mouseUinputFd_, UI_DEV_DESTROY);
        ::close(mouseUinputFd_);
    }
}

void HostControlAdapter::emitState(const HostControlGamepadState& state) {
    if (uinputFd_ < 0) return;

    bool sentAnything = false;
    for (const auto& mapping : kButtonMappings) {
        bool held = state.*mapping.field;
        if (held != lastEmitted_.*mapping.field) {
            writeEvent(uinputFd_, EV_KEY, static_cast<uint16_t>(mapping.code), held ? 1 : 0);
            sentAnything = true;
        }
    }
    if (state.hatX != lastEmitted_.hatX) {
        writeEvent(uinputFd_, EV_ABS, ABS_HAT0X, state.hatX);
        sentAnything = true;
    }
    if (state.hatY != lastEmitted_.hatY) {
        writeEvent(uinputFd_, EV_ABS, ABS_HAT0Y, state.hatY);
        sentAnything = true;
    }
    if (state.leftStickX != lastEmitted_.leftStickX) {
        writeEvent(uinputFd_, EV_ABS, ABS_X, state.leftStickX);
        sentAnything = true;
    }
    if (state.leftStickY != lastEmitted_.leftStickY) {
        writeEvent(uinputFd_, EV_ABS, ABS_Y, state.leftStickY);
        sentAnything = true;
    }
    if (state.rightStickX != lastEmitted_.rightStickX) {
        writeEvent(uinputFd_, EV_ABS, ABS_RX, state.rightStickX);
        sentAnything = true;
    }
    if (state.rightStickY != lastEmitted_.rightStickY) {
        writeEvent(uinputFd_, EV_ABS, ABS_RY, state.rightStickY);
        sentAnything = true;
    }
    if (state.leftTrigger != lastEmitted_.leftTrigger) {
        writeEvent(uinputFd_, EV_ABS, ABS_Z, state.leftTrigger);
        sentAnything = true;
    }
    if (state.rightTrigger != lastEmitted_.rightTrigger) {
        writeEvent(uinputFd_, EV_ABS, ABS_RZ, state.rightTrigger);
        sentAnything = true;
    }

    if (sentAnything) {
        writeEvent(uinputFd_, EV_SYN, SYN_REPORT, 0);
    }
    lastEmitted_ = state;
}

void HostControlAdapter::emitMouseState(const HostControlMouseState& state) {
    if (mouseUinputFd_ < 0) return;

    bool sentAnything = false;
    // Relative motion, not diffed against lastEmittedMouse_ -- every
    // nonzero delta is a fresh motion to apply this tick, unlike the
    // button/hat/stick fields above which are absolute/level state.
    if (state.dx != 0) {
        writeEvent(mouseUinputFd_, EV_REL, REL_X, state.dx);
        sentAnything = true;
    }
    if (state.dy != 0) {
        writeEvent(mouseUinputFd_, EV_REL, REL_Y, state.dy);
        sentAnything = true;
    }
    if (state.leftDown != lastEmittedMouse_.leftDown) {
        writeEvent(mouseUinputFd_, EV_KEY, BTN_LEFT, state.leftDown ? 1 : 0);
        sentAnything = true;
    }
    if (state.rightDown != lastEmittedMouse_.rightDown) {
        writeEvent(mouseUinputFd_, EV_KEY, BTN_RIGHT, state.rightDown ? 1 : 0);
        sentAnything = true;
    }

    if (sentAnything) {
        writeEvent(mouseUinputFd_, EV_SYN, SYN_REPORT, 0);
    }
    lastEmittedMouse_ = state;
}

void HostControlAdapter::emitTouchpadState() {
    if (!touchpadEnabled_ || uinputFd_ < 0) return;

    bool sentAnything = false;
    // Position dead-reckons every tick regardless of contact state, same
    // as the mouse device's own always-relative motion above -- clamped,
    // not wrapped, since a touchpad surface has hard edges.
    int32_t clampedX = std::clamp(
        touchpadX_ + static_cast<int32_t>(lastEmittedMouse_.dx * kTouchpadPositionScale), kTouchpadPosMin,
        kTouchpadPosMax);
    int32_t clampedY = std::clamp(
        touchpadY_ + static_cast<int32_t>(lastEmittedMouse_.dy * kTouchpadPositionScale), kTouchpadPosMin,
        kTouchpadPosMax);
    if (clampedX != touchpadX_ || clampedY != touchpadY_) {
        touchpadX_ = clampedX;
        touchpadY_ = clampedY;
        writeEvent(uinputFd_, EV_ABS, ABS_MT_SLOT, 0);
        writeEvent(uinputFd_, EV_ABS, ABS_MT_POSITION_X, touchpadX_);
        writeEvent(uinputFd_, EV_ABS, ABS_MT_POSITION_Y, touchpadY_);
        sentAnything = true;
    }

    // Genuine heuristic, documented in the header: the wire protocol has
    // no real finger-down/up signal, only continuous deltas plus a
    // button-held bitmask -- use the touchpad click as a "finger present"
    // proxy.
    if (touchpadContact_ != lastEmittedTouchpadContact_) {
        writeEvent(uinputFd_, EV_ABS, ABS_MT_SLOT, 0);
        writeEvent(uinputFd_, EV_ABS, ABS_MT_TRACKING_ID, touchpadContact_ ? 0 : -1);
        writeEvent(uinputFd_, EV_KEY, BTN_TOUCH, touchpadContact_ ? 1 : 0);
        writeEvent(uinputFd_, EV_KEY, BTN_LEFT, touchpadContact_ ? 1 : 0);
        lastEmittedTouchpadContact_ = touchpadContact_;
        sentAnything = true;
    }

    if (sentAnything) {
        writeEvent(uinputFd_, EV_SYN, SYN_REPORT, 0);
    }
}

void HostControlAdapter::applyControllerState(const ControllerState& state) {
    emitState(translateControllerState(state));
    emitMouseState(translateMouseState(state));
    if (touchpadEnabled_) {
        touchpadContact_ = (state.mouseButtons & MouseButton_Left) != 0;
        emitTouchpadState();
    }
}

void HostControlAdapter::releaseAll() {
    emitState(HostControlGamepadState{});
    emitMouseState(HostControlMouseState{});
    if (touchpadEnabled_) {
        touchpadContact_ = false;
        emitTouchpadState();
    }
}

bool HostControlAdapter::getLatestFrame(std::vector<uint8_t>& outFrame, uint64_t& outFrameIndex,
                                        uint16_t& outWidth, uint16_t& outHeight) {
    if (!isMirrorReady()) return false;

    auto now = std::chrono::steady_clock::now();
    if (mirrorLastFrame_.empty() || now - mirrorLastCaptureTime_ >= mirrorCaptureInterval_) {
        std::vector<uint8_t> frame;
        uint16_t width = 0;
        uint16_t height = 0;
        bool captured = mirrorX11Capture_ && mirrorX11Capture_->isReady()
                             ? mirrorX11Capture_->capture(frame, width, height)
                             : mirrorWaylandCapture_->capture(frame, width, height);
        if (captured) {
            // See setTargetDisplaySize()'s own comment for the real
            // user report this fixes. Falls back to this project's
            // primary target device's native resolution until a real
            // client has connected and reported its own.
            int targetWidth = targetDisplayWidth_ != 0 ? targetDisplayWidth_ : kFallbackTargetDisplayWidth;
            int targetHeight = targetDisplayHeight_ != 0 ? targetDisplayHeight_ : kFallbackTargetDisplayHeight;
            int fitWidth = 0, fitHeight = 0;
            fitDownscaleTarget(width, height, targetWidth, targetHeight, fitWidth, fitHeight);
            if (fitWidth != width || fitHeight != height) {
                std::vector<uint8_t> downscaled;
                downscaleBgra8888(frame.data(), width, height, downscaled, fitWidth, fitHeight);
                mirrorLastFrame_ = std::move(downscaled);
                width = static_cast<uint16_t>(fitWidth);
                height = static_cast<uint16_t>(fitHeight);
            } else {
                mirrorLastFrame_ = std::move(frame);
            }
            mirrorLastWidth_ = width;
            mirrorLastHeight_ = height;
            mirrorLastFrameIndex_ = mirrorNextFrameIndex_++;
        }
        // Reset the deadline regardless of whether the capture actually
        // succeeded this tick -- a transient capture failure should be
        // retried at the configured rate, not on every single poll
        // (which could be up to 60/sec, per videoLoop()'s own comment).
        mirrorLastCaptureTime_ = now;
    }

    if (mirrorLastFrame_.empty()) return false;
    outFrame = mirrorLastFrame_;
    outFrameIndex = mirrorLastFrameIndex_;
    outWidth = mirrorLastWidth_;
    outHeight = mirrorLastHeight_;
    return true;
}

void HostControlAdapter::frameDimensions(uint16_t& outWidth, uint16_t& outHeight) const {
    if (mirrorX11Capture_ && mirrorX11Capture_->isReady()) {
        mirrorX11Capture_->nativeSize(outWidth, outHeight);
        return;
    }
    if (mirrorWaylandCapture_ && mirrorWaylandCapture_->isReady()) {
        mirrorWaylandCapture_->nativeSize(outWidth, outHeight);
        return;
    }
    outWidth = static_cast<uint16_t>(kFrameWidth);
    outHeight = static_cast<uint16_t>(kFrameHeight);
}

void HostControlAdapter::setTargetDisplaySize(uint16_t width, uint16_t height) {
    targetDisplayWidth_ = width;
    targetDisplayHeight_ = height;
}

} // namespace melonds_remote::host
