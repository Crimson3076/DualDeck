#pragma once

// GitHub issue #4 Phase C: lets a connected client navigate the host's
// own UI (Steam Big Picture, a window manager, etc.) via a virtual
// Xbox-360-style gamepad, created through Linux's uinput subsystem, when
// no emulator is running -- see NetServer::setTarget()
// (host/remote-server/include/host/net_server.h) and
// docs/adr/0001-host-service-and-adapter-architecture.md section 7.
//
// Deliberately implements IEmulatorInputSink/IFrameSource directly
// (matching LoggingInputSink/SyntheticFrameSource, NetServer's other two
// implementations of these interfaces) rather than going through the
// generic adapter-sdk::IEmulatorAdapter contract used by real emulator
// adapters -- host-control mode isn't "emulating a system" with its own
// capabilities/surfaces to negotiate, it's host-side input translation,
// so the extra layer would add nothing. It consumes the same wire-level
// ControllerState/DSButton fields every other IEmulatorInputSink does;
// see translateControllerState() below for why that's fine even though
// host-control mode is meant to be emulator-agnostic in spirit -- the DS
// button layout is just this project's one existing physical-button
// vocabulary today.

#include <cstdint>

#include "host/emulator_input_sink.h"
#include "host/frame_source.h"

namespace melonds_remote::host {

// Pure description of "what should currently be held" on the virtual
// gamepad, derived from a ControllerState -- deliberately has no I/O of
// its own so the DSButton -> gamepad mapping can be unit-tested without
// a real /dev/uinput node (this sandbox has none; see
// docs/known-limitations.md's Phase C entry). Field names follow
// standard Xbox-controller ABXY naming (south=A, east=B, west=X,
// north=Y), matching the exact same physical-button meaning
// host::AdapterBridge's dsButtonsToGenericButtons table already
// establishes for this codebase (DSButton_X -> west, DSButton_Y ->
// north, not the reverse) -- see that function's own comment.
struct HostControlGamepadState {
    bool south = false;  // A
    bool east = false;   // B
    bool west = false;   // X
    bool north = false;  // Y
    bool tl = false;      // L shoulder
    bool tr = false;      // R shoulder
    bool start = false;
    bool select = false;
    // D-pad as a single hat pair (-1/0/1 per axis), matching how a real
    // Xbox 360 pad reports it via ABS_HAT0X/ABS_HAT0Y -- not four
    // separate booleans.
    int8_t hatX = 0; // -1 = left, 1 = right
    int8_t hatY = 0; // -1 = up, 1 = down
    // Passed through unscaled from ControllerState -- both use the same
    // int16_t, centered-at-0 range, so no rescaling is needed to feed
    // this straight into ABS_X/ABS_Y.
    int16_t leftStickX = 0;
    int16_t leftStickY = 0;
};

// Pure translation, no I/O -- see HostControlGamepadState's own comment.
HostControlGamepadState translateControllerState(const ControllerState& state);

// Pure description of "what the virtual mouse should do this tick" --
// same no-I/O rationale as HostControlGamepadState. dx/dy are relative
// motion (protocol.h's ControllerState::mouseDeltaX/Y, forwarded
// unscaled -- both already share the same "screen-independent relative
// pixels" unit a uinput EV_REL device expects), not accumulated
// position, so there is no "previous state" for them to diff against the
// way button/hat/stick fields need; only left/right button-held state is
// level-triggered and needs diffing (see emitMouseState()).
struct HostControlMouseState {
    int16_t dx = 0;
    int16_t dy = 0;
    bool leftDown = false;
    bool rightDown = false;
};

// Pure translation, no I/O -- see HostControlMouseState's own comment.
HostControlMouseState translateMouseState(const ControllerState& state);

class HostControlAdapter : public IEmulatorInputSink, public IFrameSource {
public:
    HostControlAdapter();
    ~HostControlAdapter() override;

    HostControlAdapter(const HostControlAdapter&) = delete;
    HostControlAdapter& operator=(const HostControlAdapter&) = delete;

    // False if /dev/uinput couldn't be opened or the virtual device
    // couldn't be created (missing permissions, uinput module not
    // loaded, or -- as in this project's own CI/dev sandbox -- no
    // /dev/uinput node at all). applyControllerState()/releaseAll() are
    // safe no-ops in that case, matching this codebase's "degrade
    // gracefully, log once, never crash" convention for an unavailable
    // optional resource (e.g. net_server.cpp's audioFd_ bind failure).
    bool isDeviceReady() const { return uinputFd_ >= 0; }

    // Same idea, for the separate virtual-mouse uinput device (see
    // mouseUinputFd_'s own comment on why it's a second device rather
    // than folded into the gamepad one). Independent of isDeviceReady()
    // above: a machine can plausibly have one succeed and the other fail
    // (e.g. a stale udev rule that only grants access to one of two
    // freshly-created input nodes before the rule reloads).
    bool isMouseDeviceReady() const { return mouseUinputFd_ >= 0; }

    void applyControllerState(const ControllerState& state) override;
    void releaseAll() override;

    // Host-control mode has no video to stream -- there is no emulated
    // screen while no emulator is running; a connected client shows its
    // own local UI instead (GitHub issue #4 Phase E). Always returns
    // false; NetServer's videoLoop() already treats that as "nothing to
    // send this tick" with no special-casing needed here.
    bool getLatestFrame(std::vector<uint8_t>& outFrame, uint64_t& outFrameIndex,
                        uint16_t& outWidth, uint16_t& outHeight) override;

private:
    void emitState(const HostControlGamepadState& state);
    void emitMouseState(const HostControlMouseState& state);

    int uinputFd_ = -1;
    HostControlGamepadState lastEmitted_;

    // Separate uinput device from uinputFd_'s virtual gamepad: mixing
    // EV_REL mouse-relative-motion semantics into a device already
    // advertising itself (via the Xbox 360 vendor/product ID reuse, see
    // the .cpp) as a gamepad would confuse desktop environments/Steam
    // Input's own device-type heuristics. A distinct device whose only
    // capabilities are EV_REL (REL_X/REL_Y) + EV_KEY (BTN_LEFT/BTN_RIGHT)
    // is unambiguously "a mouse" to anything reading evdev capability
    // bits, matching how real remote-desktop/KVM tools create a
    // dedicated virtual mouse node rather than overloading one device.
    int mouseUinputFd_ = -1;
    HostControlMouseState lastEmittedMouse_;
};

} // namespace melonds_remote::host
