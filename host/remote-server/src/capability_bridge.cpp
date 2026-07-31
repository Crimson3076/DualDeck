#include "host/capability_bridge.h"

namespace melonds_remote::host {

WireDisplayRole toWireDisplayRole(melonds_remote::adapter::SurfaceRole role) {
    switch (role) {
        case melonds_remote::adapter::SurfaceRole::Top:       return WireDisplayRole::Top;
        case melonds_remote::adapter::SurfaceRole::Bottom:    return WireDisplayRole::Bottom;
        case melonds_remote::adapter::SurfaceRole::Tv:        return WireDisplayRole::Tv;
        case melonds_remote::adapter::SurfaceRole::GamePad:   return WireDisplayRole::GamePad;
        case melonds_remote::adapter::SurfaceRole::Auxiliary: return WireDisplayRole::Auxiliary;
    }
    // Every adapter::SurfaceRole value is handled above (no adapter today
    // ever produces WireDisplayRole::Primary/Secondary -- those exist for
    // a future non-Nintendo-shaped adapter, see protocol.h) -- this line
    // is unreachable but keeps the function total for a caller that
    // somehow gets an out-of-range enum value (e.g. via a raw cast).
    return WireDisplayRole::Auxiliary;
}

WirePixelFormat toWirePixelFormat(melonds_remote::adapter::PixelFormat format) {
    switch (format) {
        case melonds_remote::adapter::PixelFormat::Bgra8888: return WirePixelFormat::Bgra8888;
    }
    return WirePixelFormat::Bgra8888;
}

WireDisplayDescriptor toWireDisplayDescriptor(const melonds_remote::adapter::VideoSurfaceDescriptor& surface) {
    WireDisplayDescriptor wire;
    wire.surfaceId = surface.surfaceId;
    wire.role = toWireDisplayRole(surface.role);
    wire.width = surface.width;
    wire.height = surface.height;
    wire.pixelFormat = toWirePixelFormat(surface.pixelFormat);
    wire.touchSupported = surface.touchSupported;
    // scalingStrategy/requestedCodec/requestedBitrateKbps/targetRefreshHz
    // deliberately left at their "no enhancement, host picks" defaults --
    // adapter::VideoSurfaceDescriptor has no equivalent fields to
    // translate from yet (see protocol.h's WireDisplayDescriptor comment).
    return wire;
}

WireHostCapabilities toWireHostCapabilities(const melonds_remote::adapter::AdapterCapabilities& capabilities) {
    WireHostCapabilities wire;
    wire.availableCodecs = WireCodec_Jpeg; // only codec this codebase implements today
    wire.displays.reserve(capabilities.surfaces.size());
    for (const auto& surface : capabilities.surfaces) {
        wire.displays.push_back(toWireDisplayDescriptor(surface));
    }
    return wire;
}

} // namespace melonds_remote::host
