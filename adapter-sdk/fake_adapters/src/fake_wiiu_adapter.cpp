#include "melonds_remote/adapter/fake/fake_wiiu_adapter.h"

namespace melonds_remote::adapter::fake {

namespace {
AdapterCapabilities buildCapabilities() {
    AdapterCapabilities caps;
    caps.system = melonds_remote::SystemIdentity{"wiiu", "Nintendo Wii U"};
    caps.adapter =
        melonds_remote::AdapterIdentity{"fake-wiiu", "Fake Wii U Adapter (test fixture)", "0.0.1"};

    // 1080p TV output; real Wii U GamePad resolution (854x480).
    VideoSurfaceDescriptor tv;
    tv.surfaceId = "tv";
    tv.role = SurfaceRole::Tv;
    tv.width = 1920;
    tv.height = 1080;
    tv.touchSupported = false;
    tv.required = true;
    tv.locallyDisplayed = true; // stays on the host TV, per issue #28's Wii U layout mode

    VideoSurfaceDescriptor gamePad;
    gamePad.surfaceId = "gamepad";
    gamePad.role = SurfaceRole::GamePad;
    gamePad.width = 854;
    gamePad.height = 480;
    gamePad.touchSupported = true;
    gamePad.touchRangeX = 853;
    gamePad.touchRangeY = 479;
    gamePad.required = true;
    gamePad.remotelyDisplayed = true; // streamed to the DualDeck client

    caps.surfaces = {tv, gamePad};
    // The Wii U GamePad's L/R/ZL/ZR are all digital, not analog (unlike
    // e.g. a DualShock/Xbox trigger) -- GenericButton_L2/R2's digital
    // click covers this, supportsAnalogTriggers stays false.
    return caps;
}
} // namespace

FakeWiiUAdapter::FakeWiiUAdapter() : FakeAdapterBase(buildCapabilities()) {}

} // namespace melonds_remote::adapter::fake
