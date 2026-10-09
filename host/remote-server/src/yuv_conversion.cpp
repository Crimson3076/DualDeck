#include "host/yuv_conversion.h"

#ifdef DUALDECK_HAVE_LIBYUV
#include <libyuv/convert_from_argb.h>
#endif

#include <algorithm>

namespace dualdeck::host {

// Converts one BGRA8888 frame to I420 (planar YUV 4:2:0), the pixel
// format ISVCEncoder::EncodeFrame() requires (SSourcePicture::iColorFormat
// = videoFormatI420) and PyroWave's native CPU input format
// (PYROWAVE_CPU_BUFFER_FORMAT_YUV420P).
//
// DUALDECK_HAVE_LIBYUV builds use libyuv::ARGBToI420() -- SIMD-accelerated,
// a real measured win over the hand-rolled loop below (2026-08-26 latency
// audit: this scalar loop was 2.5-4x slower, combined with i420ToBgra()'s
// matching decode-side loop, than libjpeg-turbo's own SIMD BGRA<->YCbCr
// path, see tools/codec-benchmark). libyuv's "ARGB" format name is
// confusingly unrelated to byte order at a glance -- confirmed directly
// against its own header comments (convert.h: "ARGB little endian (bgra
// in memory) to I420"), not assumed from the name alone -- and is exactly
// this project's BGRA8888-everywhere convention (byte 0 is B, byte 1 is
// G, byte 2 is R, matching the hand-rolled fallback's own `px[0]`/`px[1]`/
// `px[2]` reads below). ARGBToI420 (not the J420 "full range" variant)
// uses the same BT.601 studio-range (16-235) matrix the hand-rolled
// fallback below does, so builds with and without libyuv produce visually
// equivalent output, not just "both somehow work" -- confirmed by
// h264_decoder_decodes_this_projects_own_encoder_output's existing
// average-pixel-difference round-trip test passing under both configs.
//
// Builds without libyuv keep this plain BT.601 studio-range hand-rolled
// version -- this project already takes the equivalent tradeoff the
// other direction for JPEG (compressFrameBgraToJpeg() in net_server.cpp
// lets libjpeg-turbo do BGRA->YCbCr internally, since that library needs
// linking anyway for the codec itself; OpenH264 has no such built-in RGB
// path, so this is the one extra step JPEG's own approach didn't need).
void bgraToI420(const uint8_t* bgra, int width, int height, std::vector<uint8_t>& outY,
                 std::vector<uint8_t>& outU, std::vector<uint8_t>& outV) {
    const int chromaWidth = (width + 1) / 2;
    const int chromaHeight = (height + 1) / 2;
    outY.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
    outU.resize(static_cast<size_t>(chromaWidth) * static_cast<size_t>(chromaHeight));
    outV.resize(static_cast<size_t>(chromaWidth) * static_cast<size_t>(chromaHeight));

#ifdef DUALDECK_HAVE_LIBYUV
    libyuv::ARGBToI420(bgra, width * 4, outY.data(), width, outU.data(), chromaWidth, outV.data(), chromaWidth,
                        width, height);
#else
    auto pixelAt = [&](int x, int y) -> const uint8_t* {
        return bgra + (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
    };

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const uint8_t* px = pixelAt(x, y);
            const int b = px[0], g = px[1], r = px[2];
            const int yVal = (66 * r + 129 * g + 25 * b + 128) / 256 + 16;
            outY[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] =
                static_cast<uint8_t>(std::clamp(yVal, 0, 255));
        }
    }
    for (int cy = 0; cy < chromaHeight; ++cy) {
        for (int cx = 0; cx < chromaWidth; ++cx) {
            // Top-left sample of each 2x2 block -- box-filtering all
            // four would be marginally more accurate but isn't worth
            // the extra passes on this real-time low-latency path (the
            // libyuv path above does box-filter, at no extra cost to us
            // since it's a real SIMD implementation, not this fallback's
            // own concern).
            const uint8_t* px = pixelAt(std::min(cx * 2, width - 1), std::min(cy * 2, height - 1));
            const int b = px[0], g = px[1], r = px[2];
            const int uVal = (-38 * r - 74 * g + 112 * b + 128) / 256 + 128;
            const int vVal = (112 * r - 94 * g - 18 * b + 128) / 256 + 128;
            outU[static_cast<size_t>(cy) * static_cast<size_t>(chromaWidth) + static_cast<size_t>(cx)] =
                static_cast<uint8_t>(std::clamp(uVal, 0, 255));
            outV[static_cast<size_t>(cy) * static_cast<size_t>(chromaWidth) + static_cast<size_t>(cx)] =
                static_cast<uint8_t>(std::clamp(vVal, 0, 255));
        }
    }
#endif // DUALDECK_HAVE_LIBYUV
}

} // namespace dualdeck::host
