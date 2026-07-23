# Cemu patches

`0001-remote-server-integration.patch` implements the remote-server
integration against upstream Cemu (github.com/cemu-project/Cemu) tag
`v2.6` (commit `a6fb0a48eb437a8a41c13b782ac8ae0433bf8f98`), the latest
stable release as of this writing, adding a third real
`IEmulatorAdapter` implementation (`melonds_remote::adapter::IEmulatorAdapter`,
the same contract melonDS's `MelonDSAdapter` and Azahar's `AzaharAdapter`
both implement) for the Nintendo Wii U.

**Rebased from a development-branch commit to this stable release** --
see "Rebase to the v2.6 stable release" below for why and what changed.
Everything through "Fourth real end-to-end run findings" documents the
integration's design and its first four rounds of real CI-and-hardware
verification against that earlier dev-branch base; the rebase section
after it covers what's different (and what still needs re-verifying)
now that the base is v2.6.

## What the patch does

1. `src/remote_server/CemuAdapter.{h,cpp}` (new) -- implements
   `IEmulatorAdapter` against Cemu's global `g_renderer` and
   `InputManager`:
   - **Video**: exposes two surfaces, `"tv"` (`SurfaceRole::Tv`) and
     `"gamepad"` (`SurfaceRole::GamePad`) -- the Wii U's TV and
     GamePad/DRC outputs. Captured via a new `Renderer::CaptureSurfaceBGRA()`
     virtual (see below) called from a hook in
     `LatteRenderTarget_itHLECopyColorBufferToScanBuffer()`
     (`LatteRenderTarget.cpp`), once per surface per real frame,
     throttled internally to `CEMU_REMOTE_CAPTURE_FPS` (default 30) so a
     Vulkan capture's full GPU sync (blit + copy-to-buffer + submit +
     wait, unavoidable on that backend) doesn't run at the game's actual
     frame rate. Deliberately *not* hooked alongside
     `HandleScreenshotRequest()` in `LatteRenderTarget_copyToBackbuffer()`
     (the first pass's approach) -- see "First real end-to-end run
     findings" below for why that broke GamePad streaming whenever
     Cemu's own "Enable GamePad View" window was closed.
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
   hook, in `LatteRenderTarget_itHLECopyColorBufferToScanBuffer()` (see
   item 1's video bullet above).
6. `src/Cafe/HW/Latte/Renderer/Renderer.h` (+ `OpenGL/OpenGLRenderer.{h,cpp}`,
   `Vulkan/VulkanRenderer.{h,cpp}`) -- the new `CaptureSurfaceBGRA()`
   virtual, default no-op (so an unimplemented backend like Metal simply
   never produces a frame rather than failing to build), with real
   OpenGL (`glGetTexImage(..., GL_BGRA, ...)`, direct byte order) and
   Vulkan (blit-to-RGBA8-if-needed + copy-to-buffer + submit + wait,
   structurally mirroring `HandleScreenshotRequest()`'s existing one-shot
   screenshot path) implementations. Both apply the same
   srcUsesSRGB/dstUsesSRGB correction (via the existing
   `Renderer::SRGBComponentToRGB`/`RGBComponentToSRGB`) that
   `HandleScreenshotRequest()` itself applies -- see "First real
   end-to-end run findings" below for why this was added after the
   first successful boot, not in the original pass.
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
- **A real CI build attempt got most of the way through the codebase**
  before failing (see below) -- vcpkg resolved and built all ~108
  packages (including wxWidgets 3.3) with no changes needed to this
  patch's dependency assumptions, and roughly 130 of Cemu's own ~545
  translation units compiled cleanly, including several files this patch
  touches (`InputAPI.h`, `VPADController.cpp`, `InputManager.cpp`) --
  before hitting the one real compile error described below.

## What is *not* verified yet

This is the first Cemu-integration pass where local builds weren't
possible during development, unlike the melonDS and Azahar patches (both
built and run end-to-end in this project's development sandbox before
being committed). That's a deliberate, explicit trade-off: Cemu's own
dependency graph resolves to ~108 vcpkg packages, the large majority of
which require downloading prebuilt binaries or source archives from
hosts this sandbox's network policy blocks (only apt mirrors and the
git-protocol mirror are reliably reachable here -- see
`docs/known-limitations.md`). Continuing to work around that
dependency-by-dependency (as was done for `vcpkg-tool` itself) was judged
not worth it for ~100 more packages; verification for this patch instead
happens via this project's GitHub Actions CI pipeline, which runs on a
normal, unrestricted-network runner.

**First real CI build attempt**: failed at
`src/Cafe/CMakeLists.txt`+`CafeSystem.cpp` -- `CafeSystem.cpp` (compiled
as part of the `CemuCafe` library) `#include`s
`remote_server/RemoteServerBridge.h`, which pulls in the vendored
`melonds_remote/adapter/...` headers; those don't live under the plain
`"../"` (src/) include root every other cross-module include in this
codebase resolves through (e.g. `"input/InputManager.h"` needs no extra
include dir), since `adapter_sdk/` is vendored a level deeper, under
`remote_server/adapter_sdk/include/`. Fixed with one extra
`target_include_directories(CemuCafe PUBLIC
"../remote_server/adapter_sdk/include")` line in `src/Cafe/CMakeLists.txt`
-- not a `target_link_libraries` dependency on `CemuRemoteServer` (that
library is defined by a later `add_subdirectory()` call in the top-level
`CMakeLists.txt`, and isn't otherwise needed since `CemuBin` already
links every top-level library directly for symbol resolution).

**Second real CI build attempt** (after the fix above): got past
`CemuCafe` entirely, then failed compiling `CemuAdapter.cpp` itself
(part of the new `CemuRemoteServer` library) with a cascade of "`uint32`
has not been declared"/"`Latte::E_DIM` has not been declared"/etc.
errors from Cemu's own `LatteAddrLib.h`/`Renderer.h`. Root cause: every
other library in this codebase force-includes `Common/precompiled.h`
via a `cemu_use_precompiled_header(<target>)` helper -- that's where
`uint32`/`uint16`/`sint32`/etc. (plain aliases for the `<cstdint>`
fixed-width types) are established project-wide, and `CemuRemoteServer`
never called it. Fixed by adding that same call to
`src/remote_server/CMakeLists.txt`, and separately adding the same
leading Latte include block `LatteRenderTarget.cpp` already uses
(`ISA/RegDefines.h`, `Core/Latte.h`, `LatteDraw.h`, `LatteShader.h`,
`LatteOverlay.h`, `LatteBufferCache.h`, `LatteTexture.h`,
`LatteCachedFBO.h`) to `CemuAdapter.cpp` before its own
`Renderer.h` include, since `Renderer.h` isn't a self-contained leaf
header either -- it assumes whichever `.cpp` includes it already pulled
those in.

Both are exactly the class of build-order/include-path/precompiled-
header mistake local compilation would have caught immediately -- expect
more rounds of this as CI gets further into the build each time.

**Third real CI build attempt** (after both fixes above): got to
530/546 -- all of `CemuCafe`, `CemuWxGui`, `CemuConfig`, almost all of
`CemuInput`, and `CemuAdapter.cpp`/`RemoteController.cpp`/
`RemoteControllerProvider.cpp` (all three files new to this patch)
compiled clean. Only `RemoteServerBridge.cpp` failed:
`wxgui/MainWindow.h` chains into `wxgui/components/wxGameList.h`, which
bare-`#include`s `"wxHelper.h"` (`gui/wxgui/wxHelper.h`) with no path
prefix -- that only resolves for `CemuWxGui`'s own files because
`CMAKE_INCLUDE_CURRENT_DIR` is set `ON` globally, which auto-adds *each
target's own* source directory to its include path, and `CemuWxGui`'s
own directory happens to be `gui/wxgui/` itself. Fixed by adding
`target_include_directories(CemuRemoteServer PUBLIC "../gui/wxgui")` to
`src/remote_server/CMakeLists.txt`.

The fourth CI build attempt (after the third fix above) succeeded, and a
real end-to-end session followed: a Wii U game booted, a DualDeck client
connected, video streamed, and the auto-wired VPAD player-1 mapping
worked.

## First real end-to-end run findings

Testing the successful build surfaced three issues:

1. **GamePad touchscreen input doesn't register.** Expected, not a bug --
   see "No GamePad touchscreen support" above and
   `docs/known-limitations.md`. Cemu has no touch-injection plumbing for
   any input backend today; fixing this would mean building an entirely
   new Cemu-side touch pipeline, out of scope for this pass.
2. **Aspect ratio on the GamePad screen was wrong.** Root cause was on
   the *client* side, not this patch: `computeAspectFitRect()`
   (`protocol/src/touch_mapping.cpp`) hardcoded a 4:3 content aspect
   ratio internally, which happened to be correct for melonDS (256x192)
   and Azahar (320x240) but not for the Wii U GamePad's 854x480 (16:9)
   output -- the first non-4:3 surface any adapter has ever streamed.
   Fixed by adding an optional `contentAspect` parameter (default
   `4.0/3.0`, preserving every existing caller) and passing the real
   connected host's aspect ratio through at the client's one real
   gameplay-loop call site (`client/src/main.cpp`), which already tracked
   the actual reported dimensions via `textureWidth`/`textureHeight` --
   no protocol or patch changes needed.
3. **Colors came out slightly darker than Cemu's own window.** Root
   cause: `CaptureSurfaceBGRA()`'s original implementation (both
   backends) read back raw pixel bytes with no color-space correction,
   on the reasoning that it was "cosmetic-only" compared to
   `HandleScreenshotRequest()`'s existing sRGB<->linear handling. That
   reasoning was wrong for a *live, continuously-displayed* stream (as
   opposed to a one-off saved screenshot): when a render target's GX2
   surface format is flagged sRGB but the buffer it's ultimately
   composited into isn't (or vice versa -- exactly the
   `srcUsesSRGB`/`dstUsesSRGB` mismatch `HandleScreenshotRequest()`
   already checks for), the raw captured bytes are in the wrong gamma
   space and read visibly darker/lighter once displayed remotely, even
   though Cemu's own window looks correct (its normal present path
   handles the conversion). Fixed by having both `CaptureSurfaceBGRA()`
   implementations apply the exact same `srcUsesSRGB`/`dstUsesSRGB`
   comparison and per-channel `SRGBComponentToRGB`/`RGBComponentToSRGB`
   correction `HandleScreenshotRequest()` already uses, which required
   adding a `padView` parameter to the virtual (to know which of
   `LatteGPUState.tvBufferUsesSRGB`/`drcBufferUsesSRGB` applies) --
   threaded through from `CemuAdapter::onSurfaceRendered()`'s existing
   `isPadView` argument.

