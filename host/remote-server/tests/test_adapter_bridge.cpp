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

namespace {

// Reproduces the exact shape of the real, shipped bug found via
// AzaharAdapter's capture-loop diagnostics (100% successful captures on
// the Azahar side, yet zero frames ever reaching NetServer for the
// entire session): in --adapter-ipc mode (GitHub issue #4), AdapterBridge
// is constructed at process startup, long before any real adapter has
// connected over the IPC channel -- capabilities() has an empty surfaces
// list at that point, and only gets populated later once a real adapter
// (e.g. Azahar) actually connects. FakeDsAdapter/FakeThreeDsAdapter both
// have real capabilities from construction, so they couldn't have caught
// this -- this fake starts empty and only gets populated via connectNow().
class LateConnectingAdapter : public IEmulatorAdapter {
public:
    AdapterCapabilities capabilities() const override { return caps_; }
    SessionState currentState() const override { return SessionState::Available; }
    void applyGenericInput(const GenericInputState& state) override { lastInput_ = state; }
    void releaseAllInputs() override { released_ = true; }
    bool latestFrame(const std::string& surfaceId, SurfaceFrame& outFrame) override {
        if (surfaceId != frame_.surfaceId || !hasFrame_) return false;
        outFrame = frame_;
        return true;
    }

    void connectNow() {
        VideoSurfaceDescriptor bottom;
        bottom.surfaceId = "bottom";
        bottom.width = 320;
        bottom.height = 240;
        bottom.remotelyDisplayed = true;
        caps_.surfaces = {bottom};
    }

    void pushFrame(std::string surfaceId, std::vector<uint8_t> pixels) {
        frame_.surfaceId = std::move(surfaceId);
        frame_.pixels = std::move(pixels);
        frame_.frameIndex = hasFrame_ ? frame_.frameIndex + 1 : 0;
        hasFrame_ = true;
    }

    const GenericInputState& lastInput() const { return lastInput_; }
    bool isReleased() const { return released_; }

private:
    AdapterCapabilities caps_;
    SurfaceFrame frame_;
    bool hasFrame_ = false;
    GenericInputState lastInput_;
    bool released_ = false;
};

} // namespace

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
    uint16_t outWidth = 0, outHeight = 0;
    MDR_CHECK(!bridge.getLatestFrame(outFrame, outIndex, outWidth, outHeight));
}

MDR_TEST(adapter_bridge_get_latest_frame_proxies_pushed_frame) {
    FakeDsAdapter ds;
    AdapterBridge bridge(ds);

    ds.pushFrame("bottom", {1, 2, 3, 4});

    std::vector<uint8_t> outFrame;
    uint64_t outIndex = 999;
    uint16_t outWidth = 0, outHeight = 0;
    MDR_CHECK(bridge.getLatestFrame(outFrame, outIndex, outWidth, outHeight));
    MDR_CHECK(outFrame == std::vector<uint8_t>({1, 2, 3, 4}));
    MDR_CHECK_EQ(outIndex, 0u);

    ds.pushFrame("bottom", {5, 6, 7, 8});
    MDR_CHECK(bridge.getLatestFrame(outFrame, outIndex, outWidth, outHeight));
    MDR_CHECK(outFrame == std::vector<uint8_t>({5, 6, 7, 8}));
    MDR_CHECK_EQ(outIndex, 1u);
}

// Real, shipped bug this covers (see SurfaceFrame's comment in
// adapter_contract.h and AdapterBridge::getLatestFrame()'s comment):
// frameDimensions() is only a coarse value negotiated once per
// connection, sampled before an adapter like CemuAdapter necessarily
// knows its own title's real capture resolution -- getLatestFrame() must
// report THIS frame's own actual width/height instead, straight off
// SurfaceFrame, not whatever was declared once via capabilities().
MDR_TEST(adapter_bridge_get_latest_frame_reports_this_frames_real_size) {
    FakeDsAdapter ds; // declares 256x192 via capabilities()
    AdapterBridge bridge(ds);

    ds.pushFrame("bottom", {1, 2, 3, 4}, 320, 240); // real captured size differs from the declared default
    std::vector<uint8_t> outFrame;
    uint64_t outIndex = 0;
    uint16_t outWidth = 0, outHeight = 0;
    MDR_CHECK(bridge.getLatestFrame(outFrame, outIndex, outWidth, outHeight));
    MDR_CHECK_EQ(outWidth, 320);
    MDR_CHECK_EQ(outHeight, 240);

    // An adapter that never sets SurfaceFrame::width/height (0x0, e.g. one
    // not yet updated for contract v2) falls back to the declared value
    // instead of handing net_server.cpp a 0x0 frame it can't encode.
    ds.pushFrame("bottom", {5, 6, 7, 8});
    MDR_CHECK(bridge.getLatestFrame(outFrame, outIndex, outWidth, outHeight));
    MDR_CHECK_EQ(outWidth, 256);
    MDR_CHECK_EQ(outHeight, 192);
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

// Reproduces the real, shipped bug: AdapterBridge constructed while the
// wrapped adapter has zero surfaces (capabilities().surfaces empty --
// exactly --adapter-ipc mode's shape at process startup, before any
// real adapter has connected), then the adapter "connects" (surfaces
// become non-empty) sometime after. getLatestFrame() must succeed once
// connected, not stay permanently broken because targetSurfaceId_ was
// cached as "" at construction time.
MDR_TEST(adapter_bridge_resolves_surface_after_late_adapter_connection) {
    LateConnectingAdapter adapter;
    AdapterBridge bridge(adapter); // constructed BEFORE connectNow()

    std::vector<uint8_t> outFrame;
    uint64_t outIndex = 0;
    uint16_t outWidth = 0, outHeight = 0;
    MDR_CHECK(!bridge.getLatestFrame(outFrame, outIndex, outWidth, outHeight)); // nothing yet, correctly

    adapter.connectNow();
    adapter.pushFrame("bottom", {10, 20, 30, 40});

    MDR_CHECK(bridge.getLatestFrame(outFrame, outIndex, outWidth, outHeight));
    MDR_CHECK(outFrame == std::vector<uint8_t>({10, 20, 30, 40}));
    MDR_CHECK(bridge.targetSurfaceId() == "bottom");
}

MDR_TEST(adapter_bridge_frame_dimensions_after_late_adapter_connection) {
    LateConnectingAdapter adapter;
    AdapterBridge bridge(adapter);

    uint16_t width = 0, height = 0;
    bridge.frameDimensions(width, height);
    MDR_CHECK_EQ(width, 256); // no surfaces yet -- falls back to DS default
    MDR_CHECK_EQ(height, 192);

    adapter.connectNow();
    bridge.frameDimensions(width, height);
    MDR_CHECK_EQ(width, 320); // now reflects the late-connected adapter's real surface
    MDR_CHECK_EQ(height, 240);
}

MDR_TEST(adapter_bridge_touch_uses_live_surface_id_after_late_connection) {
    LateConnectingAdapter adapter;
    AdapterBridge bridge(adapter);

    ControllerState state{};
    state.touchActive = true;
    state.touchX = 100;
    state.touchY = 50;
    adapter.connectNow();
    bridge.applyControllerState(state);

    MDR_CHECK_EQ(adapter.lastInput().touches.size(), static_cast<size_t>(1));
    MDR_CHECK(adapter.lastInput().touches[0].surfaceId == "bottom");
}
