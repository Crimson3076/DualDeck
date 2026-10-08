#include "h264_decoder.h"

#ifdef DUALDECK_HAVE_OPENH264

#include <wels/codec_api.h>

#include <cstring>

#include "yuv_conversion.h"

namespace melonds_remote::client {

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
