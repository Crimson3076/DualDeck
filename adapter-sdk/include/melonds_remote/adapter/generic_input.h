#pragma once

// Generic input state (GitHub issue #28's "Generic protocol model" ->
// "Generic input": replace dsButtons as the top-level protocol model
// with a generic input state). Part of the Phase 1 contract groundwork
// -- see docs/adr/0001-host-service-and-adapter-architecture.md. Not
// yet used by the live client/host wire protocol (still
// protocol.h's DS-specific ControllerState) -- adapters translate
// generic DualDeck input into native emulator input once the Host
// Service and a real non-DS adapter both exist; today only the fake
// fixtures under adapter-sdk/fake_adapters/ consume this type.

#include <cstdint>
#include <string>
#include <vector>

namespace melonds_remote::adapter {

// Generic gamepad button bitmask -- deliberately distinct from
// melonds_remote::DSButton (protocol.h), which remains the DS-specific
// wire bitmask the live v6 protocol actually sends today. A DS adapter
// maps between the two (see the ADR's "DS compatibility adapter"
// decision); a 3DS/Wii U adapter would map its own native buttons
// straight into this generic set instead.
enum GenericButton : uint32_t {
    GenericButton_South     = 1u << 0,  // A / Cross
    GenericButton_East      = 1u << 1,  // B / Circle
    GenericButton_West      = 1u << 2,  // X / Square
    GenericButton_North     = 1u << 3,  // Y / Triangle
    GenericButton_DpadUp    = 1u << 4,
    GenericButton_DpadDown  = 1u << 5,
    GenericButton_DpadLeft  = 1u << 6,
    GenericButton_DpadRight = 1u << 7,
    GenericButton_L1        = 1u << 8,
    GenericButton_R1        = 1u << 9,
    GenericButton_L2        = 1u << 10, // digital click, when an adapter has no analog trigger of its own
    GenericButton_R2        = 1u << 11,
    GenericButton_L3        = 1u << 12, // left stick click
    GenericButton_R3        = 1u << 13, // right stick click
    GenericButton_Start     = 1u << 14,
    GenericButton_Select    = 1u << 15,
    GenericButton_Home      = 1u << 16, // Wii U Home / 3DS Home-equivalent, not a Steam Deck system button
};

// Versioned emulator action bitmask -- deliberately distinct from
// melonds_remote::EmulatorAction (protocol.h), for the same reason as
// GenericButton above.
enum GenericEmulatorAction : uint32_t {
    GenericAction_PauseResume     = 1u << 0,
    GenericAction_FastForward     = 1u << 1,
    GenericAction_SaveState       = 1u << 2,
    GenericAction_LoadState       = 1u << 3,
    GenericAction_SwapSurfaces    = 1u << 4, // generalizes protocol.h's EmulatorAction_SwapScreens
    GenericAction_OpenClientMenu  = 1u << 5,
    GenericAction_Disconnect      = 1u << 6,
    GenericAction_QuitSession     = 1u << 7,
    GenericAction_QuitApplication = 1u << 8,
};

// One active touch/stylus contact, associated with a specific video
// surface by ID (issue #28: "One or more touch contacts associated
// with a surface ID") -- unlike protocol.h's ControllerState, which has
// exactly one implicit touch point on the one-and-only DS bottom
// screen, this supports zero, one, or more simultaneous contacts across
// however many touch-capable surfaces an adapter declares.
struct TouchContact {
    std::string surfaceId;
    uint16_t x = 0;
    uint16_t y = 0;
};

struct GenericInputState {
    // Mirrors protocol.h's ControllerState.sequence/clientTimestampUs --
    // same purpose (stale/out-of-order packet detection once this
    // travels over real IPC), same wall-clock convention.
    uint32_t sequence = 0;
    uint64_t clientTimestampUs = 0;

    uint32_t buttons = 0; // GenericButton bitmask, 1 = pressed
    int16_t leftStickX = 0;
    int16_t leftStickY = 0;
    int16_t rightStickX = 0;
    int16_t rightStickY = 0;

    // Analog trigger values (issue #28: "Analog triggers where
    // supported") -- 0..255, only meaningful when
    // AdapterCapabilities::supportsAnalogTriggers is true (see
    // adapter_contract.h); an adapter without analog triggers relies on
    // GenericButton_L2/R2's digital click instead.
    uint8_t leftTrigger = 0;
    uint8_t rightTrigger = 0;

    std::vector<TouchContact> touches;

    // Optional microphone state (issue #28: "Optional ... microphone
    // state") -- whether the client is actively streaming audio right
    // now, not the audio itself (that stays a separate channel, see
    // protocol.h's MicAudioFramePayload for the live DS-specific
    // equivalent).
    bool micActive = false;

    uint32_t emulatorActions = 0; // GenericEmulatorAction bitmask, 1 = active this state
};

} // namespace melonds_remote::adapter
