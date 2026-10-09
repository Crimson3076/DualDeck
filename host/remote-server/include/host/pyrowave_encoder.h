#pragma once

// PyroWave encoder wrapper -- an optional third video codec alongside
// JPEG and H.264 (see NetServer::selectVideoCodec()'s own comment and
// docs/known-limitations.md's PyroWave entry). PyroWave
// (https://github.com/Themaister/pyrowave, MIT) is an intra-only
// wavelet codec implemented entirely in Vulkan compute shaders, built
// for exactly this project's use case: LAN game streaming where encode/
// decode latency matters far more than bandwidth (~0.1ms encode at
// 1080p on a real GPU, no inter-frame dependency so no keyframe waits
// after a reconnect or a dropped frame, and exact per-frame rate
// control). The trade is bitrate: it needs several times what H.264
// does for comparable quality, so it's an opt-in for wired/fast-Wi-Fi
// setups, not a new default.
//
// Uses PyroWave's standalone C API's CPU-buffer path: BGRA frames are
// converted to I420 on the CPU (host/yuv_conversion.h, the same planes
// H264Encoder feeds OpenH264), uploaded, encoded on the GPU, and read
// back as a bitstream -- every adapter in this project already hands
// NetServer CPU-side BGRA frames, so there's no GPU image to share
// zero-copy anyway, and at DS/3DS/GamePad resolutions the upload/
// readback is a rounding error next to JPEG's own encode time.
//
// Availability is a *runtime* question, not just a build-time one,
// unlike H264Encoder: a build linked against libpyrowave-shared still
// needs a Vulkan 1.3 device whose subgroup-size control can force
// wave16/32/64 at runtime (every desktop GPU and the Steam Deck's own
// RDNA2 APU can; Mesa's software lavapipe, fixed at wave8, can't). See
// isAvailable().

#include <cstdint>
#include <memory>

#include "melonds_remote/protocol.h"

namespace melonds_remote::host {

class PyroWaveEncoder {
public:
    PyroWaveEncoder();
    ~PyroWaveEncoder();

    PyroWaveEncoder(const PyroWaveEncoder&) = delete;
    PyroWaveEncoder& operator=(const PyroWaveEncoder&) = delete;

    // True if this build was compiled with PyroWave AND this machine has
    // a Vulkan device that can actually create a PyroWave encoder. The
    // runtime half is probed once (creating a throwaway Vulkan device +
    // small encoder, typically well under a second) on first call and
    // cached for the process's lifetime -- NetServer::selectVideoCodec()
    // only calls this when a connecting client actually advertised
    // PyroWave, so a host nobody asks for PyroWave never pays for it.
    static bool isAvailable();

    // (Re)initializes for the given frame size, with each encoded frame
    // capped at maxFrameBytes (PyroWave's rate control is an exact
    // per-frame byte budget, not a bitrate target -- see
    // pyrowaveMaxFrameBytes() in net_server.cpp for how that's derived).
    // Odd widths/heights are supported by edge-replicating one extra
    // column/row (PyroWave's 4:2:0 mode requires even dimensions), so
    // the decoded frame can be one pixel wider/taller than the source.
    // Safe to call again with a different size (Cemu's mid-session
    // GamePad resolution changes, see H264Encoder::initialize()'s own
    // comment) -- the Vulkan device is created once and kept, only the
    // size-specific encoder is rebuilt. Returns false on failure,
    // including on a build or machine without PyroWave support.
    bool initialize(int width, int height, size_t maxFrameBytes);

    // Encodes one BGRA8888 frame (`width`/`height` must match the most
    // recent successful initialize()) into a single self-contained
    // PyroWave frame -- a start-of-frame sequence header followed by
    // every non-empty 32x32 coefficient block, exactly what PyroWave's
    // own pyrowave_decoder_push_packet() accepts in one call. Appended
    // to outBitstream (not cleared first, same convention as
    // H264Encoder::encodeFrame()). Every frame is independently
    // decodable (intra-only), so there's no keyframe concept and no
    // requestKeyframe() counterpart. Returns false (outBitstream
    // untouched) on any failure.
    bool encodeFrame(const uint8_t* bgra, int width, int height, ByteBuffer& outBitstream);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace melonds_remote::host
