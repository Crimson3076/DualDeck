#pragma once

// melonDS Remote wire protocol definitions.
//
// This header defines the versioned, explicitly-sized, explicitly-serialized
// packet formats used between the Steam Deck client and the HTPC host. See
// docs/protocol.md for the human-readable protocol description.
//
// All multi-byte integers are little-endian on the wire regardless of host
// byte order. Use the serialize()/deserialize() functions below rather than
// reinterpreting raw structs over the network -- struct layout is
// compiler/ABI-dependent and must never be sent directly.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace melonds_remote {

// Bumped whenever the wire format changes incompatibly.
inline constexpr uint16_t kProtocolVersion = 1;

// Sentinel at the start of every packet so malformed/foreign traffic on the
// same port can be rejected cheaply before any further parsing.
inline constexpr uint32_t kPacketMagic = 0x444D5231; // "DMR1"

enum class PacketType : uint16_t {
    Hello = 1,        // client -> host, control channel, session handshake request
    HelloAck = 2,      // host -> client, control channel, session handshake response
    ControllerState = 3, // client -> host, input channel, full input state
    Heartbeat = 4,     // either direction, control channel, keepalive
    Disconnect = 5,    // either direction, control channel, graceful teardown
    EmulatorAction = 6, // client -> host, control channel, one-shot emulator command
    VideoFrame = 7,    // host -> client, video channel, one bottom-screen frame
};

// DS button bitmask (wire format; independent from melonDS's internal
// KEYINPUT bit order -- see docs/melonds-integration-analysis.md section 2).
enum DSButton : uint16_t {
    DSButton_A      = 1u << 0,
    DSButton_B      = 1u << 1,
    DSButton_X      = 1u << 2,
    DSButton_Y      = 1u << 3,
    DSButton_Up     = 1u << 4,
    DSButton_Down   = 1u << 5,
    DSButton_Left   = 1u << 6,
    DSButton_Right  = 1u << 7,
    DSButton_L      = 1u << 8,
    DSButton_R      = 1u << 9,
    DSButton_Start  = 1u << 10,
    DSButton_Select = 1u << 11,
};

// Emulator action bitmask, independent from DSButton (spec section 7.5/9).
enum EmulatorAction : uint16_t {
    EmulatorAction_PauseResume    = 1u << 0,
    EmulatorAction_FastForward    = 1u << 1,
    EmulatorAction_SaveState      = 1u << 2,
    EmulatorAction_LoadState      = 1u << 3,
    EmulatorAction_SwapScreens    = 1u << 4,
    EmulatorAction_OpenClientMenu = 1u << 5,
    EmulatorAction_Disconnect     = 1u << 6,
    EmulatorAction_QuitSession    = 1u << 7,
};

// Native DS bottom-screen touch range (spec section 7.4).
inline constexpr uint16_t kTouchMaxX = 255;
inline constexpr uint16_t kTouchMaxY = 191;

// Full controller state, sent at a fixed rate (recommended 120 Hz) rather
// than only on button transitions, so a lost packet cannot leave a button
// stuck (spec section 6.3).
struct ControllerState {
    uint32_t sequence = 0;
    uint64_t clientTimestampUs = 0;
    uint16_t dsButtons = 0;        // DSButton bitmask, 1 = pressed
    uint16_t emulatorActions = 0;  // EmulatorAction bitmask, 1 = active this packet
    int16_t leftStickX = 0;        // -32768..32767, centered at 0
    int16_t leftStickY = 0;
    int16_t rightStickX = 0;
    int16_t rightStickY = 0;
    uint8_t touchActive = 0;       // 0 or 1
    uint16_t touchX = 0;           // 0..kTouchMaxX, valid only if touchActive
    uint16_t touchY = 0;           // 0..kTouchMaxY, valid only if touchActive
};

// Wire size of a serialized ControllerState payload (not including the
// common PacketHeader). Kept explicit so tests can catch accidental growth.
inline constexpr size_t kControllerStateWireSize =
    4 + 8 + 2 + 2 + 2 + 2 + 2 + 2 + 1 + 2 + 2;

struct PacketHeader {
    uint32_t magic = kPacketMagic;
    uint16_t protocolVersion = kProtocolVersion;
    PacketType type = PacketType::Hello;
    uint32_t payloadSize = 0;
};

inline constexpr size_t kPacketHeaderWireSize = 4 + 2 + 2 + 4;

using ByteBuffer = std::vector<uint8_t>;

// Appends a little-endian integer to `out`.
void appendU16(ByteBuffer& out, uint16_t v);
void appendU32(ByteBuffer& out, uint32_t v);
void appendU64(ByteBuffer& out, uint64_t v);
void appendI16(ByteBuffer& out, int16_t v);

// Reads a little-endian integer from `data` at `offset`.
// Caller must have already validated that offset+size <= data.size().
uint16_t readU16(const uint8_t* data, size_t offset);
uint32_t readU32(const uint8_t* data, size_t offset);
uint64_t readU64(const uint8_t* data, size_t offset);
int16_t readI16(const uint8_t* data, size_t offset);

// Serializes a packet header. Always writes kPacketHeaderWireSize bytes.
void serializeHeader(ByteBuffer& out, const PacketHeader& header);

// Parses a packet header from a buffer of at least kPacketHeaderWireSize
// bytes. Returns std::nullopt if the magic number doesn't match; does not
// validate protocol version (caller decides whether to accept/reject a
// version mismatch).
std::optional<PacketHeader> parseHeader(const uint8_t* data, size_t size);

// Serializes a ControllerState payload (header not included).
void serializeControllerState(ByteBuffer& out, const ControllerState& state);

// Parses a ControllerState payload of exactly kControllerStateWireSize
// bytes. Returns std::nullopt on malformed input (wrong size, out-of-range
// touch coordinates, or an invalid touchActive value).
std::optional<ControllerState> parseControllerState(const uint8_t* data, size_t size);

// Builds a complete Hello/HelloAck/etc. packet: header + raw payload bytes.
// `payload` may be empty for packet types that carry no body (e.g.
// Heartbeat, Disconnect).
ByteBuffer buildPacket(PacketType type, const ByteBuffer& payload);

// Builds a complete ControllerState packet (header + serialized body).
ByteBuffer buildControllerStatePacket(const ControllerState& state);

} // namespace melonds_remote
