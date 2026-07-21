# Azahar Integration Analysis (Phase 0)

This is the same kind of up-front investigation `docs/melonds-integration-analysis.md`
did before any melonDS code was touched, applied to a second, independent
emulator: [Azahar](https://github.com/azahar-emu/azahar) (Nintendo 3DS),
chosen over Lime3DS/other Citra forks because Azahar absorbed both
PabloMK7's Citra fork and Lime3DS in 2026 and is the actively-maintained
one going forward -- Lime3DS itself stopped receiving independent
updates after the merge.

This document does not implement anything. It exists so the real
3DS-adapter work (whenever it starts) begins from an informed patch
boundary and a realistic sense of the actual work involved, the same way
melonDS's Phase 0 did -- not so this project can claim "3DS support" it
hasn't built.

## 0. Source inspected

Cloned `https://github.com/azahar-emu/azahar` at commit `75134fca82ea`
(2026-07-18, `master`) into a scratch directory -- not vendored into
this repo, and no patch exists yet. `license.txt` is GPLv2 (melonDS is
GPLv3; both copyleft, and this project's existing pattern of shipping an
external patch file plus a from-source build script rather than
redistributing modified source directly, already established for
melonDS, applies the same way here -- no new licensing model needed).

## 1. Where completed frames become available

Structurally simpler than it first looks, and arguably *cleaner* than
melonDS's situation (which needed one code path for the software
renderer and a separate `GLBottomScreenCapture` class for the OpenGL 3D
renderer -- see that doc's sections 1.1/1.2). Azahar's rendering is
backend-agnostic at the point that matters:

- `video_core/renderer_base.h`'s `RendererBase` (the common base for
  the OpenGL, Vulkan, *and* software renderer backends) exposes
  `RequestScreenshot(void* data, std::function<void(bool)> callback, const Layout::FramebufferLayout& layout)`.
  Set `settings.screenshot_requested = true` plus the target layout;
  next frame, the renderer writes into `data` and fires `callback`.
  `citra_qt/bootmanager.cpp`'s existing `CaptureScreenshot()` (used for
  the ordinary "save a PNG" feature) is a working, real caller of this
  exact API -- not a theoretical hook, a proven one.
- `FramebufferLayout` (`core/frontend/framebuffer_layout.h`) composes
  **both** 3DS screens into one buffer, with `top_screen`/`bottom_screen`
  as sub-rectangles of one `width`/`height` canvas -- one screenshot
  request, not two. A custom, non-default layout (e.g. top-screen-only,
  or bottom-screen-only, matching what this project already does for
  the DS: TV gets the "main" screen, the Steam Deck gets the touch
  screen) is just a different `FramebufferLayout` value, not new
  renderer code.
- The frame-complete point to trigger this from is
  `citra_qt/bootmanager.cpp`'s `GRenderWindow::SwapBuffers()`
  (`system.GPU().Renderer().TryPresent(...); context->SwapBuffers();`)
  -- the direct analog of melonDS's `EmuThread::run()` calling
  `drawScreen()`, which is where that project's frame-capture patch
  point lives today.

**Net assessment**: video capture looks *more* tractable than it was
for melonDS, not less, because Azahar already ships a renderer-agnostic
screenshot mechanism this project doesn't have to invent. The real
unknown is performance: 3DS games render real 3D scenes (unlike the
DS's largely-2D workload melonDS's software path handles trivially),
so calling `RequestScreenshot` every frame at a real framerate needs to
actually be measured, not assumed -- this analysis does not attempt
that measurement.

## 2. How input enters the emulation core

Also more uniform than melonDS's approach (which merged remote state
directly into a raw `inputMask` right before the emulator core reads
it -- see that doc's section 2). Azahar's HID service
(`core/hle/service/hid/hid.cpp`) reads circle-pad/buttons/etc. through a
registered `Input::InputDevice`/`Input::Factory` abstraction (e.g.
`circle_pad = Input::CreateDevice<Input::AnalogDevice>(...)`, then
`circle_pad->GetStatus()` polled once per HID update), the same
mechanism `input_common/sdl/` and `input_common/udp/` already use to
register real input backends by name (an "engine" string in a
`Common::ParamPackage`).

This suggests the integration point is registering a new custom engine
(e.g. `"engine:melonds_remote"`) that reads from a thread-safe buffer
this project's own bridge code fills from incoming `ControllerState`
packets, mirroring `input_common/udp/`'s existing pattern (also a
network-fed input source) rather than needing to touch every call site
that reads a button/circle-pad/touch value individually. Touch-screen
input specifically needs its own check (3DS touch is a distinct HID
service module) -- not confirmed here, only the button/circle-pad path.

**Net assessment**: plausible and idiomatic (this is precisely what the
engine/factory system is *for*), but not yet confirmed end-to-end --
touch input and how the HID service instantiates which engine(s) are
active at once both need a closer read before writing real code.

