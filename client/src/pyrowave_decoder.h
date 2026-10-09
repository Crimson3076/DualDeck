#pragma once

// PyroWave decoder wrapper -- the client-side counterpart to
// host/remote-server/include/host/pyrowave_encoder.h (see that header's
// own comment for what PyroWave is and why it's here). Backed by
// PyroWave's standalone C API (libpyrowave-shared) when this client was
// built with it -- see client/CMakeLists.txt's optional detection, the
// same pattern OpenH264 already uses. Decodes on the GPU via Vulkan
// compute and reads the result back into CPU memory, so its output
// slots into exactly the same BGRA8888 texture-upload path JPEG/H.264
// already use; nothing downstream changes.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace melonds_remote::client {

class PyroWaveDecoder {
public:
    PyroWaveDecoder();
    ~PyroWaveDecoder();

    PyroWaveDecoder(const PyroWaveDecoder&) = delete;
    PyroWaveDecoder& operator=(const PyroWaveDecoder&) = delete;

    // True if this build was compiled with PyroWave AND a Vulkan device
    // on this machine can actually create a PyroWave decoder -- probed
    // once (a throwaway device + small decoder) on first call and cached.
    // NetClient::connect() only asks when the user opted into PyroWave,
    // so a client that never picks it never initializes Vulkan for it.
    static bool isAvailable();

    // Decodes one complete PyroWave frame (one VideoFrame packet's
    // payload, as produced by host::PyroWaveEncoder::encodeFrame()) into
    // BGRA8888, the same output convention as H264Decoder::decodeFrame().
    // No prior width/height needed: the frame's own start-of-frame
    // header (PyroWave bitstream v1's BitstreamSequenceHeader) carries
    // them, and the underlying decoder is transparently recreated
    // whenever they change mid-session. outHasFrame is false (with a
    // true return) only if the payload was well-formed but didn't add
    // up to a complete frame -- not expected over this project's TCP
    // video channel, but treated as "skip this one" rather than fatal.
    // Returns false on a malformed payload, a decode error, or a build/
    // machine without PyroWave.
    bool decodeFrame(const uint8_t* data, size_t size, std::vector<uint8_t>& outBgra, int& outWidth,
                      int& outHeight, bool& outHasFrame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace melonds_remote::client
