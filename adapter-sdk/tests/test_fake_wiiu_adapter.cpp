#include "melonds_remote/adapter/fake/fake_wiiu_adapter.h"
#include "test_framework.h"

using namespace melonds_remote::adapter;
using namespace melonds_remote::adapter::fake;

MDR_TEST(fake_wiiu_adapter_reports_wiiu_identity_and_tv_gamepad_surfaces) {
    FakeWiiUAdapter wiiu;
    AdapterCapabilities caps = wiiu.capabilities();
    MDR_CHECK(caps.system.systemId == "wiiu");
    MDR_CHECK(caps.system.systemName == "Nintendo Wii U");
    MDR_CHECK_EQ(caps.surfaces.size(), 2u);

    const VideoSurfaceDescriptor* tv = nullptr;
    const VideoSurfaceDescriptor* gamePad = nullptr;
    for (const auto& s : caps.surfaces) {
        if (s.surfaceId == "tv") tv = &s;
        if (s.surfaceId == "gamepad") gamePad = &s;
    }
    MDR_CHECK(tv != nullptr);
    MDR_CHECK(gamePad != nullptr);
    MDR_CHECK(tv->role == SurfaceRole::Tv);
    MDR_CHECK(gamePad->role == SurfaceRole::GamePad);
    MDR_CHECK(!tv->touchSupported);
    MDR_CHECK(gamePad->touchSupported);
    // TV is dramatically larger than the GamePad screen -- a distinct
    // shape from DS/3DS's much closer top/bottom sizes, still described
    // by the same VideoSurfaceDescriptor fields with no special-casing.
    MDR_CHECK(tv->width > gamePad->width);
    MDR_CHECK(tv->height > gamePad->height);
}

MDR_TEST(fake_wiiu_adapter_gamepad_role_distinct_from_ds_3ds_bottom_role) {
    // Confirms Wii U's touch-capable remote surface uses SurfaceRole::GamePad,
    // not SurfaceRole::Bottom -- client-side layout code (GitHub issue
    // #28: "Select a default controller profile per emulated system")
    // needs this distinction to label things correctly, even though both
    // roles happen to be "the touch surface streamed to the client."
    FakeWiiUAdapter wiiu;
    AdapterCapabilities caps = wiiu.capabilities();
    for (const auto& s : caps.surfaces) {
        if (s.touchSupported) {
            MDR_CHECK(s.role == SurfaceRole::GamePad);
            MDR_CHECK(s.role != SurfaceRole::Bottom);
        }
    }
}
