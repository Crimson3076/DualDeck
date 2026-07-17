#include "melonds_remote/adapter/fake/fake_ds_adapter.h"
#include "test_framework.h"

using namespace melonds_remote;
using namespace melonds_remote::adapter;
using namespace melonds_remote::adapter::fake;

MDR_TEST(fake_ds_adapter_reports_nds_identity_and_one_surface) {
    FakeDsAdapter ds;
    AdapterCapabilities caps = ds.capabilities();
    MDR_CHECK(caps.system.systemId == "nds");
    MDR_CHECK(caps.system.systemName == "Nintendo DS");
    MDR_CHECK(caps.adapter.adapterId == "fake-ds");
    MDR_CHECK_EQ(caps.surfaces.size(), 1u);
    MDR_CHECK(caps.surfaces[0].surfaceId == "bottom");
    MDR_CHECK_EQ(caps.surfaces[0].width, 256);
    MDR_CHECK_EQ(caps.surfaces[0].height, 192);
    MDR_CHECK(caps.surfaces[0].touchSupported);
    // Matches protocol.h's kTouchMaxX/kTouchMaxY exactly -- the whole
    // point of this fixture is proving the generic contract can
    // describe the DS's real existing shape, not an approximation of it.
    MDR_CHECK_EQ(caps.surfaces[0].touchRangeX, kTouchMaxX);
    MDR_CHECK_EQ(caps.surfaces[0].touchRangeY, kTouchMaxY);
}

MDR_TEST(fake_ds_adapter_starts_available_and_released) {
    FakeDsAdapter ds;
    MDR_CHECK(ds.currentState() == SessionState::Available);
    MDR_CHECK(ds.isReleased());
}

MDR_TEST(fake_ds_adapter_apply_input_then_release) {
    FakeDsAdapter ds;
    GenericInputState state;
    state.buttons = GenericButton_South | GenericButton_DpadUp;
    ds.applyGenericInput(state);
    MDR_CHECK(!ds.isReleased());
    MDR_CHECK_EQ(ds.lastAppliedInput().buttons, state.buttons);

    ds.releaseAllInputs();
    MDR_CHECK(ds.isReleased());
    MDR_CHECK_EQ(ds.lastAppliedInput().buttons, 0u);
}

MDR_TEST(fake_ds_adapter_latest_frame_wins) {
    FakeDsAdapter ds;
    SurfaceFrame outFrame;
    MDR_CHECK(!ds.latestFrame("bottom", outFrame)); // nothing pushed yet

    ds.pushFrame("bottom", {1, 2, 3});
    MDR_CHECK(ds.latestFrame("bottom", outFrame));
    MDR_CHECK_EQ(outFrame.frameIndex, 0u);
    MDR_CHECK(outFrame.pixels == std::vector<uint8_t>({1, 2, 3}));

    ds.pushFrame("bottom", {4, 5, 6});
    MDR_CHECK(ds.latestFrame("bottom", outFrame));
    MDR_CHECK_EQ(outFrame.frameIndex, 1u); // incremented, not reset
    MDR_CHECK(outFrame.pixels == std::vector<uint8_t>({4, 5, 6})); // latest wins, not queued
}

MDR_TEST(fake_ds_adapter_lifecycle_full_cycle) {
    FakeDsAdapter ds;
    MDR_CHECK(ds.setState(SessionState::Starting));
    MDR_CHECK(ds.setState(SessionState::Running));
    MDR_CHECK(ds.setState(SessionState::Paused));
    MDR_CHECK(ds.setState(SessionState::Running));
    MDR_CHECK(ds.setState(SessionState::Stopped));
    MDR_CHECK(ds.currentState() == SessionState::Stopped);
}

MDR_TEST(fake_ds_adapter_rejects_invalid_transition_and_keeps_prior_state) {
    FakeDsAdapter ds;
    MDR_CHECK(!ds.setState(SessionState::Running)); // Available -> Running skips Starting
    MDR_CHECK(ds.currentState() == SessionState::Available); // unchanged
}
