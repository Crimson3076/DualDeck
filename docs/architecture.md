# Architecture

This document describes the system as implemented so far (Phase 0 analysis
+ Phase 1 skeleton). See `SPEC.md` for the full target design; this file
tracks what actually exists in this repository and how the pieces fit
together.

## Components

```text
protocol/            Transport-independent wire format + pure logic.
                      No sockets, no melonDS, no SDL. Fully unit tested.
                      - protocol.h/.cpp        packet framing, ControllerState
                      - touch_mapping.h/.cpp   render-rect <-> DS coordinate math
                      - input_state_tracker.h/.cpp
                                               sequence/timeout/fail-safe logic

host/remote-server/  Standalone host process. Depends on protocol/ only.
                      - IEmulatorInputSink     seam for melonDS input injection
                        - LoggingInputSink     Phase 1 stand-in (logs, doesn't drive melonDS)
                      - IFrameSource           seam for bottom-screen frames
                        - SyntheticFrameSource Phase 1 stand-in (animated test pattern)
                      - NetServer              TCP control + UDP input + TCP video threads

host/melonds-patches/ Where the melonDS fork's diff will live once the
                      integration described in
                      docs/melonds-integration-analysis.md is implemented.
                      Empty until that patch is proposed and reviewed.

client/               Steam Deck client (SDL3). Depends on protocol/ only.

docs/                 This directory.
scripts/              Convenience launch scripts for local development.
```

## Why a standalone host server before a melonDS patch

Section 24 of `SPEC.md` requires the melonDS integration analysis to exist,
and its proposed patch boundary to be reviewed, before invasive melonDS
changes are made. Rather than block all other progress on getting a full
Qt6/SDL2/OpenGL melonDS build working in every development environment,
`host/remote-server` implements the network and threading model against two
small interfaces (`IEmulatorInputSink`, `IFrameSource`). Today they're
satisfied by a logging stub and a synthetic frame generator; the real
melonDS fork will implement the same two interfaces by reading
`GPU::GetFramebuffers()` and calling `NDS::SetKeyMask`/`TouchScreen`/
`ReleaseScreen()` from the emulation thread, per the call sites documented
in `docs/melonds-integration-analysis.md`. No network code changes when
that lands.

## Threading model (implemented)

```text
NetServer::controlLoop    - one thread, accepts the single TCP control
                             connection, performs the version handshake,
                             and detects graceful disconnect.
NetServer::inputLoop      - one thread, recvfrom() on the UDP input socket,
                             validates + parses each packet, feeds accepted
                             packets to InputStateTracker and then to
                             IEmulatorInputSink::applyControllerState().
NetServer::watchdogLoop   - one thread, polls InputStateTracker::isTimedOut()
                             every 50ms and calls
                             IEmulatorInputSink::releaseAll() the moment a
                             client goes silent for longer than the
                             configured timeout (default 500ms) -- this is
                             what guarantees "no button stays held after a
                             network interruption" even if the TCP control
                             connection hasn't noticed anything is wrong yet.
NetServer::videoLoop       - one thread, accepts the single TCP video
                             connection and, at the configured send rate,
                             pulls the latest frame from IFrameSource
                             (never blocking the producer) and streams it.
SyntheticFrameSource       - its own thread, renders a new frame at a fixed
                             rate into a single-slot, mutex-guarded buffer
                             (latest-frame-wins, matching spec section 16).
```

No thread calls into another thread's blocking I/O directly; all
cross-thread communication goes through the small, mutex-protected
`InputStateTracker` or the single-slot frame buffer.

## Threading model additions (Phase 2: network robustness)

- `NetServer::controlLoop` now rate-limits connection attempts per source
  IP (`ConnectionRateLimiter`) before reading any handshake bytes, parses
  a variable-length `HelloPayload` (client name/platform/display size/
  auth token), checks the auth token against `NetServerConfig::authToken`
  if one is configured, and replies with a structured `HelloAckPayload`
  (accepted flag, reject reason, session ID). It also sets a receive
  timeout (`SO_RCVTIMEO`, `controlHeartbeatTimeoutUs`, default 5s) on the
  accepted socket so a silent-but-still-open TCP connection is detected
  and torn down independently of the UDP input timeout.
- `NetServer::inputLoop` and `NetServer::videoLoop` now both check an
  atomic `clientAuthenticated_` flag plus a source-address match against
  the currently authenticated control client before acting on anything.
  This closes a real gap from the initial Phase 1 skeleton: the UDP input
  port previously accepted any well-formed `ControllerState` packet
  regardless of whether a control-channel handshake had ever completed.
- `NetClient` (client side) now sends the same `HelloPayload`/parses
  `HelloAckPayload`, and runs its own heartbeat thread that sends a
  `Heartbeat` packet on the control channel roughly once a second while
  otherwise idle, so the host's control-channel timeout doesn't fire on a
  live-but-quiet connection.
- `docs/testing.md`'s smoke test now covers: wrong-token rejection,
  confirming an unauthenticated UDP sender cannot inject input or open a
  video connection, and the full authenticated happy path.

## Known gaps vs. the full spec

- No melonDS integration yet (Phase 0 analysis exists; patch not started).
- Authentication is a single shared pre-shared token, compared with a
  constant-time comparison (`constantTimeEquals` in `net_server.cpp`) to
  avoid a length/content timing side-channel (spec section 13's
  "pre-shared token" option). No six-digit pairing code, QR code, or
  certificate-based pairing yet.
- No mDNS discovery or capability negotiation (pixel formats/codecs,
  controller/touch/microphone capability flags) yet -- `clientName`/
  `clientPlatform`/display size are on the wire but unused by the host
  beyond logging.
- No client-side automatic reconnect yet (spec section 7.2); if the host
  connection drops, the SDL3 client currently just stops sending/
  receiving rather than retrying with backoff.
- Video transport is raw BGRA8888 over TCP (Stage 1 per spec section 8.4);
  no compression yet.
- The SDL3 client (`client/`) has not been build-verified in this
  environment because no SDL3 development package is available in this
  sandbox; see `docs/building.md`.
