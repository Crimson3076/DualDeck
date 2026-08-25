// Unit tests for h264_decoder.h -- unlike host/remote-server/tests/
// test_h264_encoder.cpp (which round-trips against OpenH264's own raw
// decoder API), this uses this project's own real H264Encoder
// (host/remote-server/include/host/h264_encoder.h, linked in via
// dualdeck_host -- see this directory's CMakeLists.txt) to produce
// genuine Annex-B bytes, feeding them into H264Decoder -- a real round
// trip through both halves of this project's own codec pipeline, not
// just each one independently against OpenH264.

#include "h264_decoder.h"
#include "host/h264_encoder.h"
#include "test_framework.h"

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace melonds_remote;
using namespace melonds_remote::client;

MDR_TEST(h264_decoder_decode_garbage_fails_cleanly) {
    H264Decoder decoder;
    std::vector<uint8_t> garbage = {0x01, 0x02, 0x03, 0x04};
    std::vector<uint8_t> outBgra;
    int width = 0, height = 0;
    bool hasFrame = true;
    // Not a hard requirement that this specific input fails outright
    // (OpenH264's decoder is tolerant of garbage in real streaming use,
    // where a corrupt/partial NAL should never crash the whole
    // connection) -- the real invariant is just "never reports a frame
    // for four random bytes."
    decoder.decodeFrame(garbage.data(), garbage.size(), outBgra, width, height, hasFrame);
    MDR_CHECK(!hasFrame);
}

#ifdef DUALDECK_HAVE_OPENH264

namespace {

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

MDR_TEST(h264_decoder_availability_matches_build_config) {
    MDR_CHECK(H264Decoder::isAvailable());
    MDR_CHECK(host::H264Encoder::isAvailable());
}

MDR_TEST(h264_decoder_decodes_this_projects_own_encoder_output) {
    const int width = 64, height = 48;
    host::H264Encoder encoder;
    MDR_CHECK(encoder.initialize(width, height, 30, 1000000));

    auto sourceFrame = makeTestFrameBgra(width, height);
    ByteBuffer annexB;
    bool isKeyframe = false;
    MDR_CHECK(encoder.encodeFrame(sourceFrame.data(), width, height, annexB, isKeyframe));
    MDR_CHECK(!annexB.empty());
    MDR_CHECK(isKeyframe);

    H264Decoder decoder;
    std::vector<uint8_t> decodedBgra;
    int decodedWidth = 0, decodedHeight = 0;
    bool hasFrame = false;
    MDR_CHECK(decoder.decodeFrame(annexB.data(), annexB.size(), decodedBgra, decodedWidth, decodedHeight, hasFrame));
    if (!hasFrame) {
        // OpenH264's own documented no-delay-decode semantics can defer
        // the very first frame's output to a follow-up call fed no new
        // data -- see H264Decoder::decodeFrame()'s own comment and
        // host/remote-server/tests/test_h264_encoder.cpp's identical
        // fallback against the raw API.
        MDR_CHECK(decoder.decodeFrame(nullptr, 0, decodedBgra, decodedWidth, decodedHeight, hasFrame));
    }
    MDR_CHECK(hasFrame);
    MDR_CHECK_EQ(decodedWidth, width);
    MDR_CHECK_EQ(decodedHeight, height);
    MDR_CHECK_EQ(decodedBgra.size(), sourceFrame.size());

    // Not pixel-exact (H.264 is lossy) -- but the decoded image should
    // be recognizably close to the source gradient, not noise or a
    // solid/degenerate color. Average per-channel absolute difference
    // across every pixel is a simple, real correctness signal: a
    // completely broken color-space conversion (e.g. a channel swap, or
    // stride miscomputed) would blow this bound wide open, while real
    // lossy compression artifacts on a smooth gradient stay small.
    uint64_t totalAbsDiff = 0;
    for (size_t i = 0; i < sourceFrame.size(); ++i) {
        if (i % 4 == 3) continue; // alpha -- always 0xFF on both sides, not meaningful to compare
        totalAbsDiff += static_cast<uint64_t>(
            std::abs(static_cast<int>(sourceFrame[i]) - static_cast<int>(decodedBgra[i])));
    }
    const double avgAbsDiff = static_cast<double>(totalAbsDiff) / static_cast<double>(sourceFrame.size() / 4 * 3);
    MDR_CHECK(avgAbsDiff < 20.0);
}

MDR_TEST(h264_decoder_handles_a_resolution_change_between_encoder_sessions) {
    H264Decoder decoder;

    host::H264Encoder encoderA;
    MDR_CHECK(encoderA.initialize(32, 32, 30, 500000));
    auto frameA = makeTestFrameBgra(32, 32);
    ByteBuffer annexA;
    bool keyA = false;
    MDR_CHECK(encoderA.encodeFrame(frameA.data(), 32, 32, annexA, keyA));

    std::vector<uint8_t> decodedA;
    int widthA = 0, heightA = 0;
    bool hasFrameA = false;
    MDR_CHECK(decoder.decodeFrame(annexA.data(), annexA.size(), decodedA, widthA, heightA, hasFrameA));
    if (!hasFrameA) {
        MDR_CHECK(decoder.decodeFrame(nullptr, 0, decodedA, widthA, heightA, hasFrameA));
    }
    MDR_CHECK(hasFrameA);
    MDR_CHECK_EQ(widthA, 32);
    MDR_CHECK_EQ(heightA, 32);

    // A fresh encoder session (its own SPS/PPS, real Cemu-style
    // resolution change -- see host/remote-server/src/net_server.cpp's
    // videoLoop() comment on h264InitializedWidth/Height) at a different
    // size, decoded by the *same* long-lived decoder instance: OpenH264
    // reports the new resolution itself, no manual reinit needed on the
    // decode side (unlike the encoder, which must be told the size up
    // front).
    host::H264Encoder encoderB;
    MDR_CHECK(encoderB.initialize(48, 32, 30, 500000));
    auto frameB = makeTestFrameBgra(48, 32);
    ByteBuffer annexB;
    bool keyB = false;
    MDR_CHECK(encoderB.encodeFrame(frameB.data(), 48, 32, annexB, keyB));

    std::vector<uint8_t> decodedB;
    int widthB = 0, heightB = 0;
    bool hasFrameB = false;
    MDR_CHECK(decoder.decodeFrame(annexB.data(), annexB.size(), decodedB, widthB, heightB, hasFrameB));
    if (!hasFrameB) {
        MDR_CHECK(decoder.decodeFrame(nullptr, 0, decodedB, widthB, heightB, hasFrameB));
    }
    MDR_CHECK(hasFrameB);
    MDR_CHECK_EQ(widthB, 48);
    MDR_CHECK_EQ(heightB, 32);
}

#else // !DUALDECK_HAVE_OPENH264

MDR_TEST(h264_decoder_unavailable_without_openh264) {
    MDR_CHECK(!H264Decoder::isAvailable());
}

#endif // DUALDECK_HAVE_OPENH264
