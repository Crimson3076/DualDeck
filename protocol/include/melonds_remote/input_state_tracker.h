#pragma once

// Transport-independent logic for tracking the "current" controller state
// from a stream of possibly out-of-order, possibly-lost UDP packets, and for
// producing the all-released state required on timeout or disconnect (spec
// sections 6.3 and 6.4). Kept free of any socket code so it can be unit
// tested directly.

#include <cstdint>
#include <optional>

#include "melonds_remote/protocol.h"

namespace melonds_remote {

class InputStateTracker {
public:
    // Feeds a newly received, already-parsed ControllerState in at time
    // `nowUs` (microseconds, monotonic clock recommended). Packets whose
    // sequence number is not newer than the last accepted one (using
    // wraparound-aware comparison over the 32-bit sequence space) are
    // dropped, per spec 6.3 ("host must discard old or out-of-order
    // packets"). A tracker instance is scoped to one client session; when a
    // client reconnects and gets a new session from the control channel,
    // call reset() explicitly rather than relying on sequence-number
    // heuristics to detect the restart.
    //
    // Returns true if the packet was accepted and is now the current state.
    bool onPacketReceived(const ControllerState& state, uint64_t nowUs);

    // The most recently accepted state, or an all-released state if nothing
    // has been accepted yet.
    const ControllerState& currentState() const { return current_; }

    uint64_t lastReceivedAtUs() const { return lastReceivedAtUs_; }

    bool hasReceivedAnyPacket() const { return hasReceivedAnyPacket_; }

    // True if no packet has been accepted within `timeoutUs` of `nowUs`.
    // Only meaningful once hasReceivedAnyPacket() is true; a client that
    // never sent anything is not "timed out", it simply never connected.
    bool isTimedOut(uint64_t nowUs, uint64_t timeoutUs) const;

    // Resets to the all-released state and forgets sequence/timing history.
    // Must be called on disconnect/timeout so no button can remain held
    // (spec 6.4).
    void reset();

    // The canonical "everything released" state: no buttons, no touch, no
    // analog deflection, no pending emulator actions.
    static ControllerState releasedState();

private:
    // Sequence numbers wrap around at 2^32; a "new" packet is one whose
    // signed circular distance ahead of the last accepted one is positive
    // (standard wraparound-aware sequence comparison).
    static bool isNewerSequence(uint32_t candidate, uint32_t last);

    ControllerState current_ = releasedState();
    uint64_t lastReceivedAtUs_ = 0;
    bool hasReceivedAnyPacket_ = false;
};

} // namespace melonds_remote
