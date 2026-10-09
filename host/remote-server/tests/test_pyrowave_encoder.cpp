// Unit tests for pyrowave_encoder.h. The real-encode tests need more
// than a PyroWave build: they need a Vulkan device whose subgroup-size
// control can force wave16/32/64 (see PyroWaveEncoder::isAvailable()),
// which a GPU-less CI runner's software lavapipe can't -- so those tests
// check isAvailable() at runtime and pass trivially (with a note on
// stderr) when it's false, rather than failing on hardware this project
// doesn't control. client/tests/test_pyrowave_decoder.cpp holds the
// full encode->decode round trip, under the same runtime gate.

#include "host/pyrowave_encoder.h"
#include "test_framework.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace dualdeck;
using namespace dualdeck::host;

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

uint32_t readU32Le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

bool skipWithoutCapableDevice(const char* testName) {
    if (PyroWaveEncoder::isAvailable()) return false;
    std::fprintf(stderr, "  [%s] skipped: no PyroWave-capable Vulkan device (or no PyroWave in this build)\n",
                 testName);
    return true;
}

} // namespace

MDR_TEST(pyrowave_encoder_uninitialized_encode_fails_cleanly) {
    PyroWaveEncoder encoder;
    auto frame = makeTestFrameBgra(64, 64);
    ByteBuffer out;
    MDR_CHECK(!encoder.encodeFrame(frame.data(), 64, 64, out));
    MDR_CHECK(out.empty());
}

MDR_TEST(pyrowave_encoder_rejects_invalid_initialize_arguments) {
    PyroWaveEncoder encoder;
    MDR_CHECK(!encoder.initialize(0, 64, 65536));
    MDR_CHECK(!encoder.initialize(64, 0, 65536));
    MDR_CHECK(!encoder.initialize(64, 64, 0));
}

#ifndef DUALDECK_HAVE_PYROWAVE
MDR_TEST(pyrowave_encoder_unavailable_without_pyrowave) {
    MDR_CHECK(!PyroWaveEncoder::isAvailable());
    PyroWaveEncoder encoder;
    MDR_CHECK(!encoder.initialize(64, 64, 65536));
}
#endif

MDR_TEST(pyrowave_encoder_produces_a_start_of_frame_header_first) {
    if (skipWithoutCapableDevice("pyrowave_encoder_produces_a_start_of_frame_header_first")) return;
    PyroWaveEncoder encoder;
    constexpr int kWidth = 256, kHeight = 192; // DS screen
    MDR_CHECK(encoder.initialize(kWidth, kHeight, 32 * 1024));
    auto frame = makeTestFrameBgra(kWidth, kHeight);
    ByteBuffer out;
    MDR_CHECK(encoder.encodeFrame(frame.data(), kWidth, kHeight, out));
    MDR_CHECK(out.size() > 8);

    // BitstreamSequenceHeader (PyroWave bitstream v1): extended=1,
    // code=START_OF_FRAME(0), dimensions as initialized.
    const uint32_t w0 = readU32Le(out.data());
    const uint32_t w1 = readU32Le(out.data() + 4);
    MDR_CHECK(((w0 >> 31) & 1u) == 1u);
    MDR_CHECK(((w1 >> 24) & 3u) == 0u);
    MDR_CHECK_EQ(static_cast<int>(w0 & 0x3FFFu) + 1, kWidth);
    MDR_CHECK_EQ(static_cast<int>((w0 >> 14) & 0x3FFFu) + 1, kHeight);
}

MDR_TEST(pyrowave_encoder_respects_the_frame_byte_budget) {
    if (skipWithoutCapableDevice("pyrowave_encoder_respects_the_frame_byte_budget")) return;
    PyroWaveEncoder encoder;
    constexpr int kWidth = 256, kHeight = 192;
    constexpr size_t kBudget = 8 * 1024;
    MDR_CHECK(encoder.initialize(kWidth, kHeight, kBudget));
    auto frame = makeTestFrameBgra(kWidth, kHeight);
    ByteBuffer out;
    MDR_CHECK(encoder.encodeFrame(frame.data(), kWidth, kHeight, out));
    // The budget covers coefficient blocks; packetize() adds the 8-byte
    // start-of-frame header on top.
    MDR_CHECK(out.size() <= kBudget + 8);
}

MDR_TEST(pyrowave_encoder_pads_odd_dimensions_to_even) {
    if (skipWithoutCapableDevice("pyrowave_encoder_pads_odd_dimensions_to_even")) return;
    PyroWaveEncoder encoder;
    constexpr int kWidth = 255, kHeight = 191;
    MDR_CHECK(encoder.initialize(kWidth, kHeight, 32 * 1024));
    auto frame = makeTestFrameBgra(kWidth, kHeight);
    ByteBuffer out;
    MDR_CHECK(encoder.encodeFrame(frame.data(), kWidth, kHeight, out));
    MDR_CHECK(out.size() > 8);
    const uint32_t w0 = readU32Le(out.data());
    MDR_CHECK_EQ(static_cast<int>(w0 & 0x3FFFu) + 1, kWidth + 1);
    MDR_CHECK_EQ(static_cast<int>((w0 >> 14) & 0x3FFFu) + 1, kHeight + 1);
}

MDR_TEST(pyrowave_encoder_rejects_a_frame_size_mismatch) {
    if (skipWithoutCapableDevice("pyrowave_encoder_rejects_a_frame_size_mismatch")) return;
    PyroWaveEncoder encoder;
    MDR_CHECK(encoder.initialize(64, 64, 16 * 1024));
    auto frame = makeTestFrameBgra(128, 64);
    ByteBuffer out;
    MDR_CHECK(!encoder.encodeFrame(frame.data(), 128, 64, out));
    MDR_CHECK(out.empty());
}
