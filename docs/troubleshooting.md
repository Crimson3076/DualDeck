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

### `NetServer: no static auth token configured; using device-approval mode...`

Expected and intentional (spec section 13, adapted) if you didn't pass
`--auth-token` -- this is the recommended default, not a warning to fix.
An unrecognized client's connection request gets queued and logged here
(and pops an Approve/Deny dialog in the melonDS window for the integrated
host); once approved, that client is remembered and won't be asked again.
No code is ever typed on the client (this replaced an earlier 6-digit
pairing code specifically because Steam Input doesn't reliably bring up a
virtual keyboard in Gaming Mode). If you'd rather skip device approval
entirely (e.g. scripted testing), pass
`--auth-token`/`MELONDS_REMOTE_AUTH_TOKEN` on the host and the matching
`--auth-token` on the client.

### Client's handshake keeps getting rejected with "awaiting approval"

- This is expected the *first* time a client connects to a host -- that's
  the point at which a human needs to approve it. Look at the host's log
  for a `pending connection request` line and type the `approve ...`
  command it shows (or, for the melonDS-integrated host, click
  **Approve** on the popup dialog).
- If it keeps happening on every run instead of just the first: check
  that `$HOME` (or whatever the client is running as) is writable and
  that `~/.config/melonds-remote-client/device_id.txt` is actually
  present after the first run -- a read-only home directory would make
  the client generate a brand-new (unapproved) identity every restart
  instead of reusing the same one. The same applies host-side to
  `--state-dir`/`$MELONDS_REMOTE_STATE_DIR`'s `approved_devices.txt`.
- A pending request not retried within `--pending-request-ttl-s` (default
  60s) is evicted from the queue -- if you waited too long before
  approving, the client's next automatic retry will just re-queue it.

### Client's handshake is rejected but the static token looks right

(Only relevant if you're using `--auth-token`/`MELONDS_REMOTE_AUTH_TOKEN`
instead of device-approval mode.)

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

- `--bind` on the host now defaults to `0.0.0.0` (all interfaces), so a
  fresh install should be reachable out of the box -- if you've passed
  `--bind 127.0.0.1` yourself (or set an older `MELONDS_REMOTE_BIND` on
  the melonDS-integrated host from before this default changed), that
  restricts the host to same-machine-only connections, which is exactly
  the "discovery finds it, then every connection attempt says 'connection
  refused'" symptom -- drop the flag/env var, or point it at the host's
  real LAN IP.
- If using `--auth-token`, confirm client and host tokens match exactly.
- Check for a firewall blocking the three ports (control TCP, input UDP,
  video TCP) *and* the discovery UDP port (default 8763) between client
  and host.
- The client now shows a small status banner on screen whenever it isn't
  currently connected (including during reconnect retries), instead of a
  silent dark screen with only stderr logging: "CONNECTING TO
  &lt;address&gt;..." for a plain connection failure, or "WAITING FOR
  APPROVAL ON HOST &lt;address&gt;..." specifically when the host rejected
  the handshake with `ApprovalRequired` (nothing to fix reachability-wise
  in that case -- a human just needs to approve the device on the host,
  see "Client's handshake keeps getting rejected with 'awaiting approval'"
  below). Either way, the address shown is the one discovery found (or
  the one you passed via `--host`), and it's the host at that address you
  need to look at, not a client bug.

### Reconnect doesn't seem to happen after I kill the host

The client's reconnect thread only starts a new `connect()` attempt when
`NetClient::isConnected()` is false, which requires either
`videoReceiveLoop()` or `heartbeatLoop()` to have noticed the connection
is gone (their next `send()`/`recv()` call fails). This can take up to
the OS's default TCP timeout if the host process was killed uncleanly
(no FIN sent) rather than shut down gracefully -- a clean `Ctrl+C`
shutdown on the host is much faster to detect than `kill -9`.

### Client connects fine (controls/touch work) but the screen stays blank

The 3D renderer (**Software**, **OpenGL**, or **OpenGLCompute**) is now
captured either way (see `docs/known-limitations.md`'s
"OpenGL/OpenGLCompute 3D renderer" section), so this should be rare on a
current build. If you still hit it, check the host's log for this
one-time line:

```
melonds-remote: could not capture a bottom-screen frame for the remote
client (GPU::GetFramebuffers() returned no RAM pointer, and the OpenGL
readback fallback also didn't produce one) -- the client will show a
blank/test-pattern screen instead of real video.
```

That means neither capture path worked -- most likely `OpenGLCompute`
specifically hitting some renderer-internal state this project's GL
capture code doesn't handle (it was verified against the plain
`OpenGL` renderer, not `OpenGLCompute` -- see the caveats in
`docs/known-limitations.md`). As a workaround, switch the host's 3D
renderer to **Software** or plain **OpenGL** (**Config > Emu Settings >
Video Settings > Renderer**) and reconnect; please also report which
renderer/GPU combination triggered it. Note that touch/controller input
can work correctly even with no video, since input doesn't depend on the
framebuffer at all -- "controls work but I can't see anything" is this
bug, not a separate touch/input problem.

### The in-app menu pops open by itself when I press a single button (Start, or B) on the real Deck controller

The menu is supposed to require a deliberate Start+Select hold, not a
single button. If a single button opens it on real hardware but not in
any keyboard-driven testing, the most likely cause is Steam Input's
default controller-binding template for a freshly-added non-Steam
shortcut synthesizing a keyboard `Escape` press for individual buttons
(commonly done so keyboard-only UI still works via a controller) --
the client's `Escape` shortcut is Desktop-Mode/no-gamepad testing
convenience only and is ignored whenever a real gamepad is connected, and
the actual Start+Select chord additionally requires a ~350ms continuous
hold before it fires, specifically to guard against this. If you still
see it after updating to a build with both of those fixes, open Steam's
overlay for that non-Steam shortcut (while it's running, or via **Manage
Game > Controller Layout** in Desktop Mode) and check what's bound to
Start/View/B -- setting the layout to a plain "Gamepad" template (rather
than whatever Steam auto-picked) makes buttons pass straight through
rather than also firing keyboard shortcuts.

## Diagnostics

Run the host with `--stats-interval-ms 1000` (or another short interval)
to get frequent periodic logging of input packet rate/out-of-order/
malformed counts, video frame rate/dropped count, and latency avg/min/max
-- see `docs/architecture.md`'s Phase 2 section and `docs/testing.md` for
how this was verified. This is usually the fastest way to tell whether a
"feels laggy" report is actually a network-layer problem or something
else (rendering, input polling rate, etc.) once a real melonDS
integration exists.
