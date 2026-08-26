#include "h264_decoder.h"

#ifdef DUALDECK_HAVE_OPENH264

#include <wels/codec_api.h>

#ifdef DUALDECK_HAVE_LIBYUV
#include <libyuv/convert_argb.h>
#endif

#include <algorithm>
#include <cstring>

namespace melonds_remote::client {

namespace {

// I420 (planar YUV 4:2:0) -> BGRA8888, the inverse of
// host/remote-server/src/h264_encoder.cpp's bgraToI420().
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
// bgraToI420()'s own forward coefficients: real H.264 compression is
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

} // namespace

struct H264Decoder::Impl {
    ISVCDecoder* decoder = nullptr;

    Impl() {
        if (WelsCreateDecoder(&decoder) != 0) {
            decoder = nullptr;
            return;
        }
        SDecodingParam param;
        std::memset(&param, 0, sizeof(param));
        param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
        if (decoder->Initialize(&param) != 0) {
            WelsDestroyDecoder(decoder);
            decoder = nullptr;
        }
    }

    ~Impl() {
        if (decoder) {
            decoder->Uninitialize();
            WelsDestroyDecoder(decoder);
        }
    }
};

H264Decoder::H264Decoder() : impl_(std::make_unique<Impl>()) {}
H264Decoder::~H264Decoder() = default;

bool H264Decoder::isAvailable() { return true; }

bool H264Decoder::decodeFrame(const uint8_t* annexB, size_t size, std::vector<uint8_t>& outBgra, int& outWidth,
                               int& outHeight, bool& outHasFrame) {
    outHasFrame = false;
    if (!impl_->decoder) {
        return false;
    }

    unsigned char* dst[3] = {nullptr, nullptr, nullptr};
    SBufferInfo bufInfo;
    std::memset(&bufInfo, 0, sizeof(bufInfo));
    DECODING_STATE state = impl_->decoder->DecodeFrame2(annexB, static_cast<int>(size), dst, &bufInfo);
    if (state != dsErrorFree) {
        return false;
    }
    if (bufInfo.iBufferStatus != 1) {
        // No complete picture from this call -- see this method's own
        // header comment on why that's expected, not an error (an
        // SPS/PPS-only unit, or the documented no-delay-decode deferral
        // of the very first frame's output to a following call).
        return true;
    }

    const auto& sysBuf = bufInfo.UsrData.sSystemBuffer;
    if (sysBuf.iWidth <= 0 || sysBuf.iHeight <= 0 || !dst[0] || !dst[1] || !dst[2]) {
        return false;
    }

    i420ToBgra(dst[0], sysBuf.iStride[0], dst[1], dst[2], sysBuf.iStride[1], sysBuf.iWidth, sysBuf.iHeight, outBgra);
    outWidth = sysBuf.iWidth;
    outHeight = sysBuf.iHeight;
    outHasFrame = true;
    return true;
}

} // namespace melonds_remote::client

#else // !DUALDECK_HAVE_OPENH264

namespace melonds_remote::client {

// This build was configured without OpenH264 -- every method is a
// no-op that reports unavailable, matching h264_encoder.cpp's own
// stub for the exact same reason.
struct H264Decoder::Impl {};

H264Decoder::H264Decoder() : impl_(std::make_unique<Impl>()) {}
H264Decoder::~H264Decoder() = default;

bool H264Decoder::isAvailable() { return false; }
bool H264Decoder::decodeFrame(const uint8_t*, size_t, std::vector<uint8_t>&, int&, int&, bool& outHasFrame) {
    outHasFrame = false;
    return false;
}

} // namespace melonds_remote::client

#endif // DUALDECK_HAVE_OPENH264
