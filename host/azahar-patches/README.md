# Azahar patches

`0001-remote-server-integration.patch` implements the remote-server
integration against upstream Azahar (github.com/azahar-emu/azahar)
commit `75134fca82eab4e1a86dca0aaa4a188cefff5469` (master, 2026-07-18),
adding a second real `IEmulatorAdapter` implementation
(`melonds_remote::adapter::IEmulatorAdapter`, the same contract
melonDS's own `MelonDSAdapter` implements) for the Nintendo 3DS, proving
the contract generalizes across emulator codebases without any changes
to it. See `docs/azahar-integration-analysis.md` for the Phase 0
investigation this patch is based on, and
`docs/adr/0001-host-service-and-adapter-architecture.md` section 11 for
the architectural writeup.

## What the patch does

1. `src/citra_qt/remote_server/AzaharAdapter.{h,cpp}` (new) --
   implements `IEmulatorAdapter` against Azahar's `Core::System`:
   - **Video**: a background thread calls
     `system.GPU().Renderer().RequestScreenshot(...)` with a
     `Layout::SingleFrameLayout(320, 240, /*swapped=*/true, false)`
     (bottom-screen-only layout) at roughly 30fps -- this is Azahar's
     own backend-agnostic screenshot API, so it works unmodified across
     the software, OpenGL, and Vulkan renderers, unlike melonDS's
     OpenGL-specific `GLBottomScreenCapture` path. Exposes exactly one
     surface, `"bottom"`, 320x240, touch-capable.
   - **Input**: registers a `"melonds_remote"` `Input::Factory` engine
     (`RemoteButtonDevice`/`RemoteAnalogDevice`/`RemoteTouchDevice`,
     mirroring the pattern Azahar's own `input_common/sdl` and
     `input_common/udp` engines use) and overwrites
     `Settings::values.current_input_profile` to point every button,
     both analog sticks (circle pad + C-Stick), and the touch screen at
     it. The 12 base-3DS buttons map onto `GenericInputState`'s
     `buttons` bitmask at the same bit positions `DSButton` already
     uses (confirmed by reading `Settings::NativeButton::Values`, not
     assumed) so `protocol.h` and `host/adapter_bridge.cpp` needed zero
     changes. New3DS-exclusive ZL/ZR have no wire representation --
     documented limitation, see `docs/known-limitations.md`.
2. `src/citra_qt/remote_server/RemoteServerBridge.{h,cpp}` (new) --
   owns the `AzaharAdapter` plus an `AdapterIpcClient` that connects
   *out* to an already-running `dualdeck-host-service --adapter-ipc`
   over the local Unix socket (`AZAHAR_REMOTE_ADAPTER_SOCKET`), with the
   same 1s-5s exponential-backoff reconnect loop melonDS's own
   out-of-process mode and `client/`'s `NetClient` both already use.
3. `src/citra_qt/citra_qt.{h,cpp}` -- `GMainWindow::BootGame()`
   constructs and starts the bridge (only if `AZAHAR_REMOTE_ENABLE` is
   set in the environment); `ShutdownGame()` tears it down first, before
   any other emulation-shutdown work, mirroring melonDS's
   `EmuInstance` constructor/destructor lifecycle hook points.
4. `src/citra_qt/CMakeLists.txt` -- adds the new sources and a vendored
   `adapter_sdk/` subset (`protocol.h/.cpp`, `adapter_contract.h`,
   `generic_input.h`, `session_state.h/.cpp`, `video_surface.h`,
   `ipc/adapter_ipc_client.h/.cpp`, `ipc/ipc_protocol.h/.cpp`,
   `ipc/socket_path.h/.cpp`) copied byte-for-byte from this repository's
   `adapter-sdk/` and `protocol/`, plus the matching include path.

## A deliberate departure from melonDS's integration: no in-process mode

Unlike melonDS (which defaults to running the remote server in-process,
inside the same binary, with an interactive Approve/Deny Qt dialog per
new device), **AzaharAdapter only supports the out-of-process
`AdapterIpcClient` path** -- it always connects out to an independently
running Host Service, authenticated with a static shared secret
(`AZAHAR_REMOTE_AUTH_TOKEN`) instead of per-device approval. Building a
cross-process device-approval bridge (proxying an Approve/Deny prompt
from the Host Service back into Azahar's Qt UI, or vice versa) was
explicitly scoped out of this pass as separate, unfinished work -- see
`docs/known-limitations.md`'s AzaharAdapter entry for the full
reasoning. This mirrors the same trade-off already shipped for
host-control mode (issue #4).

## What has actually been verified

- **The patch applies cleanly** with `git apply --check` against a
  pristine, independent checkout of the pinned commit (not just the
  scratch tree it was authored in) -- confirmed via
  `scripts/patch-existing-emulator.sh --system 3ds`, which is also how
  an end user would apply it to their own existing checkout.
- **It builds.** A full `-DCMAKE_BUILD_TYPE=Release` build (36 git
  submodules, Qt6, Vulkan/OpenGL/software renderers) completes and
  produces `build/bin/Release/azahar`, a real ~57MB linked executable
  containing the new `AzaharAdapter`/`RemoteServerBridge` code and the
  vendored `melonds_remote::` symbols (confirmed via `nm`), not just a
  clean compile of the new files in isolation.
- **Idempotency and mismatch handling**: re-running
  `patch-existing-emulator.sh` against an already-patched checkout
  correctly detects "already applied" and no-ops rather than
  double-applying or erroring.

## What is *not* verified yet

- **No real 3DS game has been run against this build.** The sandbox
  this was developed in has no display or GPU, so the video-capture
  path (`RequestScreenshot`, the software/OpenGL/Vulkan renderers
  themselves, actual frame delivery to a connected client) has been
  read and reasoned about carefully but not exercised end-to-end the
  way melonDS's DS-side capture path was (see
  `tests/homebrew-test-rom/README.md` for that verification standard --
  the 3DS side has no equivalent yet).
- Input has similarly not been exercised against a running 3DS core --
  the button/analog/touch bit-mapping was confirmed by reading Azahar's
  actual `Settings::NativeButton`/`NativeAnalog` enums and
  `host/adapter_bridge.cpp`'s existing forwarding logic, not by
  observing a real button press take effect.
- `scripts/build-release.sh`'s own packaged Azahar build step (as
  opposed to this standalone scratch-clone build) has not been run
  end-to-end in this session -- only the underlying clone/patch/build
  commands it wraps were verified directly.
- No Distrobox path for Azahar on Bazzite yet (melonDS has one).
- Fedora/Arch package names for Azahar's build dependencies are
  unverified (Debian/Ubuntu names were used and tested here).
