#include "gamepad_input.h"

#include <cstdint>

#include "client_log.h"
#include "melonds_remote/protocol.h"

namespace melonds_remote::client {

namespace {

// DS button bit <- SDL gamepad button, per SPEC.md section 7.3.
struct ButtonMapping {
    SDL_GamepadButton sdlButton;
    uint16_t dsBit;
};

// Mapped by PHYSICAL POSITION, not by the letter printed on the client's
// controller. Real user report, 2026-08-04: the face buttons were wired
// letter-to-letter (Deck A -> DS A, Deck B -> DS B, ...), which is the
// one arrangement that is wrong on every single button, because the two
// consoles put their letters in mirrored places:
//
//       Steam Deck (Xbox layout)        Nintendo DS / 3DS
//                  Y                            X
//               X     B                      Y     A
//                  A                            B
//
// SDL3's SOUTH/EAST/WEST/NORTH names are already positional (that is why
// SDL renamed them from A/B/X/Y), so the fix is to map position to
// position: the button under your thumb on the bottom is "confirm" on
// both machines, and on a DS that button is labelled B.
//
// This cannot be corrected downstream in the emulator's own controller
// config: remote input arrives as an already-decoded DS button bitmask
// over the protocol (see buildButtonsFromGamepad() below and protocol.h's
// DSButton_* bits), so it never passes through Azahar's or melonDS's SDL
// binding layer at all. The reporting user confirmed exactly that --
// resetting Azahar's controls and running EmuDeck's Reset Controls
// changed nothing, while Azahar's own local SDL mapping was already
// correct.
constexpr ButtonMapping kButtonMappings[] = {
    {SDL_GAMEPAD_BUTTON_SOUTH, DSButton_B},
    {SDL_GAMEPAD_BUTTON_EAST, DSButton_A},
    {SDL_GAMEPAD_BUTTON_WEST, DSButton_Y},
    {SDL_GAMEPAD_BUTTON_NORTH, DSButton_X},
    {SDL_GAMEPAD_BUTTON_DPAD_UP, DSButton_Up},
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, DSButton_Down},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, DSButton_Left},
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, DSButton_Right},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, DSButton_L},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, DSButton_R},
    {SDL_GAMEPAD_BUTTON_START, DSButton_Start},
    {SDL_GAMEPAD_BUTTON_BACK, DSButton_Select}, // "View" on Steam Deck
};

// Compile-time regression guard for the four face buttons. The bug this
// replaces was invisible in review -- {SOUTH, DSButton_A} reads
// perfectly naturally, and is wrong only once you know the two consoles
// mirror their labels -- and it survived until somebody played a game
// and found every face button transposed. It is also exactly the kind of
// line a future edit would "tidy" back into letter order.
//
// Asserted here rather than in client/tests/ because these are the SDL
// enum constants: the client test target deliberately doesn't link SDL3,
// and a test that redeclared the values would only be checking its own
// copy of them. A static_assert costs nothing, cannot drift from the
// table it guards, and fails the build in CI rather than in a game.
constexpr uint16_t dsBitFor(SDL_GamepadButton button) {
    for (const auto& mapping : kButtonMappings) {
        if (mapping.sdlButton == button) return mapping.dsBit;
    }
    return 0;
}
static_assert(dsBitFor(SDL_GAMEPAD_BUTTON_SOUTH) == DSButton_B,
              "Face buttons map by physical position: the bottom button is DS B, not DS A (SPEC.md 7.3)");
static_assert(dsBitFor(SDL_GAMEPAD_BUTTON_EAST) == DSButton_A,
              "Face buttons map by physical position: the right button is DS A, not DS B (SPEC.md 7.3)");
static_assert(dsBitFor(SDL_GAMEPAD_BUTTON_WEST) == DSButton_Y,
              "Face buttons map by physical position: the left button is DS Y, not DS X (SPEC.md 7.3)");
static_assert(dsBitFor(SDL_GAMEPAD_BUTTON_NORTH) == DSButton_X,
              "Face buttons map by physical position: the top button is DS X, not DS Y (SPEC.md 7.3)");

} // namespace

constexpr int16_t kStickDeadzone = 8000;

int16_t negateStickAxis(int16_t axis) {
    return axis == INT16_MIN ? INT16_MAX : static_cast<int16_t>(-axis);
}

