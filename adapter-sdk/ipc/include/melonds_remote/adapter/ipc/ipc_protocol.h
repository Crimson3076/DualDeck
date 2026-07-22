#pragma once

// Wire format for the Host-Service<->adapter local IPC channel (GitHub
// issue #28 Phase 2, implementing the transport decided in
// docs/adr/0001-host-service-and-adapter-architecture.md section 3).
//
// Deliberately mirrors protocol.h's own conventions closely (explicit
// little-endian integers, length-prefixed strings via the same
// appendString()/readString() helpers, a magic+version+type+size
// header validated before any payload is trusted) -- this is a
// different channel serving a different purpose (Host Service <->
// local adapter, not client <-> host over a LAN), so it gets its own
// magic number and its own kAdapterContractVersion (already declared in
// adapter_contract.h), never melonds_remote::kPacketMagic/
// kProtocolVersion. A message on the wrong channel is rejected exactly
// like a foreign packet on protocol.h's channels is: by magic mismatch,
// before anything else is parsed.
//
// This channel is local-machine-only (a Unix domain socket, see
// socket_path.h) -- unlike the client<->host protocol, it never
// crosses a network, so it has no equivalent of device-approval/auth
// tokens. Access control is the socket's filesystem permissions plus a
// same-UID peer-credential check (see adapter_ipc_server.h) -- see the
// ADR for why that's sufficient here specifically.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "melonds_remote/adapter/adapter_contract.h"
#include "melonds_remote/adapter/generic_input.h"
#include "melonds_remote/adapter/session_state.h"
#include "melonds_remote/protocol.h" // ByteBuffer, appendString/readString, etc.

namespace melonds_remote::adapter::ipc {

// "DAI1" ("DualDeck Adapter IPC", version 1 of the byte layout below) --
// distinct from melonds_remote::kPacketMagic ("DMR1") so a message from
// the wrong channel is rejected immediately rather than partially
// parsed.
inline constexpr uint32_t kAdapterIpcMagic = 0x44414931;

enum class IpcMessageType : uint16_t {
    Hello         = 1, // adapter -> service: AdapterCapabilities + contract version
    HelloAck      = 2, // service -> adapter: accepted/reason
    InputState    = 3, // service -> adapter: GenericInputState
    ReleaseInputs = 4, // service -> adapter: no payload
    Frame         = 5, // adapter -> service: SurfaceFrame
    StateChanged  = 6, // adapter -> service: SessionState
    Heartbeat     = 7, // either direction: no payload
    Disconnect    = 8, // either direction: no payload, graceful teardown
    // service -> adapter: 1 byte, 1 = a remote client is now connected
    // and streaming, 0 = disconnected. Lets an out-of-process adapter's
    // own frontend (e.g. Azahar's Qt window) mirror melonDS's in-process
    // "show only the top screen locally while a client is streaming"
    // behavior, even though the adapter's own process has no other way
    // to know a client is connected -- that state lives entirely in the
    // separate Host Service process (NetServer), not here.
    ClientConnectionChanged = 9,
};

struct IpcHeader {
    uint32_t magic = kAdapterIpcMagic;
    uint16_t contractVersion = kAdapterContractVersion;
    IpcMessageType type = IpcMessageType::Heartbeat;
    uint32_t payloadSize = 0;
};

inline constexpr size_t kIpcHeaderWireSize = 4 + 2 + 2 + 4;

void serializeIpcHeader(melonds_remote::ByteBuffer& out, const IpcHeader& header);
std::optional<IpcHeader> parseIpcHeader(const uint8_t* data, size_t size);

// Builds a complete message: header + payload. `payload` may be empty
// for payload-less types (Heartbeat, Disconnect, ReleaseInputs).
melonds_remote::ByteBuffer buildIpcMessage(IpcMessageType type, const melonds_remote::ByteBuffer& payload);

// Upper bound on a single Frame message's pixel payload (16 MiB) --
// comfortably above any surface this project defines today (the
// largest fixture, Wii U's 1920x1080 BGRA8888 TV surface, is 1920 * 1080
// * 4 = 8,294,400 bytes) while still bounding it before ever allocating,
// per the same "validate every declared size before trusting it"
// discipline protocol.h already documents.
inline constexpr uint32_t kMaxIpcFramePixelBytes = 16u * 1024u * 1024u;

// Upper bound on how many VideoSurfaceDescriptors one AdapterCapabilities
// may declare -- generous for any real system (DS: 1, 3DS/Wii U: 2) while
// still bounding a hostile/buggy adapter's declared count before ever
// allocating for it.
inline constexpr uint32_t kMaxIpcSurfaceCount = 16;

enum class AdapterHelloAckReason : uint8_t {
    None = 0,
    ContractVersionMismatch = 1,
    AlreadyRegistered = 2, // another adapter is already connected (one at a time, like NetServer's control channel)
};

struct AdapterHelloAckPayload {
    uint8_t accepted = 0;
    AdapterHelloAckReason reason = AdapterHelloAckReason::None;
};

void serializeAdapterCapabilities(melonds_remote::ByteBuffer& out, const AdapterCapabilities& caps);
std::optional<AdapterCapabilities> parseAdapterCapabilities(const uint8_t* data, size_t size);

void serializeAdapterHelloAckPayload(melonds_remote::ByteBuffer& out, const AdapterHelloAckPayload& ack);
std::optional<AdapterHelloAckPayload> parseAdapterHelloAckPayload(const uint8_t* data, size_t size);

void serializeGenericInputState(melonds_remote::ByteBuffer& out, const GenericInputState& state);
std::optional<GenericInputState> parseGenericInputState(const uint8_t* data, size_t size);

void serializeSurfaceFrame(melonds_remote::ByteBuffer& out, const SurfaceFrame& frame);
// Rejects (returns std::nullopt) a declared pixel count over
// kMaxIpcFramePixelBytes, or a size mismatch between the declared count
// and what actually follows -- same validation discipline as
// protocol.h's parseMicAudioFramePayload().
std::optional<SurfaceFrame> parseSurfaceFrame(const uint8_t* data, size_t size);

void serializeSessionState(melonds_remote::ByteBuffer& out, SessionState state);
std::optional<SessionState> parseSessionState(const uint8_t* data, size_t size);

void serializeClientConnectionChanged(melonds_remote::ByteBuffer& out, bool connected);
std::optional<bool> parseClientConnectionChanged(const uint8_t* data, size_t size);

} // namespace melonds_remote::adapter::ipc