Fixes 2 and 3 (fix 2 client-only, fix 3 inside this patch) went through
a fifth CI build (v0.1.61), which succeeded. Still unverified: **real
confirmation that fix 3 actually resolves the color difference against
real Wii U game content in practice** -- the fix mirrors Cemu's own
established correctness logic exactly, but hasn't been re-tested
against the same game/scene that showed the darkening.

## Second real end-to-end run findings

Testing the v0.1.61 build (aspect ratio + color fixes applied) surfaced
one more issue:

4. **GamePad video only streamed while Cemu's own "Enable GamePad
   View" window was open locally.** Root cause: the video-capture hook
   originally lived in `LatteRenderTarget_copyToBackbuffer()`, alongside
   `HandleScreenshotRequest()` -- but for the GamePad surface, that
   function is only called at all
   (`if ((renderTarget & RENDER_TARGET_DRC) && g_renderer->IsPadWindowActive())`
   in `LatteRenderTarget_itHLECopyColorBufferToScanBuffer()`) when
   Cemu's own local pad window actually exists. Fine for
   `HandleScreenshotRequest()` (a one-shot screenshot of whatever's
   currently on screen), wrong for a headless streaming capture that
   should work whether or not the user has any particular local window
   open. Fixed by moving the hook to
   `LatteRenderTarget_itHLECopyColorBufferToScanBuffer()` itself, which
   hands over the real TV/DRC scan-buffer texture unconditionally, once
   per real frame per surface -- `CaptureSurfaceBGRA()` reads straight
   from that GPU texture, so it needs no window or backbuffer of any
   kind. This also incidentally fixes a subtler, previously-undiscovered
   correctness issue: the old hook fired from the *on-screen
   presentation* path, which includes a local Tab/Ctrl+Tab (or VPAD
   "screen active" button) toggle that can swap GamePad content onto
   the "TV" backbuffer -- meaning the old "tv"/"gamepad" capture could
   have been swapped depending on what a local player was looking at.
   The new hook captures directly off the real TV/DRC scan buffers
   Cemu's HLE layer produces, before any such local presentation
   toggling, so `"tv"`/`"gamepad"` now always correspond to the actual
   Wii U TV/GamePad outputs regardless of local window state.

