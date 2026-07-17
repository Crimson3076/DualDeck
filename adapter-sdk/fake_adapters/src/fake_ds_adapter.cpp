#include "melonds_remote/adapter/fake/fake_ds_adapter.h"

namespace melonds_remote::adapter::fake {

namespace {
AdapterCapabilities buildCapabilities() {
    AdapterCapabilities caps;
    caps.system = melonds_remote::SystemIdentity{"nds", "Nintendo DS"};
    caps.adapter = melonds_remote::AdapterIdentity{"fake-ds", "Fake DS Adapter (test fixture)", "0.0.1"};

    VideoSurfaceDescriptor bottom;
    bottom.surfaceId = "bottom";
    bottom.role = SurfaceRole::Bottom;
    bottom.width = 256;
    bottom.height = 192;
    bottom.touchSupported = true;
    bottom.touchRangeX = 255;
    bottom.touchRangeY = 191;
    bottom.required = true;
    bottom.remotelyDisplayed = true;
    caps.surfaces = {bottom};

    caps.supportsMicrophone = true; // matches the real melonDS adapter (GitHub issue #2)
    return caps;
}
} // namespace

FakeDsAdapter::FakeDsAdapter() : FakeAdapterBase(buildCapabilities()) {}

} // namespace melonds_remote::adapter::fake
