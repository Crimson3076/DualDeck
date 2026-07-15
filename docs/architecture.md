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

host/melonds-patches/ 0001-remote-server-integration.patch: implements the
                      integration described in
                      docs/melonds-integration-analysis.md against real
                      melonDS, vendoring protocol/ + host/remote-server's
                      networking code (unchanged) into melonDS's own
                      build alongside three new melonDS-specific adapter
                      files (MelonDSFrameSource, MelonDSInputSink,
                      RemoteServerBridge). Confirmed to build from a
                      fresh clone and its handshake/auth verified against
                      the running patched binary; the video path is not
                      yet exercised with a real frame -- see
                      host/melonds-patches/README.md.

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
- The SDL3 client now auto-reconnects (`client/src/main.cpp`) with capped
  exponential backoff on a dedicated thread, since `NetClient::connect()`
  does several blocking socket calls. This surfaced (and fixed) two real
  bugs in `NetClient`: a file-descriptor leak on any partial-connect
  failure path, and a `std::thread` reassignment over a still-joinable
  thread (undefined behavior -- calls `std::terminate`) if a reconnect
  attempt started before the previous session's video/heartbeat threads
  had been joined. Socket fd members are now `std::atomic<int>` and
  `connect()`/`disconnect()` are serialized by a mutex, since they can
  now run from a different thread than the caller of
  `sendControllerState()`/`getLatestFrame()`.
- Added periodic diagnostics logging (`NetServer::watchdogLoop`, spec
  sections 8.5 and 14): input packet accept/out-of-order/malformed
  counts and rate, video frames-sent/dropped and rate, and latency
  (avg/min/max). Frame drops are computed from a monotonic frame index
  now returned by `IFrameSource::getLatestFrame()`, so a video thread
  that polls slower than frames are produced can tell how many it
  skipped. Latency requires client and host to share a comparable clock:
  the client's `ControllerState.clientTimestampUs` was previously
  `SDL_GetTicksNS()` (time since `SDL_Init()`, useless across processes)
  and has been changed to wall-clock epoch time; the host uses
  `system_clock` only for this calculation and still uses `steady_clock`
  (immune to wall-clock jumps) for all timeout/sequence logic.
- Fixed an unrelated, pre-existing robustness gap surfaced while testing
  the frame-drop counter: `NetServer::videoLoop`'s `sendAll()` had no
  send timeout, so a stalled/frozen client (TCP receive window never
  drained) could block that thread's `send()` indefinitely once the
  socket send buffer filled. Added `SO_SNDTIMEO` (1s) on the accepted
  video socket; verified live that a stalled reader gets dropped and a
  fresh video connection is accepted again within a few seconds, while
  the rest of the server keeps running throughout (see the
  `stall_test2.py`-style scenario used to verify this, not currently
  checked into `tests/` as an automated case).

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
- Video transport is raw BGRA8888 over TCP (Stage 1 per spec section 8.4);
  no compression yet.
- The SDL3 client (`client/`) has not been build-verified in this
  environment because no SDL3 development package is available in this
  sandbox; see `docs/building.md`. This includes the auto-reconnect
  thread -- its logic was reviewed and `net_client.cpp`/`.h` compiled
  standalone with strict warnings, but reconnect has not been exercised
  end-to-end against a real host with the real SDL3 client binary.
- Latency instrumentation assumes client and host clocks are reasonably
  synced (e.g. NTP) -- there's no protocol-level clock-offset negotiation,
  so on an unsynced pair the latency numbers are meaningless (the host
  silently skips recording an implausible/negative delta rather than
  logging a nonsense value, but that's a coarse guard, not a fix).
- The stalled-video-reader fix (`SO_SNDTIMEO`) was verified with a manual
  script, not an automated test in `tests/`, since simulating a full TCP
  send buffer reliably in a fast unit/integration test is fiddly (it
  depends on OS socket buffer sizing).
