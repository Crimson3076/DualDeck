#pragma once

// Translates between dualdeck::adapter's capability/surface types
// (adapter_contract.h, video_surface.h) and protocol.h's Wire* mirror
// types -- the one layer allowed to depend on both (see protocol.h's
// "Capability negotiation data model" section for why adapter-sdk can't
// depend on these Wire* types, nor protocol.h on adapter-sdk's). Same
// role as adapter_bridge.h's DS-compatibility translation, one level up:
// that class translates generic input/frames down to the DS-specific
// wire shape NetServer speaks today; these functions translate generic
// capability/surface descriptions up to the Wire* shape a future
// negotiation phase will actually put on the wire.
//
// Not called by anything yet -- HelloAckPayload doesn't carry
// WireHostCapabilities yet (see protocol.h) -- but the translation logic
// itself is real and worth having correct and tested now rather than
// written under time pressure once the wire format actually needs it.

#include "dualdeck/adapter/adapter_contract.h"
#include "dualdeck/adapter/video_surface.h"
#include "dualdeck/protocol.h"

namespace dualdeck::host {

WireDisplayRole toWireDisplayRole(dualdeck::adapter::SurfaceRole role);
WirePixelFormat toWirePixelFormat(dualdeck::adapter::PixelFormat format);
WireDisplayDescriptor toWireDisplayDescriptor(const dualdeck::adapter::VideoSurfaceDescriptor& surface);
WireHostCapabilities toWireHostCapabilities(const dualdeck::adapter::AdapterCapabilities& capabilities);

} // namespace dualdeck::host
