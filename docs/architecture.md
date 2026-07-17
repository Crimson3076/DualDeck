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
                      - net_client.h/.cpp      control/input/video sockets
                      - device_identity.h/.cpp persisted device identity (one, reused for every host)
                      - discovery_client.h/.cpp  one-shot LAN host scan (UDP broadcast)
                      - discovery_store.h/.cpp   persisted last-picked host
                      - bitmap_font.h/.cpp     self-contained 5x7 pixel font,
                                               no font/text-rendering dependency
                                               (needed for the host-selection list --
                                               Gaming Mode has no visible terminal)

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

## Threading model additions (device-approval flow)

- `NetServer` owns a `DeviceApprovalManager`
  (`host/remote-server/include/host/device_approval_manager.h`), consulted
  from `controlLoop`'s handshake path only when `NetServerConfig::authToken`
  is empty. `DeviceApprovalManager` has its own internal mutex
  (independent of `NetServer`'s other locks), since it's a self-contained
  state machine (approved-device set + pending-request queue) that
  doesn't touch any other `NetServer` state.
- The pending-requests-changed callback
  (`NetServerConfig::onPendingRequestsChanged`) fires synchronously on
  `controlLoop`'s thread (or the watchdog thread, for stale evictions),
  while still holding `DeviceApprovalManager`'s internal mutex. The
  melonDS integration (`RemoteServerBridge`) is the one example of a
  caller that needs to hop threads from there (to touch a Qt widget) --
  it does so with `QMetaObject::invokeMethod(qApp, ..., Qt::QueuedConnection)`,
  using `qApp` rather than the not-yet-constructed `mainWindow` as the
  marshaling target, since the remote server starts before
  `EmuInstance::createWindow()` runs. Because `QueuedConnection` only
  enqueues the callback rather than blocking on it, calling
  `approveDevice()`/`denyDevice()` back into `NetServer` from inside the
  eventual Qt-thread handler (in response to a button click) is safe --
  by the time it runs, the original call stack (and its lock) is long
  gone.
- Client-side, there is no equivalent UI-thread/reconnect-thread
  coordination needed any more: approval happens entirely on the host, so
  the client's existing reconnect/backoff loop (unchanged since Phase 2)
  handles "not yet approved" exactly like any other reason `connect()`
  failed -- it just keeps retrying until the host approves it, at which
  point the next retry succeeds. This is simpler than the 6-digit-code
  flow it replaced, which needed a shared `awaitingPairingCode` atomic to
  stop the reconnect thread from retrying while the render thread waited
  on user text input; see git history if that coordination pattern is
  ever needed again for something else.

## Emulator identity model (GitHub issue #28 foundation milestone)

GitHub issue #28 tracks evolving DualDeck from a melonDS-specific tool
into an emulator-independent platform (future Nintendo 3DS and Wii U
adapters alongside the existing melonDS/DS integration). This section
covers the first foundation slice of that issue: shared identity types so
a host can say what it is, without any of that work requiring the full
Host Service extraction, generic input/video-surface model, or 3DS/Wii U
adapters the rest of the issue describes -- those remain future work (see
"What issue #28 still needs" below).

**The types** (`melonds_remote::SystemIdentity`/`AdapterIdentity` in
`protocol/include/melonds_remote/protocol.h`):

```cpp
struct SystemIdentity {
    std::string systemId;    // e.g. "nds", "3ds", "wiiu" -- stable, code may switch on it
    std::string systemName;  // e.g. "Nintendo DS" -- human-readable, for UI only
};

struct AdapterIdentity {
    std::string adapterId;       // e.g. "melonds", "synthetic-test" -- stable
    std::string adapterName;     // e.g. "melonDS" -- human-readable, for UI only
    std::string adapterVersion;  // the adapter's own version string, may be empty
};
```

They're deliberately two separate structs, not one merged blob:
`SystemIdentity` is a fact about *what's being emulated*; `AdapterIdentity`
is a fact about *which integration is driving it*. A future second
DS-capable adapter would share `{"nds", "Nintendo DS"}` but report a
different `AdapterIdentity` -- and the client must never assume otherwise.
This is also why `adapterVersion` is distinct from both
`HelloPayload`/`HelloAckPayload::appVersion` (DualDeck's own release
string) and `kProtocolVersion` (the wire format version): three
independent axes of "what version is this," each answering a different
question.

**Where they travel**: both structs are embedded in `DiscoveryResponsePayload`
(so the host-selection list can show identity before any connection is
attempted) and in `HelloAckPayload` (so a client that already knows a
host's address, bypassing discovery, still learns it, and so the identity
is visible even on a *rejected* handshake -- same convention as
`appVersion`). See `docs/protocol.md`'s "Emulator identity model" section
for the exact wire layout and the `kProtocolVersion` 5→6 bump this
required.

**Who sets them, and how "no hardcoded emulator comparisons" is kept**:
`host::NetServerConfig` carries `systemIdentity`/`adapterIdentity` fields
with clearly-labeled synthetic defaults
(`{"synthetic", "Synthetic Test System"}` / `{"synthetic-test", "Synthetic
Test Adapter", ""}`) -- so the standalone `host/remote-server` binary (and
any test harness constructing `NetServerConfig` directly) is never
mistaken in client UI for a real DS/melonDS session. It can also be
overridden per-instance via `--system-id`/`--system-name`/`--adapter-id`/
`--adapter-name`/`--adapter-version`, letting the standalone binary stand
in as a fake fixture for a future adapter's client-UI tests without that
real adapter existing yet (see `tests/smoke_test.py`'s use of a fake
`"3ds"`/`"Fake 3DS Adapter"` identity to prove the plumbing is generic).
The melonDS integration (`RemoteServerBridge`'s constructor, in
`host/melonds-patches/`) hardcodes the real identity --
`{"nds", "Nintendo DS"}` / `{"melonds", "melonDS", MELONDS_VERSION}` --
directly in its own constructor rather than threading it through as more
constructor parameters, since that class specifically *is* the melonDS
adapter; there's no scenario where a caller would want it to claim to be
anything else. A future 3DS/Wii U adapter's own bridge class would do the
same one-line override in its own constructor with its own values.

Client-side, nothing compares `adapterId`/`systemId` against a hardcoded
string to decide what to render (issue #28: "Do not scatter hardcoded
emulator-name comparisons throughout the Client") -- `client/src/main.cpp`
only ever displays `systemName`/`adapterName` as opaque strings reported
by the host (host-selection list, the connected-session in-app menu's
subtitle line, and the disconnected/reconnecting overlay's "last known"
identity line, which is deliberately never cleared on disconnect so a
dropped connection's status screen still shows what it was connected to).
The one client-side string literal referencing identity content
(`" - "` as a separator, in place of the middle-dot the GitHub issue's
own example text uses) is a rendering-only concession to the client's
self-contained bitmap font having no glyph for that character
(`client/src/bitmap_font.cpp` only has space, `0-9`, `A-Z`, `.`, `-`,
`:`) -- not a comparison against the identity's content.

**Compatibility decision**: adding these fields to `HelloAckPayload`/
`DiscoveryResponsePayload` changed their wire layout, so `kProtocolVersion`
moved 5→6, same policy as every previous wire-layout change in this
project (see `protocol.h`'s version-history comment on
`kProtocolVersion`). A v5 client/host talking to a v6 peer is rejected
outright by the existing whole-packet version check (both
`net_server.cpp` and `discovery_client.cpp` already reject on any
`protocolVersion` mismatch, unchanged by this work) -- there is no partial
compatibility mode where an old peer gets a default/empty identity while
everything else still works. This mirrors every prior bump in this
project's history (v2→v3 pairing removal, v3→v4 app-version fields, v4→v5
microphone support) and keeps the "a release must never silently connect
incompatible Host and Client versions" requirement from issue #28 trivially
true: incompatible versions never complete a handshake at all, so there is
no window where a mismatched pair could produce an incorrect mapping.

**What issue #28 still needs** (explicitly out of scope for this
milestone; tracked as follow-up phases on the issue itself, not attempted
here): extracting a standalone `dualdeck-host-service` decoupled from the
melonDS patch (today's `NetServer` still gets vendored directly into the
melonDS build, unchanged by this work); a versioned emulator-adapter
contract beyond the identity types here (session lifecycle, capability
negotiation, local IPC to an out-of-process adapter); replacing the
single fixed 256x192 DS framebuffer with a list of described video
surfaces; replacing `ControllerState.dsButtons` with a generic input
model that adapters translate into native emulator input; a real 3DS or
Wii U adapter (a *fake* one exists only as a test fixture identity, e.g.
`tests/smoke_test.py`'s `"3ds"`/`"Fake 3DS Adapter"`, not an actual
emulator integration); and installer/manifest support for selecting
adapters independently (coordinated with issue #26).

## Known gaps vs. the full spec

- Authentication offers both a static pre-shared token (compared with a
  constant-time comparison, `constantTimeEquals` in `net_server.cpp`, spec
  section 13's "pre-shared token" option) and, as the default, device
  approval -- a human at the host approves or denies each new client by
  name and address (see `docs/protocol.md`'s "Authentication and device
  approval"), adapted from spec section 13's "six-digit pairing code"
  option after Steam Input turned out not to reliably bring up a virtual
  keyboard in Gaming Mode (see "History: the 6-digit pairing code" in the
  same doc). No QR code or certificate-based pairing, and no UI to
  list/revoke individual approved devices, yet.
- LAN discovery is implemented (custom UDP broadcast request/response,
  not mDNS/SSDP -- see `docs/protocol.md`'s "Discovery payload" section):
  the host advertises itself and the client scans for it instead of
  needing `--host`, always showing a gamepad/keyboard-navigable
  host-selection screen on launch (`client/src/bitmap_font.h`'s
  self-contained bitmap-font renderer, since Steam Deck Gaming Mode has
  no visible terminal for host names to be printed to) -- never silently
  auto-connecting, even with only one host, so switching to a different
  HTPC is always available. No capability negotiation yet, though --
  `clientName`/`clientPlatform`/display size are on the wire but unused
  by the host beyond logging.
- Video transport is raw B,G,R,A bytes-in-memory over TCP (Stage 1 per
  spec section 8.4); no compression yet. On the client this must be
  `SDL_PIXELFORMAT_BGRA32`, not `SDL_PIXELFORMAT_BGRA8888` -- see
  `docs/known-limitations.md`'s "Real-usage bug fixes" section for why
  those aren't the same thing on a little-endian machine.
- The host defaults melonDS's own window to showing only the top screen
  while a client is actively streaming the bottom one (matching SPEC.md's
  "Wii U GamePad" model), restoring whatever screen layout was configured
  before once the client disconnects -- `NetServerConfig::onClientConnectionChanged`,
  wired to melonDS's existing `ScreenSizing` config in `EmuInstance.cpp`.
- The client has an in-app menu (Resume/Change Host/Exit), opened by
  holding both stick clicks (L3+R3) together (or Escape in Desktop
  Mode) -- see `docs/steam-deck-setup.md`. "Change Host" re-enters the discovery/
  selection screen without exiting the process, via an outer loop in
  `main()` around the per-host connect/render/reconnect-thread logic.
- The SDL3 client (`client/`) is now build- and run-verified: SDL3 3.2.16
  was built from source (not packaged for this sandbox's distro -- see
  `docs/building.md`) and the real client binary was run against both the
  standalone host prototype and the actual patched melonDS host,
  including the auto-reconnect thread and the device-approval flow.
  Not yet tested: real Steam Deck hardware/gamepad (see
  `docs/known-limitations.md`).
- Latency instrumentation assumes client and host clocks are reasonably
  synced (e.g. NTP) -- there's no protocol-level clock-offset negotiation,
  so on an unsynced pair the latency numbers are meaningless (the host
  silently skips recording an implausible/negative delta rather than
  logging a nonsense value, but that's a coarse guard, not a fix).
- The stalled-video-reader fix (`SO_SNDTIMEO`) was verified with a manual
  script, not an automated test in `tests/`, since simulating a full TCP
  send buffer reliably in a fast unit/integration test is fiddly (it
  depends on OS socket buffer sizing).
