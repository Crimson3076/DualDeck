# ADR 0001: Host Service + Adapter Architecture

**Status**: Accepted; section 3's IPC transport is now implemented (see
its "Implemented" note below) -- see "What this ADR does not decide
yet" for what's still outstanding.
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
section). **Still not done**: `host/remote-server`'s `NetServer` does
not speak this channel yet -- there is no production Host Service
listening on this socket anywhere, only the test/demo harnesses that
prove the mechanism itself works. That wiring remains future work (see
"What this ADR does not decide yet" below).

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

Concretely: the live wire protocol (`protocol.h`, `kProtocolVersion 6`,
`ControllerState.dsButtons` + the fixed `VideoFrame` payload) is **not
changed by this ADR or this phase** and keeps working exactly as today.
When Phase 2/3 extract the Host Service, `NetServer`'s existing
DS-specific parsing stays in place at the wire boundary; a new,
small "DS compatibility adapter" translates a parsed `ControllerState`
into a `GenericInputState` (and a raw DS framebuffer into a
single-surface `SurfaceFrame`) before it reaches the generic
`IEmulatorAdapter` boundary, and translates back the other direction for
anything the adapter needs to report outward. A DS/melonDS client never
has to know the internal model changed underneath it.

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

## Consequences

**Positive**: a concrete, testable target contract exists for a future
3DS/Wii U adapter to implement, proven generic today via fake fixtures
rather than only in prose; the IPC/versioning/compatibility decisions
are made once, deliberately, rather than improvised piecemeal during
the (larger, riskier) Host Service extraction; nothing about today's
live client/host behavior changes, so this phase carries effectively
zero regression risk to the working v0.1 system.

**Negative / accepted cost**: for a while, two independent sets of
types exist side by side -- the production wire types in `protocol.h`
(`DSButton`, `EmulatorAction`, `ControllerState`) and the new generic
contract types in `adapter-sdk/` (`GenericButton`,
`GenericEmulatorAction`, `GenericInputState`) -- with no code path
connecting them yet (the DS compatibility adapter described above is
Phase 2/3 work, not built here). Anyone touching input-related code
during this window must be careful not to conflate the two; this ADR
and the header comments on both sides call out the distinction
explicitly to reduce that risk.

## What this ADR does not decide yet

Explicitly deferred, tracked as later phases on issue #28 itself, not
attempted here:

- Actually extracting `NetServer`/discovery/auth/diagnostics out of the
  melonDS-specific patch into a standalone Host Service process, and
  making that process listen on the `AdapterIpcServer` socket described
  in section 3 (which exists and is tested as a standalone component,
  but nothing in `host/remote-server` constructs or starts one yet) --
  issue #28 Phase 2's remaining work.
- Implementing the DS compatibility adapter described in section 4 --
  only the approach is decided; `protocol.h`'s wire format and
  `host/remote-server`'s `NetServer` are completely unchanged by this
  phase.
- A real 3DS or Wii U adapter (issue #28 Phases 4/5) -- the fake
  fixtures here are capability-shape test doubles only, built with no
  target emulator selected for either system.
- The repository reorganization issue #28 sketches (`core/`,
  `adapters/`, `adapter-sdk/` as siblings of a real Host Service, etc.)
  -- this phase adds a new `adapter-sdk/` top-level directory (additive
  only, nothing existing moved), matching that sketch's naming without
  committing to the rest of it yet.
- Installer/manifest support for selecting adapters independently
  (issue #28 Phase 6, coordinated with issue #26).
