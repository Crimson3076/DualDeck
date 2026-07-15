# melonDS Integration Analysis (Phase 0)

Status: initial investigation, based on direct inspection of the upstream
melonDS repository. This document is the required Phase 0 deliverable and
gates any invasive melonDS modification (Section 24 of `SPEC.md`).

## 0. Source inspected

- Repository: `https://github.com/melonDS-emu/melonDS`
- Commit inspected: `10a173b5536fc75cd93f8a3868349dad963542ef` (master, 2026-06-07)
- License: GPLv3 "or later" (see upstream `LICENSE`). Any code that links
  against or is derived from melonDS (i.e. `host/melonds-patches/` and
  anything built into a patched melonDS binary) must be GPLv3-compatible.
  The protocol library and the Steam Deck client are standalone processes
  that only talk to melonDS over a socket, so they are not required to be
  GPL, but this repository adopts GPLv3 throughout for simplicity and to
  avoid any ambiguity if code is later upstreamed.
- This analysis was originally done by reading source only, in a sandbox
  without Qt6/SDL2/GL packages available. **That has since changed**: the
  dependencies were installed, an unmodified baseline build of melonDS at
  this exact commit was confirmed working, the patch proposed in section
  5 below was implemented and confirmed to build (including a from-scratch
  build of a fresh clone with the patch applied via `git apply`, not just
  the in-place working tree it was developed against), and the patched
  binary's embedded remote server was confirmed to perform a real
  authenticated handshake against a raw-socket test client under Xvfb.
  See section 8 below and `host/melonds-patches/README.md` for exactly
  what was and wasn't verified -- notably, the video path (frame capture
  and delivery) was reviewed and wired but not exercised with a real
  emulated frame, since that requires firmware/BIOS assets not available
  (and not appropriate to seek out) in this environment, for the same
  reason ROMs aren't included in this repository.

## 1. Where completed frames become available

### 1.1 Software renderer (`src/GPU_Soft.cpp`) — the practical integration point

`GPU::GetFramebuffers(void** top, void** bottom)` (`src/GPU.cpp:347`)
forwards to the active `Renderer` implementation.

`SoftRenderer::GetFramebuffers` (`src/GPU_Soft.cpp:447`):

```cpp
bool SoftRenderer::GetFramebuffers(void** top, void** bottom)
{
    int frontbuf = BackBuffer ^ 1;
    *top = Framebuffer[frontbuf][0];
    *bottom = Framebuffer[frontbuf][1];
    return true;
}
```

- Returns raw pointers to completed, CPU-readable framebuffers.
- Each buffer is `256 × 192` pixels, 32 bits/pixel, **BGRA8888** — not RGBA.
  See `SoftRenderer::ExpandColor` (`src/GPU_Soft.cpp:428`), which explicitly
  converts to "32-bit BGRA" because it is more broadly compatible with
  consumers such as Direct2D and cairo. Any frame transport code must
  either send BGRA verbatim (documenting it) or convert to RGBA/RGB565
  before sending.
- The renderer double-buffers (`Framebuffer[2][2]`) and the returned
  pointer always refers to the just-completed frame, not the buffer being
  written by the current frame — i.e. it is safe to read without tearing
  as long as the read happens on the frontend thread between frames (see
  §1.3).
- **Software rendering is melonDS's configured default** — `Config.cpp`
  defines `{"3D.Renderer", renderer3D_Software}` as the default value. This
  means a fresh melonDS installation already produces framebuffers this
  code path can read directly with zero configuration changes.

### 1.2 OpenGL renderer (`src/GPU_OpenGL.cpp`) — deferred, not the initial target

`GLRenderer::GetFramebuffers` (`src/GPU_OpenGL.cpp:915`):

```cpp
bool GLRenderer::GetFramebuffers(void** top, void** bottom)
{
    // since we use an array texture, we only need one of the pointer fields
    int frontbuf = BackBuffer ^ 1;
    *top = &FPOutputTex[frontbuf];
    *bottom = nullptr;
    return false;
}
```

