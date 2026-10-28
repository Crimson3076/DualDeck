// chooseAutoVideoCodec(): which codec VIDEO CODEC: AUTO lands on.

#include "net_server_internal.h"
#include "test_framework.h"

using namespace dualdeck;
using namespace dualdeck::host::net_detail;

namespace {
constexpr uint8_t kAllCodecs = kVideoCodecBit_Jpeg | kVideoCodecBit_H264 | kVideoCodecBit_PyroWave | kVideoCodecFlag_Auto;
// Cemu's GamePad surface at the large-surface default quality.
constexpr uint16_t kCemuWidth = 854;
constexpr uint16_t kCemuHeight = 480;
constexpr int kCemuQuality = 60;
} // namespace

MDR_TEST(auto_codec_picks_pyrowave_when_the_link_has_room) {
    const uint32_t needed = pyrowavePeakKbps(kCemuQuality, kCemuWidth, kCemuHeight);
    MDR_CHECK(chooseAutoVideoCodec(kAllCodecs, true, true, HostMode::Emulation, kCemuQuality, kCemuWidth,
                                   kCemuHeight, needed * 2) == VideoCodec::PyroWave);
}

MDR_TEST(auto_codec_falls_back_to_h264_on_a_slow_or_unmeasured_link) {
    const uint32_t needed = pyrowavePeakKbps(kCemuQuality, kCemuWidth, kCemuHeight);
    MDR_CHECK(chooseAutoVideoCodec(kAllCodecs, true, true, HostMode::Emulation, kCemuQuality, kCemuWidth,
                                   kCemuHeight, needed) == VideoCodec::H264);
    MDR_CHECK(chooseAutoVideoCodec(kAllCodecs, true, true, HostMode::Emulation, kCemuQuality, kCemuWidth,
                                   kCemuHeight, 0) == VideoCodec::H264);
}

MDR_TEST(auto_codec_uses_h264_for_host_control) {
    MDR_CHECK(chooseAutoVideoCodec(kAllCodecs, true, true, HostMode::HostControl, kCemuQuality, kCemuWidth,
                                   kCemuHeight, 10'000'000) == VideoCodec::H264);
}

MDR_TEST(auto_codec_needs_both_sides_to_support_a_codec) {
    // Host without Vulkan: H.264 even on a fast link.
    MDR_CHECK(chooseAutoVideoCodec(kAllCodecs, true, false, HostMode::Emulation, 80, 256, 192, 10'000'000) ==
              VideoCodec::H264);
    // Client without PyroWave or H.264 decode: JPEG.
    MDR_CHECK(chooseAutoVideoCodec(kVideoCodecBit_Jpeg | kVideoCodecFlag_Auto, true, true, HostMode::Emulation, 80,
                                   256, 192, 10'000'000) == VideoCodec::Jpeg);
    // Host without OpenH264, slow link: JPEG.
    MDR_CHECK(chooseAutoVideoCodec(kAllCodecs, false, true, HostMode::Emulation, 80, 256, 192, 100) ==
              VideoCodec::Jpeg);
}

MDR_TEST(auto_codec_small_ds_surface_fits_on_modest_wifi) {
    // DS bottom screen at quality 80: well under 20 Mbit/s even at the cap.
    MDR_CHECK(pyrowavePeakKbps(80, 256, 192) < 10'000);
    MDR_CHECK(chooseAutoVideoCodec(kAllCodecs, true, true, HostMode::Emulation, 80, 256, 192, 20'000) ==
              VideoCodec::PyroWave);
}
