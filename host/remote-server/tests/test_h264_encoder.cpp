// Unit tests for h264_encoder.h -- see that header's own comment for
// why this exists as a standalone, independently-tested building block
// (protocol v13's negotiation is scaffolded, but no live NetServer code
// path uses this encoder yet).
//
// When this build has OpenH264 (see host/remote-server/tests/
// CMakeLists.txt's own detection, mirroring the main library target's),
// the round-trip test below feeds the encoder's real Annex-B output
// into OpenH264's own decoder -- not this project's code -- for the
// strongest verification available without a second, independent H.264
// implementation to cross-check against. Without OpenH264, only the
// always-available-regardless-of-build-config behavior is covered
// (unavailable, initialize()/encodeFrame() fail cleanly).

#include "host/h264_encoder.h"
#include "test_framework.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#ifdef DUALDECK_HAVE_OPENH264
#include <wels/codec_api.h>
#endif

using namespace melonds_remote;
using namespace melonds_remote::host;

namespace {

// Deterministic gradient pattern -- not required to look like anything
// real, just needs to vary across the frame so a real encode/decode
// pass has actual data to move, unlike a solid color a buggy pipeline
// could accidentally pass through.
std::vector<uint8_t> makeTestFrameBgra(int width, int height) {
    std::vector<uint8_t> frame(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint8_t* px = &frame[(static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4];
            px[0] = static_cast<uint8_t>(x * 255 / std::max(width - 1, 1));
            px[1] = static_cast<uint8_t>(y * 255 / std::max(height - 1, 1));
            px[2] = static_cast<uint8_t>(((x + y) * 255) / std::max(width + height - 2, 1));
            px[3] = 0xFF;
        }
    }
    return frame;
}

} // namespace

MDR_TEST(h264_encoder_encode_before_initialize_fails) {
    H264Encoder encoder;
    auto frame = makeTestFrameBgra(16, 16);
    ByteBuffer out;
    bool isKeyframe = false;
    MDR_CHECK(!encoder.encodeFrame(frame.data(), 16, 16, out, isKeyframe));
    MDR_CHECK(out.empty());
}

MDR_TEST(h264_encoder_initialize_rejects_invalid_size) {
    H264Encoder encoder;
    MDR_CHECK(!encoder.initialize(0, 0, 30, 2000000));
    MDR_CHECK(!encoder.initialize(-1, 16, 30, 2000000));
    MDR_CHECK(!encoder.initialize(16, 16, 0, 2000000));
}

#ifdef DUALDECK_HAVE_OPENH264

MDR_TEST(h264_encoder_availability_matches_build_config) {
    MDR_CHECK(H264Encoder::isAvailable());
}

MDR_TEST(h264_encoder_encodes_and_openh264_decodes_it_back) {
    const int width = 64, height = 48;
    H264Encoder encoder;
    MDR_CHECK(encoder.initialize(width, height, 30, 1000000));

    auto frame = makeTestFrameBgra(width, height);
    ByteBuffer annexB;
    bool isKeyframe = false;
    MDR_CHECK(encoder.encodeFrame(frame.data(), width, height, annexB, isKeyframe));
    MDR_CHECK(!annexB.empty());
    // First frame after initialize() must be an IDR -- nothing else
    // would be decodable at all.
    MDR_CHECK(isKeyframe);

    ISVCDecoder* decoder = nullptr;
    MDR_CHECK(WelsCreateDecoder(&decoder) == 0);
    MDR_CHECK(decoder != nullptr);

    SDecodingParam decParam;
    std::memset(&decParam, 0, sizeof(decParam));
    decParam.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    MDR_CHECK(decoder->Initialize(&decParam) == 0);

    unsigned char* dst[3] = {nullptr, nullptr, nullptr};
    SBufferInfo bufInfo;
    std::memset(&bufInfo, 0, sizeof(bufInfo));
    DECODING_STATE state =
        decoder->DecodeFrame2(annexB.data(), static_cast<int>(annexB.size()), dst, &bufInfo);
    if (bufInfo.iBufferStatus != 1) {
        // codec_api.h's own documented "no-delay decoding" pattern:
        // some configurations only surface the first frame's output on
        // a follow-up call with NULL input, not the call that actually
        // fed the bitstream in -- real decoder behavior, not this
        // test's bug.
        state = decoder->DecodeFrame2(nullptr, 0, dst, &bufInfo);
    }
    MDR_CHECK(state == dsErrorFree);
    MDR_CHECK_EQ(bufInfo.iBufferStatus, 1);
    MDR_CHECK_EQ(bufInfo.UsrData.sSystemBuffer.iWidth, width);
    MDR_CHECK_EQ(bufInfo.UsrData.sSystemBuffer.iHeight, height);
    MDR_CHECK(dst[0] != nullptr);
    MDR_CHECK(dst[1] != nullptr);
    MDR_CHECK(dst[2] != nullptr);

    decoder->Uninitialize();
    WelsDestroyDecoder(decoder);
}

MDR_TEST(h264_encoder_resolution_change_reinitializes_cleanly) {
    H264Encoder encoder;
    MDR_CHECK(encoder.initialize(32, 32, 30, 500000));
    auto frameA = makeTestFrameBgra(32, 32);
    ByteBuffer outA;
    bool keyA = false;
    MDR_CHECK(encoder.encodeFrame(frameA.data(), 32, 32, outA, keyA));

    // A frame at a different size than what initialize() was called
    // with must fail -- see encodeFrame()'s own comment on why this is
    // a safety net, not the intended way to handle a real resolution
    // change (that's initialize() again, exercised below).
    auto frameB = makeTestFrameBgra(48, 48);
    ByteBuffer outB;
    bool keyB = false;
    MDR_CHECK(!encoder.encodeFrame(frameB.data(), 48, 48, outB, keyB));

    MDR_CHECK(encoder.initialize(48, 48, 30, 500000));
    MDR_CHECK(encoder.encodeFrame(frameB.data(), 48, 48, outB, keyB));
    MDR_CHECK(!outB.empty());
    MDR_CHECK(keyB);
}

#else // !DUALDECK_HAVE_OPENH264

MDR_TEST(h264_encoder_unavailable_without_openh264) {
    MDR_CHECK(!H264Encoder::isAvailable());
    H264Encoder encoder;
    MDR_CHECK(!encoder.initialize(64, 48, 30, 1000000));
}

#endif // DUALDECK_HAVE_OPENH264
