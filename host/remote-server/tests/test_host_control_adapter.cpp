// Unit tests for translateControllerState() (GitHub issue #4 Phase C) --
// pure DSButton -> virtual-gamepad-state mapping, deliberately kept free
// of any I/O so it's testable without a real /dev/uinput node. This
// sandbox has none, so HostControlAdapter's actual device creation
// (open("/dev/uinput"), the UI_DEV_SETUP/UI_ABS_SETUP/UI_DEV_CREATE
// ioctls) is exercised only by compiling clean and by
// isDeviceReady()-gated manual testing on a real Linux host with uinput
// access -- see docs/known-limitations.md's Phase C entry.

#include "host/host_control_adapter.h"
#include "test_framework.h"

#include <cstdlib>
#include <string>

using namespace melonds_remote;
using namespace melonds_remote::host;

MDR_TEST(translate_no_buttons_is_all_released) {
    ControllerState state;
    HostControlGamepadState out = translateControllerState(state);
    MDR_CHECK(!out.south);
    MDR_CHECK(!out.east);
    MDR_CHECK(!out.west);
    MDR_CHECK(!out.north);
    MDR_CHECK(!out.tl);
    MDR_CHECK(!out.tr);
    MDR_CHECK(!out.start);
    MDR_CHECK(!out.select);
    MDR_CHECK_EQ(out.hatX, 0);
    MDR_CHECK_EQ(out.hatY, 0);
}

// A -> south, B -> east: DS's low two button bits map straight onto the
// Xbox pad's low two face buttons.
MDR_TEST(translate_a_and_b_map_to_south_and_east) {
    ControllerState state;
    state.dsButtons = DSButton_A | DSButton_B;
    HostControlGamepadState out = translateControllerState(state);
    MDR_CHECK(out.south);
    MDR_CHECK(out.east);
    MDR_CHECK(!out.west);
    MDR_CHECK(!out.north);
}

// DS X -> west, DS Y -> north: matches host::AdapterBridge's
// dsButtonsToGenericButtons table exactly (see that function's own
// comment on why X/Y don't map straight onto their own bit positions),
// so this project has one consistent DS-button-meaning across every
// translation table, not two disagreeing ones.
MDR_TEST(translate_x_and_y_map_to_west_and_north_not_the_reverse) {
    ControllerState state;
    state.dsButtons = DSButton_X | DSButton_Y;
    HostControlGamepadState out = translateControllerState(state);
    MDR_CHECK(out.west);
    MDR_CHECK(out.north);
    MDR_CHECK(!out.south);
    MDR_CHECK(!out.east);
}

MDR_TEST(translate_shoulders_and_start_select) {
    ControllerState state;
    state.dsButtons = DSButton_L | DSButton_R | DSButton_Start | DSButton_Select;
    HostControlGamepadState out = translateControllerState(state);
    MDR_CHECK(out.tl);
    MDR_CHECK(out.tr);
    MDR_CHECK(out.start);
    MDR_CHECK(out.select);
}

MDR_TEST(translate_dpad_to_hat_axes) {
    {
        ControllerState state;
        state.dsButtons = DSButton_Left;
        MDR_CHECK_EQ(translateControllerState(state).hatX, static_cast<int8_t>(-1));
    }
    {
        ControllerState state;
        state.dsButtons = DSButton_Right;
        MDR_CHECK_EQ(translateControllerState(state).hatX, static_cast<int8_t>(1));
    }
    {
        ControllerState state;
        state.dsButtons = DSButton_Up;
        MDR_CHECK_EQ(translateControllerState(state).hatY, static_cast<int8_t>(-1));
    }
    {
        ControllerState state;
        state.dsButtons = DSButton_Down;
        MDR_CHECK_EQ(translateControllerState(state).hatY, static_cast<int8_t>(1));
    }
}

// Opposite d-pad directions held simultaneously (a malformed/unusual
// client state, but not something translateControllerState() should
// crash or produce an out-of-range hat value for) -- Left wins over
// Right, Up wins over Down, matching the if/else-if order in the
// implementation. Not a meaningful real-world input, just a bounds
// check.
MDR_TEST(translate_opposite_dpad_directions_does_not_crash_or_go_out_of_range) {
    ControllerState state;
    state.dsButtons = DSButton_Left | DSButton_Right | DSButton_Up | DSButton_Down;
    HostControlGamepadState out = translateControllerState(state);
    MDR_CHECK(out.hatX == -1 || out.hatX == 1);
    MDR_CHECK(out.hatY == -1 || out.hatY == 1);
}

MDR_TEST(translate_left_stick_passes_through_unscaled) {
    ControllerState state;
    state.leftStickX = 12345;
    state.leftStickY = -6789;
    HostControlGamepadState out = translateControllerState(state);
    MDR_CHECK_EQ(out.leftStickX, static_cast<int16_t>(12345));
    MDR_CHECK_EQ(out.leftStickY, static_cast<int16_t>(-6789));
}

