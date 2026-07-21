// Unit tests for AdapterBridge's translation logic (GitHub issue #28
// Phase 2's "DS compatibility adapter"). Uses the fake DS/3DS fixtures
// from adapter-sdk/fake_adapters/ as the wrapped IEmulatorAdapter,
// since they already expose the test-only observation hooks
// (lastAppliedInput(), isReleased(), pushFrame()) needed to verify what
// AdapterBridge actually forwarded, without needing a real socket.

#include "host/adapter_bridge.h"
#include "melonds_remote/adapter/fake/fake_3ds_adapter.h"
#include "melonds_remote/adapter/fake/fake_ds_adapter.h"
#include "test_framework.h"

using namespace melonds_remote;
using namespace melonds_remote::host;
using namespace melonds_remote::adapter;
using namespace melonds_remote::adapter::fake;

MDR_TEST(adapter_bridge_picks_the_remotely_displayed_surface) {
    // FakeDsAdapter has exactly one surface ("bottom"), remotely
    // displayed -- the unambiguous case.
    FakeDsAdapter ds;
    AdapterBridge bridge(ds);
    MDR_CHECK(bridge.targetSurfaceId() == "bottom");
}

MDR_TEST(adapter_bridge_prefers_remotely_displayed_over_first_declared) {
    // FakeThreeDsAdapter declares "top" (locallyDisplayed) before
    // "bottom" (remotelyDisplayed) -- AdapterBridge must pick "bottom"
    // by its remotelyDisplayed flag, not just take the first surface in
    // the list.
    FakeThreeDsAdapter ds3;
    AdapterBridge bridge(ds3);
    MDR_CHECK(bridge.targetSurfaceId() == "bottom");
}

MDR_TEST(adapter_bridge_translates_every_ds_button) {
    struct Case {
        uint16_t dsBit;
        uint32_t genericBit;
    };
    const Case cases[] = {
        {DSButton_A, GenericButton_South},     {DSButton_B, GenericButton_East},
        {DSButton_X, GenericButton_West},      {DSButton_Y, GenericButton_North},
        {DSButton_Up, GenericButton_DpadUp},   {DSButton_Down, GenericButton_DpadDown},
        {DSButton_Left, GenericButton_DpadLeft}, {DSButton_Right, GenericButton_DpadRight},
        {DSButton_L, GenericButton_L1},        {DSButton_R, GenericButton_R1},
        {DSButton_Start, GenericButton_Start}, {DSButton_Select, GenericButton_Select},
    };

    for (const auto& c : cases) {
        FakeDsAdapter ds;
        AdapterBridge bridge(ds);

        ControllerState state;
        state.dsButtons = c.dsBit;
        bridge.applyControllerState(state);

        MDR_CHECK_EQ(ds.lastAppliedInput().buttons, c.genericBit);
    }
}

MDR_TEST(adapter_bridge_translates_button_combination) {
    FakeDsAdapter ds;
    AdapterBridge bridge(ds);

    ControllerState state;
    state.dsButtons = DSButton_A | DSButton_Up | DSButton_L;
    bridge.applyControllerState(state);

    uint32_t expected = GenericButton_South | GenericButton_DpadUp | GenericButton_L1;
    MDR_CHECK_EQ(ds.lastAppliedInput().buttons, expected);
    // Bits reserved for concepts the DS has no equivalent of (L2/R2/L3/R3)
    // must never spuriously come out set.
    MDR_CHECK_EQ(ds.lastAppliedInput().buttons & GenericButton_L2, 0u);
}

MDR_TEST(adapter_bridge_translates_touch_active) {
    FakeDsAdapter ds;
    AdapterBridge bridge(ds);

    ControllerState state;
    state.touchActive = 1;
    state.touchX = 100;
    state.touchY = 50;
    bridge.applyControllerState(state);

    MDR_CHECK_EQ(ds.lastAppliedInput().touches.size(), 1u);
    MDR_CHECK(ds.lastAppliedInput().touches[0].surfaceId == "bottom");
    MDR_CHECK_EQ(ds.lastAppliedInput().touches[0].x, 100);
    MDR_CHECK_EQ(ds.lastAppliedInput().touches[0].y, 50);
}

