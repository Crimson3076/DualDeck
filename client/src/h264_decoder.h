#pragma once

// H.264 decoder wrapper -- the client-side counterpart to
// host/remote-server/include/host/h264_encoder.h (see that header's own
// comment and docs/known-limitations.md's 2026-08-25 video-codec-
// negotiation entries for the full design). Backed by OpenH264 (Cisco,
// BSD-licensed) when this client was built with it available -- see
// client/CMakeLists.txt's/client/tests/CMakeLists.txt's optional
// detection, the same DUALDECK_HAVE_OPENH264-gated pattern the host
// side already uses.

#include <cstdint>
#include <memory>
#include <vector>

namespace melonds_remote::client {

class H264Decoder {
public:
    H264Decoder();
    ~H264Decoder();

    H264Decoder(const H264Decoder&) = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;

    // True if this build was compiled with OpenH264 available.
    static bool isAvailable();

    // Decodes one Annex-B H.264 access unit (everything
    // NetClient::videoReceiveLoop() received in a single VideoFrame
    // packet -- unlike the encoder, the decoder needs no prior
    // width/height: SPS/PPS in the bitstream itself carries that,
    // reported back via outWidth/outHeight) into BGRA8888, matching
    // this project's existing decompressJpegToBgra()'s output
    // convention exactly so nothing downstream (NetClient::
    // latestFrame_, main.cpp's texture upload) needs to change.
    //
    // outHasFrame reports whether a complete decoded picture was
    // actually produced by *this* call -- false is not a failure: an
    // SPS/PPS-only access unit produces no picture by design, and
    // OpenH264's own documented "no-delay decoding" semantics can
    // occasionally defer a frame's output to the following call (see
    // codec_api.h's own decode-loop example). A caller should simply
    // skip updating its displayed frame when outHasFrame is false, the
    // same way NetServer::videoLoop() already skips sending on an
    // encoder-side "nothing new this tick." Returns false (outBgra/
    // outHasFrame unspecified) only on an actual decode error, or if
    // this build has no OpenH264.
    bool decodeFrame(const uint8_t* annexB, size_t size, std::vector<uint8_t>& outBgra, int& outWidth,
                      int& outHeight, bool& outHasFrame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace melonds_remote::client
