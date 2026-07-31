// Unit tests for capability_bridge.h's pure translation functions --
// adapter::VideoSurfaceDescriptor/AdapterCapabilities to protocol.h's
// Wire* mirror types. Not yet exercised by anything end-to-end (neither
// type is on the wire yet, see protocol.h), so this is the only coverage
// for this translation logic today.

#include "host/capability_bridge.h"
#include "test_framework.h"

using namespace melonds_remote;
using namespace melonds_remote::host;
using namespace melonds_remote::adapter;

MDR_TEST(capability_bridge_translates_every_surface_role) {
    MDR_CHECK(toWireDisplayRole(SurfaceRole::Top) == WireDisplayRole::Top);
    MDR_CHECK(toWireDisplayRole(SurfaceRole::Bottom) == WireDisplayRole::Bottom);
    MDR_CHECK(toWireDisplayRole(SurfaceRole::Tv) == WireDisplayRole::Tv);
    MDR_CHECK(toWireDisplayRole(SurfaceRole::GamePad) == WireDisplayRole::GamePad);
    MDR_CHECK(toWireDisplayRole(SurfaceRole::Auxiliary) == WireDisplayRole::Auxiliary);
}

MDR_TEST(capability_bridge_translates_pixel_format) {
    MDR_CHECK(toWirePixelFormat(PixelFormat::Bgra8888) == WirePixelFormat::Bgra8888);
}

MDR_TEST(capability_bridge_translates_a_full_surface_descriptor) {
    VideoSurfaceDescriptor surface;
    surface.surfaceId = "gamepad";
    surface.role = SurfaceRole::GamePad;
    surface.width = 854;
    surface.height = 480;
    surface.pixelFormat = PixelFormat::Bgra8888;
    surface.touchSupported = true;
    surface.touchRangeX = 853;
    surface.touchRangeY = 479;

    WireDisplayDescriptor wire = toWireDisplayDescriptor(surface);
    MDR_CHECK(wire.surfaceId == "gamepad");
    MDR_CHECK(wire.role == WireDisplayRole::GamePad);
    MDR_CHECK(wire.width == 854);
    MDR_CHECK(wire.height == 480);
    MDR_CHECK(wire.pixelFormat == WirePixelFormat::Bgra8888);
    MDR_CHECK(wire.touchSupported);
    // Enhancement/negotiation fields not yet sourced from anything on the
    // adapter side -- always their "no enhancement, host picks" default.
    MDR_CHECK(wire.scalingStrategy == WireDisplayDescriptor::ScalingStrategy::None);
    MDR_CHECK(wire.requestedCodec == 0);
    MDR_CHECK(wire.requestedBitrateKbps == 0);
    MDR_CHECK(wire.targetRefreshHz == 0);
}

MDR_TEST(capability_bridge_translates_capabilities_with_multiple_surfaces) {
    // Mirrors Cemu's real two-surface shape (Tv + GamePad, see
    // host/cemu-patches/0001-remote-server-integration.patch) -- the
    // concrete case this project's own design docs point to as evidence
    // multi-surface data already exists per-adapter even though the wire
    // protocol only ever streams one of them today.
    AdapterCapabilities caps;
    VideoSurfaceDescriptor tv;
    tv.surfaceId = "tv";
    tv.role = SurfaceRole::Tv;
    tv.width = 1280;
    tv.height = 720;
    VideoSurfaceDescriptor gamepad;
    gamepad.surfaceId = "gamepad";
    gamepad.role = SurfaceRole::GamePad;
    gamepad.width = 854;
    gamepad.height = 480;
    caps.surfaces = {tv, gamepad};

    WireHostCapabilities wire = toWireHostCapabilities(caps);
    MDR_CHECK(wire.availableCodecs == WireCodec_Jpeg);
    MDR_CHECK(wire.displays.size() == 2);
    MDR_CHECK(wire.displays[0].surfaceId == "tv");
    MDR_CHECK(wire.displays[0].role == WireDisplayRole::Tv);
    MDR_CHECK(wire.displays[1].surfaceId == "gamepad");
    MDR_CHECK(wire.displays[1].role == WireDisplayRole::GamePad);
}

MDR_TEST(capability_bridge_translates_empty_capabilities) {
    AdapterCapabilities caps; // no adapter connected yet -- default-constructed, no surfaces
    WireHostCapabilities wire = toWireHostCapabilities(caps);
    MDR_CHECK(wire.displays.empty());
    MDR_CHECK(wire.availableCodecs == WireCodec_Jpeg);
}
