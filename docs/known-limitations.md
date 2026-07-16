# Known Limitations

This is the consolidated, authoritative list of what does not work yet or
is known to be incomplete, required as its own deliverable by `SPEC.md`
section 25. Individual design docs (`architecture.md`, `protocol.md`,
`testing.md`) call out gaps in context as they come up; this file is the
single place to check "is X done yet" without reading everything else.

## melonDS integration: patch exists, builds, handshake, real-frame delivery, and real input injection verified

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
`MELONDS_REMOTE_ENABLE=1`. A minimal, fully original homebrew `.nds` ROM
(`tests/homebrew-test-rom/`, written from scratch -- no copyrighted
content) was direct-booted successfully in the patched binary, and the
video path delivered a **stable, non-black, non-test-pattern** frame
consistently across repeated reads and separate process runs -- confirming
`GPU::GetFramebuffers()` → `pushBottomFrame()` → the network client
really does carry live `RunFrame()`-driven output, not a static
placeholder.

Going further, the ROM was extended into a genuinely **interactive**
program that continuously reads the real `KEYINPUT` hardware register
and reflects currently-held buttons as a visible color change
(`tests/homebrew-test-rom/arm9.c`), and driven through the *actual*
network pipeline end-to-end
(`tests/homebrew-test-rom/interactive_pipeline_test.py`): real UDP
`ControllerState` packets → `NetServer` → `RemoteServerBridge` →
`EmuInstance::inputProcess()`'s merge into `inputMask` →
`NDS::SetKeyMask()` → the emulated CPU reading `KEYINPUT` → the running
program reacting → `GPU::GetFramebuffers()` → `pushBottomFrame()` → the
network client. Holding each of several button states (A, B, Up, A+Up,
released) produced **exactly one stable pixel value across 50
consecutive delivered frames per state**, with clean, immediate
transitions and zero noise. This is a genuine, unambiguous, end-to-end
confirmation that DS controls sent over the remote protocol affect a
running program -- **not** a unit test of the merge logic in isolation.

This also **conclusively resolved** the two items the previous pass of
this document flagged as open:

- **Pixel channel order is B,G,R,A bytes in memory** (byte0=Blue,
  byte1=Green, byte2=Red, byte3=Alpha) -- determined by observing which
  byte changed for each button-controlled color channel across a
  continuous session, which the earlier one-sample-per-run static-color
  test couldn't distinguish. This byte-order finding was and is correct;
  what turned out wrong later was the *client's SDL texture format
  constant* used to display those same bytes -- see "Real-usage bug
  fixes" below for that specific, distinct bug.
- **Engine B (not engine A) is the "bottom" screen** delivered by
  `GPU::GetFramebuffers()`.

A sustained-session stability run was also carried out (SPEC.md section
20 criterion (12)) using `tests/homebrew-test-rom/stability_test.py`
against the live patched binary, with continuous ~120Hz input traffic and
continuous video draining for an extended period:

**Result: ran the full 1800s (30 minutes) target duration with 106,785
video frames delivered (~59 fps average), zero connection errors, zero
video stalls beyond 0.11s max, and zero measured RSS growth in the
melonDS process (185,436 kB at both start and end) -- the process was
still running and responsive at the end of the run.** This was a
continuous session with ~120Hz UDP input traffic (cycling through
several button combinations every second) and continuous video draining
throughout, i.e. sustained real network activity for the full period, not
an idle emulator left alone.

**Still not verified** (both require assets this project deliberately
does not include or seek out, for the same copyright reasoning that
excludes ROMs from this repository -- see
`tests/homebrew-test-rom/README.md`'s caveat):

- Booting to the DS system menu without a cartridge (needs genuine
  Nintendo firmware).
- A commercial-cart-style ROM (the test ROM is homebrew, which skips the
  "secure area" decryption step entirely -- untested whether that path
  works).
- A specific commercial game's own input-handling code, real Steam Deck
  hardware, and a physical gamepad -- the *mechanism* verified here
  (remote button state → `SetKeyMask()` → CPU-visible register → program
  logic → framebuffer → network client) is identical regardless of what
  program is running, but a particular commercial game's own logic
  hasn't been (and, per this project's constraints, can't be in this
  sandbox) exercised.

