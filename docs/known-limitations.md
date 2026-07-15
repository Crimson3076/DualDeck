# Known Limitations

This is the consolidated, authoritative list of what does not work yet or
is known to be incomplete, required as its own deliverable by `SPEC.md`
section 25. Individual design docs (`architecture.md`, `protocol.md`,
`testing.md`) call out gaps in context as they come up; this file is the
single place to check "is X done yet" without reading everything else.

## melonDS integration: patch exists, builds, handshake and real-frame delivery verified

`host/melonds-patches/0001-remote-server-integration.patch` implements
the integration against melonDS commit
`10a173b5536fc75cd93f8a3868349dad963542ef`. Unlike the standalone
`host/remote-server` (which still runs against `SyntheticFrameSource` and
`LoggingInputSink` for its own independent testing), the patch wires the
same protocol/host networking code into real melonDS state via
`MelonDSFrameSource`/`MelonDSInputSink`/`RemoteServerBridge`.

**Verified**: the patch applies to a fresh clone and builds from scratch;
the patched binary's embedded remote server starts and correctly performs
a real authenticated TCP handshake (including rejecting a wrong auth
token) against a raw-socket test client, under Xvfb with
`MELONDS_REMOTE_ENABLE=1`. Beyond that, a minimal, fully original homebrew
`.nds` ROM (`tests/homebrew-test-rom/`, written from scratch -- no
copyrighted content) was direct-booted successfully in the patched
binary, and the video path delivered a **stable, non-black, non-test-
pattern** frame consistently across repeated reads and separate process
runs -- confirming `GPU::GetFramebuffers()` → `pushBottomFrame()` → the
network client really does carry live `RunFrame()`-driven output, not a
static placeholder. This was the specific gap flagged as unverified in
an earlier pass of this document.

**Still not verified**:

- The exact pixel channel order (RGBA vs BGRA) of the delivered frame --
  a test with distinctly different R/G/B palette values across two runs
  produced the same output color both times, which doesn't cleanly
  confirm either hypothesis. See `tests/homebrew-test-rom/README.md`.
  Double-check `docs/protocol.md`'s BGRA claim against a real game before
  trusting the client's rendered colors.
- Input injection (`inputProcess()`'s merge of remote `ControllerState`
  into `inputMask`/hotkeys) against a game that actually reads input --
  the test ROM used for verification never reads input, only writes
  display registers.
- Booting to the DS system menu without a cartridge (needs genuine
  Nintendo firmware, deliberately not sought out here, same reasoning as
  not including ROMs in this repository) -- only cartridge direct boot
  was exercised.
- A commercial-cart-style ROM (the test ROM is homebrew, which skips the
  "secure area" decryption step entirely -- untested whether that path
  works).

Concretely, these acceptance criteria from `SPEC.md` section 20 are
**still not met** (need a real commercial ROM + a display + actual
gameplay to attempt):

- (1) melonDS runs a Nintendo DS *game* on the host (a trivial homebrew
  test program has been run, not a game)
- (4)-(8) DS controls/touch actually affect a running game
- (9) touches outside the rendered rect are ignored *in the context of a
  real game*
- (12) 30-minute emulator stability

## The SDL3 client has not been build-verified

`client/` was written and reviewed against the SDL3 API but this
development environment has no SDL3 package available, so:

- `client/src/main.cpp` (which needs SDL3) has never been compiled.
- `client/src/net_client.cpp`/`.h` (which don't need SDL3) have been
  compiled standalone with `-Wall -Wextra -Wpedantic -Wconversion
  -Wshadow -Werror` and are exercised indirectly by
  `tests/smoke_test.py` acting as a bare-socket stand-in for the real
  client, but the actual `NetClient` class integrated with SDL3's event
  loop has not been run.
- The auto-reconnect thread added in the second Phase 2 pass has been
  reviewed for correctness (see `docs/architecture.md`) but not exercised
  against a real host with the real client binary.
- No manual testing has happened on Steam Deck LCD, Steam Deck OLED,
  Gaming Mode, or Desktop Mode (`SPEC.md` section 19's "Manual Steam Deck
  tests" list is entirely open).

**Action required before relying on this**: build `client/` on a machine
with SDL3 installed (`docs/building.md`), fix any API mismatches against
the SDL3 version you have, and run it against `melonds-remote-server`.

## Video transport is Stage 1 only

Raw BGRA8888 frames over TCP, no compression, per `SPEC.md` section 8.4's
"Stage 1: development transport". Bandwidth at 60 FPS is ~11.8 MB/s,
acceptable for LAN but not evaluated against the Stage 2 options
(H.264/H.265/AV1/MJPEG/custom delta encoding) at all yet.

## Authentication is minimal

A single pre-shared token (`--auth-token`), compared in constant time,
checked once at handshake. Not implemented: six-digit pairing codes, QR
codes, certificate-based pairing, or any post-handshake re-authentication
(a session that's been accepted stays accepted until it disconnects or
times out; there is no `sessionId` validation on subsequent packets in
this version, so nothing currently distinguishes a stale session from a
current one at the protocol level beyond the one-connection-at-a-time
plus source-IP-match rule enforced by the transport).

## No discovery, no multi-client, no IPv6

- No mDNS/host discovery -- the client must be given a host address
  directly (`SPEC.md` section 8.1 lists these as "future versions").
- Only one client at a time, by design (`SPEC.md` section 7.1's initial
  scope, and an explicit non-goal in section 21).
- IPv4 only.

## Latency instrumentation assumes synced clocks

The host's periodic latency stat (`docs/protocol.md`'s note on
`clientTimestampUs`) is computed as `hostWallClockNow -
clientTimestampUs`. This only produces a meaningful number if the client
and host system clocks are reasonably synchronized (e.g. both running
NTP), which is a reasonable assumption on a home LAN but is not verified
or negotiated by the protocol. On badly-skewed clocks the number will be
wrong (though implausible deltas are excluded from the running average
rather than silently included).

## No CI-verified client build

`.github/workflows/ci.yml` builds and tests `protocol/` and
`host/remote-server/` (no SDL3 needed) and compiles `client/src/net_client.cpp`
standalone, but does not attempt a full SDL3 client build, since no CI
runner image with SDL3 pre-installed has been wired up. A follow-up should
either install SDL3 in CI or add a client-specific job once a known-good
SDL3 version is pinned.

## Things intentionally out of scope for v0.1

Per `SPEC.md` section 21 (explicit non-goals): ROM transfer, cloud saves,
internet play, multiple simultaneous clients, user accounts, remote
desktop/filesystem browsing, Android/Windows/iOS clients, microphone
streaming, camera emulation, rumble, voice chat, spectator mode, artwork
scraping, cheat databases, a custom emulator core, or replacing melonDS's
rendering. These are not bugs or gaps in this implementation -- they are
deliberately not attempted yet.
