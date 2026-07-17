#include "melonds_remote/adapter/fake/fake_3ds_adapter.h"
#include "test_framework.h"

using namespace melonds_remote::adapter;
using namespace melonds_remote::adapter::fake;

MDR_TEST(fake_3ds_adapter_reports_3ds_identity_and_two_surfaces) {
    FakeThreeDsAdapter ds3;
    AdapterCapabilities caps = ds3.capabilities();
    MDR_CHECK(caps.system.systemId == "3ds");
    MDR_CHECK(caps.system.systemName == "Nintendo 3DS");
    MDR_CHECK_EQ(caps.surfaces.size(), 2u);
}

MDR_TEST(fake_3ds_adapter_top_surface_has_no_touch_bottom_has_touch) {
    FakeThreeDsAdapter ds3;
    AdapterCapabilities caps = ds3.capabilities();

    const VideoSurfaceDescriptor* top = nullptr;
    const VideoSurfaceDescriptor* bottom = nullptr;
    for (const auto& s : caps.surfaces) {
        if (s.surfaceId == "top") top = &s;
        if (s.surfaceId == "bottom") bottom = &s;
    }
    MDR_CHECK(top != nullptr);
    MDR_CHECK(bottom != nullptr);
    MDR_CHECK(top->role == SurfaceRole::Top);
    MDR_CHECK(!top->touchSupported);
    MDR_CHECK(bottom->role == SurfaceRole::Bottom);
    MDR_CHECK(bottom->touchSupported);
    // Different dimensions per surface (GitHub issue #28's required
    // test: "Multiple video surfaces with different dimensions").
    MDR_CHECK(top->width != bottom->width || top->height != bottom->height);
}

MDR_TEST(fake_3ds_adapter_per_surface_latest_frame_wins_independently) {
    FakeThreeDsAdapter ds3;
    ds3.pushFrame("top", {1, 1, 1});
    ds3.pushFrame("bottom", {2, 2, 2});
    ds3.pushFrame("top", {9, 9, 9}); // second top frame, bottom untouched since

    SurfaceFrame topFrame, bottomFrame;
    MDR_CHECK(ds3.latestFrame("top", topFrame));
    MDR_CHECK(ds3.latestFrame("bottom", bottomFrame));

    // Each surface's own latest-frame-wins is independent of the other's
    // -- pushing to "top" twice must not disturb "bottom"'s single
    // pushed frame or its frame index.
    MDR_CHECK_EQ(topFrame.frameIndex, 1u);
    MDR_CHECK(topFrame.pixels == std::vector<uint8_t>({9, 9, 9}));
    MDR_CHECK_EQ(bottomFrame.frameIndex, 0u);
    MDR_CHECK(bottomFrame.pixels == std::vector<uint8_t>({2, 2, 2}));
}
