# melonDS patches

`0001-remote-server-integration.patch` implements the remote-server
integration against upstream melonDS commit
`10a173b5536fc75cd93f8a3868349dad963542ef` (master, 2026-06-07). It
follows the patch boundary proposed in
`docs/melonds-integration-analysis.md` section 5 exactly:

1. `src/frontend/qt_sdl/EmuThread.cpp` — after the existing
   `emuInstance->drawScreen()` call, calls
   `GPU::GetFramebuffers()` itself and, if it returns real pointers
   (software renderer), copies the bottom buffer into the remote
   server's frame source. If it returns false (OpenGL/OpenGLCompute 3D
   renderer instead), falls back to a GPU-side readback via the new
   `GLBottomScreenCapture` class (`remote_server/GLBottomScreenCapture.{h,cpp}`)
   -- see "OpenGL/OpenGLCompute video capture" below.
2. `src/frontend/qt_sdl/EmuInstanceInput.cpp` — `inputProcess()` merges
   the latest remote `ControllerState` into `inputMask`/`isTouching`/
   `touchX`/`touchY`/`hotkeyMask`, the same way local SDL joystick input
   is merged with keyboard input just above it. A new
   `EmuInstance::remoteTouchActive` member (`EmuInstance.h`) tracks
   whether the *current* touch was caused by remote input specifically,
   so it can be released when the remote client's touch ends without
   clobbering a touch caused by local mouse/touchscreen input instead --
   see "What has actually been verified" below for the bug this fixes.
3. Emulator actions ride the same merge, mapped onto existing hotkeys
   (`HK_Pause`, `HK_FastForward`, `HK_SwapScreens`) where a direct
   equivalent already exists.
4. `src/frontend/qt_sdl/EmuInstance.{h,cpp}` — owns a
   `RemoteServerBridge` (null unless enabled), constructed/started in
   `EmuInstance`'s constructor and stopped at the top of its destructor,
   alongside the existing emulation thread's lifecycle. Also computes a
   default approved-device state-file path (`$MELONDS_REMOTE_STATE_DIR` or
   `$HOME/.config/melonds-remote/approved_devices.txt`) and wires a
   pending-requests-changed callback that marshals onto the Qt UI thread
   (`QMetaObject::invokeMethod(qApp, ..., Qt::QueuedConnection)`, since
   the callback fires on `NetServer`'s own thread) to pop a `QMessageBox`
   Approve/Deny dialog per new connection request and mirror a one-line
   summary in `mainWindow`'s status bar. LAN discovery (enabled/port/host
   name) is also configured here, from `MELONDS_REMOTE_NO_DISCOVERY`/
   `MELONDS_REMOTE_DISCOVERY_PORT`/`MELONDS_REMOTE_HOST_NAME` env vars.
   A further callback (`onClientConnectionChanged`) fires true/false as a
   client's session starts/ends, setting melonDS's own `ScreenSizing`
   config to `screenSizing_TopOnly` while someone is actively streaming
   the bottom screen (matching SPEC.md's "Wii U GamePad" model: TV/host
   shows the top screen, handheld/client shows the bottom) and restoring
   whatever was configured before once the client disconnects.
5. `src/frontend/qt_sdl/Window.cpp` — `pickROM()`'s "Open ROM" dialog
   defaults to EmuDeck's standard NDS ROM directory
   (`~/Emulation/roms/nds`) the first time it's opened, if that directory
   exists and melonDS hasn't already remembered a `LastROMFolder`. A
   small, host-local convenience -- see `docs/known-limitations.md` for
   why this doesn't cross into the client-facing ROM browsing spec
   section 13 forbids.

The protocol/host networking code itself (`protocol/`,
`host/remote-server/net_server.{h,cpp}`, `host/remote-server/device_approval_manager.{h,cpp}`,
`emulator_input_sink.h`, `frame_source.h`) is vendored byte-for-byte from
this repository into `src/frontend/qt_sdl/remote_server/` in the patch,
plus three new melonDS-specific adapter files (`MelonDSFrameSource`,
`MelonDSInputSink`, `RemoteServerBridge`) that implement
`IFrameSource`/`IEmulatorInputSink` against real melonDS state instead of
the standalone prototype's synthetic frame source / logging sink.
`DeviceApprovalManager` (the device-approval authentication state
machine, spec section 13) lives in `host/remote-server/` alongside
`NetServer` specifically so it's shared unchanged between the standalone
prototype and this patch, rather than being melonDS-specific.