MDR_TEST(adapter_bridge_touch_inactive_produces_no_touch_contact) {
    FakeDsAdapter ds;
    AdapterBridge bridge(ds);

    ControllerState state;
    state.touchActive = 0;
    state.touchX = 100; // must be ignored since touchActive is 0
    state.touchY = 50;
    bridge.applyControllerState(state);

    MDR_CHECK(ds.lastAppliedInput().touches.empty());
}

MDR_TEST(adapter_bridge_emulator_actions_pass_through_bit_identical) {
    FakeDsAdapter ds;
    AdapterBridge bridge(ds);

    ControllerState state;
    state.emulatorActions = EmulatorAction_PauseResume | EmulatorAction_QuitApplication;
    bridge.applyControllerState(state);

    MDR_CHECK_EQ(ds.lastAppliedInput().emulatorActions,
                 static_cast<uint32_t>(EmulatorAction_PauseResume | EmulatorAction_QuitApplication));
}

MDR_TEST(adapter_bridge_sequence_and_timestamp_pass_through) {
    FakeDsAdapter ds;
    AdapterBridge bridge(ds);

    ControllerState state;
    state.sequence = 12345;
    state.clientTimestampUs = 999999;
    bridge.applyControllerState(state);

    MDR_CHECK_EQ(ds.lastAppliedInput().sequence, 12345u);
    MDR_CHECK_EQ(ds.lastAppliedInput().clientTimestampUs, 999999u);
}

MDR_TEST(adapter_bridge_release_all_reaches_the_adapter) {
    FakeDsAdapter ds;
    AdapterBridge bridge(ds);

    ControllerState state;
    state.dsButtons = DSButton_A;
    bridge.applyControllerState(state);
    MDR_CHECK(!ds.isReleased());

    bridge.releaseAll();
    MDR_CHECK(ds.isReleased());
}

MDR_TEST(adapter_bridge_get_latest_frame_false_when_none_pushed) {
    FakeDsAdapter ds;
    AdapterBridge bridge(ds);

    std::vector<uint8_t> outFrame;
    uint64_t outIndex = 0;
    MDR_CHECK(!bridge.getLatestFrame(outFrame, outIndex));
}

MDR_TEST(adapter_bridge_get_latest_frame_proxies_pushed_frame) {
    FakeDsAdapter ds;
    AdapterBridge bridge(ds);

    ds.pushFrame("bottom", {1, 2, 3, 4});

    std::vector<uint8_t> outFrame;
    uint64_t outIndex = 999;
    MDR_CHECK(bridge.getLatestFrame(outFrame, outIndex));
    MDR_CHECK(outFrame == std::vector<uint8_t>({1, 2, 3, 4}));
    MDR_CHECK_EQ(outIndex, 0u);

    ds.pushFrame("bottom", {5, 6, 7, 8});
    MDR_CHECK(bridge.getLatestFrame(outFrame, outIndex));
    MDR_CHECK(outFrame == std::vector<uint8_t>({5, 6, 7, 8}));
    MDR_CHECK_EQ(outIndex, 1u);
}

// Real bug this covers: AzaharAdapter reports its bottom surface as
// 320x240, but AdapterBridge::frameDimensions() used to not exist at
// all -- net_server.cpp's HelloAck always claimed DS's fixed 256x192
// regardless of what was actually connected, so every 3DS video frame
// was silently rejected by the client's own size check. Never caught
// until a user ran Azahar on real hardware.
MDR_TEST(adapter_bridge_frame_dimensions_matches_ds_surface) {
    FakeDsAdapter ds;
    AdapterBridge bridge(ds);

    uint16_t width = 0, height = 0;
    bridge.frameDimensions(width, height);
    MDR_CHECK_EQ(width, 256);
    MDR_CHECK_EQ(height, 192);
}

MDR_TEST(adapter_bridge_frame_dimensions_matches_3ds_surface) {
    FakeThreeDsAdapter ds3;
    AdapterBridge bridge(ds3);

    uint16_t width = 0, height = 0;
    bridge.frameDimensions(width, height);
    MDR_CHECK_EQ(width, 320);
    MDR_CHECK_EQ(height, 240);
}
