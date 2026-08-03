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

HostControlGamepadState translateControllerState(const ControllerState& state) {
    HostControlGamepadState out;
    out.south = (state.dsButtons & DSButton_A) != 0;
    out.east = (state.dsButtons & DSButton_B) != 0;
    out.west = (state.dsButtons & DSButton_X) != 0;
    out.north = (state.dsButtons & DSButton_Y) != 0;
    out.tl = (state.dsButtons & DSButton_L) != 0;
    out.tr = (state.dsButtons & DSButton_R) != 0;
    out.start = (state.dsButtons & DSButton_Start) != 0;
    out.select = (state.dsButtons & DSButton_Select) != 0;

    if (state.dsButtons & DSButton_Left) out.hatX = -1;
    else if (state.dsButtons & DSButton_Right) out.hatX = 1;
    if (state.dsButtons & DSButton_Up) out.hatY = -1;
    else if (state.dsButtons & DSButton_Down) out.hatY = 1;

    out.leftStickX = state.leftStickX;
    out.leftStickY = state.leftStickY;
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

constexpr ButtonMapping kButtonMappings[] = {
    {&HostControlGamepadState::south, BTN_SOUTH}, {&HostControlGamepadState::east, BTN_EAST},
    {&HostControlGamepadState::west, BTN_WEST},   {&HostControlGamepadState::north, BTN_NORTH},
    {&HostControlGamepadState::tl, BTN_TL},       {&HostControlGamepadState::tr, BTN_TR},
    {&HostControlGamepadState::start, BTN_START}, {&HostControlGamepadState::select, BTN_SELECT},
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
    // either. X11 tried first (cheap, synchronous); WaylandScreenCapture
    // (portal + PipeWire) only constructed as a fallback if X11 didn't
    // work -- real user report, 2026-08-03: X11 "succeeding" against
    // XWayland's own empty compositing root on a Wayland session
    // produced a uniform grey, not a crash, so isReady() alone can't be
    // trusted to mean "this will actually show something" on every
    // system -- but it's still cheaper to try first on a real X11
    // desktop session where it genuinely does work, rather than always
    // paying for a portal session negotiation (including its one-time
    // interactive permission prompt) unconditionally.
    const char* mirrorEnv = std::getenv("DUALDECK_HOSTCONTROL_MIRROR_SCREEN");
    mirrorEnabled_ = mirrorEnv != nullptr && mirrorEnv[0] != '\0' && std::strcmp(mirrorEnv, "0") != 0;
    if (mirrorEnabled_) {
        mirrorX11Capture_ = std::make_unique<X11ScreenCapture>();
        if (mirrorX11Capture_->isReady()) {
            std::fprintf(stderr, "HostControlAdapter: screen-mirror capture ready (X11)\n");
        } else {
            std::fprintf(stderr,
                          "HostControlAdapter: DUALDECK_HOSTCONTROL_MIRROR_SCREEN was set but no usable X11 "
                          "display/XShm was found -- trying the Wayland portal + PipeWire path instead.\n");
            mirrorWaylandCapture_ = std::make_unique<WaylandScreenCapture>();
            if (!mirrorWaylandCapture_->isReady()) {
                std::fprintf(stderr,
                              "HostControlAdapter: Wayland portal screen-mirror capture also unavailable -- "
                              "screen mirroring stays disabled; everything else continues to work normally.\n");
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
    bool absOk = setupAbs(ABS_X) && setupAbs(ABS_Y);
    hatSetup.code = ABS_HAT0X;
    hatSetup.absinfo.minimum = -1;
    hatSetup.absinfo.maximum = 1;
    absOk = absOk && ::ioctl(uinputFd_, UI_ABS_SETUP, &hatSetup) >= 0;
    hatSetup.code = ABS_HAT0Y;
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
            mirrorLastFrame_ = std::move(frame);
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

} // namespace melonds_remote::host
