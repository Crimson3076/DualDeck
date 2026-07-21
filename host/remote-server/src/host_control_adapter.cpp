#include "host/host_control_adapter.h"

#include <fcntl.h>
#include <linux/uinput.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
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

namespace {

constexpr int kAbsRangeMin = -32768;
constexpr int kAbsRangeMax = 32767;

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

    uinput_setup usetup{};
    usetup.id.bustype = BUS_USB;
    // Reuses the real Xbox 360 controller's USB vendor/product IDs --
    // the same convention several other open-source virtual-gamepad
    // projects use -- so desktop environments/Steam Input recognize this
    // device's button layout correctly out of the box, rather than
    // falling back to a generic/unrecognized-gamepad heuristic.
    usetup.id.vendor = 0x045e;
    usetup.id.product = 0x028e;
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

    if (!absOk || ::ioctl(uinputFd_, UI_DEV_CREATE) < 0) {
        std::fprintf(stderr, "HostControlAdapter: failed to create the virtual gamepad device (%s)\n",
                      std::strerror(errno));
        ::close(uinputFd_);
        uinputFd_ = -1;
        return;
    }

    std::fprintf(stderr, "HostControlAdapter: virtual gamepad ready\n");
}

HostControlAdapter::~HostControlAdapter() {
    if (uinputFd_ >= 0) {
        ::ioctl(uinputFd_, UI_DEV_DESTROY);
        ::close(uinputFd_);
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

void HostControlAdapter::applyControllerState(const ControllerState& state) {
    emitState(translateControllerState(state));
}

void HostControlAdapter::releaseAll() {
    emitState(HostControlGamepadState{});
}

bool HostControlAdapter::getLatestFrame(std::vector<uint8_t>& /*outFrame*/, uint64_t& /*outFrameIndex*/) {
    return false;
}

} // namespace melonds_remote::host
