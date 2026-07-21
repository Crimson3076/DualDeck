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

    void applyControllerState(const ControllerState& state) override;
    void releaseAll() override;

    // Host-control mode has no video to stream -- there is no emulated
    // screen while no emulator is running; a connected client shows its
    // own local UI instead (GitHub issue #4 Phase E). Always returns
    // false; NetServer's videoLoop() already treats that as "nothing to
    // send this tick" with no special-casing needed here.
    bool getLatestFrame(std::vector<uint8_t>& outFrame, uint64_t& outFrameIndex) override;

private:
    void emitState(const HostControlGamepadState& state);

    int uinputFd_ = -1;
    HostControlGamepadState lastEmitted_;
};

} // namespace melonds_remote::host
