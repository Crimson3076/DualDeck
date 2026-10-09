#include "yuv_conversion.h"

#ifdef DUALDECK_HAVE_LIBYUV
#include <libyuv/convert_argb.h>
#endif

#include <algorithm>

namespace dualdeck::client {

// I420 (planar YUV 4:2:0) -> BGRA8888, the inverse of
// host/remote-server/src/yuv_conversion.cpp's bgraToI420().
//
// DUALDECK_HAVE_LIBYUV builds use libyuv::I420ToARGB() -- SIMD-
// accelerated, same real measured win (and same "ARGB" naming/byte-order
// verification against libyuv's own header comments, not assumed from
// the name) as bgraToI420()'s own comment describes for the encode
// direction; see that comment for the full explanation and
// tools/codec-benchmark's real numbers.
//
// Builds without libyuv keep this plain BT.601 integer-coefficient
// fallback (the same well-known fixed-point approximation libyuv/ffmpeg
// use internally) -- not derived as an exact algebraic inverse of
// bgraToI420()'s own forward coefficients: real H.264/PyroWave compression is
// lossy in between the two anyway, so matching a standard, independently-
// verifiable formula here is more meaningful than chasing bit-exactness
// against one specific encoder's rounding.
void i420ToBgra(const uint8_t* y, int yStride, const uint8_t* u, const uint8_t* v, int chromaStride, int width,
                 int height, std::vector<uint8_t>& outBgra) {
    outBgra.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
#ifdef DUALDECK_HAVE_LIBYUV
    libyuv::I420ToARGB(y, yStride, u, chromaStride, v, chromaStride, outBgra.data(), width * 4, width, height);
#else
    for (int row = 0; row < height; ++row) {
        const uint8_t* yRow = y + static_cast<size_t>(row) * static_cast<size_t>(yStride);
        const uint8_t* uRow = u + static_cast<size_t>(row / 2) * static_cast<size_t>(chromaStride);
        const uint8_t* vRow = v + static_cast<size_t>(row / 2) * static_cast<size_t>(chromaStride);
        uint8_t* outRow = &outBgra[static_cast<size_t>(row) * static_cast<size_t>(width) * 4];
        for (int col = 0; col < width; ++col) {
            const int c = yRow[col] - 16;
            const int d = uRow[col / 2] - 128;
            const int e = vRow[col / 2] - 128;
            const int r = (298 * c + 409 * e + 128) >> 8;
            const int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
            const int b = (298 * c + 516 * d + 128) >> 8;
            uint8_t* px = outRow + col * 4;
            px[0] = static_cast<uint8_t>(std::clamp(b, 0, 255));
            px[1] = static_cast<uint8_t>(std::clamp(g, 0, 255));
            px[2] = static_cast<uint8_t>(std::clamp(r, 0, 255));
            px[3] = 0xFF;
        }
    }
#endif // DUALDECK_HAVE_LIBYUV
}

} // namespace dualdeck::client
