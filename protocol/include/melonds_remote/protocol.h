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
#include <string>
#include <vector>

namespace melonds_remote {

// Bumped whenever the wire format changes incompatibly.
inline constexpr uint16_t kProtocolVersion = 6;

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
    DiscoveryRequest = 8,  // client -> host, UDP discovery port, broadcast "who's out there"
    DiscoveryResponse = 9, // host -> client, UDP discovery port, unicast reply to the sender
    // client -> host, UDP audio port, one chunk of captured microphone PCM
    // (GitHub issue #2). Only sent when HelloAckPayload::micSupported was
    // true and the user hasn't muted -- absence of packets is the "no
    // audio" state, matching how melonDS's own Mic input already treats
    // silence (see docs/known-limitations.md's microphone section).
    MicAudioFrame = 10,
    // host -> client, control channel, unsolicited notification that the
    // host switched which mode/adapter is driving the session (GitHub
    // issue #4 Phase B) -- e.g. melonDS launching or exiting while a
    // client stays connected throughout, swapping NetServer's target
    // between HostControlAdapter and the emulator. Sent only to an
    // already-authenticated, currently-connected client; a new
    // connection just learns the current state from HelloAck instead
    // (see HelloAckPayload::system/adapter), so there is no negotiated
    // payload shape to keep compatible here and this addition does not
    // need a kProtocolVersion bump -- same reasoning as
    // DiscoveryRequest/DiscoveryResponse's original introduction. An
    // older client that doesn't yet read anything from the control
    // channel post-handshake simply never consumes these bytes; they sit
    // harmlessly unread rather than causing any error.
    ModeChanged = 11,
};

// Which of two states the host is currently in (GitHub issue #4): a live
// emulation session with a real adapter (e.g. melonDS) connected and
// driving input/video, or idle host-control navigation with no emulator
// running at all (a virtual gamepad -- HostControlAdapter -- the client
// can use to browse/launch the host's own UI instead). Every host today
// implicitly starts in, and stays in, Emulation; HostControl only
// exists once a Host Service that outlives the emulator process is
// wired up (issue #4's later phases) -- see NetServer::setTarget().
enum class HostMode : uint8_t {
    Emulation = 0,
    HostControl = 1,
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
    // Ejects the current ROM (melonDS's own "Eject Cart" action) --
    // ends this play session without closing melonDS itself. Named
    // "QuitSession" per SPEC.md section 7.5's original wording, which
    // reads most naturally as "end the current game session" rather
    // than "close the whole app" (see EmulatorAction_QuitApplication
    // for that).
    EmulatorAction_QuitSession    = 1u << 7,
    // Closes melonDS entirely (GitHub issue #25: "prompt the user to
    // either exit the current ROM, or exit the entire application").
    // A new bit value within the same existing uint16_t field --
    // adding it doesn't change the wire layout, so this doesn't need a
    // kProtocolVersion bump (unlike a new *field*, e.g.
    // HelloPayload::appVersion).
    EmulatorAction_QuitApplication = 1u << 8,
};

// Native DS bottom-screen touch range (spec section 7.4).
inline constexpr uint16_t kTouchMaxX = 255;
inline constexpr uint16_t kTouchMaxY = 191;

// Bound on any length-prefixed string field below, to reject an
// obviously-hostile declared length before ever allocating for it (spec
// section 13: validate packet sizes and values).
inline constexpr size_t kMaxProtocolStringLength = 64;

// Emulator-independent identity (GitHub issue #28's architecture
// foundation milestone: decouple DualDeck from melonDS so 3DS/Wii U
// adapters can be added later without rewriting the protocol or client).
//
// Deliberately two separate structs, not one merged "identity" blob:
// SystemIdentity describes *what is being emulated* (a fact about the
// game/console, e.g. "nds"/"Nintendo DS"), while AdapterIdentity
// describes *which specific integration is driving it*
// (e.g. "melonds"/"melonDS" today; a hypothetical second DS-capable
// adapter down the line would share the same SystemIdentity but report
// a different AdapterIdentity). The client must never infer one from the
// other -- always use both fields as reported by the host, never a
// hardcoded table keyed by e.g. adapterId (issue #28: "Never infer the
// emulator from the system alone; use the identity reported by the
// negotiated adapter").
struct SystemIdentity {
    // Short, stable, machine-comparable identifier for the emulated
    // system, e.g. "nds", "3ds", "wiiu" -- future code (controller
    // profile selection, layout selection) may switch on this, so a
    // value must never change meaning once shipped. Up to
    // kMaxProtocolStringLength bytes. Not free text.
    std::string systemId;
    // Human-readable name for UI, e.g. "Nintendo DS". Up to
    // kMaxProtocolStringLength bytes.
    std::string systemName;
};

