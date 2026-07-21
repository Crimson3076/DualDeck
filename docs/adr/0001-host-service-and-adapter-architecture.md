# ADR 0001: Host Service + Adapter Architecture

**Status**: Accepted; section 3's IPC transport, section 4's DS
compatibility adapter, and section 5's melonDS reference adapter are now
implemented (see their "Implemented" notes below) -- see "What this ADR
does not decide yet" for what's still outstanding.
**Related**: GitHub issue #28 ("Architecture: Decouple DualDeck from
melonDS and add 3DS/Wii U emulator adapters")

## Context

DualDeck (formerly melonDS Remote) started as, and today still is, a
melonDS-specific tool: the wire protocol sends `dsButtons` and fixed
256x192 touch coordinates (`protocol.h`), the client assumes one fixed
DS bottom-screen layout (`client/src/main.cpp`), and the networking
stack (`host/remote-server`'s `NetServer`) is vendored directly into the
melonDS patch (`host/melonds-patches/0001-remote-server-integration.patch`)
rather than existing as anything a second emulator could reuse.

The repository already has two small seams that hint at the intended
shape: `host::IFrameSource` and `host::IEmulatorInputSink`
(`host/remote-server/include/host/`), satisfied today by a
`SyntheticFrameSource`/`LoggingInputSink` pair for the standalone
prototype and by melonDS-specific implementations
(`MelonDSFrameSource`/`MelonDSInputSink`) in the patch. A prior session
phase (GitHub issue #28's "foundation milestone") added shared,
emulator-independent identity types (`SystemIdentity`/`AdapterIdentity`
in `protocol.h`) so the host can say *what* it's emulating and *which*
adapter is driving it, without the client hardcoding any assumptions --
see that phase's own summary in `docs/known-limitations.md`. That work
did not touch surfaces, input, or the networking/adapter boundary
itself.

This ADR covers the rest of issue #28's own "Phase 1: Architecture
decision and generic contracts" milestone: recording the Host Service +
adapter split as an actual decision, defining the generic lifecycle/
video-surface/input/capability contract in code, deciding (not yet
implementing) the local IPC mechanism and its versioning, deciding the
protocol migration/backward-compatibility approach, and adding fake
DS/3DS/Wii U capability fixtures against that contract.

## Decision

### 1. Split into a Host Service and an Emulator Adapter Contract

Two concepts, matching issue #28's proposal:

- **A DualDeck Host Service**: owns LAN discovery, authentication/
  device-approval, the control/input/video/audio transport, protocol
  negotiation, connection ownership, diagnostics, and update
  integration. Runs outside the emulator where practical, owns the
  public network ports, so adapters never open competing listeners.
  Today this role is played by `host/remote-server`'s `NetServer`,
  which is still vendored directly into the melonDS patch -- **that
  extraction is Phase 2 of issue #28, not this ADR's scope.** See "What
  this ADR does not decide yet" below.
- **An Emulator Adapter**: a small, versioned contract each emulator
  integration implements -- identity, session lifecycle, video
  surfaces, input capabilities/mapping, touch surfaces, emulator
  actions, and frame/health reporting. melonDS's `RemoteServerBridge` is
  the reference implementation-to-be; a future 3DS or Wii U integration
  implements the same contract instead of inventing its own networking.

### 2. The contract is a versioned in-process C++ interface today

`adapter-sdk/include/melonds_remote/adapter/adapter_contract.h` defines
`IEmulatorAdapter`, versioned by `kAdapterContractVersion` (independent
of `kProtocolVersion` and `AdapterIdentity::adapterVersion` -- three
separate axes of "what version is this," matching the reasoning
`protocol.h`'s `HelloAckPayload` comments already give for `appVersion`
vs. `kProtocolVersion`). It covers:

- `AdapterCapabilities` (identity + declared `VideoSurfaceDescriptor`
  list + coarse motion/microphone/analog-trigger flags) -- reported
  once at registration.
- `SessionState` (`session_state.h`): `Available -> Starting -> Running
  <-> Paused -> Stopped`, any state `-> Error`, `Stopped`/`Error ->
  Available`. `isValidTransition()` is a free function, not embedded in
  a class, so both a fake fixture and a future real Host Service
  session tracker can share the exact same rule.
- `VideoSurfaceDescriptor` (`video_surface.h`): stable surface ID,
  `SurfaceRole` (Top/Bottom/Tv/GamePad/Auxiliary), dimensions, pixel
  format, orientation, touch support + coordinate range, required/
  locally-displayed/remotely-displayed/selectable flags, nominal/max
  frame rate -- replacing the single fixed 256x192 DS framebuffer
  assumption with a list.
- `GenericInputState`/`TouchContact`/`GenericButton`/
  `GenericEmulatorAction` (`generic_input.h`): standard gamepad buttons,
  D-pad, dual sticks, analog triggers, zero-or-more touch contacts each
  tied to a surface ID, a microphone-active flag, and a versioned
  emulator-action bitmask -- deliberately distinct types from
  `protocol.h`'s `DSButton`/`EmulatorAction`, which remain the DS-wire
  bitmasks the live v6 protocol actually sends. A DS adapter maps
  between the two (see "Protocol migration" below); a non-DS adapter
  maps its own native input straight into the generic set instead.
- `IEmulatorAdapter::applyGenericInput()`/`releaseAllInputs()`/
  `latestFrame()`: mirror `IEmulatorInputSink`/`IFrameSource`'s existing
  no-blocking, latest-frame-wins contracts, generalized to arbitrary
  surfaces instead of "the one DS bottom screen."

No method anywhere in this interface takes a free-text command or path
-- lifecycle/actions are fixed, bounded enums, satisfying issue #28's
"must not expose arbitrary shell execution" requirement structurally,
not by convention.

Proven out today by three fake fixtures under
`adapter-sdk/fake_adapters/` (`FakeDsAdapter`, `FakeThreeDsAdapter`,
`FakeWiiUAdapter`) -- test doubles, not real emulator integrations,
implementing the contract with a DS single-surface shape, a 3DS
top+bottom shape, and a Wii U TV+GamePad shape respectively.
`adapter-sdk/tests/test_adapter_contract_generic.cpp` runs the same
assertions against all three purely through `IEmulatorAdapter&`/
`FakeAdapterBase&`, with no per-adapter branches, as the concrete proof
that the contract doesn't secretly only fit melonDS.

### 3. Local IPC: decided here, now implemented

**Implemented** (`adapter-sdk/ipc/`, GitHub issue #28 Phase 2): the
decision below was carried out in full as new, additive infrastructure
-- `AdapterIpcServer` (implements `IEmulatorAdapter` itself, proxying
every call over the socket to whatever's connected -- callers cannot
tell a local adapter from a remote one apart), `AdapterIpcClient` (wraps
any local `IEmulatorAdapter` and exposes it to a Host Service), and a
real out-of-process `dualdeck-synthetic-adapter` binary
(`adapter-sdk/synthetic_adapter/`) proven to exchange real, changing
frames with a real server across two genuinely separate OS processes
(not just an in-test-process pair) -- see
`docs/known-limitations.md`'s matching entry for the exact verification
performed, including a real bug this caught (see that entry's "Verified"
section).

`host/remote-server`'s `NetServer` now speaks this channel: `main.cpp`
gained an opt-in `--adapter-ipc`/`--adapter-socket PATH` mode that
constructs an `AdapterIpcServer`, waits for an adapter to connect, and
hands it to `NetServer` wrapped in the `AdapterBridge` described in
section 4 below, instead of the default `LoggingInputSink`+
`SyntheticFrameSource` pair. The default (no-flag) invocation is
completely unchanged -- this is additive, not a replacement. See section
4's "Implemented" note for the compatibility-adapter details and
`docs/known-limitations.md` for the real client/server/adapter
cross-process verification performed against this mode.

For an adapter that runs **in the same process** as the Host Service
(today's melonDS patch, and the fake fixtures above), no IPC is needed
at all -- `IEmulatorAdapter` is called directly, matching issue #28's
own "An in-process adapter interface may remain available for the
standalone synthetic host and testing" allowance.

For an **out-of-process** adapter (a future 3DS/Wii U integration that
can't or shouldn't be vendored into the Host Service binary), the
decision is:

- **Transport**: a Unix domain socket on Linux. Path under
  `$XDG_RUNTIME_DIR/dualdeck/adapter.sock` (falling back to
  `~/.cache/dualdeck/adapter.sock` if `XDG_RUNTIME_DIR` is unset, same
  fallback style already used elsewhere in this codebase for
  `~/.config/melonds-remote*` paths), directory mode `0700`, socket mode
  `0600` -- the filesystem permission bits are the "only the current
  user may register an adapter" enforcement (issue #28's requirement),
  no separate authentication token needed for this specific channel
  (distinct from, and not a replacement for, the client<->host
  device-approval flow, which is unaffected by any of this).
- **Framing**: a small versioned binary protocol reusing
  `protocol/`'s existing `appendU16`/`appendString`/`readString`-style
  helpers (length-prefixed fields, explicit little-endian integers) --
  chosen over JSON specifically for consistency with the rest of this
  codebase (there is no JSON dependency anywhere in this repo today) and
  because the exact same "never trust a declared length before
  validating it" discipline `protocol.h` already documents applies
  identically here.
- **Versioning**: every message on this channel is prefixed by
  `kAdapterContractVersion` (see above), rejected outright on mismatch
  -- the same "no partial compatibility, reject the whole message"
  policy `kProtocolVersion` already applies to the client<->host wire
  format, applied here to the Host-Service<->adapter boundary instead.
- **Liveness/stale-adapter detection**: a periodic heartbeat with a
  timeout, mirroring the existing `MelonDSMicAudioSink::isActive(nowUs,
  timeoutUs)` 500ms-liveness-check pattern already in this codebase
  (`host/melonds-patches/`'s `MelonDSMicAudioSink.h`) -- "has this
  adapter said anything recently," not just "does the socket file
  exist."
- **Backpressure / bounds**: per-surface latest-frame-wins (already
  implemented and tested at the fake-adapter level in this phase); a
  maximum frame size bound sized to the largest declared surface's
  `width * height * 4` bytes, enforced the same way `protocol.h`'s
  `kMaxProtocolStringLength`/`kMicAudioSamplesPerPacket` bound every
  other declared size today.

None of the actual socket server/client code exists yet -- that's
Phase 2's "Extract the shared Host Service" work, which needs this
decision made first so the extraction has a target to build toward
rather than improvising the wire format mid-refactor.

### 4. Protocol migration and backward compatibility

Per issue #28's explicit requirement ("The migration must define one of
these explicitly"), the chosen option is:

> A compatibility adapter that maps the old DS messages into the new
> internal model.

**Implemented** (`host/remote-server/src/adapter_bridge.{h,cpp}`,
GitHub issue #28 Phase 2): `AdapterBridge` implements the existing
`host::IEmulatorInputSink`/`host::IFrameSource` interfaces by
translating to/from `melonds_remote::adapter::IEmulatorAdapter` --
exactly the compatibility adapter this section describes. It picks one
target surface at construction time (preferring the first
`remotelyDisplayed` surface, falling back to the first declared one),
maps `ControllerState.dsButtons` to `GenericButton` via an explicit
bit-by-bit table (their bit layouts coincide for A/B/X/Y/D-pad/L/R but
diverge at Start/Select, since `GenericButton` reserves bits for
L2/R2/L3/R3 that DS has no equivalent of), and passes
`EmulatorAction`/`GenericEmulatorAction` straight through as a raw
`uint32_t` since those two enums were deliberately given identical bit
positions. Mic audio is **not** bridged in this phase -- see
`docs/known-limitations.md`.

`host/remote-server/src/main.cpp` gained an opt-in
`--adapter-ipc`/`--adapter-socket PATH` mode: it starts an
`AdapterIpcServer`, blocks until an adapter connects, auto-detects
`SystemIdentity`/`AdapterIdentity` from the connected adapter's
capabilities (unless the corresponding `--system-*`/`--adapter-*` flags
were explicitly given), then constructs `NetServer` with an
`AdapterBridge` wrapping that adapter instead of the default
`LoggingInputSink`+`SyntheticFrameSource` pair. The wire protocol
(`protocol.h`, `kProtocolVersion` still 6) and `NetServer` itself are
**completely unchanged** -- confirmed by re-running the existing
`tests/smoke_test.py`/`tests/device_approval_smoke_test.py` against the
default (no-flag) invocation after every change in this phase, and by a
real cross-process run (a genuine SDL3 `melonds-remote-client`, a real
`melonds-remote-server --adapter-socket`, and the real
`dualdeck-synthetic-adapter` binary as three separate OS processes) that
confirmed the client renders live, animating frames sourced entirely
from the separate adapter process. See `docs/known-limitations.md` for
the exact verification performed.

Concretely: the live wire protocol (`protocol.h`, `kProtocolVersion 6`,
`ControllerState.dsButtons` + the fixed `VideoFrame` payload) is **not
changed by this ADR or this phase** and keeps working exactly as today.
`NetServer`'s existing DS-specific parsing stays in place at the wire
boundary; `AdapterBridge` translates a parsed `ControllerState` into a
`GenericInputState` (and a raw DS framebuffer into a single-surface
`SurfaceFrame`) before it reaches the generic `IEmulatorAdapter`
boundary, and translates back the other direction for anything the
adapter needs to report outward. A DS/melonDS client never has to know
the internal model changed underneath it.

This was chosen over introducing a parallel `kProtocolVersion 7`
generic wire format alongside v6 because there is exactly one wire
format in production today and no second real adapter yet that actually
needs generic surfaces/input *on the wire* -- inventing that format now,
before a 3DS or Wii U adapter exists to validate it against, risks
guessing wrong and having to bump the version again once one does. The
compatibility-adapter approach defers that wire-format decision to
whichever of Phase 4/5 (3DS/Wii U) lands first, informed by a real
adapter's actual needs, while still letting Phase 2's Host Service
extraction proceed today against the stable internal contract defined
here.

**A release must never silently connect incompatible Host and Client
versions and produce incorrect input mappings** (issue #28's explicit
acceptance criterion): this already holds today via
`kProtocolVersion`'s whole-packet-reject-on-mismatch policy, unchanged
by anything in this ADR, and will continue to hold once the internal
generic-model translation described above sits behind that same
unchanged wire boundary.

### 5. melonDS's `RemoteServerBridge` is now the reference `IEmulatorAdapter` implementation, in-process

**Implemented** (`host/melonds-patches/0001-remote-server-integration.patch`,
GitHub issue #28 Phase 2 continuation): melonDS's DS-specific input/video
handling goes through the generic adapter contract now, fulfilling
section 1's "melonDS's `RemoteServerBridge` is the reference
implementation-to-be" line. A new `MelonDSAdapter` class implements
`melonds_remote::adapter::IEmulatorAdapter` -- it wraps the same
`MelonDSInputSink`/`MelonDSFrameSource` this patch already used
(unchanged internally), translating `GenericInputState` back to a wire
`ControllerState` via the exact inverse of `host::AdapterBridge`'s
DS-button table (see that class's own comment for why it's an explicit
bit-by-bit table, not a shift/mask trick) before handing it to
`MelonDSInputSink` exactly as before. `RemoteServerBridge` now
constructs `MelonDSAdapter` + `host::AdapterBridge` (the identical class
`host/remote-server`'s `--adapter-ipc` mode uses) and hands the bridge
to `NetServer` instead of exposing `MelonDSInputSink`/`MelonDSFrameSource`
as `IEmulatorInputSink`/`IFrameSource` directly.

**Deliberately still in-process, not out-of-process**: this reuses the
ADR's own section 3 allowance ("for an adapter that runs in the same
process as the Host Service... no IPC is needed at all") rather than
spawning `melonds-remote-server` as a child process and connecting over
`adapter-sdk/ipc/`. `NetServer`, `DeviceApprovalManager`, LAN discovery,
and `MelonDSMicAudioSink` (mic audio is wired directly to `NetServer` as
its own `IMicAudioSink`, independent of the adapter contract, which only
covers video + controller/touch input) are all completely untouched --
same construction pattern, same public `RemoteServerBridge` interface,
so `EmuInstance.cpp`/`EmuInstanceInput.cpp`/`EmuInstanceAudio.cpp`/
`Window.cpp`/`Config.cpp` needed **zero changes**. This was a deliberate
scope choice: a true out-of-process melonDS adapter (spawning
`melonds-remote-server --adapter-ipc` as a child process) was considered
and set aside for a later milestone, because it opens a real, unresolved
question -- `DeviceApprovalManager`'s pending-request queue and
`approveDevice()`/`denyDevice()` are in-process method calls with no
persisted "pending" state on disk, so a Qt approval dialog running in a
different process than the one actually holding `DeviceApprovalManager`
would need a new cross-process RPC surface that doesn't exist yet. Doing
the in-process version first proves the contract genuinely drives a real
emulator (not just fake fixtures and a synthetic test-pattern generator)
with zero risk to today's device-approval UX, leaving that harder
process-boundary question for when out-of-process operation is actually
attempted.

**Verified**: the full `interactive_pipeline_test.py`-style real-pipeline
proof (see `docs/known-limitations.md` for the exact run) -- a real UDP
`ControllerState` packet for each of A/B/Up, sent through the actual
`NetServer` → `AdapterBridge` → `MelonDSAdapter` → `MelonDSInputSink` →
`EmuInstance::inputProcess()` → `NDS::SetKeyMask()` chain, produced three
distinct, correctly-mapped colors (Red/Green/Blue respectively) via
`tests/homebrew-test-rom`'s real, JIT-executing ROM -- conclusive proof
the double translation (wire `DSButton` → `GenericButton` → wire
`DSButton` again) didn't scramble which physical button does what.

### 6. melonDS as an opt-in out-of-process adapter (GitHub issue #4 Phase A)

**Implemented**: section 5's "deliberately still in-process" deferral is
now partially lifted -- `RemoteServerBridge` gained a second
constructor that connects to an *already-running, standalone*
`melonds-remote-server --adapter-ipc` process over `adapter-sdk/ipc/`,
strictly opt-in via `MelonDSRemote.OutOfProcess` (default false, the
in-process constructor stays the default path with zero behavior
change). This is the prerequisite for GitHub issue #4 (host-side
controls reaching the client before melonDS starts and after it
closes): that needs a Host Service that outlives melonDS's process,
which section 5's in-process design cannot provide by construction.

Section 5's blocking concern -- `DeviceApprovalManager`'s pending-queue/
`approveDevice()`/`denyDevice()` having no cross-process RPC surface --
is **not** solved here, deliberately: out-of-process mode's
`approveDevice()`/`denyDevice()` simply return `false`. A user opting
into out-of-process mode today therefore needs an already-approved
device (state-dir-persisted from a prior in-process run, or a static
`--auth-token`) or Qt's own approval dialog is unreachable. Solving that
properly is still deferred, same as section 5 left it; this phase just
proves the IPC connection itself works reliably enough to build on,
including surfacing and fixing two real concurrency/lifecycle bugs in
`adapter-sdk/ipc/` along the way (reconnect-after-drop crash from an
unjoined thread, and an adapter-side idle timeout when no client is
connected yet) -- see `docs/known-limitations.md`'s matching entry for
both fixes and the full real-pipeline verification, including a
deliberate 7-second zero-input-traffic gap proving the second fix.

### 7. NetServer's target is now runtime-swappable, with a ModeChanged notification (GitHub issue #4 Phase B)

**Implemented**: `NetServer` no longer binds to one
`IEmulatorInputSink`/`IFrameSource` pair for its whole lifetime.
`setTarget(inputSink, frameSource, mode, systemIdentity,
adapterIdentity)` atomically swaps the active pair, releases whatever
the previous target thought was still held, and -- if a client is
already connected -- sends it a new `ModeChanged` packet on the control
channel so it learns the new mode/identity without reconnecting. This
is the other half of Phase A's prerequisite: a Host Service can now
start pointed at a placeholder target (the future `HostControlAdapter`,
issue #4 Phase C) and swap to a real emulator adapter once one connects
via `--adapter-ipc`, then swap back on disconnect, all while a single
client session spans the whole thing.

This section deliberately does **not** decide how or when something
actually *calls* `setTarget()` in response to an emulator
connecting/disconnecting -- that coordination logic (and the manual
override a user might want) is issue #4 Phase D, still open. Nor does
it give the client anything to react to yet: no client build reads from
the control channel after its handshake today, so a sent `ModeChanged`
packet is real on the wire but currently unconsumed (issue #4 Phase E).
This phase only had to prove the mechanism itself -- swap, release,
notify -- works correctly under real concurrent socket traffic; see
`docs/known-limitations.md`'s matching entry for the real end-to-end
test suite that verifies it.

### 8. HostControlAdapter: a virtual gamepad for host navigation (GitHub issue #4 Phase C)

**Implemented**: `HostControlAdapter` is the target section 7's
`setTarget()` will eventually swap *to* -- the thing a Host Service
points `NetServer` at before an emulator has connected and after it
disconnects. It implements `IEmulatorInputSink`/`IFrameSource` directly
(the same interfaces `LoggingInputSink`/`SyntheticFrameSource` already
implement), not the generic `adapter-sdk::IEmulatorAdapter` contract
section 1-3 define for real emulator adapters -- host navigation isn't
"emulating a system" with capabilities/surfaces to negotiate, so that
layer would add nothing. It translates `ControllerState`/`DSButton`
input onto a virtual Xbox-360-style gamepad created via Linux's uinput
subsystem, and always reports no video frame available (there's nothing
to stream while no emulator is running).

This section explicitly does not decide *when* a Host Service actually
constructs a `HostControlAdapter` and calls `setTarget()` with it --
that coordination (start-up default, swap-out on an adapter connecting,
swap-back on disconnect, and any manual override) is issue #4 Phase D,
still open. See `docs/known-limitations.md`'s matching entry for the
translation-logic test coverage and this sandbox's `/dev/uinput`
limitation.

### 9. ModeCoordinator answers section 8's open question (GitHub issue #4 Phase D)

**Implemented**: `host/remote-server`'s `--adapter-ipc` mode now
constructs a real `HostControlAdapter` and starts `NetServer` pointed at
it immediately, instead of blocking until an adapter connects. A new
`ModeCoordinator` polls `AdapterIpcServer::hasConnectedAdapter()` and
calls `NetServer::setTarget()` on every transition -- `HostMode::Emulation`
whenever an adapter is connected, `HostMode::HostControl` otherwise --
answering section 8's deferred "when" question directly: at start-up
(before any client could connect), on an adapter connecting, and on
disconnect. A manual override (`hostcontrol`/`resume` console commands,
sharing the same stdin-reading loop the existing device-approval
commands already use) lets an operator force `HostMode::HostControl`
even while an adapter stays connected, always winning over
auto-detection -- the actual point of an override.

This closes out the four-phase sequence sections 6-9 form together: an
emulator can now connect out-of-process (6), `NetServer` can swap
targets live (7), there is a real target to swap to before/after an
emulator runs (8), and something now actually drives that swap
automatically with a manual escape hatch (9). What remained
unaddressed by all four, at the time this section was written:
`HostControlAdapter`'s virtual gamepad had only ever been exercised via
its pure translation function and a graceful-failure path in this
sandbox (no `/dev/uinput`); and no client build reacted to any of this
yet -- `ModeChanged` packets went out correctly (section 7) and the mode
genuinely changed server-side, but nothing consumed it. See section 10
for how that second gap was closed.

### 10. NetClient reads the control channel; a client UI actually uses host-control mode (GitHub issue #4 Phase E)

**Implemented**: the client half of the gap section 9 left open. Two
things had to exist before a client UI could react to anything: a way
for the client to receive `ModeChanged` at all (it never read the
control channel post-handshake before this), and a way for a client
connecting while the host was *already* in `HostControl` mode to learn
that immediately, since `ModeChanged` only reaches an already-connected
client (section 7) and so can never be the sole mechanism.

`NetClient` gained `controlReceiveLoop()`, a background thread paired
with the pre-existing `videoReceiveLoop()`, that parses everything
arriving on the control socket and reacts to `ModeChanged` while
tolerantly ignoring anything else it doesn't specifically handle --
deliberately the same tolerant-parsing posture `NetServer::controlLoop()`
already has, so a future packet type doesn't need every existing client
build to be reactively patched. `HelloAckPayload` gained a `mode` field
(protocol v7) for the "already in HostControl before I connected" case,
following the exact precedent section 4's protocol-migration policy set
for `micSupported`/`system`/`adapter`: a new negotiated field bumps
`kProtocolVersion`, whereas `ModeChanged` itself (section 7) deliberately
did not, because it isn't part of the version-gated handshake.

`client/src/main.cpp` reacts to the resulting `NetClient::hostMode()` by
showing a dedicated screen in place of the video texture -- there is
nothing to show, since `HostControlAdapter::getLatestFrame()` always
returns `false` -- while continuing to send `ControllerState` on the
same unconditional per-frame cadence it always has, since input was
never the part of this that needed gating on mode; only the picture was.

This closes the loop sections 6-9 opened: an emulator can connect
out-of-process (6) and disconnect again, `NetServer` swaps targets live
and announces it (7), `HostControlAdapter` is a real target to swap to
(8), `ModeCoordinator` drives the swap automatically with a manual
override (9), and now a client can actually be sitting in front of a
person while all of that happens, transparently, without a reconnect.
See `docs/known-limitations.md`'s matching entry for what was verified
(a real `NetClient`-against-real-`NetServer` test suite, and a full
real-binary run of the actual client executable against the actual
server and synthetic-adapter binaries) and what remains
sandbox-unverifiable (uinput on real hardware, a real display).

**What section 10 does not close**: the product-packaging gap noted in
`docs/known-limitations.md`'s Phase E entry -- nothing yet starts the
standalone Host Service independently of melonDS in the actual shipped
release, so host-control mode has no trigger in the real product outside
of this phase's own scripted verification against the standalone
binaries. GitHub issue #4 Phase F closes this, but deliberately as an
opt-in, clearly-labeled-experimental launch path rather than the
default -- see `docs/known-limitations.md`'s matching Phase F entry for
why: it needs a static auth token because melonDS's interactive
device-approval dialog has no bridge to a Host Service running in a
different process, which is real, separate, unfinished work this ADR's
"What this ADR does not decide yet" section already flagged
(`approveDevice()`/`denyDevice()` returning `false` in out-of-process
mode) and Phase F does not attempt to rush.

### 11. AzaharAdapter proves the generic contract with a second real emulator (Nintendo 3DS)

**Implemented**: `AzaharAdapter`, implementing this ADR's
`IEmulatorAdapter` contract (section 2/issue #28 Phase 1) against
[Azahar](https://github.com/azahar-emu/azahar) instead of melonDS -- the
first real test of whether that contract, designed and proven only
against fake DS/3DS/Wii U fixtures until now, actually holds up against
a second genuine emulator. It does, unmodified: no change to
`adapter_contract.h`, `generic_input.h`, `video_surface.h`, or
`session_state.h` was needed. See `docs/azahar-integration-analysis.md`
(this milestone's own Phase 0) and `docs/known-limitations.md`'s
matching entry for the full technical detail; this section records the
architectural decisions.

Two decisions worth calling out because they depart from melonDS's own
pattern:

- **Out-of-process only.** melonDS defaults to in-process (owning its
  own `NetServer` and interactive device-approval), with out-of-process
  as Phase A's later opt-in addition. Azahar has no in-process mode at
  all -- it only ever connects out via `AdapterIpcClient` to an
  already-running standalone Host Service, using a static shared secret
  in place of device-approval (the same trade-off host-control mode,
  section 9's `ModeCoordinator` work, already shipped and documented).
  This was a scope decision, not a technical limitation of Azahar
  itself: reimplementing `NetServer`'s device-approval Qt dialog a
  second time, for a second emulator, in a first integration that also
  needed to rework the host launcher and add a
  patch-your-own-emulator feature in the same pass, was not worth
  doing under time pressure. A future pass could give Azahar (or any
  future adapter) the same in-process/device-approval treatment melonDS
  has, once the cross-process device-approval gap section 10 already
  flagged is actually solved for everyone at once, rather than solving
  it once per adapter.
- **The host launcher no longer assumes melonDS.** Previously,
  `melonds-remote-host.sh`'s only "launch" action went straight to
  melonDS -- there was no other emulator to choose. It now opens a
  picker (DS / 3DS / host-control-only / custom), which is also where
  `scripts/patch-existing-emulator.sh` and
  `host/internal/launch-custom-emulator.sh`'s "bring your own patched
  emulator, no duplicate install" feature plugs in. This is the first
  real evidence the Host Service + Adapter split this whole ADR argues
  for is more than a theoretical nicety: adding a second real emulator
  required zero changes to the wire protocol, the adapter contract, or
  `host/remote-server`'s own code -- only a new adapter implementation
  and launcher-level plumbing.

**What section 11 does not close**: everything `docs/known-limitations.md`'s
matching entry lists under "Still open" -- no real 3DS game has been
tested against this (no display/GPU stack in this sandbox), no
Distrobox path exists for Azahar yet, and this remains deliberately
opt-in and labeled experimental. It also does not solve the
cross-process device-approval gap section 10 flagged -- Azahar's static
shared secret sidesteps it the same way host-control mode already does,
rather than closing it.

## Consequences

**Positive**: a concrete, testable target contract exists for a future
3DS/Wii U adapter to implement, proven generic today via fake fixtures
rather than only in prose; the IPC/versioning/compatibility decisions
are made once, deliberately, rather than improvised piecemeal during
the (larger, riskier) Host Service extraction; nothing about today's
live client/host behavior changes, so this phase carries effectively
zero regression risk to the working v0.1 system.

**Negative / accepted cost**: two independent sets of types still exist
side by side -- the production wire types in `protocol.h` (`DSButton`,
`EmulatorAction`, `ControllerState`) and the generic contract types in
`adapter-sdk/` (`GenericButton`, `GenericEmulatorAction`,
`GenericInputState`). `AdapterBridge` now connects them for
`host/remote-server`'s `--adapter-ipc` mode and, per sections 5-6,
melonDS's own `RemoteServerBridge` in both its in-process and
out-of-process forms -- but `host/remote-server`'s *default* invocation
(no `--adapter-ipc`) still uses `LoggingInputSink`/`SyntheticFrameSource`
directly, bypassing the contract entirely. Anyone touching
input-related code must still be careful not to conflate the two type
sets; this ADR and the header comments on both sides call out the
distinction explicitly to reduce that risk.

## What this ADR does not decide yet

Explicitly deferred, tracked as later phases on issue #28 itself, not
attempted here:

- Actually extracting `NetServer`/discovery/auth/diagnostics out of the
  melonDS-specific patch into a standalone Host Service process *as the
  default*. `host/remote-server` already builds and runs as its own
  binary (`melonds-remote-server`) and can drive a real out-of-process
  adapter via `--adapter-ipc`; melonDS's own patch can now connect to it
  as a genuinely out-of-process adapter too (section 6), but only
  opt-in -- `RemoteServerBridge`'s in-process constructor (vendoring its
  own copy of `NetServer`, section 5) is still the default, unchanged
  behavior. Section 6's opt-in mode also does not solve
  device-approval's cross-process gap (its `approveDevice()`/
  `denyDevice()` just return `false`); making out-of-process the
  default, and giving it real device approval, are both still open.
- A real Wii U adapter (issue #28 Phase 5) -- still just a
  capability-shape test double, no target emulator selected. The 3DS
  side of this bullet is now done: see section 11's `AzaharAdapter`,
  the first real (non-fixture) adapter besides melonDS's own.
- The repository reorganization issue #28 sketches (`core/`,
  `adapters/`, `adapter-sdk/` as siblings of a real Host Service, etc.)
  -- this phase adds a new `adapter-sdk/` top-level directory (additive
  only, nothing existing moved), matching that sketch's naming without
  committing to the rest of it yet.
- Installer/manifest support for selecting adapters independently
  (issue #28 Phase 6, coordinated with issue #26).
