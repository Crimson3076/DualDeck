#pragma once

// BGRA8888 -> I420 (planar YUV 4:2:0) conversion shared by every planar-
// YUV video encoder on the host: H264Encoder (OpenH264) and
// PyroWaveEncoder (see each one's own header). Both codecs consume the
// exact same input planes, so this lives here rather than duplicated in
// each encoder's own .cpp -- see yuv_conversion.cpp for the libyuv vs.
// scalar-fallback details. client/src/yuv_conversion.h is the inverse
// (decode-side) counterpart.

#include <cstdint>
#include <vector>

namespace melonds_remote::host {

// Writes width*height luma bytes to outY and ((width+1)/2)*((height+1)/2)
// chroma bytes each to outU/outV (BT.601 studio range), resizing them as
// needed.
void bgraToI420(const uint8_t* bgra, int width, int height, std::vector<uint8_t>& outY,
                 std::vector<uint8_t>& outU, std::vector<uint8_t>& outV);

} // namespace melonds_remote::host
