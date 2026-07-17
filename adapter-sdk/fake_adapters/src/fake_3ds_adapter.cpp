#include "melonds_remote/adapter/fake/fake_3ds_adapter.h"

namespace melonds_remote::adapter::fake {

namespace {
AdapterCapabilities buildCapabilities() {
    AdapterCapabilities caps;
    caps.system = melonds_remote::SystemIdentity{"3ds", "Nintendo 3DS"};
    caps.adapter =
        melonds_remote::AdapterIdentity{"fake-3ds", "Fake 3DS Adapter (test fixture)", "0.0.1"};

    // Real 3DS top-screen resolution in non-stereoscopic mode (issue
    // #28: "Initial scope does not require stereoscopic 3D"); real
    // 3DS bottom-screen resolution.
    VideoSurfaceDescriptor top;
    top.surfaceId = "top";
    top.role = SurfaceRole::Top;
    top.width = 400;
    top.height = 240;
    top.touchSupported = false;
    top.required = true;
    top.locallyDisplayed = true; // host TV/monitor, per issue #28's DS/3DS layout mode

    VideoSurfaceDescriptor bottom;
    bottom.surfaceId = "bottom";
    bottom.role = SurfaceRole::Bottom;
    bottom.width = 320;
    bottom.height = 240;
    bottom.touchSupported = true;
    bottom.touchRangeX = 319;
    bottom.touchRangeY = 239;
    bottom.required = true;
    bottom.remotelyDisplayed = true; // DualDeck client, mirroring the DS bottom-screen model

    caps.surfaces = {top, bottom};
    return caps;
}
} // namespace

FakeThreeDsAdapter::FakeThreeDsAdapter() : FakeAdapterBase(buildCapabilities()) {}

} // namespace melonds_remote::adapter::fake
