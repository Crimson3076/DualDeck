# Testing

## Unit tests (`protocol/tests/`)

Dependency-free (no third-party test framework, no network, no melonDS).
Covers exactly the areas required by `SPEC.md` section 19 that exist in
this repo so far:

- Packet header serialization/parsing, magic-number and short-buffer
  rejection (`test_header.cpp`)
- `ControllerState` serialization round-trip, malformed-size rejection,
  out-of-range touch coordinate rejection, invalid `touchActive` rejection
  (`test_controller_state.cpp`)
- Aspect-fit rectangle math and touch coordinate mapping/clamping,
  including "touch outside rendered rect is ignored"
  (`test_touch_mapping.cpp`)
- Sequence-number acceptance/rejection (including 32-bit wraparound),
  timeout detection, and fail-safe reset-to-released-state
  (`test_input_state_tracker.cpp`)
- `Hello`/`HelloAck` payload serialization round-trip, truncated-buffer
  and trailing-garbage rejection, and length-prefixed string bounds
  checking (`test_handshake.cpp`)
- Connection-attempt rate limiting: per-key sliding window, independent
  budgets per key, recovery after the window elapses, and stale-entry
  pruning (`test_rate_limiter.cpp`)

`NetServer`'s periodic diagnostics logging (input accept/out-of-order/
malformed counts and rate, video sent/dropped and rate, latency avg/min/
max -- spec sections 8.5 and 14) and the `SO_SNDTIMEO` fix for a stalled
video reader were verified manually (see `docs/architecture.md`'s Phase 2
section) rather than as an automated `tests/` case, since reliably
triggering a full TCP send buffer in a fast test is OS-buffer-size
dependent. Run the server with `--stats-interval-ms 1000` and watch
stderr while driving it with `tests/smoke_test.py`-style traffic if you
want to see the log line yourself.

Run:

```sh
cmake -S . -B build -DMELONDS_REMOTE_BUILD_CLIENT=OFF
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

## Integration smoke test (`tests/smoke_test.py`)

Exercises the real `melonds-remote-server` binary (started with
`--auth-token`, the static-token path) over actual TCP/UDP sockets:

- a handshake with the wrong auth token is rejected (`accepted=0`,
  `rejectReason=AuthenticationFailed`)
- an unauthenticated UDP sender cannot inject `ControllerState` input, and
  cannot open a video connection, even if it knows the ports -- this is
  the fix for the gap described in `docs/architecture.md` where the
  Phase 1 skeleton accepted UDP input from anyone regardless of
  handshake state
- a handshake with the correct token is accepted and receives a session ID
- a valid UDP `ControllerState` packet is accepted
- a deliberately corrupted packet (bad magic) is dropped, not crash the
  process
- a TCP video-frame read returns the expected size
- a control-channel disconnect triggers the host's fail-safe input
  release
- rapid repeated connection attempts are eventually rate-limited

This is the fastest way to validate the acceptance criteria in `SPEC.md`
section 20, items 10 ("host releases all inputs when client disconnects")
and 11 ("bottom screen runs at or near 60 FPS" -- indirectly, by
confirming the video path delivers correctly-sized frames), plus section
13's authentication and rate-limiting requirements, without needing
melonDS or the SDL3 client built.

```sh
python3 tests/smoke_test.py build/host/remote-server/melonds-remote-server
```

Exits non-zero and prints the server's log output on any failure.

## Device-approval smoke test (`tests/device_approval_smoke_test.py`)

Exercises the same `melonds-remote-server` binary in its **default**
mode (no `--auth-token`, so device-approval mode is active), covering the
state machine `DeviceApprovalManager` implements (spec section 13,
adapted -- see below): an unrecognized device identity is rejected with
`ApprovalRequired` and gets a pending-request line logged; sending
`approve <id>` on the server's stdin approves it and lets that same
identity connect; reconnecting with it again succeeds silently with no
re-approval; a different, never-seen identity is not accidentally
approved; `deny <id>` leaves an identity rejected; and an approved
identity survives a full host process restart when `--state-dir` is
given.

```sh
python3 tests/device_approval_smoke_test.py build/host/remote-server/melonds-remote-server
```

This is a protocol-level test (raw sockets standing in for a client, and
writing directly to the server's stdin standing in for a human operator).
The corresponding real-client verification -- the actual
`melonds-remote-client` binary detecting `ApprovalRequired`, showing its
"awaiting approval" status, and connecting once approved on the host,
with no typing anywhere -- is documented in
`host/melonds-patches/README.md`.

Note: this replaces an earlier `tests/pairing_smoke_test.py` that
exercised a 6-digit-code-typed-on-the-client flow (spec section 13's
"six-digit pairing code" option as originally proposed). That flow was
replaced with device-approval because Steam Input doesn't reliably bring
up a virtual keyboard in Gaming Mode, so requiring the client to type
anything wasn't a workable UX -- see `docs/known-limitations.md`.

## What is not yet tested

- Real Steam Deck hardware: the SDL3 client has been build- and
  run-verified (real handshake/device-approval, real sustained
  video/input traffic against both the standalone host and the actual
  patched melonDS host -- see `docs/known-limitations.md`), but only
  headlessly in this sandbox, with no physical gamepad. Manual testing on
  real Deck hardware (Gaming Mode and Desktop Mode, both LCD and OLED
  models) is still needed.
- No test yet exercises packet loss, delayed/out-of-order UDP delivery
  under real network conditions (only the pure sequence-number logic is
  unit tested).
- A real commercial DS game has not been (and, per this project's own
  constraints around not including/sourcing commercial ROMs, can't be in
  this environment) tested; verification of input injection and video
  capture used a minimal, fully original homebrew program instead --
  see `tests/homebrew-test-rom/README.md` and
  `host/melonds-patches/README.md`.
- `.github/workflows/ci.yml` builds and tests `protocol/` and
  `host/remote-server/` and compiles `client/src/net_client.cpp`
  standalone, but does not attempt a full SDL3 client build (no SDL3
  system package pinned in the CI image yet) -- see
  `docs/known-limitations.md`.
