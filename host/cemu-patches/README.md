# Cemu patches

`0001-remote-server-integration.patch` implements the remote-server
integration against upstream Cemu (github.com/cemu-project/Cemu) commit
`50b9e4ba1d4d7cf9821a9cd416378bb94e1ba0ca` (main, 2026-07-22), adding a
third real `IEmulatorAdapter` implementation
(`melonds_remote::adapter::IEmulatorAdapter`, the same contract melonDS's
`MelonDSAdapter` and Azahar's `AzaharAdapter` both implement) for the
Nintendo Wii U.

## What the patch does

1. `src/remote_server/CemuAdapter.{h,cpp}` (new) -- implements
   `IEmulatorAdapter` against Cemu's global `g_renderer` and
   `InputManager`:
   - **Video**: exposes two surfaces, `"tv"` (`SurfaceRole::Tv`) and
     `"gamepad"` (`SurfaceRole::GamePad`) -- the Wii U's TV and
     GamePad/DRC outputs. Captured via a new `Renderer::CaptureSurfaceBGRA()`
     virtual (see below) called from a small hook alongside Cemu's own
     `HandleScreenshotRequest()` call in `LatteRenderTarget.cpp`, once per
     surface per real frame, throttled internally to
     `CEMU_REMOTE_CAPTURE_FPS` (default 30) so a Vulkan capture's full
     GPU sync (blit + copy-to-buffer + submit + wait, unavoidable on that
     backend) doesn't run at the game's actual frame rate.
   - **Input**: registers a `RemoteControllerProvider`
     (`InputAPI::DualDeckRemote`, a new enum value) through
     `InputManager`'s existing `create_provider<TProvider>()` template,
     and auto-wires its `RemoteController` onto VPAD player 1 by directly
     calling `add_controller()` + `set_default_mapping()` -- the same
     pairing Cemu's own Controller Settings dialog uses when a user
     manually adds a controller (confirmed by reading
     `gui/wxgui/input/InputSettings2.cpp`, the only existing call sites
     of `set_default_mapping()`), just invoked programmatically instead
     of through the GUI. `RemoteController::raw_state()` mirrors
     `VPADController::set_default_mapping()`'s existing
     `InputAPI::XInput` mapping table bit-for-bit, so that function
     needed only a second `case` label, not a new table.
   - **No GamePad touchscreen support**: confirmed by reading
     `Cafe/OS/libs/vpad/vpad.cpp`'s `VPADRead()` that Cemu hard-codes
     GamePad touch validity to invalid regardless of which controller (if
     any) is mapped -- no existing Cemu input backend can inject GamePad
     touch today, so there's no plumbing for this adapter to hook into
     without inventing an entirely new Cemu-side touch pipeline. Out of
     scope for this pass; see `docs/known-limitations.md`.
2. `src/remote_server/RemoteController.{h,cpp}`,
   `RemoteControllerProvider.{h,cpp}`, `RemoteInputStatus.h` (new) -- the
   `ControllerBase`/`ControllerProviderBase` implementations backing the
   input side above.
3. `src/remote_server/RemoteServerBridge.{h,cpp}` (new) -- owns the
   `CemuAdapter` plus an `AdapterIpcClient` that connects *out* to an
   already-running `dualdeck-host-service --adapter-ipc` over the local
   Unix socket (`CEMU_REMOTE_ADAPTER_SOCKET`), same 1s-5s
   exponential-backoff reconnect loop as melonDS's/Azahar's own
   out-of-process modes. Also exposes the single process-lifetime
   `currentAdapter()` pointer `LatteRenderTarget.cpp`'s render hook reads.
   QuitSession/QuitApplication (GitHub issue #25) are wired to Cemu's own
   wx GUI via `wxQueueEvent()` -- `EVT_DUALDECK_QUIT_SESSION` (bound to
   `MainWindow::EndEmulation()`) and a real `wxCloseEvent` (routed to
   `MainWindow`'s existing `EVT_CLOSE(MainWindow::OnClose)` handler) --
   both defined and `Bind()`'d entirely inside this file via wx's
   attach-a-handler-to-any-window API, so **no changes to
   `gui/wxgui/MainWindow.{h,cpp}` were needed at all**, unlike Azahar's
   patch (which needed `QMetaObject::invokeMethod` against
   `GMainWindow`'s own named slots).
4. `src/Cafe/CafeSystem.cpp` -- `LaunchForegroundTitle()` constructs and
   starts the bridge (only if `CEMU_REMOTE_ENABLE` is set); `ShutdownTitle()`
   tears it down first, before any other title-shutdown work. Chosen over
   the wx GUI layer (`gui/wxgui/MainWindow.cpp`) because it's the
   engine-level boot/shutdown lifecycle hook, independent of whichever
   frontend is active.
5. `src/Cafe/HW/Latte/Core/LatteRenderTarget.cpp` -- the video-capture
   hook, alongside the existing `HandleScreenshotRequest()` call.
6. `src/Cafe/HW/Latte/Renderer/Renderer.h` (+ `OpenGL/OpenGLRenderer.{h,cpp}`,
   `Vulkan/VulkanRenderer.{h,cpp}`) -- the new `CaptureSurfaceBGRA()`
   virtual, default no-op (so an unimplemented backend like Metal simply
   never produces a frame rather than failing to build), with real
   OpenGL (`glGetTexImage(..., GL_BGRA, ...)`, direct byte order, no
   sRGB correction) and Vulkan (blit-to-RGBA8-if-needed + copy-to-buffer +
   submit + wait, structurally mirroring `HandleScreenshotRequest()`'s
   existing one-shot screenshot path) implementations.
7. `src/input/api/InputAPI.h` -- new `InputAPI::DualDeckRemote` enum
   value + `to_string()`/`from_string()` cases.
8. `src/input/emulated/VPADController.cpp` -- `set_default_mapping()`
   gets a second `case InputAPI::DualDeckRemote:` label on the existing
   `InputAPI::XInput` case (the two share the exact same mapping table).
9. `src/input/InputManager.cpp` -- registers
   `create_provider<DualDeck::RemoteServer::RemoteControllerProvider>()`
   alongside the other built-in providers.
10. `src/CMakeLists.txt`, `src/remote_server/CMakeLists.txt` (new) -- a
    new `CemuRemoteServer` static library (the files in 1-3 above, plus a
    vendored `adapter_sdk/` subset copied byte-for-byte from this
    repository's `adapter-sdk/` and `protocol/`), linked into `CemuBin`
    alongside `CemuCafe`/`CemuInput`/etc.

## What has actually been verified

- **Careful source-reading verification of every hook point** --
  `HandleScreenshotRequest`'s exact call site and both renderer
  backends' existing screenshot-readback code, `ControllerProviderBase`/
  `Controller<TProvider>`/`EmulatedController`'s exact
  add_controller/set_mapping/set_default_mapping semantics (including
  the add_controller-before-set_default_mapping ordering requirement,
  confirmed by reading the only two real call sites of
  `set_default_mapping()` in the whole codebase, not assumed), and
  `InputManager`'s provider-registration timing (ruling out a
  chicken-and-egg construction-order bug in `RemoteControllerProvider`)
  were all done by reading Cemu's actual source at the pinned commit
  before writing this patch, not by guessing at API shapes.

## What is *not* verified yet

This is the first Cemu-integration pass where **no local build was
attempted at all**, unlike the melonDS and Azahar patches (both of which
were built and run end-to-end in this project's development sandbox
before being committed). That's a deliberate, explicit trade-off: Cemu's
own dependency graph resolves to ~108 vcpkg packages, the large majority
of which require downloading prebuilt binaries or source archives from
hosts this sandbox's network policy blocks (only apt mirrors and the
git-protocol mirror are reliably reachable here -- see
`docs/known-limitations.md`). Continuing to work around that
dependency-by-dependency (as was done for `vcpkg-tool` itself) was judged
not worth it for ~100 more packages; verification for this patch
specifically is deferred to this project's GitHub Actions CI pipeline,
which runs on a normal, unrestricted-network runner.

Concretely, still unverified:
- **Does it compile at all.** No C++ compiler has seen any file in this
  patch. Template instantiation errors, missing includes, and CMake
  target-linkage mistakes (in particular, whether `CemuRemoteServer`'s
  `target_link_libraries(... PRIVATE CemuGui)` is sufficient to pull in
  wxWidgets' own include paths transitively for `RemoteServerBridge.cpp`,
  and whether `CemuInput`'s existing `target_include_directories(CemuInput
  PUBLIC "../")` is sufficient for `InputManager.cpp` to `#include
  "remote_server/RemoteControllerProvider.h"`) are all reasoned through
  by reading Cemu's existing CMake conventions, not confirmed by an
  actual `cmake --build`.
- **No real Wii U game has been run.** Video capture, the auto-injected
  VPAD mapping, and the wx GUI quit-session/quit-application wiring have
  all been read and reasoned about but never observed taking effect.
- `scripts/build-release.sh`'s Cemu packaging step, the host launcher's
  emulator picker, and end-to-end verification against a real DualDeck
  client are all still pending (see the open Cemu tasks tracked in this
  session).