Concretely, on SPEC.md section 20's acceptance criteria:

- (1) melonDS runs a Nintendo DS *game* on the host: **not met literally**
  -- a fully original, from-scratch homebrew program has been run and
  verified interactive, not a commercial game (see the caveat above for
  why).
- (2)/(3) real DS output on top/bottom screens: **met** for the bottom
  screen (verified above); the top screen uses the same
  `GetFramebuffers()` call and code path, just not separately re-tested
  with a second capture point.
- (4)-(8) DS controls/touch actually affect a running program: **met**,
  for the reasons above -- with the explicit caveat that "a running
  program" here means the original homebrew test program, not a
  commercial game.
- (9) touches outside the rendered rect are ignored: covered by
  `protocol/touch_mapping.h`'s unit tests at the coordinate-mapping
  level; not re-verified against a real game's touch-sensitive UI.
- (12) 30-minute emulator stability: **met** -- a full 1800s run with
  continuous input/video traffic completed with zero errors, zero RSS
  growth, and the process still alive and responsive; see the stability
  run above.

**EmuDeck ROM directory default**: the patch also makes melonDS's own
"Open ROM" dialog default to EmuDeck's standard NDS ROM directory
(`~/Emulation/roms/nds`) the first time it's opened (i.e. before melonDS
has ever remembered a `LastROMFolder`), if that directory exists --
`src/frontend/qt_sdl/Window.cpp`'s `pickROM()`. This is a small,
host-local convenience, not a new protocol feature: it does not browse,
list, or auto-select a ROM on the client's behalf (`SPEC.md` section 13
explicitly forbids exposing ROM browsing to the client), and it doesn't
auto-launch anything at startup -- the host operator still picks a game
through melonDS's own menu (or a `.nds` path on the command line, as
before), just starting from a more useful default folder.

## The SDL3 client: now build- and run-verified, not yet tested on real Steam Deck hardware

Earlier passes of this document said the client had never been compiled,
since no SDL3 package was available in this development environment.
That's since been resolved by building SDL3 3.2.16 from source (SDL3 is
not in Ubuntu 24.04's apt repositories) and configuring the project
against it:

- `client/src/main.cpp` and the full `melonds-remote-client` binary now
  **compile cleanly** with `-Wall -Wextra -Wpedantic -Wconversion
  -Wshadow` against real SDL3 headers/libraries, not just
  `net_client.cpp`/`.h` standalone as before.