struct AdapterIdentity {
    // Short, stable, machine-comparable identifier for the specific
    // emulator/adapter driving the session, e.g. "melonds",
    // "synthetic-test". Up to kMaxProtocolStringLength bytes. Not free
    // text -- distinct from SystemIdentity::systemId since more than one
    // adapter could eventually target the same emulated system.
    std::string adapterId;
    // Human-readable emulator name for UI, e.g. "melonDS". Up to
    // kMaxProtocolStringLength bytes.
    std::string adapterName;
    // The adapter's own version string (e.g. melonDS's own release
    // version for the melonDS adapter, or this repository's release
    // version for the synthetic test adapter) -- distinct from
    // HelloPayload/HelloAckPayload::appVersion (DualDeck's own release)
    // and kProtocolVersion (wire format version) above. May be empty if
    // unknown. Up to kMaxProtocolStringLength bytes.
    std::string adapterVersion;
};

// Hello (client -> host) handshake payload. See docs/protocol.md
// "Handshake" for the full negotiation description; this is the minimal
// version implemented so far -- capability negotiation fields beyond
// display size are not yet included.
//
// `authToken` does double duty (spec section 13's "later pairing options"
// list): if the host was started with a static `--auth-token`, this must
// match it exactly. Otherwise the host runs in device-approval mode (see
// `docs/protocol.md`'s "Authentication and device approval" section) --
// this field is the client's own self-generated, persistent device
// identity (not something the host issues), the same value on every
// connection attempt from this client, to any host. An unrecognized value
// gets rejected with `ApprovalRequired`, the same on every retry, until a
// human at the host approves that device -- no code is ever typed on
// either side.
struct HelloPayload {
    std::string clientName;    // up to kMaxProtocolStringLength bytes
    std::string clientPlatform; // up to kMaxProtocolStringLength bytes
    uint16_t displayWidth = 0;
    uint16_t displayHeight = 0;
    std::string authToken;     // up to kMaxProtocolStringLength bytes; empty if host requires none
    // Release version string (the archive's VERSION file, e.g. "v0.1.24"),
    // up to kMaxProtocolStringLength bytes -- distinct from kProtocolVersion
    // above, which only guards wire-format compatibility. Two builds can
    // share a wire format (same kProtocolVersion) while being different
    // releases with different features/fixes; this field lets the host
    // reject a connection from a client it knows predates it, or vice
    // versa, per HelloRejectReason::AppVersionMismatch below. Left empty by
    // a build with no known version (e.g. a from-source dev build not
    // packaged via scripts/build-release.sh) -- see net_server.cpp's
    // comparison logic for how an empty value on either side is handled.
    std::string appVersion;
};

enum class HelloRejectReason : uint8_t {
    None = 0,
    ProtocolVersionMismatch = 1,
    AuthenticationFailed = 2,
    HostBusy = 3,
    // No static auth token configured and the presented device identity
    // isn't (yet) in the host's approved-devices list. The request has
    // been queued for a human at the host to approve or deny (see
    // `docs/protocol.md`'s "Authentication and device approval" section);
    // the client should keep retrying automatically (same as any other
    // connection failure) rather than prompt the user for anything --
    // there's nothing for the client side to do but wait.
    ApprovalRequired = 4,
    // The host's and client's HelloPayload/HelloAckPayload appVersion
    // fields are both non-empty and differ. Distinct from
    // ProtocolVersionMismatch: the wire format itself parsed fine (same
    // kProtocolVersion), this is purely "you two are different releases."
    // Never raised when either side's appVersion is empty (unknown/dev
    // build) -- see net_server.cpp.
    AppVersionMismatch = 5,
};

