#pragma once

// The versioned Emulator Adapter Contract (GitHub issue #28). Part of
// the Phase 1 "architecture decision and generic contracts" milestone
// -- see docs/adr/0001-host-service-and-adapter-architecture.md for the
// full design rationale, including why this is an in-process C++
// interface today rather than already being an IPC wire protocol.
//
// Nothing here is wired into the live client/host yet: the production
// wire protocol (protocol.h, kProtocolVersion 6) and the production
// host (host/remote-server's NetServer + IFrameSource/
// IEmulatorInputSink) are both completely unchanged by this file. This
// is the target shape a real Host Service extraction (issue #28 Phase
// 2) will migrate towards, proven out today only by the fake DS/3DS/
// Wii U fixtures under adapter-sdk/fake_adapters/.

#include <cstdint>
#include <string>
#include <vector>

#include "melonds_remote/adapter/generic_input.h"
#include "melonds_remote/adapter/session_state.h"
#include "melonds_remote/adapter/video_surface.h"
#include "melonds_remote/protocol.h" // SystemIdentity, AdapterIdentity

namespace melonds_remote::adapter {

// Bumped whenever this contract's shape changes incompatibly -- the
// same versioning discipline protocol.h's kProtocolVersion already
// applies to the client<->host wire format, applied here to the
// Host-Service<->adapter boundary instead. Distinct from
// kProtocolVersion (client<->host) and AdapterIdentity::adapterVersion
// (a specific adapter's own release, e.g. melonDS's version) -- three
// independent axes of "what version is this," matching the same
// reasoning protocol.h's HelloAckPayload comments already give for
// appVersion vs. kProtocolVersion.
inline constexpr uint16_t kAdapterContractVersion = 1;

// Everything the Host Service needs to know about an adapter before
// negotiating a session with it: identity, declared video surfaces, and
// coarse input capability flags. Reported once when the adapter
// registers (issue #28: "Adapter identity and version", "Emulated
// system identity"), not re-sent per frame/input state.
struct AdapterCapabilities {
    melonds_remote::SystemIdentity system;
    melonds_remote::AdapterIdentity adapter;
    std::vector<VideoSurfaceDescriptor> surfaces;

    // Optional capability flags (issue #28: "Optional motion,
    // microphone, audio, and status capabilities"). Deliberately coarse
    // booleans here, not a full sensor/audio-format description --
    // that level of detail is deferred until a real adapter actually
    // needs it (issue #28's 3DS/Wii U sections both explicitly mark
    // motion/microphone as "later", not required for the first real
    // integration of either).
    bool supportsMotion = false;
    bool supportsMicrophone = false;
    bool supportsAnalogTriggers = false;
};

// A single frame for a specific surface, latest-frame-wins per surface
// (issue #28: "Frame submission using latest-frame-wins behavior" and
// the required test "Per-surface latest-frame-wins behavior"). Payload
// is raw pixels in the surface's declared PixelFormat/width/height --
// no compression, matching protocol.h's existing Stage-1 raw-buffer
// choice for the live VideoFrame payload.
struct SurfaceFrame {
    std::string surfaceId;
    uint64_t frameIndex = 0; // monotonically increasing per surface, starts at 0
    std::vector<uint8_t> pixels;
};

// The contract every emulator adapter implements (issue #28's
// "Emulator Adapter Contract" section). Implemented in-process today by
// the fake DS/3DS/Wii U fixtures under adapter-sdk/fake_adapters/; a
// real melonDS adapter and, eventually, out-of-process 3DS/Wii U
// adapters would implement this same interface -- an out-of-process one
// behind a local-IPC shim, per the ADR, rather than the Host Service
// growing a second, parallel API for "adapters that happen to run
// elsewhere."
//
// Security note (issue #28: "The adapter boundary must not expose
// arbitrary shell execution"): every method here takes a typed,
// bounded argument (a fixed enum bitmask, a coordinate, a surface ID
// string) -- there is no "run this command" or "read this path" entry
// point anywhere in this interface, and there never should be one added
// to it.
class IEmulatorAdapter {
public:
    virtual ~IEmulatorAdapter() = default;

    virtual AdapterCapabilities capabilities() const = 0;
    virtual SessionState currentState() const = 0;

    // Applies the latest generic input state. Implementations must not
    // block (no I/O, no lock that could stall the emulation thread) --
    // the same no-blocking contract
    // host::IEmulatorInputSink::applyControllerState() already
    // documents for today's DS-specific equivalent.
    virtual void applyGenericInput(const GenericInputState& state) = 0;

    // Called on client disconnect, input timeout, adapter shutdown, or
    // session change (issue #28: "Input release on disconnect,
    // timeout, adapter shutdown, or session change"). Must immediately
    // reflect an all-released state across every surface/button/touch
    // this adapter exposes -- generalizes
    // host::IEmulatorInputSink::releaseAll().
    virtual void releaseAllInputs() = 0;

    // Pull-based, mirrors host::IFrameSource::getLatestFrame()'s
    // existing never-blocks/latest-frame-wins contract, generalized to
    // a named surface instead of "the one DS bottom screen." Returns
    // false if no frame has been produced yet for that surface ID (or
    // the surface ID is unknown to this adapter).
    virtual bool latestFrame(const std::string& surfaceId, SurfaceFrame& outFrame) = 0;
};

} // namespace melonds_remote::adapter
