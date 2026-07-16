# Troubleshooting

Practical fixes for problems you're likely to hit running this prototype.
For "is this feature done yet" questions see `docs/known-limitations.md`
instead.

## Build problems

### `find_package(SDL3)` fails / client won't configure

The client target requires an SDL3 development package
(`libsdl3-dev`/`sdl3-devel`, or a local build with `SDL3Config.cmake` on
`CMAKE_PREFIX_PATH`). If you only want to build/test the protocol and
host (no display needed), pass `-DMELONDS_REMOTE_BUILD_CLIENT=OFF`
(the default) -- see `docs/building.md`.

### Strict-warnings build fails with `-Werror`

`-Wconversion` in particular is noisy across compiler versions. If you
hit a warning-as-error in code you didn't touch, check whether your
compiler version differs from what this was developed against (GCC 13).
Report it rather than silently dropping `-Werror` -- per `SPEC.md`
section 22, code quality requirements aren't optional.

### `ctest` reports 0 tests

You likely built with `-DMELONDS_REMOTE_BUILD_TESTS=OFF`. It defaults to
`ON`; check your CMake command line if you passed extra flags.

## Runtime problems (host)

### `bind (tcp): Address already in use`

Another `melonds-remote-server` instance (or something else) is already
listening on that port. Either stop it (`pkill melonds-remote-server`
or find the PID with `ss -tlnp | grep 8760`) or pick different ports
with `--control-port`/`--input-port`/`--video-port`.

### `NetServer: no static auth token configured; using pairing-code mode...`

Expected and intentional (spec section 13) if you didn't pass
`--auth-token` -- this is the recommended default, not a warning to fix.
An unrecognized client gets a 6-digit pairing code (shown here, and in
the melonDS window's status bar for the integrated host); once the
client's entered it, it's remembered and won't be asked again. If you'd
rather skip pairing entirely (e.g. scripted testing), pass
`--auth-token`/`MELONDS_REMOTE_AUTH_TOKEN` on the host and the matching
`--auth-token` on the client.

### Client's handshake keeps getting rejected with "pairing required"

- This is expected the *first* time a client connects to a host --
  that's the point at which you're supposed to enter the code. Look at
  the host's log (or, for the melonDS-integrated host, its window's
  status bar) for the current 6-digit code and enter it on the client.
- If it keeps happening on every run instead of just the first: check
  that `$HOME` (or whatever the code is running as) is writable and that
  `~/.config/melonds-remote-client/pairing_tokens.txt` is actually being
  written after a successful pairing -- a read-only home directory would
  make the client "forget" its token every restart.
- A code only stays valid for 5 minutes (`--pairing-code-ttl-s` on the
  host) and is single-use; if you waited too long or already used it,
  the host will show a new one on the next attempt.

### Client's handshake is rejected but the static token looks right

(Only relevant if you're using `--auth-token`/`MELONDS_REMOTE_AUTH_TOKEN`
instead of pairing mode.)

- Check for accidental whitespace/newline differences if the token is
  coming from a shell variable, config file, or copy-paste.
- Confirm the server actually restarted after you changed
  `--auth-token` -- it isn't hot-reloaded.
- Check the server's stderr for the specific `HelloRejectReason`
  (`docs/protocol.md`); `1` is a protocol version mismatch (client/server
  built from different commits), not an auth failure, and needs a
  matching rebuild instead.

### Rapid reconnect attempts get silently refused

You've hit the connection-attempt rate limiter (default: 5 attempts per
10 seconds per source IP, spec section 13). This is by design -- back off
and retry, or raise `NetServerConfig::maxConnectionAttemptsPerWindow`/
`connectionAttemptWindowUs` if you're doing rapid manual testing and find
the default too aggressive.

### Video connects but no frames arrive / connection just sits there

- Confirm the control handshake actually completed and is still
  authenticated -- both the UDP input path and the video path are gated
  on it (`docs/protocol.md` "Handshake" step 3). If the control
  connection dropped (heartbeat timeout, TCP error) after the video
  socket connected, `NetServer::videoLoop` closes that video connection
  outright rather than continuing to stream to an unauthenticated
  session.
- Check the host's stderr for `NetServer: rejecting video connection (no
  authenticated session)`.

### A client seems "stuck" and won't let a new one connect

Only one control connection and one video connection are accepted at a
time (spec section 7.1). If a previous client didn't disconnect cleanly,
wait for the control-channel heartbeat timeout (default 5s of silence) or
the UDP input timeout (default 500ms of silence) to release it, or
restart the server.

### Buttons stay "stuck" after a disconnect

This should never happen -- `IEmulatorInputSink::releaseAll()` is called
unconditionally on graceful disconnect, malformed-control-packet
disconnect, and input/heartbeat timeout (spec section 6.4). If you
observe stuck input with the current `LoggingInputSink`, check its
`lastState()` right after a disconnect; it should read back as
`InputStateTracker::releasedState()` (all zero). If it doesn't, that's a
genuine regression worth a bug report with the exact repro steps -- this
behavior has unit test coverage
(`protocol/tests/test_input_state_tracker.cpp`) and an end-to-end check
in `tests/smoke_test.py`, so it shouldn't regress silently.

## Runtime problems (client)

### Client can't connect at all

- Confirm the host is actually listening on the address/port you passed
  (`--bind` on the host defaults to `127.0.0.1`, which is **not**
  reachable from another machine -- you likely want your LAN IP or
  `0.0.0.0` plus a firewall rule, understanding the security tradeoff in
  spec section 13). The client's reconnect thread will keep retrying with
  backoff regardless, so a fixable misconfiguration doesn't need a
  client restart once corrected.
- If using `--auth-token`, confirm client and host tokens match exactly.
- Check for a firewall blocking the three ports (control TCP, input UDP,
  video TCP) between client and host.

### Reconnect doesn't seem to happen after I kill the host

The client's reconnect thread only starts a new `connect()` attempt when
`NetClient::isConnected()` is false, which requires either
`videoReceiveLoop()` or `heartbeatLoop()` to have noticed the connection
is gone (their next `send()`/`recv()` call fails). This can take up to
the OS's default TCP timeout if the host process was killed uncleanly
(no FIN sent) rather than shut down gracefully -- a clean `Ctrl+C`
shutdown on the host is much faster to detect than `kill -9`.

## Diagnostics

Run the host with `--stats-interval-ms 1000` (or another short interval)
to get frequent periodic logging of input packet rate/out-of-order/
malformed counts, video frame rate/dropped count, and latency avg/min/max
-- see `docs/architecture.md`'s Phase 2 section and `docs/testing.md` for
how this was verified. This is usually the fastest way to tell whether a
"feels laggy" report is actually a network-layer problem or something
else (rendering, input polling rate, etc.) once a real melonDS
integration exists.