This went through a sixth CI build (v0.1.62), which succeeded -- but
real testing found it made things *worse*: GamePad video stopped
streaming entirely, regardless of whether the local GamePad View window
was open or closed.

## Third real end-to-end run findings

Root cause of the v0.1.62 regression: `LatteRenderTarget_copyToBackbuffer()`
(the function the hook used to live in) always calls
`LatteTexture_UpdateDataToLatest(textureView->baseTexture)` and
`LatteTC_MarkTextureStillInUse(textureView->baseTexture)` *before*
touching the texture in any way -- see its own leading comment
("make sure texture is updated to latest data in cache"). Every other
consumer of a `LatteTextureView*` in this codebase
(`HandleScreenshotRequest`, `DrawBackbufferQuad`) relies on
`copyToBackbuffer` having already done this first. The relocated hook
runs earlier in the pipeline, in
`LatteRenderTarget_itHLECopyColorBufferToScanBuffer()`, which does
neither call itself -- so `CaptureSurfaceBGRA()` was reading a texture
that Cemu's own on-demand texture cache (`LatteTC`) hadn't necessarily
resolved/uploaded yet, likely an empty or wrong-layout GPU texture
object fresh out of `LatteTC_GetTextureSliceViewOrTryCreate()`. Fixed
by adding both calls immediately before the capture hook, mirroring
`copyToBackbuffer()`'s own preamble exactly. Calling them twice per
frame (once here, once again later in `copyToBackbuffer()` when that
path also runs) is expected and harmless -- the function's job is
specifically to make the cache idempotently current, not a one-shot
action.

