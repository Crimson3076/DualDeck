#pragma once

// Adapter session lifecycle (GitHub issue #28's "Emulator Adapter
// Contract": "Session lifecycle: available, starting, running, paused,
// stopped, error"). Part of the Phase 1 generic-contract groundwork --
// see docs/adr/0001-host-service-and-adapter-architecture.md for why
// this exists before the Host Service itself is extracted (Phase 2).

namespace melonds_remote::adapter {

enum class SessionState {
    Available,  // adapter registered, no active emulation session
    Starting,   // session initializing (e.g. ROM/title loading)
    Running,    // actively emulating: producing frames, accepting input
    Paused,     // emulation paused, session still owned by this adapter
    Stopped,    // session ended cleanly (ROM ejected, emulator closed)
    Error,      // session ended abnormally
};

const char* toString(SessionState state);

// Valid transitions for the lifecycle above:
//   Available -> Starting -> Running <-> Paused -> Stopped
//   (any state) -> Error            -- an adapter may fault at any point
//   Stopped -> Available            -- ready for a new session
//   Error -> Available              -- ready for a new session after cleanup
// Everything else (e.g. Available -> Paused, skipping Starting/Running
// entirely) is rejected. This is deliberately a plain function rather
// than embedding the check inside a state-machine class, so both the
// fake adapter fixtures here and, later, a real Host Service session
// tracker can reuse the exact same rule without depending on each
// other's types.
bool isValidTransition(SessionState from, SessionState to);

} // namespace melonds_remote::adapter
