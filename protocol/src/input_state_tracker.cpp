#include "melonds_remote/input_state_tracker.h"

namespace melonds_remote {

bool InputStateTracker::isNewerSequence(uint32_t candidate, uint32_t last) {
    return static_cast<int32_t>(candidate - last) > 0;
}

ControllerState InputStateTracker::releasedState() {
    ControllerState released;
    released.sequence = 0;
    released.clientTimestampUs = 0;
    released.dsButtons = 0;
    released.emulatorActions = 0;
    released.leftStickX = 0;
    released.leftStickY = 0;
    released.rightStickX = 0;
    released.rightStickY = 0;
    released.touchActive = 0;
    released.touchX = 0;
    released.touchY = 0;
    return released;
}

bool InputStateTracker::onPacketReceived(const ControllerState& state, uint64_t nowUs) {
    if (hasReceivedAnyPacket_ && !isNewerSequence(state.sequence, current_.sequence)) {
        return false;
    }

    current_ = state;
    lastReceivedAtUs_ = nowUs;
    hasReceivedAnyPacket_ = true;
    return true;
}

bool InputStateTracker::isTimedOut(uint64_t nowUs, uint64_t timeoutUs) const {
    if (!hasReceivedAnyPacket_) {
        return false;
    }
    return (nowUs - lastReceivedAtUs_) > timeoutUs;
}

void InputStateTracker::reset() {
    current_ = releasedState();
    lastReceivedAtUs_ = 0;
    hasReceivedAnyPacket_ = false;
}

} // namespace melonds_remote
