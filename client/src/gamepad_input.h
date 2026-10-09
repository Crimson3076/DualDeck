#pragma once

// Gamepad -> DS button mapping (SPEC.md section 7.3), the L3+R3 menu
// chord timing every screen shares, and gamepad diagnostics logging.
// Split out of main.cpp.

#include <SDL3/SDL.h>

#include <cstdint>

namespace melonds_remote::client {

// Deliberate-hold duration for the L3+R3 "open menu" chord, shared
// by discoverAndSelectHost() and main()'s inner loop so every screen uses
// the same chord (GitHub issues #8, #9: the discovery/host-selection
// screen previously had no exit control at all, despite already showing
// the "HOLD START + SELECT" hint -- the hint just wasn't backed by any
// actual handling there). Requires a sustained hold rather than firing
// the instant both buttons are seen down in the same polled frame -- real
// Steam Deck hardware testing showed a single button press (Start, or B)
// opening the menu, most likely via Steam Input's default binding
// template synthesizing a keyboard Escape for individual buttons (see the
// gating on !gamepad in the KEY_DOWN handlers below); requiring a
// sustained two-button hold is defense in depth against any single
// spurious button/synthesized-input report on top of that fix.
//
// Originally Start+Select, changed to the left/right stick clicks (L3+R3)
// because Start+Select is also Steam Input's own default chord for
// switching between the "gamepad" and "desktop" action sets on Steam
// Deck -- holding it was being intercepted before this app ever saw a
// sustained press, rather than opening this menu. L3/R3 aren't mapped to
// any DS button (the DS has no analog sticks), so they're free, and
// unlike Start+Select they're not a Steam Deck system chord.
inline constexpr uint64_t kMenuChordHoldUs = 350'000; // 350ms deliberate hold

// Plain `-axis` overflows int16_t's range at the negative extreme
// (-(-32768) doesn't fit in 16 bits) -- clamps to the wire's own
// documented -32768..32767 range instead of relying on wraparound.
int16_t negateStickAxis(int16_t axis);

// `stickEmulatesDpad` (spec section 7.3's "left stick as an alternate
// D-pad") defaults to on since it's harmless local visual feedback for
// the setup wizard's controller test, the only other caller. The real
// per-frame gameplay call site below passes false for a 3DS/Azahar
// session: that stick has a genuine analog Circle Pad of its own (sent
// separately, see buildControllerState()), and folding the same stick
// tilt into the physical D-Pad's digital bits too would fire both at
// once for one physical motion -- confirmed via AzaharAdapter.cpp's
// registerInputEngine(), which binds the D-Pad's four native buttons to
// GenericButton_Dpad* independently of the Circle Pad's own analog
// engine binding, so a game watching either one would see it.
uint16_t buildButtonsFromGamepad(SDL_Gamepad* gamepad, bool stickEmulatesDpad = true);

void logGamepadTouchpadDiagnostics(SDL_Gamepad* gamepad);

} // namespace melonds_remote::client
