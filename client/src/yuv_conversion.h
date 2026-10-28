#pragma once

// I420 (planar YUV 4:2:0) -> BGRA8888 conversion shared by every planar-
// YUV video decoder on the client: H264Decoder (OpenH264) and
// PyroWaveDecoder (see each one's own header) -- the inverse of
// host/remote-server/include/host/yuv_conversion.h. See
// yuv_conversion.cpp for the libyuv vs. scalar-fallback details.

#include <cstdint>
#include <vector>

namespace dualdeck::client {

// Resizes outBgra to width*height*4 and fills it (alpha forced to 0xFF).
void i420ToBgra(const uint8_t* y, int yStride, const uint8_t* u, const uint8_t* v, int chromaStride, int width,
                 int height, std::vector<uint8_t>& outBgra);

} // namespace dualdeck::client
