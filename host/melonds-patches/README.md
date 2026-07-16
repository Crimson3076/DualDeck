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
   server's frame source.
2. `src/frontend/qt_sdl/EmuInstanceInput.cpp` — `inputProcess()` merges
   the latest remote `ControllerState` into `inputMask`/`isTouching`/
   `touchX`/`touchY`/`hotkeyMask`, the same way local SDL joystick input
   is merged with keyboard input just above it.
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

There is no Config/UI toggle yet (deliberately -- see "smallest viable
implementation" in `docs/melonds-integration-analysis.md`). Enable it via
environment variables before launching the patched `melonDS`:

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

Only instance 0 (melonDS supports multiple emulator instances for local
multiplayer testing) starts the remote server, matching this project's
one-client-at-a-time v0.1 scope.

## Known limitations of this patch specifically

- No Config/UI toggle -- environment variables only.
- Only the software 3D renderer path produces frames for the remote
  client (see `docs/melonds-integration-analysis.md` section 1.2 for why
  the OpenGL renderer path is deferred).
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