This went through a seventh CI build (v0.1.63), which succeeded, and
real testing confirmed it: GamePad mirroring now works (with the local
GamePad View window either open or closed), and the earlier aspect
ratio and color fixes both hold up against real game content -- video
now looks correct. Two issues remained:

1. **Touch still doesn't register.** Re-confirmed expected, unchanged
   from the "No GamePad touchscreen support" note above.
2. **Face buttons were all swapped**: Steam Deck's "A" landed on Wii
   U's A, "B" on B, "X" on X, "Y" on Y -- i.e. by *label*, not by
   *physical position*, even though Xbox-style and Nintendo-style pads
   put those labels in different physical spots (Xbox: A bottom, B
   right, X left, Y top; Nintendo: B bottom, A right, Y left, X top).

## Fourth real end-to-end run findings

Root cause of the button-mapping bug: `RemoteController::raw_state()`
(`src/remote_server/RemoteController.cpp`) fed `GenericButton_South`
(Steam Deck's physical south/"A" button) into `kButton13`, reasoning
(per its own now-incorrect comment) that `kButtonId_A <- kButton13` in
`VPADController::set_default_mapping()`'s shared XInput/DualDeckRemote
table meant "kButton13 is the slot that produces Wii U A". That table
was written for, and is shared verbatim with, *real* XInput
controllers -- and it already performs the Xbox-layout ->
Nintendo-layout physical-position correction those need, given raw
XInput hardware bit positions as input (confirmed by reading
`XInputController.cpp`, which populates `kButton12..15` directly from
`XINPUT_GAMEPAD_A/B/X/Y`'s own bit values -- bits 12/13/14/15
respectively -- not from which Wii U button they end up driving).
Wiring `GenericButton_South` straight to `kButton13` (the slot that
happens to drive Wii U A) applied that same correction a *second* time
on top of the client's already-physical-position button identity,
cancelling it out -- so physical south ended up back on Wii U A instead
of the physically-correct Wii U B.

Fixed by mapping each face button to the raw XInput bit position it
physically corresponds to instead (`GenericButton_South -> kButton12`,
`_East -> kButton13`, `_West -> kButton14`, `_North -> kButton15`,
matching `XINPUT_GAMEPAD_A/B/X/Y`'s real bit assignments exactly), so
the shared table's correction is applied exactly once, end to end.
D-pad and shoulder/stick-click/start-select mappings were already
correct (verified against the same real XInput bit layout) and needed
no change -- only the four face buttons were affected.

Concretely, still unverified: **a CI build of this fix and real
confirmation that face buttons now land in the physically-correct
position.**

## Rebase to the v2.6 stable release

Everything above was developed and verified (through four real
end-to-end rounds) against a development-branch commit taken from
Cemu's `main` at whatever point it happened to be cloned. Rebased onto
`v2.6` (the latest tagged stable release) on user request, since a dev
build can carry in-progress work with its own performance regressions
or instability -- not something to build a release on top of by
default. Note that Cemu's own public development slowed sharply after
the project's late-2024 acquisition, so `v2.6` (tagged
2025-02-06) and the dev-branch commit this was previously pinned to
turned out to be closer in real content than the raw commit count
suggests (278 commits, 775 files touched between them) --
still substantial, but the actual overlap with the specific files and
hook points this patch touches was smaller than that number implies.

**Method**: cloned Cemu at the `v2.6` tag into a scratch checkout,
attempted `git apply --check` with the existing (dev-branch) patch,
and worked through every resulting conflict by reading `v2.6`'s actual
source at each point rather than forcing the old hunks to fit -- the
same source-reading discipline the original patch was written with,
just applied to a diff instead of a blank slate. 30 of the 35
previously-patched files applied with line-offset-only changes (no
real conflicts), which is itself informative: it means the specific
functions and call patterns this patch hooks into (`LatteRenderTarget_
itHLECopyColorBufferToScanBuffer`'s exact body, `Renderer::CaptureSurfaceBGRA`'s
sibling `HandleScreenshotRequest`/`DrawBackbufferQuad`/`SRGBComponentToRGB`
declarations, `LatteTexture_UpdateDataToLatest`/`LatteTC_MarkTextureStillInUse`'s
signatures, the OpenGL/Vulkan capture backends' exact API surface,
`VPADController`'s XInput mapping table byte-for-byte, `InputManager`'s
provider-registration pattern) are unchanged between `v2.6` and the
later dev commit -- not just superficially re-appliable, but the same
underlying code. Five files genuinely needed manual rework:

- **`src/CMakeLists.txt`**: `v2.6` lacks the dev branch's `asm`
  subdirectory and links `SDL2::SDL2` directly into `CemuBin` (the dev
  branch had moved to SDL3) -- neither relevant to this patch. Added
  `add_subdirectory(remote_server)` and `CemuRemoteServer` to
  `CemuBin`'s link list in the equivalent places for `v2.6`'s actual
  file.
- **`src/Cafe/CMakeLists.txt`**: same `target_include_directories(CemuCafe
  PUBLIC "../remote_server/adapter_sdk/include")` addition as before,
  just re-anchored to `v2.6`'s surrounding `target_link_libraries(CemuCafe
  ...)` block, which has a different (but equivalent) set of linked
  libraries.
- **`src/Cafe/CafeSystem.cpp`**: `v2.6` calls `gui_notifyGameLoaded()`
  where the dev branch called `WindowSystem::NotifyGameLoaded()` (an
  abstraction-layer rename) -- same hook points otherwise
  (`LaunchForegroundTitle()`/`ShutdownTitle()`), just re-anchored.
- **`src/Cafe/HW/Latte/Renderer/Renderer.h`**: `v2.6` doesn't yet have
  the dev branch's `RequestScreenshot`/`ScreenshotSaveFunction`
  preamble ahead of `HandleScreenshotRequest()` -- irrelevant to this
  patch either way; the new `CaptureSurfaceBGRA()` virtual just moved
  to sit directly after `HandleScreenshotRequest()` in `v2.6`'s actual
  layout.
- **`src/input/emulated/VPADController.cpp`**: purely a missing
  `#endif` block the dev branch had immediately before `case
  InputAPI::XInput:` that doesn't exist in `v2.6` -- the
  `InputAPI::DualDeckRemote` fallthrough case (and the XInput mapping
  table itself, confirmed byte-for-byte identical) needed no other
  change.

Two further things needed rework beyond what `git apply --check` alone
surfaces, since they're new files (no upstream diff to conflict) whose
*content* still assumes dev-branch APIs:

- **`src/remote_server/CMakeLists.txt`**: `v2.6` has no
  `cemu_use_precompiled_header()` helper function (a later addition) --
  `CemuCommon` there instead declares its precompiled header
  `PUBLIC` (`target_precompile_headers(CemuCommon PUBLIC precompiled.h)`,
  `src/Common/CMakeLists.txt`), which every other real module (e.g.
  `CemuCafe`) already gets for free purely by linking `CemuCommon` --
  confirmed by reading `Cafe/CMakeLists.txt`, which does exactly that
  and calls no separate helper. Replaced the helper call with an
  explicit `CemuCommon` link, matching that convention. Also dropped
  the dev branch's extra `target_include_directories(CemuRemoteServer
  PUBLIC "../gui/wxgui")` line entirely: `v2.6`'s `gui/` tree has no
  `wxgui/` subdirectory at all (introduced later, presumably when Cemu
  added an alternate Qt frontend) -- it's still flat
  (`gui/MainWindow.h`, `gui/components/wxGameList.h`, ...), and none of
  `MainWindow.h`'s own transitive includes bare-`#include` `wxHelper.h`
  the way the dev-branch chain did, so the extra include directory the
  dev-branch version needed to resolve that isn't needed here at all.
