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
        adapter.applyControllerState(state);
        adapter.releaseAll();
        std::vector<uint8_t> frame;
        uint64_t frameIndex = 0;
        uint16_t width = 0, height = 0;
        MDR_CHECK(!adapter.getLatestFrame(frame, frameIndex, width, height));
    }
    // If this ever runs somewhere uinput *is* available (a real Linux
    // dev machine with the right permissions, not this sandbox/CI), the
    // same calls above still must not crash -- no separate assertion
    // needed since isDeviceReady() being true just means the "expected"
    // no-op path isn't exercised this run.
}