- Returns `false` and hands back an OpenGL array-texture index, not a
  pixel buffer. Reading pixels back would require a `glReadPixels`/PBO
  path we would have to add ourselves, executed on the GL context's
  thread, which risks stalling the render pipeline if done naively.
- Per Section 6.1 of the spec ("support both software and hardware
  renderer paths where practical"), this is real but secondary work. The
  proposed patch boundary below only touches the software path in
  Phase 1; a follow-up patch would add an async PBO readback for the
  OpenGL path once the software path is proven.

### 1.3 Frame-complete timing — the Qt/SDL frontend's main loop

`src/frontend/qt_sdl/EmuThread.cpp` runs the emulation loop. Order of
operations per iteration (line numbers as inspected):

```
255  nds->SetKeyMask(inputMask)          // apply this frame's buttons
257  nds->TouchScreen(...) / ReleaseScreen()  // apply this frame's touch
311  nds->RunFrame()                     // emulate one frame
323  emuInstance->drawScreen()           // -> ScreenPanelNative::drawScreen()
                                         //    -> nds->GPU.GetFramebuffers(...)
                                         //    under bufferLock
```

`ScreenPanelNative::drawScreen()` (`src/frontend/qt_sdl/Screen.cpp:781`)
stores the two pointers under `bufferLock`; the Qt paint event later does
`memcpy(dst, buffer, 256*192*4)` under both `renderLock` and `bufferLock`.

**Conclusion:** the stable, documented point to capture a frame is
immediately after `NDS::RunFrame()` returns, guarded by a mutex, exactly
where the existing frontend already reads it for on-screen display. A
remote-server integration should hook in at this same point (e.g. right
after the existing `emuInstance->drawScreen()` call, or by copying the
bottom buffer into our own bounded queue in that spot) rather than trying
to intercept anything inside the renderer itself.

## 2. How input enters the emulation core

Two public `NDS` methods carry all controller/touch state into the core
(`src/NDS.h:419-422`, implementation `src/NDS.cpp:1165-1217`):

```cpp
void NDS::TouchScreen(u16 x, u16 y);   // SPI.GetTSC()->SetTouchCoords(x, y)
void NDS::ReleaseScreen();             // SetTouchCoords(0x000, 0xFFF) - sentinel "released" value
void NDS::SetKeyMask(u32 mask);        // active-low 12-bit button mask -> KeyInput register
```

- `SetKeyMask` takes a 12-bit mask (bits 0-9 map directly, bits 10-11 are
  relocated into `KeyInput` bits 16-17 to match the real DS `KEYINPUT`/
  `KEYCNT` hardware register layout). **Buttons are active-low**: a set
  bit means "not pressed" in the mask the frontend builds, since
  `EmuInstance::inputProcess()` starts from `joyInputMask = 0xFFF` and
  clears bits for pressed buttons (`src/frontend/qt_sdl/EmuInstanceInput.cpp`,
  around `inputProcess()`). Our remote protocol should keep our own
  button bitmask active-*high* (1 = pressed) for sanity on the wire, and
  invert only at the point we call `SetKeyMask`.
- Touch coordinates are raw DS panel coordinates in the `0-255 / 0-191`
  range (matching Section 7.4 of the spec exactly); `ReleaseScreen()` must
  be called whenever touch is not active — there is no separate "touch
  up" flag inside the core, only the sentinel coordinate value.
- The button ordering used by the existing frontend
  (`EmuInstance::buttonNames`, `src/frontend/qt_sdl/EmuInstanceInput.cpp:29`)
  is: `A, B, Select, Start, Right, Left, Up, Down, R, L, X, Y` — bit
  order 0..11. This differs from the bitmask proposed in `SPEC.md` §9
  (`A,B,X,Y,Up,Down,Left,Right,L,R,Start,Select`). **The protocol's wire
  bitmask is an independent, versioned format** (per spec: "the exact
  binary layout may change"); the host adapter is responsible for
  translating our wire bitmask into melonDS's `SetKeyMask` bit order. This
  mapping is a single small table and should be unit-tested.
- Call site: exactly one frontend thread calls these methods per frame,
  immediately before `RunFrame()` (`EmuThread.cpp:254-260`). A remote-server
  integration must feed its "latest received input state" into that same
  call site — it must not call these methods from the network thread
  directly, since `NDS` is not thread-safe with respect to the emulation
  thread.

## 3. Save-state and fast-forward APIs

- Save/load state: `EmuInstance::saveState(const std::string&)` /
  `EmuInstance::loadState(const std::string&)`
  (`src/frontend/qt_sdl/EmuInstance.h:186-187`). These take on-disk
  filenames chosen by the existing frontend UI; per Section 13 of the
  spec ("Do not accept file paths from the client"), the remote-action
  handler must supply its own fixed slot filenames, never anything
  derived from client input.
- Fast-forward / slow-mo / frame-step / pause and all other emulator
  shortcuts are implemented as **hotkeys**, not direct method calls:
  `EmuInstance::hotkeyNames[HK_MAX]` enumerates them (`HK_Lid`, `HK_Mic`,
  `HK_Pause`, `HK_Reset`, `HK_FastForward`, `HK_FrameLimitToggle`,
  `HK_FullscreenToggle`, `HK_SwapScreens`, `HK_SwapScreenEmphasis`,
  `HK_SolarSensorDecrease/Increase`, `HK_FrameStep`, `HK_PowerButton`, and
  more). The main loop reads `hotkeyDown`/`hotkeyPressed`/`hotkeyReleased`
  each frame (`EmuThread.cpp:336-342` etc.), fed by
  `keyHotkeyMask | joyHotkeyMask` in `inputProcess()`. This is a second,
  independent bitmask, matching the spec's design of a separate
  "emulator actions" bitmask distinct from DS buttons (§7.5, §9). The
  remote-server's emulator-action packets should be merged into this
  hotkey mask the same way a third input source (keyboard, joystick,
  network) would be, rather than calling `saveState`/`loadState` directly
  from network code.
- `HK_SwapScreens` / `HK_SwapScreenEmphasis` already implement the
  "swap local and remote screens" emulator action required by spec
  §7.5 — no new melonDS-side behavior is needed for that action, only a
  way to trigger the existing hotkey remotely.

## 4. Existing networking in melonDS

melonDS already ships local multiplayer / netplay functionality
(`LANDialog`, `NetplayDialog`, `MPSettingsDialog` in
`src/frontend/qt_sdl/`) built on **ENet** (`vcpkg.json` lists `enet` as a
dependency). This is unrelated to our bottom-screen streaming use case
(it synchronizes multiple emulator instances, not a thin remote display),
but it establishes that ENet is an already-accepted dependency in this
codebase, which is useful precedent if a later phase wants a reliable-UDP
library instead of hand-rolled UDP sequencing. The initial prototype
still starts from plain TCP + UDP per spec §8.3, since that has zero new
dependencies and is sufficient to validate the architecture.

## 5. Frontend architecture and best integration point

melonDS has exactly one actively maintained GUI frontend under
`src/frontend/qt_sdl/` (Qt widgets + SDL2 for input/audio backend, despite
the directory name — there is no separate "SDL frontend" distinct from
Qt). There is no plugin/extension API and no `frontend/` abstraction layer
that a remote server could hook into without touching the Qt frontend's
source.

This confirms **Option A vs Option C from `SPEC.md` §11** are the only
realistic paths (there is no plugin architecture to target for Option B).
Per the recommended path in §11, this project will develop against **a
maintained fork** of melonDS during the proof-of-concept phase
(`host/melonds-patches/`), with changes isolated to:

1. `EmuThread.cpp` — after the existing `emuInstance->drawScreen()` call,
   push the bottom-screen buffer pointer into a bounded, lock-protected
   single-slot queue owned by the remote-server module (copy, not
   reference, since the buffer is reused next frame).
2. `EmuInstanceInput.cpp` / the `inputProcess()` call site — OR into the
   existing `inputMask` / `isTouching` / `touchX` / `touchY` fields
   directly, merging remote input the same way local SDL joystick input
   is merged today, before `SetKeyMask`/`TouchScreen` are called.
3. A new hotkey-mask contribution (see §3) for remote emulator actions.
4. Frontend startup/shutdown lifecycle (`EmuInstance`/main window
   construction and teardown) to start and stop the remote-server thread
   alongside the existing emulation thread.

No other melonDS subsystem needs to change. This patch boundary is
proposed here for review before any invasive change is made, per
Section 24, item 10 of `SPEC.md`.

## 6. Known limitations of this analysis

- The OpenGL renderer path (§1.2) is understood at the API level and
  confirmed by reading `GLRenderer::GetFramebuffers()`; no PBO/async-
  readback design has been prototyped, and the patch (§8) only handles
  the software renderer.
- DSi-specific paths (microphone, camera, NAND) were not investigated
  since they are explicit non-goals for v0.1 (`SPEC.md` §21).
- The call sites documented in sections 1-3 were confirmed to still
  match at the exact commit inspected (§0) by successfully building and
  patching against it; they have not been re-verified against any later
  upstream commit.

## 7. Historical: originally recommended next step (superseded)

This section is kept for context on how the work actually proceeded. The
original recommendation here was to build a standalone protocol library
and host `remote-server` binary first, without touching melonDS, since
this sandbox initially had no Qt6/SDL2/GL packages. That work happened
(see `protocol/`, `host/remote-server/`, and `docs/architecture.md`), and
once the dependencies were later installed in the same environment, the
melonDS patch described in section 8 below was implemented on top of it,
reusing the same protocol/host code by vendoring it into melonDS's tree
rather than writing it twice.

## 8. The patch: what exists and what was verified

`host/melonds-patches/0001-remote-server-integration.patch` implements
exactly the boundary proposed in section 5. See
`host/melonds-patches/README.md` for the full, honest account of what was
verified: baseline build, patched build, patch-file-applies-to-a-fresh-clone,
a real authenticated handshake against the running patched binary under
Xvfb, and -- going further than "no ROM available" -- a minimal, fully
original homebrew `.nds` ROM written from scratch (`tests/homebrew-test-rom/`)
that was direct-booted successfully in the patched binary, with the video
path confirmed to deliver a real, stable, non-static frame from actual
`RunFrame()` execution. Three real things were caught during this
verification pass rather than shipped or assumed correct:

- The frame-push gate in `EmuThread.cpp` was initially written as
  `!useOpenGL`, which turns out to test the wrong thing --
  `EmuInstance::usesOpenGL()` is true whenever *either* the Qt display
  widget uses GL *or* the 3D renderer is hardware-accelerated, so it
  doesn't by itself indicate whether `GetFramebuffers()` will return real
  pointers. Fixed to rely solely on `GetFramebuffers()`'s own return
  value, which is the correct, self-documented check.
- A pre-existing melonDS quirk unrelated to this patch: a freshly created
  `$HOME` without an existing `~/.config` directory makes
  `Config::Load()` fail and pop a blocking `QMessageBox::critical` that
  never resolves headlessly. Worked around in testing, not patched (out
  of this patch's scope) -- see `tests/homebrew-test-rom/README.md`.
- Debug `fprintf` diagnostics added along the way to track down the above
  two issues were removed before finalizing the patch, with a clean
  rebuild confirming afterward that removing them didn't reintroduce
  either original symptom.

**What's still open, honestly**: the exact pixel channel order (RGBA vs
BGRA) of the delivered frame was not conclusively pinned down -- a test
with distinctly different R/G/B values across two runs produced identical
output both times, which doesn't cleanly fit either hypothesis. Input
injection into a game that reads input, booting to the system menu
(needs real firmware), and a commercial-cart-style ROM (secure-area
decryption path) are all still unexercised. See
`docs/known-limitations.md` for the complete list.

**Next recommended step**: get a real ROM (or continue with homebrew) on
a machine with a display and a gamepad, verify DS controls/touch actually
affect the running program, resolve the RGBA/BGRA question definitively,
and attempt a 30-minute stability run -- i.e. work through the remainder
of `SPEC.md` section 20's acceptance criteria.
