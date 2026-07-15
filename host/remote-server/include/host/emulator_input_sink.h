#pragma once

// Seam between the network layer and the emulator core.
//
// Today (Phase 1) this is implemented by LoggingInputSink, since no melonDS
// fork is wired in yet (see docs/melonds-integration-analysis.md). Once the
// melonDS integration lands, a MelonDSInputSink will implement this same
// interface and call NDS::SetKeyMask / NDS::TouchScreen / NDS::ReleaseScreen
// on the emulation thread using the last state handed to it here -- never
// directly from the network thread (spec section 16).

#include "melonds_remote/protocol.h"

namespace melonds_remote::host {

class IEmulatorInputSink {
public:
    virtual ~IEmulatorInputSink() = default;

    // Called whenever the network layer has a new accepted controller
    // state. Implementations must not block (no I/O, no locking that could
    // stall the emulation thread) -- store the latest state and let the
    // emulation thread's own loop pick it up, matching how melonDS's
    // existing frontend consumes its own local input (see analysis doc
    // section 2).
    virtual void applyControllerState(const ControllerState& state) = 0;

    // Called on client disconnect or input timeout. Must immediately
    // reflect an all-released state: no DS buttons held, touchscreen
    // released, analog cleared, no emulator actions pending (spec 6.4).
    virtual void releaseAll() = 0;
};

} // namespace melonds_remote::host