// HelloAck (host -> client) handshake payload.
struct HelloAckPayload {
    uint8_t accepted = 0; // 0 or 1
    HelloRejectReason rejectReason = HelloRejectReason::None; // meaningful only if !accepted
    uint32_t sessionId = 0;
    uint16_t nativeWidth = 256;
    uint16_t nativeHeight = 192;
    // The host's own release version string, always populated (regardless
    // of accepted/rejectReason) so the client can show e.g. "host is on
    // vX.Y, you're on vA.B" even on an AppVersionMismatch rejection. Empty
    // if the host doesn't know its own version (see HelloPayload::appVersion).
    std::string appVersion;
    // Whether this host build/config can accept and inject microphone
    // audio (GitHub issue #2) -- false for a synthetic/standalone host
    // that never wires a mic sink, or when the patched melonDS host has
    // no working mic feed path available. A host predating this field
    // is simply a different kProtocolVersion (see above) and gets
    // rejected before either side's payload is parsed, so no separate
    // "field absent" case exists to handle here. The client should not
    // bother opening a capture device or sending MicAudioFrame packets
    // unless this is true.
    uint8_t micSupported = 0; // 0 or 1
    // Which emulated system and which adapter/emulator is actually
    // running it (GitHub issue #28), sent regardless of accepted/
    // rejectReason -- same convention as appVersion above, so the client
    // can show "you tried to connect to a Nintendo DS / melonDS host"
    // even on a rejected handshake. Also advertised in
    // DiscoveryResponsePayload below so the host-selection list can show
    // it before a handshake is ever attempted; present here too so a
    // client that already knows a host's address (bypassing discovery)
    // still learns it. A host predating this field is simply a different
    // kProtocolVersion (see above) and gets rejected before either side's
    // payload is parsed, so there is no separate "field absent" case to
    // handle -- see docs/protocol.md's "Emulator identity model" section
    // for the full compatibility decision.
    SystemIdentity system;
    AdapterIdentity adapter;
};

// DiscoveryRequest (client -> host) has no payload: a bare packet header
// broadcast to the LAN discovery port is the whole request. Any host
// listening replies with a unicast DiscoveryResponse to the sender's
// address. This is deliberately unauthenticated (it only ever reveals a
// host name and port numbers, never anything sensitive) -- the actual
// pairing/auth flow still gates the control/input/video ports themselves,
// see docs/protocol.md's "Authentication and pairing" section. Discovery
// existing at all doesn't weaken that; it's equivalent in spirit to
// mDNS/SSDP advertising a service's presence without granting access to it.

// DiscoveryResponse (host -> client) handshake-free "here I am" reply.
struct DiscoveryResponsePayload {
    std::string hostName;      // up to kMaxProtocolStringLength bytes, e.g. hostname
    uint16_t controlPort = 8760;
    uint16_t inputPort = 8761;
    uint16_t videoPort = 8762;
    // UDP port for MicAudioFrame packets (GitHub issue #2). Advertised
    // here (rather than only in HelloAck) so a client that already knows
    // a host's address (--host/last-used, bypassing discovery) still
    // needs the control-channel handshake's HelloAckPayload::micSupported
    // to decide whether to use it -- this field alone doesn't imply the
    // host actually accepts audio, only which port it would listen on if
    // it does.
    uint16_t audioPort = 8765;
    // Which emulated system and which adapter/emulator this host is
    // currently running (GitHub issue #28) -- lets the host-selection
    // list show e.g. "Nintendo DS · melonDS" before a connection is ever
    // attempted. See HelloAckPayload::system/adapter above for the
    // matching handshake-time fields and the shared compatibility
    // decision (docs/protocol.md's "Emulator identity model").
    SystemIdentity system;
    AdapterIdentity adapter;
};

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

// Fixed sample format for MicAudioFrame (GitHub issue #2): mono, 16-bit
// signed PCM, little-endian on the wire (like every other multi-byte
// field here), at a single fixed rate rather than negotiated per-client.
// 48 kHz matches melonDS's own default local-mic capture rate
// (EmuInstanceAudio.cpp's micFreq), so host-side resampling into the DS's
// actual ~47.6 kHz mic-buffer consumption rate reuses the exact same
// Mic::Advance-driven resample path melonDS already runs for a physical
// microphone -- remote audio is just another producer for that same
// pipeline (see host/melonds-patches/README.md for the injection point).
inline constexpr uint32_t kMicAudioSampleRate = 48000;

// One packet's worth of audio at 10ms per packet (480 samples @ 48kHz) --
// small enough to keep mic latency low and stay well under typical LAN
// MTU (480 * 2 bytes = 960 bytes payload, plus the 12-byte PacketHeader
// and 18-byte MicAudioFramePayload fixed prefix below, is nowhere near
// the ~1400-byte practical UDP payload ceiling). Also the hard cap
// `parseMicAudioFramePayload` enforces on `numSamples` (spec section 13:
// validate declared sizes before trusting them) -- a well-behaved client
// always sends exactly this many samples per packet except possibly a
// shorter final packet right before muting.
inline constexpr uint16_t kMicAudioSamplesPerPacket = 480;

