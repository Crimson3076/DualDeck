# melonDS patches

Empty for now. This directory will hold the diff/patch series (or a
`melonDS/` submodule pointing at a maintained fork — see `SPEC.md`
section 11, "Option C") that adds remote-server integration directly to
melonDS's frontend.

Do not add invasive melonDS changes here until the proposed patch
boundary in `docs/melonds-integration-analysis.md` (section 5) has been
reviewed. The boundary as currently proposed touches only:

1. `src/frontend/qt_sdl/EmuThread.cpp` — push the completed bottom-screen
   buffer into the remote-server's frame source after the existing
   `emuInstance->drawScreen()` call.
2. `src/frontend/qt_sdl/EmuInstanceInput.cpp` (or the `inputMask`/
   `isTouching`/`touchX`/`touchY` fields it populates) — merge remote
   controller/touch state in the same way local SDL joystick input is
   merged today.
3. The hotkey mask (`EmuInstance::hotkeyDown`/`hotkeyPressed`/
   `hotkeyReleased`) — merge remote emulator actions.
4. `EmuInstance`/main window construction and teardown — start and stop
   the remote-server thread alongside the existing emulation thread.

No other melonDS subsystem should need to change for v0.1.