## 3. What does *not* exist yet (dead ends worth recording so they
aren't rediscovered later)

- `citra_cli` looked, from its directory name alone, like it might be a
  ready-made headless (no-Qt) frontend -- useful for an eventual
  out-of-process adapter, the same shape `MelonDSAdapter` eventually
  became. It is not: `citra_cli.cpp` is 45 lines and only handles a
  `--compress`/`--decompress` file-compression utility subcommand,
  nothing related to booting or running a game. A headless 3DS-adapter
  binary, if ever built, would still need to start from the Qt
  frontend's boot sequence (`citra_qt.cpp`), same as melonDS's
  `EmuInstance` did.
- `network/artic_base` exists (an "Artic Base Server" -- a real,
  existing Azahar feature letting a real 3DS console feed game-card
  data to the emulator over the network) but solves a completely
  different problem (physical-cartridge dumping over LAN) and has
  nothing to do with remote display/input streaming. Not a reusable
  precedent for this project's purposes, despite the superficial
  "network server already exists" similarity.
- `citra_room`/`citra_room_standalone` are the existing multiplayer
  lobby/relay server -- real precedent for "this codebase already ships
  a standalone non-Qt network binary built from the same source tree,"
  which is mildly reassuring about build-system feasibility, but not a
  frame/input transport precedent (it relays game-to-game multiplayer
  packets, not screen pixels or HID state).

## 4. Frontend architecture and where a patch boundary would likely sit

Same overall shape as melonDS: a Qt6 main window/frontend
(`src/citra_qt/`) owning a `GRenderWindow` (`bootmanager.h/.cpp`) that
in turn owns the active `RendererBase`. The natural analog of melonDS's
`RemoteServerBridge` + `EmuInstance` integration would be a new
`src/citra_qt/remote_server/` (or similarly-scoped) subdirectory,
constructed/started from wherever `citra_qt.cpp` finishes booting a
game, calling `RequestScreenshot()` on a timer/per-frame from
`GRenderWindow` and registering the custom input engine described in
section 2 -- conceptually the same three-piece shape
(`MelonDSAdapter`/`RemoteServerBridge`/patch-tree-vendored
protocol+host code) this project already built once, adapted to
Azahar's specific hook points.

Critically, **this project's existing `IEmulatorAdapter` contract
(`adapter-sdk/include/melonds_remote/adapter/adapter_contract.h`,
issue #28 Phase 1) does not need to change** to support this -- it was
deliberately built generic (proven against fake DS/3DS/Wii U fixtures
specifically so a real adapter could be swapped in later without
protocol changes). An `AzaharAdapter` implementing that same contract
is additive work, not a rearchitecture.

## 5. Known limitations of this analysis

- No build was attempted. `docs/building.md`-equivalent dependencies
  (Qt6, Vulkan SDK, boost, etc. -- see the wiki's Building From Source
  page) were read about, not verified against this sandbox, which is
  very likely missing several of them (Vulkan headers/loader in
  particular).
- Touch-screen input injection specifically, and exactly how/when the
  HID service decides which registered engine(s) are "active," were not
  traced to a concrete call site -- flagged as a real unknown in
  section 2, not confirmed.
- `RequestScreenshot`'s actual runtime cost at real framerates against a
  real running game was not measured (no game/ROM available in this
  sandbox to test with, same category of limitation
  `docs/known-limitations.md` already records for melonDS's own
  real-hardware-only gaps).
- Save-states, fast-forward, and other emulator-action equivalents
  (mirroring melonDS's `EmulatorAction` bitmask -- `EmulatorAction_SaveState`,
  `_LoadState`, etc.) were not investigated at all.
- This analysis covers the Linux desktop Qt6 build only, matching this
  project's existing HTPC-focused scope -- Azahar's Android build was
  not considered.

## 6. Proposed next step (not started -- needs sign-off before real code)

Mirroring how the melonDS integration actually proceeded (Phase 0
analysis -> adapter contract already existed from issue #28 -> a real
`MelonDSAdapter` implementing it -> vendoring into a patch -> real
end-to-end verification), the equivalent next step here would be:

1. Get a real Azahar build working in an environment with a display and
   a real Vulkan/OpenGL stack (this sandbox cannot do this -- same
   constraint noted for melonDS's own uinput/real-hardware gaps).
2. Confirm `RequestScreenshot()`'s real per-frame cost and the touch-input
   engine question from section 5 against an actual running game, not
   just source reading.
3. Implement `AzaharAdapter : IEmulatorAdapter` (this project's existing,
   already-generic contract -- no protocol/adapter-sdk changes needed)
   as a small `src/citra_qt/remote_server/` patch, following the same
   patch-file-not-vendored-source discipline this project already uses
   for melonDS.
4. Real end-to-end verification against the actual client, the same bar
   every other milestone in this project has been held to.

This is a genuinely large, multi-session undertaking on the same order
as the original melonDS integration -- not a quick follow-on. No
adapter code has been written yet.
