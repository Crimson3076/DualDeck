#include "host/logging_input_sink.h"

#include <cstdio>

#include "melonds_remote/input_state_tracker.h"

namespace melonds_remote::host {

void LoggingInputSink::applyControllerState(const ControllerState& state) {
    std::lock_guard<std::mutex> lock(mutex_);
    lastState_ = state;
}

void LoggingInputSink::releaseAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    lastState_ = InputStateTracker::releasedState();
    std::fprintf(stderr, "[input] released all inputs (disconnect/timeout)\n");
}

ControllerState LoggingInputSink::lastState() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastState_;
}

} // namespace melonds_remote::host
