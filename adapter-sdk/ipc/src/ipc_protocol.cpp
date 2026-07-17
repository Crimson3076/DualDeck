#include "melonds_remote/adapter/ipc/ipc_protocol.h"

using melonds_remote::ByteBuffer;
using melonds_remote::appendAdapterIdentity;
using melonds_remote::appendString;
using melonds_remote::appendSystemIdentity;
using melonds_remote::appendU16;
using melonds_remote::appendU32;
using melonds_remote::appendU64;
using melonds_remote::readAdapterIdentity;
using melonds_remote::readString;
using melonds_remote::readSystemIdentity;
using melonds_remote::readU16;
using melonds_remote::readU32;
using melonds_remote::readU64;

namespace melonds_remote::adapter::ipc {

namespace {
void appendI16(ByteBuffer& out, int16_t v) {
    appendU16(out, static_cast<uint16_t>(v));
}
int16_t readI16(const uint8_t* data, size_t offset) {
    return static_cast<int16_t>(readU16(data, offset));
}
} // namespace

void serializeIpcHeader(ByteBuffer& out, const IpcHeader& header) {
    appendU32(out, header.magic);
    appendU16(out, header.contractVersion);
    appendU16(out, static_cast<uint16_t>(header.type));
    appendU32(out, header.payloadSize);
}

std::optional<IpcHeader> parseIpcHeader(const uint8_t* data, size_t size) {
    if (data == nullptr || size < kIpcHeaderWireSize) {
        return std::nullopt;
    }
    IpcHeader header;
    header.magic = readU32(data, 0);
    if (header.magic != kAdapterIpcMagic) {
        return std::nullopt;
    }
    header.contractVersion = readU16(data, 4);
    header.type = static_cast<IpcMessageType>(readU16(data, 6));
    header.payloadSize = readU32(data, 8);
    return header;
}

ByteBuffer buildIpcMessage(IpcMessageType type, const ByteBuffer& payload) {
    IpcHeader header;
    header.type = type;
    header.payloadSize = static_cast<uint32_t>(payload.size());

    ByteBuffer out;
    out.reserve(kIpcHeaderWireSize + payload.size());
    serializeIpcHeader(out, header);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

namespace {

void appendVideoSurfaceDescriptor(ByteBuffer& out, const VideoSurfaceDescriptor& s) {
    appendString(out, s.surfaceId);
    out.push_back(static_cast<uint8_t>(s.role));
    appendU16(out, s.width);
    appendU16(out, s.height);
    out.push_back(static_cast<uint8_t>(s.pixelFormat));
    out.push_back(static_cast<uint8_t>(s.orientation));
    out.push_back(s.touchSupported ? 1 : 0);
    appendU16(out, s.touchRangeX);
    appendU16(out, s.touchRangeY);
    out.push_back(s.required ? 1 : 0);
    out.push_back(s.locallyDisplayed ? 1 : 0);
    out.push_back(s.remotelyDisplayed ? 1 : 0);
    out.push_back(s.selectable ? 1 : 0);
    appendU16(out, s.nominalFps);
    appendU16(out, s.maxFps);
}

// kFixedVideoSurfaceBytesAfterId: everything in appendVideoSurfaceDescriptor()
// above except the leading length-prefixed surfaceId string.
constexpr size_t kFixedVideoSurfaceBytesAfterId =
    1 + 2 + 2 + 1 + 1 + 1 + 2 + 2 + 1 + 1 + 1 + 1 + 2 + 2;

std::optional<VideoSurfaceDescriptor> readVideoSurfaceDescriptor(const uint8_t* data, size_t size,
                                                                    size_t& offset) {
    auto surfaceId = readString(data, size, offset);
    if (!surfaceId) return std::nullopt;

    if (offset + kFixedVideoSurfaceBytesAfterId > size) return std::nullopt;

    VideoSurfaceDescriptor s;
    s.surfaceId = std::move(*surfaceId);

    uint8_t role = data[offset]; offset += 1;
    if (role > static_cast<uint8_t>(SurfaceRole::Auxiliary)) return std::nullopt;
    s.role = static_cast<SurfaceRole>(role);

    s.width = readU16(data, offset); offset += 2;
    s.height = readU16(data, offset); offset += 2;

    uint8_t pixelFormat = data[offset]; offset += 1;
    if (pixelFormat > static_cast<uint8_t>(PixelFormat::Bgra8888)) return std::nullopt;
    s.pixelFormat = static_cast<PixelFormat>(pixelFormat);

    uint8_t orientation = data[offset]; offset += 1;
    if (orientation > static_cast<uint8_t>(Orientation::Portrait)) return std::nullopt;
    s.orientation = static_cast<Orientation>(orientation);

    uint8_t touchSupported = data[offset]; offset += 1;
    if (touchSupported != 0 && touchSupported != 1) return std::nullopt;
    s.touchSupported = touchSupported != 0;

    s.touchRangeX = readU16(data, offset); offset += 2;
    s.touchRangeY = readU16(data, offset); offset += 2;

    uint8_t required = data[offset]; offset += 1;
    if (required != 0 && required != 1) return std::nullopt;
    s.required = required != 0;

    uint8_t locallyDisplayed = data[offset]; offset += 1;
    if (locallyDisplayed != 0 && locallyDisplayed != 1) return std::nullopt;
    s.locallyDisplayed = locallyDisplayed != 0;

    uint8_t remotelyDisplayed = data[offset]; offset += 1;
    if (remotelyDisplayed != 0 && remotelyDisplayed != 1) return std::nullopt;
    s.remotelyDisplayed = remotelyDisplayed != 0;

    uint8_t selectable = data[offset]; offset += 1;
    if (selectable != 0 && selectable != 1) return std::nullopt;
    s.selectable = selectable != 0;

    s.nominalFps = readU16(data, offset); offset += 2;
    s.maxFps = readU16(data, offset); offset += 2;

    return s;
}

} // namespace

void serializeAdapterCapabilities(ByteBuffer& out, const AdapterCapabilities& caps) {
    appendSystemIdentity(out, caps.system);
    appendAdapterIdentity(out, caps.adapter);
    appendU16(out, static_cast<uint16_t>(caps.surfaces.size()));
    for (const auto& s : caps.surfaces) {
        appendVideoSurfaceDescriptor(out, s);
    }
    out.push_back(caps.supportsMotion ? 1 : 0);
    out.push_back(caps.supportsMicrophone ? 1 : 0);
    out.push_back(caps.supportsAnalogTriggers ? 1 : 0);
}

std::optional<AdapterCapabilities> parseAdapterCapabilities(const uint8_t* data, size_t size) {
    if (data == nullptr) return std::nullopt;

    size_t offset = 0;
    AdapterCapabilities caps;

    auto system = readSystemIdentity(data, size, offset);
    if (!system) return std::nullopt;
    caps.system = std::move(*system);

    auto adapter = readAdapterIdentity(data, size, offset);
    if (!adapter) return std::nullopt;
    caps.adapter = std::move(*adapter);

    if (offset + 2 > size) return std::nullopt;
    uint16_t surfaceCount = readU16(data, offset); offset += 2;
    if (surfaceCount > kMaxIpcSurfaceCount) return std::nullopt;

    caps.surfaces.reserve(surfaceCount);
    for (uint16_t i = 0; i < surfaceCount; ++i) {
        auto surface = readVideoSurfaceDescriptor(data, size, offset);
        if (!surface) return std::nullopt;
        caps.surfaces.push_back(std::move(*surface));
    }

    if (offset + 3 > size) return std::nullopt;
    uint8_t motion = data[offset]; offset += 1;
    uint8_t mic = data[offset]; offset += 1;
    uint8_t triggers = data[offset]; offset += 1;
    if ((motion != 0 && motion != 1) || (mic != 0 && mic != 1) || (triggers != 0 && triggers != 1)) {
        return std::nullopt;
    }
    caps.supportsMotion = motion != 0;
    caps.supportsMicrophone = mic != 0;
    caps.supportsAnalogTriggers = triggers != 0;

    if (offset != size) return std::nullopt; // trailing garbage
    return caps;
}

void serializeAdapterHelloAckPayload(ByteBuffer& out, const AdapterHelloAckPayload& ack) {
    out.push_back(ack.accepted ? 1 : 0);
    out.push_back(static_cast<uint8_t>(ack.reason));
}

std::optional<AdapterHelloAckPayload> parseAdapterHelloAckPayload(const uint8_t* data, size_t size) {
    if (data == nullptr || size != 2) return std::nullopt;

    uint8_t accepted = data[0];
    if (accepted != 0 && accepted != 1) return std::nullopt;

    uint8_t reason = data[1];
    if (reason > static_cast<uint8_t>(AdapterHelloAckReason::AlreadyRegistered)) return std::nullopt;

    AdapterHelloAckPayload ack;
    ack.accepted = accepted;
    ack.reason = static_cast<AdapterHelloAckReason>(reason);
    return ack;
}

void serializeGenericInputState(ByteBuffer& out, const GenericInputState& state) {
    appendU32(out, state.sequence);
    appendU64(out, state.clientTimestampUs);
    appendU32(out, state.buttons);
    appendI16(out, state.leftStickX);
    appendI16(out, state.leftStickY);
    appendI16(out, state.rightStickX);
    appendI16(out, state.rightStickY);
    out.push_back(state.leftTrigger);
    out.push_back(state.rightTrigger);
    appendU16(out, static_cast<uint16_t>(state.touches.size()));
    for (const auto& t : state.touches) {
        appendString(out, t.surfaceId);
        appendU16(out, t.x);
        appendU16(out, t.y);
    }
    out.push_back(state.micActive ? 1 : 0);
    appendU32(out, state.emulatorActions);
}

std::optional<GenericInputState> parseGenericInputState(const uint8_t* data, size_t size) {
    if (data == nullptr) return std::nullopt;

    constexpr size_t kFixedPrefix = 4 + 8 + 4 + 2 + 2 + 2 + 2 + 1 + 1;
    if (size < kFixedPrefix) return std::nullopt;

    size_t offset = 0;
    GenericInputState state;
    state.sequence = readU32(data, offset); offset += 4;
    state.clientTimestampUs = readU64(data, offset); offset += 8;
    state.buttons = readU32(data, offset); offset += 4;
    state.leftStickX = readI16(data, offset); offset += 2;
    state.leftStickY = readI16(data, offset); offset += 2;
    state.rightStickX = readI16(data, offset); offset += 2;
    state.rightStickY = readI16(data, offset); offset += 2;
    state.leftTrigger = data[offset]; offset += 1;
    state.rightTrigger = data[offset]; offset += 1;

    if (offset + 2 > size) return std::nullopt;
    uint16_t touchCount = readU16(data, offset); offset += 2;
    // Bounded generously -- a real client has at most a handful of
    // simultaneous touch/stylus contacts across however many
    // touch-capable surfaces an adapter declares; this rejects an
    // absurd declared count before ever looping/allocating for it.
    constexpr uint16_t kMaxTouchContacts = 64;
    if (touchCount > kMaxTouchContacts) return std::nullopt;

    state.touches.reserve(touchCount);
    for (uint16_t i = 0; i < touchCount; ++i) {
        auto surfaceId = readString(data, size, offset);
        if (!surfaceId) return std::nullopt;
        if (offset + 4 > size) return std::nullopt;
        TouchContact contact;
        contact.surfaceId = std::move(*surfaceId);
        contact.x = readU16(data, offset); offset += 2;
        contact.y = readU16(data, offset); offset += 2;
        state.touches.push_back(std::move(contact));
    }

    if (offset + 1 > size) return std::nullopt;
    uint8_t micActive = data[offset]; offset += 1;
    if (micActive != 0 && micActive != 1) return std::nullopt;
    state.micActive = micActive != 0;

    if (offset + 4 > size) return std::nullopt;
    state.emulatorActions = readU32(data, offset); offset += 4;

    if (offset != size) return std::nullopt; // trailing garbage
    return state;
}

void serializeSurfaceFrame(ByteBuffer& out, const SurfaceFrame& frame) {
    appendString(out, frame.surfaceId);
    appendU64(out, frame.frameIndex);
    appendU32(out, static_cast<uint32_t>(frame.pixels.size()));
    out.insert(out.end(), frame.pixels.begin(), frame.pixels.end());
}

std::optional<SurfaceFrame> parseSurfaceFrame(const uint8_t* data, size_t size) {
    if (data == nullptr) return std::nullopt;

    size_t offset = 0;
    auto surfaceId = readString(data, size, offset);
    if (!surfaceId) return std::nullopt;

    if (offset + 8 + 4 > size) return std::nullopt;
    SurfaceFrame frame;
    frame.surfaceId = std::move(*surfaceId);
    frame.frameIndex = readU64(data, offset); offset += 8;
    uint32_t pixelCount = readU32(data, offset); offset += 4;

    if (pixelCount > kMaxIpcFramePixelBytes) return std::nullopt;
    if (offset + pixelCount != size) return std::nullopt; // declared size must match what actually follows

    frame.pixels.assign(data + offset, data + offset + pixelCount);
    return frame;
}

void serializeSessionState(ByteBuffer& out, SessionState state) {
    out.push_back(static_cast<uint8_t>(state));
}

std::optional<SessionState> parseSessionState(const uint8_t* data, size_t size) {
    if (data == nullptr || size != 1) return std::nullopt;
    uint8_t value = data[0];
    if (value > static_cast<uint8_t>(SessionState::Error)) return std::nullopt;
    return static_cast<SessionState>(value);
}

} // namespace melonds_remote::adapter::ipc
