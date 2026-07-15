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

## Known gaps vs. the full spec

- No melonDS integration yet (Phase 0 analysis exists; patch not started).
- No pairing/authentication token yet (spec section 13) -- the prototype
  only binds explicitly to a configured address and accepts one client;
  it does not yet reject unauthenticated connections with a token check.
- No mDNS discovery, reconnect-with-backoff, or capability negotiation
  beyond a bare protocol-version check.
- Video transport is raw BGRA8888 over TCP (Stage 1 per spec section 8.4);
  no compression yet.
- The SDL3 client (`client/`) has not been build-verified in this
  environment because no SDL3 development package is available in this
  sandbox; see `docs/building.md`.