## What has actually been verified

This was tested against a real, unmodified-then-patched melonDS build in
the same environment this repository was developed in (Ubuntu 24.04,
Qt6, SDL2, GCC 13) — not just written and assumed correct:

1. **Baseline build**: cloned upstream melonDS at the commit above,
   installed its documented dependencies (`cmake extra-cmake-modules
   libcurl4-gnutls-dev libpcap0.8-dev libsdl2-dev libarchive-dev
   libenet-dev libzstd-dev libfaad-dev qt6-base-dev qt6-base-private-dev
   qt6-multimedia-dev qt6-svg-dev`), and confirmed it builds cleanly
   unmodified.
2. **Patched build**: applied the changes described above and confirmed
   `melonDS` still builds cleanly (no new warnings introduced beyond
   what upstream already had), including a full **from-scratch build
   starting from a fresh clone with the patch file applied via `git
   apply`** (not just the in-place edits used to develop it) — this is
   what confirms the `.patch` file itself is complete and self-contained,
   not just the working tree it was extracted from.
3. **Runtime, no ROM**: ran the patched `melonDS` binary under Xvfb
   (`QT_QPA_PLATFORM=xcb`) with `MELONDS_REMOTE_ENABLE=1` and confirmed
   its embedded remote server actually starts and accepts a real TCP
   control handshake from a raw-socket test client -- not the standalone
   prototype, the genuine melonDS process:
   - Handshake with no `MELONDS_REMOTE_AUTH_TOKEN` set: accepted, logs
     the "no auth token configured" warning (spec section 13).
   - Handshake with `MELONDS_REMOTE_AUTH_TOKEN` set: a wrong token is
     rejected (`accepted=0`, `rejectReason=AuthenticationFailed`); the
     correct token is accepted with a real session ID.
4. **Runtime, firmware boot attempt (`--boot always`, no ROM)**: firmware
   CRC checks pass and the boot sequence proceeds, but full "boot to DS
   menu" fails on `Failed to open "firmware.mch"` in this environment
   (no real firmware/NAND assets available -- FreeBIOS alone isn't
   sufficient for a full menu boot, since booting to the system menu
   without a cartridge needs genuine Nintendo firmware, which was
   deliberately not sought out here for the same reason ROMs aren't
   included in this repository).
