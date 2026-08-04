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

#include <sys/stat.h>
#include <unistd.h>

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

// DS B -> south, DS A -> east. This target is a real uinput virtual
// gamepad, so the output names are physical positions, and the DS puts B
// at the bottom and A on the right -- mirrored from the Xbox arrangement
// those position names describe. Updated 2026-08-04 alongside the
// client-side positional fix: the previous expectation here (A -> south)
// encoded a label-for-label copy, which put "confirm" on the wrong
// physical button for desktop navigation.
MDR_TEST(translate_a_and_b_map_by_physical_position_not_by_letter) {
    ControllerState state;
    state.dsButtons = DSButton_B;
    HostControlGamepadState out = translateControllerState(state);
    MDR_CHECK(out.south); // bottom button -> bottom button
    MDR_CHECK(!out.east);

    state.dsButtons = DSButton_A;
    out = translateControllerState(state);
    MDR_CHECK(out.east); // DS A sits on the right
    MDR_CHECK(!out.south);
}

// DS X -> north, DS Y -> west, for the same reason: DS X is the top
// button and DS Y the left one.
//
// Deliberately NOT the same table as host::AdapterBridge's
// dsButtonsToGenericButtons(), which keeps A -> South. That asymmetry is
// correct rather than an oversight: the GenericButton_* values it emits
// are consumed by emulator adapters that map straight back (South -> A,
// see the Azahar patch's button table), so that path is a label
// round-trip and stays right. This path terminates at a virtual gamepad
// where only position carries meaning, so it must convert rather than
// round-trip.
MDR_TEST(translate_x_and_y_map_by_physical_position_not_by_letter) {
    ControllerState state;
    state.dsButtons = DSButton_X;
    HostControlGamepadState out = translateControllerState(state);
    MDR_CHECK(out.north); // DS X is the top button
    MDR_CHECK(!out.west);

    state.dsButtons = DSButton_Y;
    out = translateControllerState(state);
    MDR_CHECK(out.west); // DS Y is the left button
    MDR_CHECK(!out.north);
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

// X passes through unscaled (both sides agree positive = right), but Y is
// re-negated -- see translateControllerState()'s own comment: the wire's
// leftStickY already has client/src/main.cpp's 3DS-circle-pad-convention
// flip applied (positive = up), which must be undone here since this
// feeds a uinput ABS_Y axis, where standard evdev/joystick convention is
// positive = down. Real user report, 2026-08-03: "L joystick reads down
// when going up" before this fix.
MDR_TEST(translate_left_stick_x_passes_through_y_is_renegated) {
    ControllerState state;
    state.leftStickX = 12345;
    state.leftStickY = -6789;
    HostControlGamepadState out = translateControllerState(state);
    MDR_CHECK_EQ(out.leftStickX, static_cast<int16_t>(12345));
    MDR_CHECK_EQ(out.leftStickY, static_cast<int16_t>(6789));
}

MDR_TEST(translate_left_stick_y_negation_clamps_at_int16_min) {
    // INT16_MIN negated overflows int16_t's range -- must clamp to
    // INT16_MAX rather than wrap, matching client/src/main.cpp's own
    // negateStickAxis() behavior at this same edge case.
    ControllerState state;
    state.leftStickY = INT16_MIN;
    HostControlGamepadState out = translateControllerState(state);
    MDR_CHECK_EQ(out.leftStickY, INT16_MAX);
}

// Same X-passthrough/Y-renegation rule as the left stick above applies to
// the right stick.
MDR_TEST(translate_right_stick_x_passes_through_y_is_renegated) {
    ControllerState state;
    state.rightStickX = -4321;
    state.rightStickY = 8765;
    HostControlGamepadState out = translateControllerState(state);
    MDR_CHECK_EQ(out.rightStickX, static_cast<int16_t>(-4321));
    MDR_CHECK_EQ(out.rightStickY, static_cast<int16_t>(-8765));
}

// Real user report (Steam Controller Tester), 2026-08-03: "Triggers do
// not work. same with stick clicking" -- protocol v12 added
// leftTrigger/rightTrigger/extraButtons specifically so Host
// Control mode's virtual gamepad could report these.
MDR_TEST(translate_triggers_pass_through_unscaled) {
    ControllerState state;
    state.leftTrigger = 128;
    state.rightTrigger = 255;
    HostControlGamepadState out = translateControllerState(state);
    MDR_CHECK_EQ(out.leftTrigger, static_cast<uint8_t>(128));
    MDR_CHECK_EQ(out.rightTrigger, static_cast<uint8_t>(255));
}

MDR_TEST(translate_thumb_clicks_from_host_control_buttons) {
    ControllerState state;
    state.extraButtons = ExtraButton_ThumbLeft;
    HostControlGamepadState out = translateControllerState(state);
    MDR_CHECK(out.thumbL);
    MDR_CHECK(!out.thumbR);

    state.extraButtons = ExtraButton_ThumbRight;
    out = translateControllerState(state);
    MDR_CHECK(!out.thumbL);
    MDR_CHECK(out.thumbR);
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

// Small RAII helper: saves an env var, clears it, restores it on scope
// exit (even if a CHECK fails partway through) -- used below to force
// both the X11 and the Wayland-fallback capture paths into their
// "nothing reachable" branch deterministically, regardless of what
// desktop session (if any) actually runs this test suite. This matters
// more here than it did for the X11-only test it's replacing:
// WaylandScreenCapture's constructor, on a real desktop machine with a
// real session bus AND a real xdg-desktop-portal ScreenCast backend,
// would otherwise pop a genuine, real, interactive system permission
// dialog (see that class's own header comment) just from running this
// unit test -- clearing DBUS_SESSION_BUS_ADDRESS (and DISPLAY, so
// dbus_bus_get can't fall back to X11-based auto-launch either) makes
// dbus_bus_get_private() fail fast and silently instead, exactly the
// "no session bus reachable" degrade-gracefully path this test means to
// exercise.
struct ScopedUnsetEnv {
    std::string name;
    std::string savedValue;
    bool hadValue = false;

    explicit ScopedUnsetEnv(std::string envName) : name(std::move(envName)) {
        const char* previous = std::getenv(name.c_str());
        hadValue = previous != nullptr;
        if (hadValue) savedValue = previous;
        ::unsetenv(name.c_str());
    }
    ~ScopedUnsetEnv() {
        if (hadValue) ::setenv(name.c_str(), savedValue.c_str(), 1);
    }
};

// Same idea, opposite direction -- redirects XDG_RUNTIME_DIR to a
// directory this process just created and knows to be empty, so
// libdbus's systemd-style "$XDG_RUNTIME_DIR/bus" autodetection (which
// runs independently of DBUS_SESSION_BUS_ADDRESS -- a real fallback
// path on any systemd-based desktop, i.e. plausibly the very machine
// running this test suite) can't stumble onto a genuine session bus
// either.
struct ScopedEmptyXdgRuntimeDir {
    ScopedUnsetEnv inner{"XDG_RUNTIME_DIR"};
    std::string path;

    ScopedEmptyXdgRuntimeDir() {
        path = "/tmp/dualdeck_test_empty_xdg_runtime_dir_" + std::to_string(::getpid());
        ::mkdir(path.c_str(), 0700);
        ::setenv("XDG_RUNTIME_DIR", path.c_str(), 1);
    }
    ~ScopedEmptyXdgRuntimeDir() { ::rmdir(path.c_str()); }
};

MDR_TEST(host_control_adapter_mirror_opt_in_degrades_gracefully_without_any_backend) {
    ScopedUnsetEnv noDisplay("DISPLAY");
    ScopedUnsetEnv noDbus("DBUS_SESSION_BUS_ADDRESS");
    ScopedEmptyXdgRuntimeDir noXdgRuntimeBus;

    ::setenv("DUALDECK_HOSTCONTROL_MIRROR_SCREEN", "1", 1);
    HostControlAdapter adapter;
    ::unsetenv("DUALDECK_HOSTCONTROL_MIRROR_SCREEN");

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

// Real user report, 2026-08-03: a 4096x2160 host desktop mirrored to a
// Steam Deck-sized display was consistently sluggish -- the full native
// resolution was being captured/JPEG-compressed/transmitted/decoded
// every frame for no visual benefit. fitDownscaleTarget()/
// downscaleBgra8888() are the fix; both are pure math/pixel logic, no
// I/O, so directly testable without a real capture backend (matching
// translateControllerState()'s own testing rationale above).

MDR_TEST(fit_downscale_target_matches_the_reported_scenario) {
    // 4096x2160 (the exact resolution from the report) fit within
    // 1280x800 (Steam Deck's native LCD, this project's fallback
    // target): width is the binding constraint (4096/1280 = 3.2 >
    // 2160/800 = 2.7), so height comes out to 2160/3.2 = 675.
    int outWidth = 0, outHeight = 0;
    fitDownscaleTarget(4096, 2160, 1280, 800, outWidth, outHeight);
    MDR_CHECK_EQ(outWidth, 1280);
    MDR_CHECK_EQ(outHeight, 675);
}

MDR_TEST(fit_downscale_target_never_upscales) {
    // A source already smaller than the target (e.g. a 1280x800 desktop
    // mirrored to a 1280x800 client) must come back unchanged -- there's
    // no benefit to upscaling before JPEG compression, since the
    // client's own render path already scales-to-fit whatever size
    // frame actually arrives.
    int outWidth = 0, outHeight = 0;
    fitDownscaleTarget(640, 480, 1280, 800, outWidth, outHeight);
    MDR_CHECK_EQ(outWidth, 640);
    MDR_CHECK_EQ(outHeight, 480);
}

MDR_TEST(fit_downscale_target_exact_fit_is_unchanged) {
    int outWidth = 0, outHeight = 0;
    fitDownscaleTarget(1280, 800, 1280, 800, outWidth, outHeight);
    MDR_CHECK_EQ(outWidth, 1280);
    MDR_CHECK_EQ(outHeight, 800);
}

MDR_TEST(downscale_bgra_averages_a_solid_quadrant_correctly) {
    // A 4x4 frame split into four solid-color 2x2 quadrants, downscaled
    // to 2x2 -- each output pixel should be exactly the pure color of
    // the one quadrant it maps to (box-filter over a uniform region is
    // just that region's own color, an easy exactness check for the
    // averaging logic itself).
    std::vector<uint8_t> src(4 * 4 * 4, 0);
    auto setPixel = [&](int x, int y, uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
        uint8_t* p = src.data() + (static_cast<size_t>(y) * 4 + x) * 4;
        p[0] = b;
        p[1] = g;
        p[2] = r;
        p[3] = a;
    };
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x) setPixel(x, y, 10, 20, 30, 255);       // top-left
    for (int y = 0; y < 2; ++y)
        for (int x = 2; x < 4; ++x) setPixel(x, y, 40, 50, 60, 255);      // top-right
    for (int y = 2; y < 4; ++y)
        for (int x = 0; x < 2; ++x) setPixel(x, y, 70, 80, 90, 255);      // bottom-left
    for (int y = 2; y < 4; ++y)
        for (int x = 2; x < 4; ++x) setPixel(x, y, 100, 110, 120, 255);   // bottom-right

    std::vector<uint8_t> dst;
    downscaleBgra8888(src.data(), 4, 4, dst, 2, 2);
    MDR_CHECK_EQ(dst.size(), static_cast<size_t>(2 * 2 * 4));

    auto getPixel = [&](int x, int y) { return dst.data() + (static_cast<size_t>(y) * 2 + x) * 4; };
    const uint8_t* topLeft = getPixel(0, 0);
    MDR_CHECK_EQ(topLeft[0], static_cast<uint8_t>(10));
    MDR_CHECK_EQ(topLeft[1], static_cast<uint8_t>(20));
    MDR_CHECK_EQ(topLeft[2], static_cast<uint8_t>(30));
    const uint8_t* bottomRight = getPixel(1, 1);
    MDR_CHECK_EQ(bottomRight[0], static_cast<uint8_t>(100));
    MDR_CHECK_EQ(bottomRight[1], static_cast<uint8_t>(110));
    MDR_CHECK_EQ(bottomRight[2], static_cast<uint8_t>(120));
}

MDR_TEST(downscale_bgra_to_one_pixel_is_the_true_average) {
    // 1x2 source (two distinct pixels) downscaled to 1x1 must produce
    // the exact arithmetic mean of the two -- catches an off-by-one in
    // the box filter's source-range computation that a uniform-color
    // test (like the one above) can't, since averaging identical values
    // trivially reproduces them regardless of exactly which pixels were
    // summed.
    std::vector<uint8_t> src = {
        10, 20, 30, 255, // row 0
        50, 60, 70, 255, // row 1
    };
    std::vector<uint8_t> dst;
    downscaleBgra8888(src.data(), 1, 2, dst, 1, 1);
    MDR_CHECK_EQ(dst.size(), static_cast<size_t>(4));
    MDR_CHECK_EQ(dst[0], static_cast<uint8_t>(30)); // (10+50)/2
    MDR_CHECK_EQ(dst[1], static_cast<uint8_t>(40)); // (20+60)/2
    MDR_CHECK_EQ(dst[2], static_cast<uint8_t>(50)); // (30+70)/2
    MDR_CHECK_EQ(dst[3], static_cast<uint8_t>(255));
}
