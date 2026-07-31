#pragma once

// The Host Service's own session lifecycle -- broader than HostMode
// (protocol.h, Emulation/HostControl) and melonds_remote::adapter::
// SessionState (an individual adapter's own lifecycle): this describes
// what the *host process as a whole* is doing, from before any client has
// even connected through an emulator running and back down again. See
// docs/known-limitations.md for the design this is the foundation of.
//
// Not yet wired into the wire protocol (no HostSessionState field exists
// in HelloAckPayload/ModeChangedPayload yet -- protocol.h's HostMode is
// still what's actually sent) and not every state below has a real
// signal source in the current codebase yet (Discoverable/Pairing/
// Streaming/CompanionModeActive/Suspending/Sleeping have no code path
// that produces them today -- see mode_coordinator.h's
// computeDesiredHostSessionState() for exactly which subset is wired to
// a real signal right now). Defined in full anyway, matching this
// project's existing practice (adapter-sdk/include/melonds_remote/
// adapter/session_state.h) of designing a state machine's complete shape
// up front even before every transition has a driver, so later work only
// has to wire up new triggers, not invent new states under time pressure.

namespace melonds_remote::host {

enum class HostSessionState {
    Idle,               // host process up, nothing connected yet
    Discoverable,        // actively responding to LAN discovery broadcasts
    Pairing,              // a client is mid-handshake/device-approval
    Connected,            // a client is connected, no adapter/emulator active
    Streaming,            // actively sending video to a connected client
    Launching,            // an adapter/emulator session is starting up
    EmulatorRunning,      // an adapter is actively emulating
    CompanionModeActive,  // host-control/navigation mode, actively assisting
    Suspending,           // host process is winding down for sleep
    Sleeping,             // host process is suspended
    Error,                // something failed; needs operator/log attention
};

const char* toString(HostSessionState state);

// Valid transitions:
//   Idle -> Discoverable -> Pairing -> Connected
//   Idle -> Connected                       -- a client can also connect
//                                               directly (e.g. typed IP),
//                                               skipping discovery/pairing
//   Connected -> Launching -> EmulatorRunning -> Streaming <-> EmulatorRunning
//   Connected -> CompanionModeActive -> Streaming
//   Launching -> Connected                  -- launch cancelled
//   EmulatorRunning -> Connected             -- emulator exited
//   CompanionModeActive -> Connected         -- navigation session ended
//   Streaming -> Connected
//   {Idle, Discoverable, Pairing, Connected, CompanionModeActive} -> Idle
//                                             -- client disconnected
//   Idle -> Suspending -> Sleeping -> Idle    -- host process sleep/wake
//   (any state) -> Error                     -- may fault at any point,
//                                                same convention as
//                                                adapter::SessionState
//   Error -> Idle                            -- ready for a new session
//                                                after cleanup
// Everything else is rejected. Deliberately a plain function, not a
// class, for the same reason session_state.h's isValidTransition() is:
// shareable by a future host-side session tracker and its tests without
// either depending on the other's types.
bool isValidTransition(HostSessionState from, HostSessionState to);

} // namespace melonds_remote::host