// MicAudioFrame (client -> host) payload: a fixed-rate mono PCM chunk.
// `sequence`/`clientTimestampUs` mirror ControllerState's fields (same
// purpose: host-side ordering/staleness checks), even though, unlike
// controller input, losing one audio packet is not silently self-healing
// -- see docs/known-limitations.md for the resulting audible-gap
// tradeoff of sending raw PCM over UDP with no forward-error-correction.
struct MicAudioFramePayload {
    uint32_t sequence = 0;
    uint64_t clientTimestampUs = 0;
    uint16_t numSamples = 0; // <= kMicAudioSamplesPerPacket
    std::vector<int16_t> samples;
};

// ModeChanged (host -> client) payload: the new mode plus the identity
// now driving it, mirroring HelloAckPayload::system/adapter's fields and
// encoding so a mid-session UI update (GitHub issue #4 Phase E) needs no
// separate lookup -- e.g. "Host Menu" while HostMode::HostControl, or
// "Nintendo DS · melonDS" once HostMode::Emulation resumes.
struct ModeChangedPayload {
    HostMode mode = HostMode::Emulation;
    SystemIdentity system;
    AdapterIdentity adapter;
};

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

// Appends a length-prefixed (u16 length + bytes) string. Rejects (asserts
// via caller contract, not enforced here) building a packet with a string
// longer than kMaxProtocolStringLength -- callers should truncate first.
void appendString(ByteBuffer& out, const std::string& s);

// Reads a length-prefixed string starting at `offset`, advancing `offset`
// past it. Returns std::nullopt if the buffer is too short for the
// declared length or the declared length exceeds
// kMaxProtocolStringLength (spec section 13: validate every field).
std::optional<std::string> readString(const uint8_t* data, size_t size, size_t& offset);

// Appends/reads a SystemIdentity/AdapterIdentity as a pair of
// length-prefixed strings (systemId+systemName, adapterId+adapterName+
// adapterVersion respectively). Shared by HelloAckPayload and
// DiscoveryResponsePayload's (de)serialization so the two payloads can
// never drift apart on how this is encoded. `readX` advances `offset`
// past what it consumed and returns std::nullopt on the same failure
// conditions as readString() (declared-length overrun or over
// kMaxProtocolStringLength).
void appendSystemIdentity(ByteBuffer& out, const SystemIdentity& id);
std::optional<SystemIdentity> readSystemIdentity(const uint8_t* data, size_t size, size_t& offset);
void appendAdapterIdentity(ByteBuffer& out, const AdapterIdentity& id);
std::optional<AdapterIdentity> readAdapterIdentity(const uint8_t* data, size_t size, size_t& offset);

void serializeHelloPayload(ByteBuffer& out, const HelloPayload& hello);
std::optional<HelloPayload> parseHelloPayload(const uint8_t* data, size_t size);
ByteBuffer buildHelloPacket(const HelloPayload& hello);

void serializeHelloAckPayload(ByteBuffer& out, const HelloAckPayload& ack);
std::optional<HelloAckPayload> parseHelloAckPayload(const uint8_t* data, size_t size);
ByteBuffer buildHelloAckPacket(const HelloAckPayload& ack);

// Builds a complete DiscoveryRequest packet (header only, empty payload).
ByteBuffer buildDiscoveryRequestPacket();

void serializeDiscoveryResponsePayload(ByteBuffer& out, const DiscoveryResponsePayload& response);
std::optional<DiscoveryResponsePayload> parseDiscoveryResponsePayload(const uint8_t* data, size_t size);
ByteBuffer buildDiscoveryResponsePacket(const DiscoveryResponsePayload& response);

// Serializes a MicAudioFramePayload (header not included).
void serializeMicAudioFramePayload(ByteBuffer& out, const MicAudioFramePayload& frame);

// Parses a MicAudioFramePayload. Returns std::nullopt if the buffer is
// too short for its own declared numSamples, numSamples exceeds
// kMicAudioSamplesPerPacket, or there's trailing garbage past the last
// sample.
std::optional<MicAudioFramePayload> parseMicAudioFramePayload(const uint8_t* data, size_t size);

// Builds a complete MicAudioFrame packet (header + serialized body).
ByteBuffer buildMicAudioFramePacket(const MicAudioFramePayload& frame);

// Serializes a ModeChangedPayload (header not included).
void serializeModeChangedPayload(ByteBuffer& out, const ModeChangedPayload& modeChanged);

// Parses a ModeChangedPayload. Returns std::nullopt on malformed input
// (unrecognized mode byte, a malformed identity, or trailing garbage).
std::optional<ModeChangedPayload> parseModeChangedPayload(const uint8_t* data, size_t size);

// Builds a complete ModeChanged packet (header + serialized body).
ByteBuffer buildModeChangedPacket(const ModeChangedPayload& modeChanged);

} // namespace melonds_remote
