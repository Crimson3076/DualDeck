#include "melonds_remote/adapter/session_state.h"

namespace melonds_remote::adapter {

const char* toString(SessionState state) {
    switch (state) {
        case SessionState::Available: return "Available";
        case SessionState::Starting:  return "Starting";
        case SessionState::Running:   return "Running";
        case SessionState::Paused:    return "Paused";
        case SessionState::Stopped:   return "Stopped";
        case SessionState::Error:     return "Error";
    }
    return "Unknown";
}

bool isValidTransition(SessionState from, SessionState to) {
    // An adapter may fault out of any state, including into itself
    // (repeated faults) -- always allowed.
    if (to == SessionState::Error) {
        return true;
    }

    switch (from) {
        case SessionState::Available:
            return to == SessionState::Starting;
        case SessionState::Starting:
            return to == SessionState::Running;
        case SessionState::Running:
            return to == SessionState::Paused || to == SessionState::Stopped;
        case SessionState::Paused:
            return to == SessionState::Running || to == SessionState::Stopped;
        case SessionState::Stopped:
            return to == SessionState::Available;
        case SessionState::Error:
            return to == SessionState::Available;
    }
    return false;
}

} // namespace melonds_remote::adapter
