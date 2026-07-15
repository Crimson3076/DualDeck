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

Run:

```sh
cmake -S . -B build -DMELONDS_REMOTE_BUILD_CLIENT=OFF
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

## Integration smoke test (`tests/smoke_test.py`)

Exercises the real `melonds-remote-server` binary (started with
`--auth-token`) over actual TCP/UDP sockets:

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

## What is not yet tested

- The SDL3 client (`client/`) has no automated tests; it has not been
  build-verified in this development sandbox (no SDL3 package available
  here -- see `docs/building.md`). Manual testing on a Steam Deck or Linux
  desktop with SDL3 installed is required before relying on it. This
  includes the client's auto-reconnect thread (`client/src/main.cpp`) --
  its logic was reviewed and `client/src/net_client.cpp`/`.h` were
  compiled standalone (outside the SDL3-gated CMake target, since only
  `main.cpp` needs SDL3) with strict warnings to catch data races around
  the added reconnect thread, but the reconnect behavior itself has not
  been exercised end-to-end against a real host.
- No test yet exercises packet loss, delayed/out-of-order UDP delivery
  under real network conditions (only the pure sequence-number logic is
  unit tested), or a long-running (30-minute) session.
- No melonDS integration exists yet, so nothing here tests actual DS
  input injection or real framebuffer capture -- only the standalone
  host/protocol layer against a synthetic frame source and logging input
  sink.
- No CI workflow has been added in this pass; `ctest` and
  `tests/smoke_test.py` are meant to be run manually (or wired into CI in
  a follow-up) until then.