- **`src/remote_server/RemoteServerBridge.cpp`** (QuitSession/
  QuitApplication wiring, GitHub issue #25): `v2.6`'s `MainWindow` has
  no public `EndEmulation()` method -- confirmed by reading the actual
  handler, `MainWindow::OnFileMenu()`'s `MAINFRAME_MENU_ID_FILE_END_EMULATION`
  branch inlines the "stop title, return to game list" sequence
  directly (`CafeSystem::ShutdownTitle()` + `DestroyCanvas()` +
  `RecreateMenu()` + `CreateGameListAndStatusBar()` + `DoLayout()` +
  `UpdateChildWindowTitleRunningState()`), and the menu-id constant it
  switches on is a private, unstably-valued enum with nothing to
  reference from outside that translation unit. Unlike every other
  change in this patch, this one **does touch upstream `gui/MainWindow.{h,cpp}`**
  (the one file this integration had specifically avoided touching
  against the dev-branch base, by relying on the `EndEmulation()`
  method that existed there) -- extracted that exact sequence into a
  new public `MainWindow::EndEmulation()` method, with `OnFileMenu()`
  itself now calling it too instead of duplicating the sequence. Also
  fixed the include from `"wxgui/MainWindow.h"` to `"gui/MainWindow.h"`,
  matching `v2.6`'s flat layout.

Also spot-verified beyond what patch application alone would catch,
since these are real behavioral dependencies this patch's own code
relies on, not just text that happens to be nearby: `InputManager`'s
`add_controller()`-before-`set_default_mapping()` ordering requirement
(re-confirmed by reading both real call sites in `v2.6`'s own
`gui/input/InputSettings2.cpp`, identical ordering to what the original
patch verified against the dev branch), and that `CemuAdapter.cpp`
doesn't depend on any Cemu-internal version-string API (`m_cemuVersion`
is purely an external string passed in via `CEMU_REMOTE_VERSION`, set
by this project's own launcher script, never read from Cemu itself).

**First `v2.6` CI build attempt**: got to 305/544 files before failing
-- `VulkanRenderer::CaptureSurfaceBGRA()` called
`baseImageTex->GetDefaultLayout()` (carried over unchanged from the
dev-branch patch) to restore a render target's Vulkan image layout
after reading it via blit; `LatteTextureVk` has no such method in
`v2.6` (`error: 'class LatteTextureVk' has no member named
'GetDefaultLayout'; did you mean 'GetImageLayout'?`) -- a later
addition, not present at this base. `GetImageLayout()` (`v2.6`'s real
API, requiring a subresource argument) isn't a drop-in replacement,
since it *queries* current layout rather than naming a fixed "resting"
one. Root-caused by reading `HandleScreenshotRequest()`'s own
unmodified, already-compiling code earlier in the same file, which
does the exact same "done reading a render target via blit, put it
back" step for the exact same kind of texture, hardcoding
`VK_IMAGE_LAYOUT_GENERAL` on both sides of the transition (not derived
from the texture at all). Fixed by replacing both
`GetDefaultLayout()` calls with the literal `VK_IMAGE_LAYOUT_GENERAL`,
matching what this Cemu version's own working code already does.

**Still unverified**: whether this was the only build error at this
base -- only one file had failed when the build stopped, so anything
past `CaptureSurfaceBGRA()`'s file (`VulkanRenderer.cpp`, the 305th of
544) hasn't been compiled yet as of this fix. Expect this to need at
least one more round of real build-error troubleshooting, the same as
every previous base-commit/major-change round of this patch has.