5. **Runtime, real ROM, direct boot (the actual gap closed in this
   pass)**: rather than stopping at "no ROM available", a minimal, fully
   original homebrew `.nds` ROM was written from scratch (two tiny ARM9/
   ARM7 programs plus a hand-packed header -- see
   `tests/homebrew-test-rom/`, no copyrighted content, no external
   downloads) and direct-booted successfully in the patched binary
   (confirmed via melonDS's own log: `Inserted cart with game code:
   ####`, `Game is now booting`). With the ROM actually running:
   - The remote server's video port delivered a **stable, non-black,
     non-test-pattern** 256x192 frame -- consistent across repeated
     reads and across separate process runs -- proving frames really do
     flow `GPU::GetFramebuffers()` → `pushBottomFrame()` → the network
     client from genuine `RunFrame()` execution, not a static
     placeholder. This is the specific gap flagged as unverified in the
     previous pass of this document.
6. **Runtime, real ROM, actual input injection (the gap closed in this
   pass)**: the static-color ROM above was extended into a genuinely
   interactive program (`tests/homebrew-test-rom/arm9.c`) that
   continuously reads the real `KEYINPUT` hardware register and reflects
   currently-held buttons as a solid backdrop color, then driven through
   the *actual* remote-control network pipeline end-to-end using
   `tests/homebrew-test-rom/interactive_pipeline_test.py`: real UDP
   `ControllerState` packets (the same wire format the SDL3 client sends)
   → `NetServer` → `RemoteServerBridge` → `EmuInstance::inputProcess()`'s
   merge into `inputMask` → `NDS::SetKeyMask()` → the emulated CPU reading
   `KEYINPUT` → the ROM writing a new backdrop color →
   `GPU::GetFramebuffers()` → `pushBottomFrame()` → the network client.
   Holding each of several button combinations (A, B, Up, A+Up, released)
   for a settle period and then sampling 50 consecutive delivered frames
   produced **exactly one stable pixel value per state**, with clean
   immediate transitions and zero noise -- a genuine, unambiguous
   confirmation that DS controls sent over the remote protocol affect a
   running program (closing SPEC.md section 20 criteria (4)-(8) for a
   real, if original/homebrew, program). This also conclusively resolved
   two previously-open questions:
   - **The pixel format is BGRA8888** (byte0=Blue, byte1=Green, byte2=Red,
     byte3=Alpha) -- determined by observing which byte changed for each
     button-controlled color channel.
   - **Engine B is the "bottom" screen** delivered by
     `GPU::GetFramebuffers()`, not engine A.
   The earlier ambiguity (two static-color runs producing the same
   output) is now understood: that test only took one sample per run and
   didn't vary input dynamically within a single run, so it couldn't
   distinguish engine assignment or byte order the way holding different
   button states within one continuous session does. See
   `tests/homebrew-test-rom/README.md` for full results.
7. **Sustained-session stability** (SPEC.md section 20 criterion (12)):
   `tests/homebrew-test-rom/stability_test.py` was run against the live
   patched binary with continuous ~120Hz input traffic and continuous
   video draining; see `docs/known-limitations.md` for the duration
   actually achieved and the frame-count/RSS/error results in this
   sandboxed environment.
8. **(Historical, superseded) Pairing-code flow, real host + real SDL3
   client** (spec section 13): an earlier version of this patch verified
   a 6-digit-pairing-code authentication flow the same way described here
   for device-approval below -- protocol-level against a fresh-clone
   patched binary, and against the standalone prototype with the actual
   `melonds-remote-client` binary, simulated keystrokes standing in for
   Steam's on-screen keyboard. **That flow no longer exists in the
   code**: Steam Input doesn't reliably bring up a virtual keyboard in
   Gaming Mode, so typing a code on the client turned out not to be a
   workable UX, and it was replaced with device-approval authentication
   (see item 11 below and `docs/protocol.md`'s "Authentication and device
   approval" section) -- a human approves/denies by name and address
   instead of the client typing anything. Kept here as a historical
   record, not a description of current behavior.
9. **(Historical, superseded) Pairing code shown at launch, not just on a
   rejected handshake**: a bug where the (now-removed) pairing code only
   ever appeared as a side effect of some client's `Hello` being
   rejected was fixed in the same patch version referenced in item 8.
   Device-approval mode has no equivalent "code visible at launch"
   concept -- there's nothing to show until a specific client actually
   attempts a connection and gets queued as pending.
10. **LAN discovery** (spec section 8.1): `RemoteServerBridge`/
    `EmuInstance.cpp` wire `discoveryEnabled`/`discoveryPort`/`hostName`
    through to `NetServer` (env var overrides:
    `MELONDS_REMOTE_NO_DISCOVERY`, `MELONDS_REMOTE_DISCOVERY_PORT`,
    `MELONDS_REMOTE_HOST_NAME`), sharing the same `NetServer` code as the
    standalone prototype (vendored byte-for-byte, per above) -- so the
    discovery responder logic itself is the exact code end-to-end
    verified against a real host binary + real `melonds-remote-client`
    (broadcast scan and the gamepad-navigable host-selection list shown
    on every launch both confirmed live; see `docs/protocol.md`'s
    "Discovery payload" section). What's confirmed specifically for
    *this* patched binary is a clean build with the new fields threaded
    through; the discovery broadcast itself was exercised against the
    standalone `host/remote-server` prototype rather than re-run against
    this exact melonDS-integrated binary.
11. **Device-approval authentication, real host + real SDL3 client**
    (spec section 13, adapted): verified against the standalone
    `host/remote-server` prototype with the **actual**
    `melonds-remote-client` binary end-to-end, exercising the exact
    `DeviceApprovalManager`/`NetServer` code vendored byte-for-byte into
    this patch (per above) -- not a protocol-level stand-in. Sequence
    confirmed live: client launched with zero prior state → discovery
    scan finds the host → host-selection screen shown (even with only one
    host, per the always-show behavior) → South/A confirms → handshake
    rejected with `ApprovalRequired` → client shows "WAITING FOR APPROVAL
    ON HOST ..." and retries automatically with no user action → a
    pending-request line appears in the host's log/console → `approve
    <id>` typed at the host's stdin → the client's next automatic retry
    is accepted and starts streaming (confirmed via the host's live
    input/video stats and a screenshot of the connected client showing a
    real animated frame, not the waiting banner). Separately confirmed:
    killing and relaunching the client process reuses its persisted
    device identity and reconnects silently with **no** new pending
    request logged and **no** waiting banner shown -- i.e. approval
    genuinely persists across a client restart, matching what
    `--state-dir` promises. What's confirmed specifically for *this*
    patched binary (as opposed to the standalone prototype) is a clean
    build with the `QMessageBox` Approve/Deny dialog wired through;
    triggering that exact dialog live requires a real Qt window with a
    human clicking it, which is why this verification pass used the
    standalone prototype's console `approve`/`deny` commands (same
    underlying `DeviceApprovalManager::approve()` call either way) rather
    than the melonDS-integrated binary's popup specifically.
12. **Auto top-screen-only layout while a client is streaming** (spec
    section 12's "Wii U GamePad" model): verified against *this exact*
    patched binary (not the standalone prototype, since this is
    melonDS-specific `ScreenSizing`/`mainWindow` behavior) running a real
    ROM under Xvfb, with the real `melonds-remote-client` connecting and
    disconnecting. Confirmed via log lines emitted by the new
    `onClientConnectionChanged` callback: connecting logged `melonds-remote:
    client streaming -- showing top screen only locally (was
    ScreenSizing=0)`, and disconnecting logged `melonds-remote: client
    disconnected -- restoring local ScreenSizing=0` -- both directions
    round-tripping correctly through the real `Config::Table` the window
    actually uses. (Stdout buffering caught and worked around during this
    same verification pass -- see the note below.)
13. **Touch-stuck-on fix**: build-verified only (compiles clean with the
    new `remoteTouchActive` tracking described above); not re-exercised
    against a real touch-reactive DS program, since that would need a new
    homebrew ROM reading the touchscreen controller registers, which
    wasn't built for this pass (see the ROM-asset caveats throughout this
    document for why new homebrew assets are added sparingly). The bug
    and fix are still high-confidence from source review alone: the
    original code had no `else` branch to ever clear `isTouching` once a
    remote touch set it, which is an unambiguous logic gap given
    `EmuThread.cpp`'s per-frame `isTouching ? TouchScreen(...) :
    ReleaseScreen()` check.
14. **One-time diagnostic log when the remote client gets no video**
    (real Steam Deck hardware surfaced this: controls/touch worked, video
    didn't): `EmuThread.cpp`'s frame-push block now has an `else` branch
    for when `GPU::GetFramebuffers()` returns false/no bottom buffer
    while `remoteServer` is active, printing a specific one-time message
    telling the host operator to switch the 3D renderer to Software (see
    `docs/known-limitations.md`'s "round 2" real-usage entry and
    `docs/troubleshooting.md`). Verified end-to-end against this exact
    patched binary + the real homebrew test ROM + the real SDL3 client
    under Xvfb: with the software renderer (this sandbox's default), the
    happy path is unaffected (`NetServer: stats -- ... video: sent=286
    (57.1 fps) ...` kept incrementing, and the new warning did **not**
    fire) -- confirming the added `else` branch doesn't regress the
    existing success path. The warning branch itself couldn't be
    triggered in this sandbox (no GPU to switch to an OpenGL renderer
    with), so it's build- and code-review-verified for the failure case,
    not observed firing for real.
15. **OpenGL/OpenGLCompute video capture** (turning item 14's diagnostic
    into an actual fix): a new `GLBottomScreenCapture` class
    (`remote_server/GLBottomScreenCapture.{h,cpp}`) reads the bottom
    screen back directly from `GLRenderer`'s `FPOutputTex` array texture
    (layer 1) when `GetFramebuffers()` returns false, using
    `glReadPixels(..., GL_BGRA, GL_UNSIGNED_BYTE, ...)` so no manual
    byte-swap is needed, with a `glBlitFramebuffer` downscale step for
    users who raised `3D.GL.ScaleFactor` above 1x (the wire protocol
    stays fixed at native 256x192). `EmuThread.cpp`'s `else` branch from
    item 14 now tries this first, keeping the diagnostic log as a
    fallback for whatever it doesn't cover. See
    `docs/known-limitations.md`'s "OpenGL/OpenGLCompute 3D renderer"
    section for the full account -- **verified end-to-end** against this
    exact patched binary + the real homebrew test ROM + the real SDL3
    client, under Xvfb with Mesa's software GL rasterizer
    (`LIBGL_ALWAYS_SOFTWARE=1`, since this sandbox has no GPU but *can*
    still create a real OpenGL context this way, unlike the
    gamepad/Steam Input hardware needed for item 14's original bug):
    with `3D.Renderer=OpenGL` and default `3D.GL.ScaleFactor=1`,
    sustained ~58 fps with the fallback diagnostic **not** firing
    (confirming the direct-read path); with `3D.GL.ScaleFactor=2`
    (512x384 texture, forcing the downscale-blit branch), sustained
    ~57.5 fps with **zero** dropped frames and the fallback diagnostic
    again not firing. Not independently verified: pixel-level content
    correctness (the KEYINPUT-reactive-color test used for the software
    path's original verification predates the protocol's v3 handshake
    bump and its handshake is now rejected -- updating it was out of
    scope for this pass), `OpenGLCompute` specifically (same code path,
    but this sandbox can't create a compute-capable GL context to check),
    and real Steam Deck hardware/GPU (this was verified against Mesa
    software rendering only).
16. **Config/UI toggle for the remote server** (closes the "No Config/UI
    toggle" limitation, and the melonds-remote project's GitHub issue
    #3 -- "integrate into melonDS without launching from run-host.sh"):
    added persisted `MelonDSRemote.*` Config keys (`Config.cpp`) mirroring
    each `MELONDS_REMOTE_*` env var, checked as a fallback whenever the
    matching env var isn't set (`EmuInstance.cpp`), plus an actual
    checkbox -- **"Enable melonDS Remote (Steam Deck streaming)"** -- on
    Emu Settings' General tab (`EmuSettingsDialog.ui`/`.cpp`) for
    `MelonDSRemote.Enable` specifically, the one setting that actually
    needs to be turned on rather than just have a sensible default.
    **Verified end-to-end** against this exact patched binary: launched
    with zero `MELONDS_REMOTE_*` env vars set (simulating a direct
    Steam/EmuDeck shortcut launch, no `run-host.sh`-style wrapper) --
    confirmed the remote server does *not* start; opened Emu Settings via
    the real Qt UI (driven with `xdotool` under Xvfb, screenshotted with
    `import` at each step to confirm the checkbox rendered and toggled),
    checked the new box, clicked OK, clicked through the resulting
    "emulation will be reset" confirmation dialog, and confirmed
    `melonDS.toml` now had `[MelonDSRemote]\nEnable = true`; killed and
    relaunched the process with the same zero-env-var command line, and
    this time the log showed `NetServer: listening on 0.0.0.0
    (control=8760...)` / `melonds-remote: remote server enabled`, with a
    real socket connection to port 8760 succeeding -- confirming the
    Config-only path genuinely starts the server with no env vars or
    wrapper script involved.
17. **Live start/stop toggle + management listener** (GitHub issue
    "Decky plugin to start/stop the host server"): `remoteServer` used
    to be constructed once at process startup and never touched again;
    it's now start-/stoppable at any point during the process's life via
    `EmuInstance::startRemoteServer()`/`stopRemoteServer()` (idempotent),
    both guarded by a new `remoteServerMutex` that `EmuThread.cpp`'s
    frame-push and `EmuInstanceInput.cpp`'s input-merge now hold for as
    long as they actually touch `remoteServer`, not the whole
    surrounding frame/function (so an in-flight toggle never has to wait
    for e.g. frame-rate-limiting sleeps elsewhere in the loop). A new
    `ManagementServer` class
    (`remote_server/ManagementServer.{h,cpp}`) is a small, separate,
    always-on (once `MelonDSRemote.ManagementToken` is set -- empty by
    default, opt-in only) TCP listener, independent of whether remote
    streaming itself is on, accepting `TOKEN ENABLE`/`DISABLE`/`STATUS`
    from anything on the LAN (e.g. the `decky-plugin/` at the repo root).
    **Verified end-to-end** against this exact patched binary: `STATUS`
    while off (`OK DISABLED`), a wrong token (`ERROR bad token`),
    `ENABLE` (`OK ENABLED`, and a real client actually connected and
    streamed), `DISABLE` while that client was connected (its connection
    was refused immediately, and the existing
    `onClientConnectionChanged` screen-sizing-restore callback fired
    correctly -- confirming a forced shutdown via this channel hits the
    same cleanup path as a graceful disconnect), then a second
    `ENABLE`/idempotent-re-`ENABLE`/`STATUS` cycle, confirming the
    remote server can be stopped and restarted repeatedly within one
    process. See `docs/known-limitations.md`'s "Live-toggle" section and
    `decky-plugin/README.md` for the Decky plugin side, including the
    boundary between what's verified there and what's an unverified
    best-effort match to the official plugin template (no real Decky
    Loader runtime exists in this project's environment).

**A stdout-buffering gotcha surfaced while verifying item 12**: an early
attempt to observe the `onClientConnectionChanged` log lines above via
`Platform::Log` (which writes to stdout) found the redirected log file
completely empty long after the events had clearly happened, because
stdout is fully buffered once redirected to a file/pipe (unlike stderr).
Switched those two log lines to `std::fprintf(stderr, ...)`, matching
`NetServer`'s own convention for exactly this reason. The client
(`client/src/main.cpp`) had the identical latent issue for its own
informational log messages and was fixed the same way -- see
`docs/known-limitations.md`'s "Real-usage bug fixes" section.

Two real things were caught and fixed during this verification, not
assumed correct from review:
- The frame-push gate in `EmuThread.cpp` was initially `!useOpenGL`,
  which tests the wrong flag (`EmuInstance::usesOpenGL()` conflates the
  display widget's renderer with the 3D renderer choice) -- fixed to
  rely solely on `GetFramebuffers()`'s own return value.
- A pre-existing melonDS quirk, unrelated to this patch: a freshly
  created `$HOME` without an existing `~/.config` directory makes
  `Config::Load()` fail and pop a blocking `QMessageBox::critical` that
  never resolves headlessly. Worked around in testing by pre-creating
  the directory; **not** patched here since it's out of this patch's
  scope (see `tests/homebrew-test-rom/README.md`).

## Applying the patch

```sh
git clone https://github.com/melonDS-emu/melonDS.git
cd melonDS
git checkout 10a173b5536fc75cd93f8a3868349dad963542ef  # commit this patch was made against
git apply /path/to/melonds-remote/host/melonds-patches/0001-remote-server-integration.patch
cmake -B build
cmake --build build -j"$(nproc)"
```

If applying against a newer upstream commit fails, the patch will need a
manual rebase -- the changes are small and isolated (see the boundary
list above), so this should be mechanical in most cases.

## Running with the remote server enabled

Two ways to enable it, checked in this order (env var wins if set, for
scripting/CI use -- see `EmuInstance.cpp`):

1. **Emu Settings checkbox** (persisted, works with any launch method --
   Steam/EmuDeck shortcut, `melonDS` run directly, whatever): open
   **Config > Emu settings > General** and check **"Enable melonDS
   Remote (Steam Deck streaming)"**, then OK through the "emulation will
   be reset" prompt. This is the fix for the "only works via a wrapper
   script setting env vars" gap (there previously was no Config/UI
   toggle at all -- see the melonds-integration-analysis.md history
   below) -- it takes effect on melonDS's *next launch* specifically
   (see option 3 below for turning it on/off live, without a restart).
   Bind address/ports/auth-token/state-dir/discovery all have matching
   `MelonDSRemote.*` Config keys (see `Config.cpp`) if you need to
   change them from their defaults without env vars either -- there's no
   UI for those yet, only the checkbox, so set them by editing
   `melonDS.toml` directly (`[MelonDSRemote]` section) if needed.
2. **Environment variables** (unchanged, still useful for scripting/CI
   -- e.g. a fresh sandbox with no persisted config, or overriding the
   checkbox for one run without changing it): set before launching the
   patched `melonDS`:

```sh
MELONDS_REMOTE_ENABLE=1 \
./melonDS
```

`MELONDS_REMOTE_BIND` defaults to `0.0.0.0` (all interfaces) -- omit it
for normal use so a Steam Deck client elsewhere on the LAN can actually
reach what its discovery scan just found (see `docs/protocol.md`'s
"Discovery payload" section); set it to `127.0.0.1` only to restrict to
same-machine testing, or to a specific address to pick one interface.
`MELONDS_REMOTE_CONTROL_PORT`/`_INPUT_PORT`/`_VIDEO_PORT` default to
8760/8761/8762 and rarely need overriding.

`MELONDS_REMOTE_AUTH_TOKEN` is optional; omit it (recommended) to use
device-approval authentication instead of a static shared secret -- see
`docs/protocol.md`'s "Authentication and device approval" section. An
unrecognized client's connection request pops a `QMessageBox`
Approve/Deny dialog in the melonDS window; once approved, that same
client reconnects silently forever, no re-prompting. Setting
`MELONDS_REMOTE_AUTH_TOKEN` disables device approval entirely in favor of
that exact pre-shared token. `MELONDS_REMOTE_STATE_DIR` overrides where
approved devices are remembered (default
`$HOME/.config/melonds-remote/approved_devices.txt`).

`MELONDS_REMOTE_VERSION` is optional; set it to this build's release
version string (`run-host.sh` in a packaged release does this
automatically, from the archive's `VERSION` file) to reject a connecting
client that reports a different, non-empty `appVersion` of its own --
see `docs/protocol.md`'s "App version mismatch" section. Omit it (the
default, and always the case for a from-source build run directly) to
skip that check entirely and accept any client regardless of its
reported version, same as before this existed.

Only instance 0 (melonDS supports multiple emulator instances for local
multiplayer testing) starts the remote server, matching this project's
one-client-at-a-time v0.1 scope.

3. **Management listener** (turn it on/off live, without restarting
   melonDS -- e.g. from `decky-plugin/`'s Quick Access Menu panel): set
   `MelonDSRemote.ManagementToken` in `melonDS.toml`'s `[MelonDSRemote]`
   section to any shared secret and restart melonDS once. From then on,
   whenever melonDS is running (regardless of whether remote streaming
   itself is currently on), it listens on `MelonDSRemote.ManagementPort`
   (`8764` by default) for one-line commands:
   ```
   echo "your-token ENABLE" | nc 127.0.0.1 8764   # -> OK ENABLED
   echo "your-token DISABLE" | nc 127.0.0.1 8764  # -> OK DISABLED
   echo "your-token STATUS" | nc 127.0.0.1 8764   # -> OK ENABLED / OK DISABLED
   ```
   This is a separate, much simpler channel than the main remote
   protocol -- see `ManagementServer.h`'s header comment. Empty token
   (the default) means this listener doesn't start at all, since it's a
   new always-on network listener whenever melonDS is running -- opt-in
   only.

## Known limitations of this patch specifically

- A basic Config/UI toggle now exists (the Emu Settings checkbox --
  see "Running with the remote server enabled" above), enough to make
  it work without a wrapper script setting env vars. Bind
  address/ports/auth-token/state-dir/discovery still have no UI, only
  Config keys you'd set by hand-editing `melonDS.toml`.
- All three 3D renderers (Software, OpenGL, OpenGLCompute) now produce
  frames for the remote client -- see "OpenGL/OpenGLCompute video
  capture" below. `OpenGLCompute` specifically hasn't been independently
  verified (same code path per `GPU_OpenGL.h`, but this project's test
  environment can't create a compute-capable GL context to check).
- Only three emulator actions are mapped to existing hotkeys (pause/
  resume, fast-forward, swap screens); save state, load state, and the
  rest of `SPEC.md` section 7.5 are not wired up yet.
- The video path has been confirmed to deliver real, non-static frames
  from a real (if minimal/homebrew) running ROM, and the pixel channel
  order is confirmed BGRA8888 -- see "What has actually been verified"
  above and `tests/homebrew-test-rom/README.md`.
- Input injection has been exercised end-to-end against a running
  (homebrew) program that reads input and visibly reacts -- see item 6
  above. Not yet exercised: a commercial-style game's own input-handling
  code (would need a real commercial ROM, out of scope for this
  repository) or a physical Steam Deck controller feeding the client.
- Device-approval authentication requires no client-side typing at all
  (see item 11 above), so it isn't affected by Steam Input's virtual
  keyboard *not* coming up in Gaming Mode -- the specific problem that
  made this repository move off the earlier 6-digit-code flow. There's no
  UI to list/revoke individual approved devices yet, only deleting the
  whole state file.
- Session IDs are generated and returned in `HelloAck` but not yet
  validated on any later packet (matches the standalone prototype's
  current scope, see `docs/known-limitations.md`).