MDR_TEST(translate_ignores_touch_and_emulator_actions) {
    // Host-control mode has no touchscreen surface and no emulator to
    // send EmulatorAction bits to -- both fields must be silently
    // ignored, not misinterpreted as some other gamepad input.
    ControllerState state;
    state.touchActive = 1;
    state.touchX = 100;
    state.touchY = 50;
    state.emulatorActions = EmulatorAction_PauseResume | EmulatorAction_SwapScreens;
    HostControlGamepadState out = translateControllerState(state);
    MDR_CHECK(!out.south);
    MDR_CHECK(!out.east);
    MDR_CHECK(!out.west);
    MDR_CHECK(!out.north);
    MDR_CHECK(!out.tl);
    MDR_CHECK(!out.tr);
    MDR_CHECK_EQ(out.hatX, 0);
    MDR_CHECK_EQ(out.hatY, 0);
}

// GitHub issue #4's own note (carried since this milestone was first
// planned): a real /dev/uinput node doesn't exist in this sandbox, so
// HostControlAdapter::isDeviceReady() is expected to be false here --
// this is a documentation-as-test guard confirming that expectation
// stays true (and exercising the constructor/destructor's failure path
// at least once), not a claim that a real device was ever created.
MDR_TEST(host_control_adapter_degrades_gracefully_without_uinput_access) {
    HostControlAdapter adapter;
    if (!adapter.isDeviceReady()) {
        // Expected in this sandbox -- applyControllerState()/releaseAll()
        // must still be safe no-ops, not crash.
        ControllerState state;
        state.dsButtons = DSButton_A;
        state.mouseDeltaX = 5;
        state.mouseDeltaY = -5;
        state.mouseButtons = MouseButton_Left;
        adapter.applyControllerState(state);
        adapter.releaseAll();
        std::vector<uint8_t> frame;
        uint64_t frameIndex = 0;
        uint16_t width = 0, height = 0;
        MDR_CHECK(!adapter.getLatestFrame(frame, frameIndex, width, height));
    }
    // Same expectation, same sandbox limitation, for the separate virtual
    // mouse device -- see isMouseDeviceReady()'s own comment on why it's
    // independent of isDeviceReady() above.
    MDR_CHECK(!adapter.isMouseDeviceReady());
    // If this ever runs somewhere uinput *is* available (a real Linux
    // dev machine with the right permissions, not this sandbox/CI), the
    // same calls above still must not crash -- no separate assertion
    // needed since isDeviceReady()/isMouseDeviceReady() being true just
    // means the "expected" no-op path isn't exercised this run.
}

// translateMouseState() -- pure ControllerState -> HostControlMouseState
// mapping, same no-I/O rationale as translateControllerState() above.
MDR_TEST(translate_mouse_no_input_is_zero_and_released) {
    ControllerState state;
    HostControlMouseState out = translateMouseState(state);
    MDR_CHECK_EQ(out.dx, static_cast<int16_t>(0));
    MDR_CHECK_EQ(out.dy, static_cast<int16_t>(0));
    MDR_CHECK(!out.leftDown);
    MDR_CHECK(!out.rightDown);
}

MDR_TEST(translate_mouse_deltas_pass_through_unscaled) {
    ControllerState state;
    state.mouseDeltaX = -1234;
    state.mouseDeltaY = 5678;
    HostControlMouseState out = translateMouseState(state);
    MDR_CHECK_EQ(out.dx, static_cast<int16_t>(-1234));
    MDR_CHECK_EQ(out.dy, static_cast<int16_t>(5678));
}

MDR_TEST(translate_mouse_buttons) {
    {
        ControllerState state;
        state.mouseButtons = MouseButton_Left;
        HostControlMouseState out = translateMouseState(state);
        MDR_CHECK(out.leftDown);
        MDR_CHECK(!out.rightDown);
    }
    {
        ControllerState state;
        state.mouseButtons = MouseButton_Right;
        HostControlMouseState out = translateMouseState(state);
        MDR_CHECK(!out.leftDown);
        MDR_CHECK(out.rightDown);
    }
    {
        ControllerState state;
        state.mouseButtons = MouseButton_Left | MouseButton_Right;
        HostControlMouseState out = translateMouseState(state);
        MDR_CHECK(out.leftDown);
        MDR_CHECK(out.rightDown);
    }
}

