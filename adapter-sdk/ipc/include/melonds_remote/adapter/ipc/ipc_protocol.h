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

// Upper bound on a single Frame message's pixel payload (128 MiB).
//
// Real-usage bug (GitHub-reported): the previous value here (16 MiB,
// sized off a Wii U TV surface assumed to top out at 1920x1080 ==
// 8,294,400 bytes) silently broke Cemu on any title whose actual
// internal render-target size exceeds that -- CemuAdapter captures the
// TV surface at Cemu's real internal render resolution
// (LatteTextureVk::GetEffectiveSize()), which is NOT always a fixed
// 1080p. The most obvious way to hit this is a user-installed 4K/5K/8K
// resolution-enhancing graphics pack, but it doesn't require one: some
// Wii U titles render their own internal framebuffer above their
// display output resolution as part of the *game's own* built-in
// anti-aliasing/supersampling (confirmed via a user report -- Twilight
// Princess HD hit this bug with zero graphics packs installed, while
// Wind Waker HD on the same setup did not, pointing at a per-title
// native render-resolution difference rather than any pack). Either
// way, a game whose real internal size is even 1440p
// (2560x1440x4 = 14,745,600 bytes) already exceeded the old cap.
// parseSurfaceFrame() rejects an over-cap payload
// as malformed, which both AdapterIpcClient::readLoop() and
// AdapterIpcServer's own receive loop treat as "this connection is
// over" -- so every attempted TV-surface Frame message failed instantly
// after a fresh handshake succeeded, producing a client that looked
// like it was rapidly bouncing between Emulation and HostControl mode
// forever, indistinguishable at a glance from a genuinely flaky
// connection (see docs/known-limitations.md's matching entry). The same
// class of bug applies to AzaharAdapter now that its capture scale
// auto-follows Settings::values.resolution_factor (up to 10x native
// 400x240 = 4000x2400x4 = 38,400,000 bytes, also over the old cap) --
// see the 2026-07-23 "Azahar capture scale now follows resolution_factor
// automatically" entry in known-limitations.md. 128 MiB comfortably
// covers even an 8K TV surface (7680x4320x4 = 132,710,400 bytes is the
// one real case this doesn't cover; 128 MiB was chosen to stay a round
// number rather than chase that specific figure, and nothing this
// project defines gets remotely close to it otherwise) -- this is purely
// local Unix-socket IPC between the emulator process and the Host
// Service on the same machine, not anything sent over the network, so
// the memory cost of a generous cap here is a non-issue on any machine
// capable of rendering at that resolution in the first place.
inline constexpr uint32_t kMaxIpcFramePixelBytes = 128u * 1024u * 1024u;

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
