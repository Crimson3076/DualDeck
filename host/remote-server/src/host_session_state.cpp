#include "host/host_session_state.h"

namespace melonds_remote::host {

const char* toString(HostSessionState state) {
    switch (state) {
        case HostSessionState::Idle:               return "Idle";
        case HostSessionState::Discoverable:        return "Discoverable";
        case HostSessionState::Pairing:              return "Pairing";
        case HostSessionState::Connected:            return "Connected";
        case HostSessionState::Streaming:            return "Streaming";
        case HostSessionState::Launching:            return "Launching";
        case HostSessionState::EmulatorRunning:      return "EmulatorRunning";
        case HostSessionState::CompanionModeActive:  return "CompanionModeActive";
        case HostSessionState::Suspending:           return "Suspending";
        case HostSessionState::Sleeping:             return "Sleeping";
        case HostSessionState::Error:                return "Error";
    }
    return "Unknown";
}

bool isValidTransition(HostSessionState from, HostSessionState to) {
    // May fault out of any state, including into itself (repeated
    // faults) -- always allowed, same convention as
    // adapter::isValidTransition().
    if (to == HostSessionState::Error) {
        return true;
    }

    switch (from) {
        case HostSessionState::Idle:
            return to == HostSessionState::Discoverable || to == HostSessionState::Connected ||
                   to == HostSessionState::Suspending;
        case HostSessionState::Discoverable:
            return to == HostSessionState::Idle || to == HostSessionState::Pairing ||
                   to == HostSessionState::Connected;
        case HostSessionState::Pairing:
            return to == HostSessionState::Idle || to == HostSessionState::Connected;
        case HostSessionState::Connected:
            return to == HostSessionState::Idle || to == HostSessionState::Launching ||
                   to == HostSessionState::CompanionModeActive;
        case HostSessionState::Streaming:
            return to == HostSessionState::EmulatorRunning || to == HostSessionState::Connected;
        case HostSessionState::Launching:
            return to == HostSessionState::EmulatorRunning || to == HostSessionState::Connected;
        case HostSessionState::EmulatorRunning:
            return to == HostSessionState::Streaming || to == HostSessionState::Connected;
        case HostSessionState::CompanionModeActive:
            return to == HostSessionState::Streaming || to == HostSessionState::Connected;
        case HostSessionState::Suspending:
            return to == HostSessionState::Sleeping;
        case HostSessionState::Sleeping:
            return to == HostSessionState::Idle;
        case HostSessionState::Error:
            return to == HostSessionState::Idle;
    }
    return false;
}

} // namespace melonds_remote::host
