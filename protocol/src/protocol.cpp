#include "melonds_remote/protocol.h"

#include <cstring>

namespace melonds_remote {

void appendU16(ByteBuffer& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void appendU32(ByteBuffer& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
}

void appendU64(ByteBuffer& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
}

void appendI16(ByteBuffer& out, int16_t v) {
    appendU16(out, static_cast<uint16_t>(v));
}

uint16_t readU16(const uint8_t* data, size_t offset) {
    return static_cast<uint16_t>(data[offset]) |
           (static_cast<uint16_t>(data[offset + 1]) << 8);
}

uint32_t readU32(const uint8_t* data, size_t offset) {
    return static_cast<uint32_t>(data[offset]) |
           (static_cast<uint32_t>(data[offset + 1]) << 8) |
           (static_cast<uint32_t>(data[offset + 2]) << 16) |
           (static_cast<uint32_t>(data[offset + 3]) << 24);
}

uint64_t readU64(const uint8_t* data, size_t offset) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(data[offset + static_cast<size_t>(i)]) << (8 * i);
    }
    return v;
}

int16_t readI16(const uint8_t* data, size_t offset) {
    return static_cast<int16_t>(readU16(data, offset));
}

void serializeHeader(ByteBuffer& out, const PacketHeader& header) {
    appendU32(out, header.magic);
    appendU16(out, header.protocolVersion);
    appendU16(out, static_cast<uint16_t>(header.type));
    appendU32(out, header.payloadSize);
}

std::optional<PacketHeader> parseHeader(const uint8_t* data, size_t size) {
    if (data == nullptr || size < kPacketHeaderWireSize) {
        return std::nullopt;
    }

    PacketHeader header;
    header.magic = readU32(data, 0);
    if (header.magic != kPacketMagic) {
        return std::nullopt;
    }
    header.protocolVersion = readU16(data, 4);
    header.type = static_cast<PacketType>(readU16(data, 6));
    header.payloadSize = readU32(data, 8);
    return header;
}

void serializeControllerState(ByteBuffer& out, const ControllerState& state) {
    appendU32(out, state.sequence);
    appendU64(out, state.clientTimestampUs);
    appendU16(out, state.dsButtons);
    appendU16(out, state.emulatorActions);
    appendI16(out, state.leftStickX);
    appendI16(out, state.leftStickY);
    appendI16(out, state.rightStickX);
    appendI16(out, state.rightStickY);
    out.push_back(state.touchActive ? 1 : 0);
    appendU16(out, state.touchX);
    appendU16(out, state.touchY);
}

std::optional<ControllerState> parseControllerState(const uint8_t* data, size_t size) {
    if (data == nullptr || size != kControllerStateWireSize) {
        return std::nullopt;
    }

    size_t offset = 0;
    ControllerState state;
    state.sequence = readU32(data, offset); offset += 4;
    state.clientTimestampUs = readU64(data, offset); offset += 8;
    state.dsButtons = readU16(data, offset); offset += 2;
    state.emulatorActions = readU16(data, offset); offset += 2;
    state.leftStickX = readI16(data, offset); offset += 2;
    state.leftStickY = readI16(data, offset); offset += 2;
    state.rightStickX = readI16(data, offset); offset += 2;
    state.rightStickY = readI16(data, offset); offset += 2;

    uint8_t touchActive = data[offset]; offset += 1;
    if (touchActive != 0 && touchActive != 1) {
        return std::nullopt;
    }
    state.touchActive = touchActive;

    state.touchX = readU16(data, offset); offset += 2;
    state.touchY = readU16(data, offset); offset += 2;

    if (state.touchActive) {
        if (state.touchX > kTouchMaxX || state.touchY > kTouchMaxY) {
            return std::nullopt;
        }
    }

    return state;
}

ByteBuffer buildPacket(PacketType type, const ByteBuffer& payload) {
    PacketHeader header;
    header.type = type;
    header.payloadSize = static_cast<uint32_t>(payload.size());

    ByteBuffer out;
    out.reserve(kPacketHeaderWireSize + payload.size());
    serializeHeader(out, header);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

ByteBuffer buildControllerStatePacket(const ControllerState& state) {
    ByteBuffer payload;
    payload.reserve(kControllerStateWireSize);
    serializeControllerState(payload, state);
    return buildPacket(PacketType::ControllerState, payload);
}

} // namespace melonds_remote