**First real end-to-end run against `v2.6`: reproducible crash on the Vulkan renderer.**
Reported directly from a user's host-service log: after roughly 15-20
seconds of streaming, Cemu segfaults inside `__libc_free`, called (per
the crash's own stack trace) from `CemuAdapter::onSurfaceRendered()` ->
`LatteCP_itHLECopyColorBufferToScanBuffer()` -> `LatteCP_ProcessRingbuffer()`.

**Correction (this crash does not explain the reported "flashing"):**
initially reported to the user as explaining the entire "flashes between
connecting and host control, controls don't work" symptom -- the theory
being Cemu crashes, the launcher restarts it, `RemoteServerBridge`/
`AdapterIpcClient` reconnects, `ModeCoordinator` flips back to
`Emulation`, and Cemu crashes again a few seconds later, on repeat. The
user pushed back: "Cemu itself was staying alive, so I doubt it was
'crashing' unless it was the network plugin or something." Re-reading
the same log confirmed this: the segfault trace appears exactly once, at
the very end, after 20+ reconnect cycles with no crash evidence between
them and continuous client input throughout. The crash below is real and
worth fixing, but it is a separate, secondary bug -- see "IPC frame-size
cap" further down for the theory that actually accounts for the repeated
flashing, root-caused after the user clarified they were sitting in a
single, stable, already-loaded game's own main menu when it happened
(which also ruled out a title-relaunch-churn theory tried in between).

Root cause (well-reasoned from reading the actual code both files
involved, not confirmed with a debugger against a live crash -- this
project has no way to build or run Cemu in its own sandbox, only CI):
`VulkanRenderer::CaptureSurfaceBGRA()` allocates a fresh Vulkan staging
buffer (`memoryManager->CreateBuffer()` + `vkMapMemory()`) and destroys
it (`vkUnmapMemory()`/`vkFreeMemory()`/`vkDestroyBuffer()`) on *every
single call* -- structurally identical to `HandleScreenshotRequest()`
just above it in the same file, which does the exact same allocate/
free-per-call pattern. The difference is call frequency:
`HandleScreenshotRequest()` only ever runs once per a rare, explicit
user action (pressing the screenshot key), while `CaptureSurfaceBGRA()`
runs continuously at up to `CEMU_REMOTE_CAPTURE_FPS` (default 30) times
a second, per surface (TV + GamePad) -- up to 60 full Vulkan
allocate-blit-submit-wait-free cycles a second, sustained for as long as
a client is connected. Repeatedly allocating and freeing GPU-visible
memory at that rate is exactly the kind of driver-level churn a
one-shot screenshot feature was never designed or tested against, and
is a well-known way to destabilize a Vulkan driver's internal allocator
over time (consistent with the ~15-20 second delay before the crash,
rather than an immediate first-call failure).

Fixed by making the staging buffer persistent: `VulkanRenderer` gains
`m_dualDeckCaptureStagingBuffer`/`..Memory`/`..Size`/`..Ptr` members
(mirroring how `m_textureReadbackBuffer` already persists for a similar
reason), allocated once and only reallocated if a later call needs more
space than it currently has, freed only in `VulkanRenderer`'s own
destructor alongside its other persistent buffers. The blit-target
`image`/`imageMemory` (only used for the non-directly-readable-format
branch, a smaller fraction of calls, and whose Vulkan image *layout*
would need correct tracking across reuse -- more involved than a plain
buffer, which has none) is deliberately left as per-call allocation for
now, not part of this fix. Also fixed in the same pass: `CreateBuffer()`'s
return value, previously unchecked (matching `HandleScreenshotRequest()`'s
own unchecked call, safe there only because it's rare) -- now checked,
failing the capture cleanly on allocation failure instead of proceeding
to map/copy through a `VK_NULL_HANDLE`.

**Verified**: regenerated via the established scratch-clone workflow --
diffed against the previously-committed patch to confirm only these
lines changed (`VulkanRenderer.cpp`'s `CaptureSurfaceBGRA()`/destructor,
`VulkanRenderer.h`'s new members). **Not verified**: this patch cannot
be compiled in this project's own sandbox (Cemu's vcpkg-based dependency
graph is unreachable here, confirmed repeatedly throughout this
integration) -- only CI, and ultimately a real user's hardware, can
confirm both that it compiles and that it actually resolves the crash.
If streaming still crashes after this fix, the next thing worth trying
is reducing `CEMU_REMOTE_CAPTURE_FPS` (e.g. to 15) to see whether the
crash becomes rarer/later (supporting the allocation-churn theory) or
is unaffected (pointing elsewhere, e.g. the blit-target `image` path
this fix left untouched, or the render-pass/command-buffer state at the
specific point in Cemu's pipeline this hook runs from).

**IPC frame-size cap: the likely actual root cause of the reported
flashing.** After the Vulkan-crash fix above was shown not to explain
the symptom (see the correction inline in that section), re-examined
`RemoteServerBridge::start()`'s reconnect loop and the adapter IPC
transport it drives. The ~1-second period of the observed
connecting/host-control cycling is an artifact of the loop's own
`backoffMs` resetting to 1000ms on every successful `connect()` -- not
evidence the connection survives close to a full second before dying.

`adapter-sdk/ipc/include/.../ipc_protocol.h` (vendored into this patch
under `src/remote_server/adapter_sdk/include/...`) defined
`kMaxIpcFramePixelBytes` as 16 MiB, sized off an assumption that Wii U's
TV surface tops out at 1920x1080 (8,294,400 bytes). `CemuAdapter`
actually captures the TV surface at Cemu's *real* internal render
resolution (`GetEffectiveSize()`), which is not always capped at 1080p.
`parseSurfaceFrame()` rejects any declared frame payload over the cap as
malformed, and both `AdapterIpcClient::readLoop()` and the server's own
receive loop treat a failed parse as "this connection is over."

That matches every symptom in the original report exactly: Cemu's
process itself is completely unaffected (consistent with the user's
correction, since this is a local Unix-socket IPC parse-rejection, not a
crash); the connection completes its small Hello/HelloAck handshake,
`ModeCoordinator` switches to `Emulation`, then the very first
oversized TV-surface `Frame` message is rejected and the connection
drops straight back to `HostControl`; and it doesn't depend on which
screen is showing, consistent with the user's clarification that it
happened while sitting stably in the game's own main menu (not during a
title transition, which is what an earlier, since-abandoned theory about
`CafeSystem.cpp`'s `LaunchForegroundTitle()`/`ShutdownTitle()` hooks had
assumed).

Fixed by raising `kMaxIpcFramePixelBytes` from 16 MiB to 128 MiB (see
the constant's own comment in `ipc_protocol.h` for the full sizing
rationale) in both the main repo's copy and this patch's vendored copy.
This is purely local same-machine IPC between Cemu and the Host Service
process, never sent over the network, so a generous cap costs nothing
bandwidth-wise. The same 16 MiB cap was independently too small for
Azahar too, once its own capture scale started auto-following
`resolution_factor` (see `docs/known-limitations.md`'s matching entry);
`host/azahar-patches/` vendors an identical copy of the same header and
was updated the same way.

**Update: the first guess at *why* the capture is oversized was tested
and disproved, but the fix stands.** Initially assumed this would need a
user-installed resolution-enhancing graphics pack to trigger. Asked the
user directly whether one was active; the answer was "no graphics packs
installed on either" -- while also reporting that the bug is specific to
Twilight Princess HD and doesn't reproduce on Wind Waker HD on the same
setup. That rules out graphics packs as the mechanism here, but not the
underlying cause: some Wii U titles render their own internal
framebuffer above their display output resolution as part of the
*game's own* built-in anti-aliasing/supersampling, independent of
anything Cemu's graphics-pack system adds -- Twilight Princess HD doing
this while Wind Waker HD doesn't would explain the exact per-title split
observed. The fix itself doesn't depend on which mechanism is at play,
since it's keyed off Cemu's real per-frame render-target size
(`GetEffectiveSize()`) rather than off detecting graphics packs
specifically, so raising the cap to 128 MiB should cover both. As
always, this patch cannot be compiled in this project's own sandbox, so
the fix is diff-verified only; a CI build and a real retest on the
user's hardware (specifically against Twilight Princess HD) are both
still needed.