MDR_TEST(translate_mouse_ignores_gamepad_and_touch_fields) {
    // Symmetric with translate_ignores_touch_and_emulator_actions above:
    // gamepad/touch input must not leak into the mouse translation, and
    // vice versa (see translate_ignores_mouse_fields below).
    ControllerState state;
    state.dsButtons = DSButton_A | DSButton_Up;
    state.touchActive = 1;
    state.touchX = 100;
    state.touchY = 50;
    state.leftStickX = 12345;
    HostControlMouseState out = translateMouseState(state);
    MDR_CHECK_EQ(out.dx, static_cast<int16_t>(0));
    MDR_CHECK_EQ(out.dy, static_cast<int16_t>(0));
    MDR_CHECK(!out.leftDown);
    MDR_CHECK(!out.rightDown);
}

// Steam Controller touchpad experiment (see isTouchpadReady()'s header
// comment for the full honest caveat) -- the dead-reckoning/contact-
// heuristic logic lives inside emitTouchpadState(), which is private and
// has real uinput I/O, so unlike translateControllerState()/
// translateMouseState() there's no pure free function to unit-test
// directly. This is a documentation-as-test guard, same style as
// host_control_adapter_degrades_gracefully_without_uinput_access above:
// confirms the opt-in env var is read without crashing and that
// isTouchpadReady() correctly reports false in this sandbox (no
// /dev/uinput -- uinputFd_ stays -1 regardless of the env var).
MDR_TEST(host_control_adapter_touchpad_opt_in_degrades_gracefully_without_uinput_access) {
    ::setenv("DUALDECK_HOSTCONTROL_STEAM_TOUCHPAD", "1", 1);
    HostControlAdapter adapter;
    ::unsetenv("DUALDECK_HOSTCONTROL_STEAM_TOUCHPAD");
    MDR_CHECK(!adapter.isTouchpadReady());
    if (!adapter.isDeviceReady()) {
        ControllerState state;
        state.mouseDeltaX = 5;
        state.mouseDeltaY = -5;
        state.mouseButtons = MouseButton_Left;
        adapter.applyControllerState(state);
        adapter.releaseAll();
    }
}

// Real user request, 2026-08-03: "add an option to the client's host
// control to mirror the screen." Same degrade-gracefully expectation as
// the touchpad opt-in test above, applied to the screen-mirror
// experiment: getLatestFrame() must stay a safe false, not crash or
// throw, whether the feature is off entirely (the default) or on but
// unable to reach a usable X11 display (this sandbox/CI's actual
// situation either way -- headless, no X server).
MDR_TEST(host_control_adapter_mirror_disabled_by_default_returns_no_frame) {
    HostControlAdapter adapter;
    MDR_CHECK(!adapter.isMirrorReady());
    std::vector<uint8_t> frame;
    uint64_t frameIndex = 0;
    uint16_t width = 0, height = 0;
    MDR_CHECK(!adapter.getLatestFrame(frame, frameIndex, width, height));
    // frameDimensions() must still fall back to the DS default rather
    // than reporting some stale/uninitialized size when mirroring was
    // never enabled at all.
    uint16_t dimWidth = 0, dimHeight = 0;
    adapter.frameDimensions(dimWidth, dimHeight);
    MDR_CHECK_EQ(dimWidth, static_cast<uint16_t>(kFrameWidth));
    MDR_CHECK_EQ(dimHeight, static_cast<uint16_t>(kFrameHeight));
}

MDR_TEST(host_control_adapter_mirror_opt_in_degrades_gracefully_without_x11_display) {
    // Deterministic regardless of what this test happens to run under:
    // force the "no reachable X11 display" path explicitly rather than
    // relying on this sandbox/CI coincidentally having no DISPLAY set.
    const char* previousDisplay = std::getenv("DISPLAY");
    std::string savedDisplay = previousDisplay ? previousDisplay : "";
    bool hadDisplay = previousDisplay != nullptr;
    ::unsetenv("DISPLAY");

    ::setenv("DUALDECK_HOSTCONTROL_MIRROR_SCREEN", "1", 1);
    HostControlAdapter adapter;
    ::unsetenv("DUALDECK_HOSTCONTROL_MIRROR_SCREEN");

    if (hadDisplay) ::setenv("DISPLAY", savedDisplay.c_str(), 1);

    MDR_CHECK(!adapter.isMirrorReady());
    std::vector<uint8_t> frame;
    uint64_t frameIndex = 0;
    uint16_t width = 0, height = 0;
    MDR_CHECK(!adapter.getLatestFrame(frame, frameIndex, width, height));
}

MDR_TEST(translate_ignores_mouse_fields) {
    ControllerState state;
    state.mouseDeltaX = 999;
    state.mouseDeltaY = -999;
    state.mouseButtons = MouseButton_Left | MouseButton_Right;
    HostControlGamepadState out = translateControllerState(state);
    MDR_CHECK(!out.south);
    MDR_CHECK(!out.east);
    MDR_CHECK(!out.west);
    MDR_CHECK(!out.north);
    MDR_CHECK_EQ(out.leftStickX, static_cast<int16_t>(0));
    MDR_CHECK_EQ(out.leftStickY, static_cast<int16_t>(0));
}