uint16_t buildButtonsFromGamepad(SDL_Gamepad* gamepad, bool stickEmulatesDpad) {
    if (!gamepad) return 0;

    uint16_t buttons = 0;
    for (const auto& mapping : kButtonMappings) {
        if (SDL_GetGamepadButton(gamepad, mapping.sdlButton)) {
            buttons |= mapping.dsBit;
        }
    }

    if (stickEmulatesDpad) {
        int16_t leftX = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
        int16_t leftY = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY);
        if (leftX > kStickDeadzone) buttons |= DSButton_Right;
        if (leftX < -kStickDeadzone) buttons |= DSButton_Left;
        if (leftY > kStickDeadzone) buttons |= DSButton_Down;
        if (leftY < -kStickDeadzone) buttons |= DSButton_Up;
    }

    return buttons;
}

//
// Real user report, 2026-08-02: a full client.log from an actual
// connected session on real Steam Deck hardware showed only "[input]
// opened gamepad: Steam Deck Controller" -- never any of the touchpad
// diagnostic lines the 2026-08-01 entry describes below, even though a
// full session ran. Root cause, found by re-reading this file: the
// Deck's own built-in controller is already present the instant this
// app starts (unlike a hot-plugged USB pad), so it gets opened by the
// separate startup path near the top of main() (SDL_GetGamepads() +
// SDL_OpenGamepad() directly, before the event loop even begins) --
// this diagnostic used to live ONLY inside the SDL_EVENT_GAMEPAD_ADDED
// handler further down, guarded by `if (!gamepad)`, which never runs
// once the startup path has already opened one. This diagnostic has
// therefore never actually collected real data from a Steam Deck at
// all -- factored into its own function so both the startup path and
// the event-driven hotplug path call the exact same logging, rather
// than the original single copy silently only covering one of the two.
//
// Real user report, 2026-08-01, the diagnostic itself: touchpad input
// in Host Control mode never registered at all, on real hardware,
// despite the code reading SDL's dedicated gamepad-touchpad API (which
// should work regardless of Steam Input's control-scheme binding). SDL
// only exposes touchpad capability at all if it identifies *this
// specific gamepad instance* as a touchpad-capable device in the first
// place (its internal gamecontrollerdb mapping, keyed off the reported
// name/VID/PID) -- logged here so a real report of what SDL actually
// sees for this exact connection replaces guessing.
//
// Real hardware report, 2026-08-03: still touchpads=0 even after
// bundling SDL 3.4.12 (which does unconditionally register 2 touchpads
// the moment HIDAPI_DriverSteamDeck_OpenJoystick() runs, confirmed by
// reading that exact function's source -- no gating condition at all).
// Since that function is unconditional once reached, touchpads=0
// despite it means that function is never being reached in the first
// place: SDL isn't opening this device through its native Steam Deck
// HIDAPI driver at all. The leading suspect: Steam Input's synthetic
// "Xbox 360-compatible" virtual gamepad (created via uinput, exposed to
// any app that doesn't request raw Steam Input access -- the exact same
// VID/PID convention this project's OWN host_control_adapter.cpp reuses
// for its own virtual gamepad, see that file's comment) reports through
// SDL's generic joystick backend, not the HIDAPI Steam Deck driver, and
// carries no touchpad data by construction -- disabling Steam Input for
// just this one shortcut may not be the same as SteamOS's system-wide
// virtual-gamepad generation being off. Logging vendor/product ID and
// type here distinguishes the two possibilities directly: Valve's own
// ID (0x28de) means the real hardware driver opened it (and touchpads=0
// would then be a genuinely new mystery); Microsoft's Xbox 360 ID
// (0x045e/0x028e) confirms it's Steam's synthetic virtual gamepad
// instead.
void logGamepadTouchpadDiagnostics(SDL_Gamepad* gamepad) {
    const char* name = gamepad ? SDL_GetGamepadName(gamepad) : nullptr;
    int numTouchpads = gamepad ? SDL_GetNumGamepadTouchpads(gamepad) : 0;
    Uint16 vendor = gamepad ? SDL_GetGamepadVendor(gamepad) : 0;
    Uint16 product = gamepad ? SDL_GetGamepadProduct(gamepad) : 0;
    const char* typeStr = gamepad ? SDL_GetGamepadStringForType(SDL_GetGamepadType(gamepad)) : nullptr;
    logLine("[input] gamepad connected: name=%s touchpads=%d vendor=0x%04x product=0x%04x type=%s\n",
            name ? name : "(null)", numTouchpads, vendor, product, typeStr ? typeStr : "(null)");
    for (int tp = 0; tp < numTouchpads; ++tp) {
        logLine("[input]   touchpad %d: %d finger slot(s)\n", tp, SDL_GetNumGamepadTouchpadFingers(gamepad, tp));
    }
}

} // namespace melonds_remote::client
