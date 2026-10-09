// Unit tests for pyrowave_decoder.h. PyroWave's decoder runs on far more
// Vulkan devices than its encoder does (any subgroup size from wave4 to
// wave128, so Mesa's software lavapipe works) -- the decoder-only tests
// here feed it hand-built frames straight from PyroWave's frozen
// bitstream v1 spec. Those frames carry zero coefficient blocks, which
// PyroWaveDecoder now resolves on the CPU (see decodeFrame()'s own
// comment for why), so they cover header parsing, resizing and the
// I420->BGRA output path, not GPU decode itself. The full round trip through this
// project's own host::PyroWaveEncoder additionally needs an encoder-
// capable device, so it checks for one at runtime and passes trivially
// otherwise (same convention as host/remote-server/tests/
// test_pyrowave_encoder.cpp).

#include "host/pyrowave_encoder.h"
#include "pyrowave_decoder.h"
#include "test_framework.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace melonds_remote;
using namespace melonds_remote::client;

namespace {

void appendU32Le(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

// A complete, spec-valid PyroWave frame with zero coefficient blocks:
// just a start-of-frame BitstreamSequenceHeader (extended=1,
// code=START_OF_FRAME, 4:2:0) declaring total_blocks=0. Every wavelet
// coefficient decodes as 0, so the output is a flat mid-level image --
// enough to prove header parsing, decoder creation, GPU decode, CPU
// readback, and I420->BGRA conversion all line up.
std::vector<uint8_t> makeEmptyFrame(int width, int height, uint32_t sequence) {
    std::vector<uint8_t> out;
    appendU32Le(out, static_cast<uint32_t>(width - 1) | (static_cast<uint32_t>(height - 1) << 14) |
                         ((sequence & 7u) << 28) | (1u << 31));
    appendU32Le(out, 0u);
    return out;
}

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

bool skip(const char* testName, const char* why) {
    std::fprintf(stderr, "  [%s] skipped: %s\n", testName, why);
    return true;
}

} // namespace

MDR_TEST(pyrowave_decoder_decode_garbage_fails_cleanly) {
    PyroWaveDecoder decoder;
    std::vector<uint8_t> garbage = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    std::vector<uint8_t> outBgra;
    int width = 0, height = 0;
    bool hasFrame = true;
    // Not a start-of-frame header (extended bit clear), so rejected
    // before ever touching Vulkan -- on every build.
    MDR_CHECK(!decoder.decodeFrame(garbage.data(), garbage.size(), outBgra, width, height, hasFrame));
    MDR_CHECK(!hasFrame);
}

MDR_TEST(pyrowave_decoder_rejects_truncated_and_odd_sized_headers) {
    PyroWaveDecoder decoder;
    std::vector<uint8_t> outBgra;
    int width = 0, height = 0;
    bool hasFrame = true;
    auto frame = makeEmptyFrame(64, 64, 0);
    MDR_CHECK(!decoder.decodeFrame(frame.data(), 7, outBgra, width, height, hasFrame));
    // 4:2:0 requires even dimensions (PyroWave bitstream v1).
    auto odd = makeEmptyFrame(63, 64, 0);
    MDR_CHECK(!decoder.decodeFrame(odd.data(), odd.size(), outBgra, width, height, hasFrame));
    MDR_CHECK(!hasFrame);
}

MDR_TEST(pyrowave_decoder_decodes_an_empty_frame_to_flat_gray) {
    if (!PyroWaveDecoder::isAvailable()) {
        skip("pyrowave_decoder_decodes_an_empty_frame_to_flat_gray", "no PyroWave-capable Vulkan device");
        return;
    }
    PyroWaveDecoder decoder;
    auto frame = makeEmptyFrame(96, 64, 0);
    std::vector<uint8_t> outBgra;
    int width = 0, height = 0;
    bool hasFrame = false;
    MDR_CHECK(decoder.decodeFrame(frame.data(), frame.size(), outBgra, width, height, hasFrame));
    MDR_CHECK(hasFrame);
    MDR_CHECK_EQ(width, 96);
    MDR_CHECK_EQ(height, 64);
    MDR_CHECK_EQ(outBgra.size(), static_cast<size_t>(96 * 64 * 4));
    // All-zero coefficients: every pixel identical and neutral (Y and
    // both chroma at mid-scale), so B == G == R, roughly mid-gray.
    const uint8_t b = outBgra[0], g = outBgra[1], r = outBgra[2];
    MDR_CHECK(std::abs(static_cast<int>(b) - static_cast<int>(g)) <= 2);
    MDR_CHECK(std::abs(static_cast<int>(r) - static_cast<int>(g)) <= 2);
    MDR_CHECK(g > 100 && g < 160);
    bool uniform = true;
    for (size_t i = 0; i < outBgra.size(); i += 4) {
        if (outBgra[i] != b || outBgra[i + 1] != g || outBgra[i + 2] != r || outBgra[i + 3] != 0xFF) {
            uniform = false;
            break;
        }
    }
    MDR_CHECK(uniform);
}

MDR_TEST(pyrowave_decoder_follows_a_mid_session_resolution_change) {
    if (!PyroWaveDecoder::isAvailable()) {
        skip("pyrowave_decoder_follows_a_mid_session_resolution_change", "no PyroWave-capable Vulkan device");
        return;
    }
    PyroWaveDecoder decoder;
    std::vector<uint8_t> outBgra;
    int width = 0, height = 0;
    bool hasFrame = false;
    auto first = makeEmptyFrame(64, 64, 0);
    MDR_CHECK(decoder.decodeFrame(first.data(), first.size(), outBgra, width, height, hasFrame));
    MDR_CHECK(hasFrame);
    MDR_CHECK_EQ(width, 64);

    auto second = makeEmptyFrame(128, 96, 1);
    MDR_CHECK(decoder.decodeFrame(second.data(), second.size(), outBgra, width, height, hasFrame));
    MDR_CHECK(hasFrame);
    MDR_CHECK_EQ(width, 128);
    MDR_CHECK_EQ(height, 96);
    MDR_CHECK_EQ(outBgra.size(), static_cast<size_t>(128 * 96 * 4));
}

MDR_TEST(pyrowave_decoder_decodes_this_projects_own_encoder_output) {
    if (!PyroWaveDecoder::isAvailable() || !host::PyroWaveEncoder::isAvailable()) {
        skip("pyrowave_decoder_decodes_this_projects_own_encoder_output",
             "no PyroWave encoder-capable Vulkan device");
        return;
    }
    constexpr int kWidth = 256, kHeight = 192;
    host::PyroWaveEncoder encoder;
    // Generous budget (~4 bits/pixel) so quantization error stays small
    // enough for a tight average-difference bound.
    MDR_CHECK(encoder.initialize(kWidth, kHeight, kWidth * kHeight / 2));
    auto source = makeTestFrameBgra(kWidth, kHeight);

    PyroWaveDecoder decoder;
    std::vector<uint8_t> outBgra;
    int width = 0, height = 0;
    bool hasFrame = false;
    // A few frames in a row, so the wrapping sequence counter advancing
    // between frames is exercised too, not just a single first frame.
    for (int i = 0; i < 3; ++i) {
        ByteBuffer bitstream;
        MDR_CHECK(encoder.encodeFrame(source.data(), kWidth, kHeight, bitstream));
        MDR_CHECK(decoder.decodeFrame(bitstream.data(), bitstream.size(), outBgra, width, height, hasFrame));
        MDR_CHECK(hasFrame);
    }
    MDR_CHECK_EQ(width, kWidth);
    MDR_CHECK_EQ(height, kHeight);
    MDR_CHECK_EQ(outBgra.size(), source.size());

    uint64_t totalDiff = 0;
    for (size_t i = 0; i < source.size(); i += 4) {
        for (size_t c = 0; c < 3; ++c) {
            totalDiff += static_cast<uint64_t>(std::abs(static_cast<int>(source[i + c]) - static_cast<int>(outBgra[i + c])));
        }
    }
    const double avgDiff = static_cast<double>(totalDiff) / static_cast<double>(kWidth * kHeight * 3);
    // Tighter than test_h264_decoder.cpp's own < 20 bound, given the
    // generous budget above -- still covers lossy compression plus the
    // BT.601 studio-range conversion each way.
    MDR_CHECK(avgDiff < 12.0);
}
