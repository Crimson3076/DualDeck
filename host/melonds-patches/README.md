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
   alongside the existing emulation thread's lifecycle.

The protocol/host networking code itself (`protocol/`,
`host/remote-server/net_server.{h,cpp}`, `emulator_input_sink.h`,
`frame_source.h`) is vendored byte-for-byte from this repository into
`src/frontend/qt_sdl/remote_server/` in the patch, plus three new
melonDS-specific adapter files (`MelonDSFrameSource`, `MelonDSInputSink`,
`RemoteServerBridge`) that implement `IFrameSource`/`IEmulatorInputSink`
against real melonDS state instead of the standalone prototype's
synthetic frame source / logging sink.

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
   sufficient for a full menu boot). This means the video path
   (`GPU::GetFramebuffers()` → `pushBottomFrame()` → served over the
   video port) was **not** exercised with a real emulated frame here;
   the control-handshake/auth path was fully exercised, and the video
   push code was reviewed line-by-line against the analysis doc's
   documented `GetFramebuffers()` contract (see the code comment at the
   push site explaining why it does *not* gate on `useOpenGL` -- an
   incorrect gate that was caught and fixed during this verification
   pass, precisely because a careful read of `EmuInstance::usesOpenGL()`
   showed it conflates two independent settings).

Obtaining real firmware/BIOS assets to close that last gap was
deliberately not attempted here, for the same reason ROMs aren't
included in this repository (spec section 19: "Do not include commercial
ROMs in the repository or test artifacts" -- real DS firmware carries the
same concern). **The next person with legitimate firmware/a ROM should
verify the video path end-to-end** and report back; the code path is
believed correct from review, but "believed correct from review" is
exactly the kind of claim this project's own instructions (`SPEC.md`
section 23) say not to make without verification, so it's flagged
explicitly here rather than glossed over.

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
MELONDS_REMOTE_BIND=127.0.0.1 \
MELONDS_REMOTE_CONTROL_PORT=8760 \
MELONDS_REMOTE_INPUT_PORT=8761 \
MELONDS_REMOTE_VIDEO_PORT=8762 \
MELONDS_REMOTE_AUTH_TOKEN=some-shared-secret \
./melonDS
```

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
- The video path has not been exercised with a real emulated frame in
  this environment (see "What has actually been verified" above) --
  verify this specifically before relying on it.
- Session IDs are generated and returned in `HelloAck` but not yet
  validated on any later packet (matches the standalone prototype's
  current scope, see `docs/known-limitations.md`).