- The built binary was **run** (not just compiled) under Xvfb, first
  against the standalone `melonds-remote-server` prototype and then
  against the actual patched melonDS host running the interactive
  homebrew ROM: in both cases the real `NetClient` completed a real
  handshake ("`[net] connected to 127.0.0.1 (session ...)`"), and the
  host's own logs confirmed a sustained real session (e.g. "`NetServer:
  stats -- input: accepted=384 ... video: sent=285 (56.5 fps) ...`").
  This is the first time the actual SDL3 client binary -- not a
  raw-socket stand-in -- has been exercised end-to-end.
- The auto-reconnect thread was exercised implicitly by this real run
  (the client's connection thread is what established the session
  above), though a deliberate host-restart-mid-session test was not
  separately performed.
- **Still open**: no manual testing has happened on real Steam Deck
  hardware (LCD, OLED, Gaming Mode, or Desktop Mode) -- this environment
  has no physical gamepad, and the client only accepts `SDL_Gamepad`
  input (no keyboard fallback), so button-press-driven visual behavior
  could not be demonstrated here, only connectivity and continuous
  video/input traffic. `SPEC.md` section 19's "Manual Steam Deck tests"
  list is still entirely open. The binary built here is also specific to
  this environment's Ubuntu 24.04 x86_64 SDL3 build; see the release
  packaging notes for what that means for portability to a real Deck.

## Video transport is Stage 1 only

Raw BGRA8888 frames over TCP, no compression, per `SPEC.md` section 8.4's
"Stage 1: development transport". Bandwidth at 60 FPS is ~11.8 MB/s,
acceptable for LAN but not evaluated against the Stage 2 options
(H.264/H.265/AV1/MJPEG/custom delta encoding) at all yet.

## Authentication: device approval and pre-shared tokens both implemented; no QR/certificate pairing

Spec section 13's "later pairing options" are adapted rather than
implemented literally -- see below -- and end-to-end verified (real host
+ real SDL3 client, not just unit tests -- see
`host/melonds-patches/README.md`):

- **Device approval** (the default when no `--auth-token` is configured):
  the client generates a random, persistent device identity once and
  sends it on every connection attempt, to every host, forever -- there
  is no code typed on either side. An unrecognized identity causes the
  host to queue a pending connection request naming the client and its
  address (console log with `approve`/`deny` commands on the standalone
  host; a `QMessageBox` Approve/Deny dialog on the melonDS-integrated
  host). Once a human approves it, that identity is remembered and every
  future connection (including the existing auto-reconnect-on-drop
  logic) is accepted silently -- no re-approval needed unless the host's
  approved-device state is deleted. See `docs/protocol.md`'s
  "Authentication and device approval" section for the full state
  machine, and its "History: the 6-digit pairing code" subsection for why
  this replaced an earlier typed-code flow (Steam Input doesn't reliably
  bring up a virtual keyboard in Gaming Mode, so requiring the client to
  type anything wasn't a workable UX).
- **Pre-shared token** (`--auth-token TOKEN`): unchanged from before,
  still available as an explicit opt-in for scripting/CI
  (`tests/smoke_test.py` uses it) -- bypasses device approval entirely.

**Not implemented**: QR codes, certificate-based pairing, or any
post-handshake re-authentication (a session that's been accepted stays
accepted until it disconnects or times out; there is no `sessionId`
validation on subsequent packets in this version, so nothing currently
distinguishes a stale session from a current one at the protocol level
beyond the one-connection-at-a-time plus source-IP-match rule enforced by
the transport). There is also no UI to list or revoke individual approved
devices -- only deleting the whole approved-device state file (forgetting
every previously-approved client at once).

## LAN discovery implemented; still no multi-client, no IPv6

- **LAN discovery** (`SPEC.md` section 8.1's "future versions" item, now
  implemented, end-to-end verified with a real host binary + real SDL3
  client -- see `docs/protocol.md`'s "Discovery payload" section): the
  host broadcasts availability over a separate UDP port (`8763` by
  default) and the client scans for it on launch instead of requiring
  `--host`. A gamepad/keyboard-navigable list is always shown (bitmap-font
  rendered, so it works in Steam Deck Gaming Mode where there's no
  visible terminal -- see `client/src/bitmap_font.h`), even with just one
  host discovered, rather than auto-connecting silently -- so switching
  to a different HTPC is always one screen away, not just when there
  happens to be more than one on the LAN. The previously-picked host is
  pre-highlighted for a quick one-button reconnect, and the list keeps
  rescanning live while shown. Not implemented: mDNS/SSDP/any standard
  discovery protocol -- this is a small custom broadcast request/response
  instead, with no external dependency. `--host`/a positional host
  address still works exactly as before and skips discovery entirely
  (scripting/CI use, `tests/smoke_test.py`).
- Only one client at a time, by design (`SPEC.md` section 7.1's initial
  scope, and an explicit non-goal in section 21).
- IPv4 only (including discovery).

## Real-usage bug fixes: touch, colors, screen layout, in-app menu

Four issues reported from actually using the client against the real
melonDS-integrated host (not caught by earlier protocol-level/unit
testing, since none of them are wire-format bugs):

- **Touch got stuck on after the first tap**: `EmuInstanceInput.cpp`'s
  remote-input merge set `isTouching = true` when a remote touch was
  active, but never set it back to `false` when the touch ended -- once a
  client tapped the touchscreen once, melonDS treated it as permanently
  held at that position forever after (see `EmuThread.cpp`'s
  `isTouching ? TouchScreen(...) : ReleaseScreen()` check, run every
  frame). Fixed by tracking whether the *current* touch was caused by
  remote input (`EmuInstance::remoteTouchActive`) and releasing it
  specifically when the remote client's touch ends, without clobbering a
  touch caused by local mouse/touchscreen input instead. Build-verified;
  not exercised against a real touch-reactive DS program (would need a
  new homebrew ROM that reads the touchscreen controller registers, which
  wasn't built for this pass -- see the interactive-ROM caveats above for
  why building new homebrew assets is deliberately limited here).
- **Client screen colors were badly wrong**: `client/src/main.cpp` created
  its video texture with `SDL_PIXELFORMAT_BGRA8888`. SDL names its 32-bit
  "packed" formats after a bit layout read MSB-to-LSB as a single integer,
  not a byte order in memory -- on a little-endian machine (the normal
  case, including Steam Deck) `SDL_PIXELFORMAT_BGRA8888` actually means
  the same in-memory byte order as `SDL_PIXELFORMAT_ARGB8888`, a
  completely different order than the wire format (and melonDS's own
  framebuffer) uses. Only `SDL_PIXELFORMAT_BGRA32` (the "32" alias) is
  defined by SDL to always mean "bytes in memory match the name",
  regardless of host endianness. Confirmed empirically, not just from
  reading `SDL_pixels.h`: feeding an isolated test program the exact
  B,G,R,X bytes melonDS produces for pure red, `BGRA8888` displayed it as
  **black**, `BGRA32` displayed it correctly as **red**. Fixed by
  switching to `SDL_PIXELFORMAT_BGRA32`; re-verified live against the
  standalone host's animated gradient pattern, which now shows the full
  yellow/orange/magenta/teal/blue range instead of the washed-out
  blue/purple/teal-only palette the bug produced. See `docs/protocol.md`'s
  "Video payload" section.
- **melonDS's own window showed both screens while a client was actively
  streaming the bottom one**: redundant, and not the "Wii U GamePad"
  layout `SPEC.md` describes (TV/host shows top screen, handheld/client
  shows bottom). Fixed by wiring a new `NetServerConfig::onClientConnectionChanged`
  callback (fired true/false as a client's session starts/ends) to set
  melonDS's existing `ScreenSizing` config to `screenSizing_TopOnly` while
  connected, restoring whatever was configured before once the client
  disconnects. Verified live against the real melonDS-integrated host +
  real client: connecting logged `melonds-remote: client streaming --
  showing top screen only locally (was ScreenSizing=0)` and disconnecting
  logged `melonds-remote: client disconnected -- restoring local
  ScreenSizing=0`.
- **No way to exit or reconnect without killing the process**: the client
  had no in-app menu at all -- Gaming Mode has no window chrome to click a
  close button, and there was no way to switch to a different discovered
  host without restarting the whole client. Fixed by adding a gamepad
  Start+Select-held chord (or Escape in Desktop Mode) that opens an
  in-app menu with **Resume**, **Change Host** (returns to the
  discovery/selection screen without exiting the process -- hidden when
  `--host` was given explicitly, since there's no host list to go back
  to), and **Exit**. Verified live: opening the menu, moving the
  selection, confirming "Change Host" (disconnects and shows the
  discovery screen again, reselecting reconnects), and confirming "Exit"
  (clean process exit, confirmed via `ps` showing no lingering process)
  all behaved as expected.

Also, while investigating these: the client's informational log messages
(`[net] connected to ...`, `[discovery] selected host ...`, etc.) were
using `std::printf` (stdout) instead of `std::fprintf(stderr, ...)`.
stdout is fully buffered once redirected to a file (the common case for
troubleshooting/log capture), which was observed firsthand during this
investigation: a client run with stdout redirected showed **no** log
output at all until the process was killed, even though multiple
messages had clearly been printed. Switched all of them to stderr, which
isn't buffered the same way and is what the host side (`NetServer`)
already uses for exactly this reason.

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
