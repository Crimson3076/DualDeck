#include "host/host_session_state.h"
#include "test_framework.h"

using namespace melonds_remote::host;

MDR_TEST(host_session_state_happy_path_transitions_are_valid) {
    MDR_CHECK(isValidTransition(HostSessionState::Idle, HostSessionState::Discoverable));
    MDR_CHECK(isValidTransition(HostSessionState::Discoverable, HostSessionState::Pairing));
    MDR_CHECK(isValidTransition(HostSessionState::Pairing, HostSessionState::Connected));
    MDR_CHECK(isValidTransition(HostSessionState::Connected, HostSessionState::Launching));
    MDR_CHECK(isValidTransition(HostSessionState::Launching, HostSessionState::EmulatorRunning));
    MDR_CHECK(isValidTransition(HostSessionState::EmulatorRunning, HostSessionState::Streaming));
    MDR_CHECK(isValidTransition(HostSessionState::Streaming, HostSessionState::EmulatorRunning));
    MDR_CHECK(isValidTransition(HostSessionState::EmulatorRunning, HostSessionState::Connected));
}

MDR_TEST(host_session_state_direct_connect_skips_discovery_and_pairing) {
    // A client connecting directly (e.g. typed IP) rather than via LAN
    // discovery is a real, supported path -- see discovery_client.h.
    MDR_CHECK(isValidTransition(HostSessionState::Idle, HostSessionState::Connected));
}

MDR_TEST(host_session_state_companion_mode_reachable_from_connected) {
    MDR_CHECK(isValidTransition(HostSessionState::Connected, HostSessionState::CompanionModeActive));
    MDR_CHECK(isValidTransition(HostSessionState::CompanionModeActive, HostSessionState::Streaming));
    MDR_CHECK(isValidTransition(HostSessionState::CompanionModeActive, HostSessionState::Connected));
}

MDR_TEST(host_session_state_sleep_cycle) {
    MDR_CHECK(isValidTransition(HostSessionState::Idle, HostSessionState::Suspending));
    MDR_CHECK(isValidTransition(HostSessionState::Suspending, HostSessionState::Sleeping));
    MDR_CHECK(isValidTransition(HostSessionState::Sleeping, HostSessionState::Idle));
}

MDR_TEST(host_session_state_any_state_may_transition_to_error) {
    const HostSessionState allStates[] = {
        HostSessionState::Idle,          HostSessionState::Discoverable,       HostSessionState::Pairing,
        HostSessionState::Connected,     HostSessionState::Streaming,          HostSessionState::Launching,
        HostSessionState::EmulatorRunning, HostSessionState::CompanionModeActive, HostSessionState::Suspending,
        HostSessionState::Sleeping,      HostSessionState::Error, // repeated fault
    };
    for (HostSessionState s : allStates) {
        MDR_CHECK(isValidTransition(s, HostSessionState::Error));
    }
}

MDR_TEST(host_session_state_error_may_recover_to_idle) {
    MDR_CHECK(isValidTransition(HostSessionState::Error, HostSessionState::Idle));
}

MDR_TEST(host_session_state_rejects_launching_without_connected_client) {
    // Can't start launching an emulator before a client is even connected
    // to drive/watch it.
    MDR_CHECK(!isValidTransition(HostSessionState::Idle, HostSessionState::Launching));
    MDR_CHECK(!isValidTransition(HostSessionState::Discoverable, HostSessionState::Launching));
}

MDR_TEST(host_session_state_rejects_streaming_directly_from_connected) {
    // Must go through Launching/EmulatorRunning or CompanionModeActive
    // first -- Connected alone has nothing to stream yet.
    MDR_CHECK(!isValidTransition(HostSessionState::Connected, HostSessionState::Streaming));
}

MDR_TEST(host_session_state_rejects_self_transitions_other_than_error) {
    MDR_CHECK(!isValidTransition(HostSessionState::Idle, HostSessionState::Idle));
    MDR_CHECK(!isValidTransition(HostSessionState::EmulatorRunning, HostSessionState::EmulatorRunning));
}

MDR_TEST(host_session_state_to_string_is_non_empty_for_every_value) {
    const HostSessionState allStates[] = {
        HostSessionState::Idle,          HostSessionState::Discoverable,       HostSessionState::Pairing,
        HostSessionState::Connected,     HostSessionState::Streaming,          HostSessionState::Launching,
        HostSessionState::EmulatorRunning, HostSessionState::CompanionModeActive, HostSessionState::Suspending,
        HostSessionState::Sleeping,      HostSessionState::Error,
    };
    for (HostSessionState s : allStates) {
        MDR_CHECK(toString(s)[0] != '\0');
    }
}
