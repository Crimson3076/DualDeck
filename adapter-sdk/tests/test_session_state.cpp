#include "melonds_remote/adapter/session_state.h"
#include "test_framework.h"

using namespace melonds_remote::adapter;

MDR_TEST(session_state_happy_path_transitions_are_valid) {
    MDR_CHECK(isValidTransition(SessionState::Available, SessionState::Starting));
    MDR_CHECK(isValidTransition(SessionState::Starting, SessionState::Running));
    MDR_CHECK(isValidTransition(SessionState::Running, SessionState::Paused));
    MDR_CHECK(isValidTransition(SessionState::Paused, SessionState::Running));
    MDR_CHECK(isValidTransition(SessionState::Running, SessionState::Stopped));
    MDR_CHECK(isValidTransition(SessionState::Paused, SessionState::Stopped));
    MDR_CHECK(isValidTransition(SessionState::Stopped, SessionState::Available));
}

MDR_TEST(session_state_any_state_may_transition_to_error) {
    MDR_CHECK(isValidTransition(SessionState::Available, SessionState::Error));
    MDR_CHECK(isValidTransition(SessionState::Starting, SessionState::Error));
    MDR_CHECK(isValidTransition(SessionState::Running, SessionState::Error));
    MDR_CHECK(isValidTransition(SessionState::Paused, SessionState::Error));
    MDR_CHECK(isValidTransition(SessionState::Stopped, SessionState::Error));
    MDR_CHECK(isValidTransition(SessionState::Error, SessionState::Error)); // repeated fault
}

MDR_TEST(session_state_error_may_recover_to_available) {
    MDR_CHECK(isValidTransition(SessionState::Error, SessionState::Available));
}

MDR_TEST(session_state_rejects_skipping_starting) {
    // Available -> Running directly (skipping Starting) is not allowed --
    // an adapter must announce it's initializing before it's running.
    MDR_CHECK(!isValidTransition(SessionState::Available, SessionState::Running));
}

MDR_TEST(session_state_rejects_skipping_running) {
    // Starting -> Paused/Stopped directly (skipping Running) is not
    // allowed -- there is nothing to pause/stop before a session ever
    // reached Running.
    MDR_CHECK(!isValidTransition(SessionState::Starting, SessionState::Paused));
    MDR_CHECK(!isValidTransition(SessionState::Starting, SessionState::Stopped));
}

MDR_TEST(session_state_rejects_stopped_to_running) {
    // A Stopped session must go through Available -> Starting -> Running
    // again, not resume directly.
    MDR_CHECK(!isValidTransition(SessionState::Stopped, SessionState::Running));
}

MDR_TEST(session_state_rejects_self_transitions_other_than_error) {
    MDR_CHECK(!isValidTransition(SessionState::Available, SessionState::Available));
    MDR_CHECK(!isValidTransition(SessionState::Running, SessionState::Running));
}

MDR_TEST(session_state_to_string_is_non_empty_for_every_value) {
    const SessionState allStates[] = {
        SessionState::Available, SessionState::Starting, SessionState::Running,
        SessionState::Paused,    SessionState::Stopped,  SessionState::Error,
    };
    for (SessionState s : allStates) {
        MDR_CHECK(toString(s)[0] != '\0');
    }
}
