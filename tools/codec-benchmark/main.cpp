// Latency-audit follow-up (2026-08-26 DualDeck-vs-Vanilla audit, item 6 of
// the agreed implementation order): a real, repeatable JPEG-vs-H.264
// benchmark using this project's own encode/decode code -- H264Encoder/
// H264Decoder and the same JPEG compress/decompress calls net_server.cpp/
// net_client.cpp make -- against synthetic content at the resolutions this
// project actually streams (DS 256x192, 3DS 320x240, Cemu's GamePad
// 854x480). Not a pass/fail test (see client/tests/ and
// host/remote-server/tests/ for those): a standalone dev tool for
// answering "is H.264 actually competitive with JPEG here," with real
// numbers, on whatever machine it's run on -- not an estimate.
//
// Caveat this tool cannot remove: the test content below is synthetic (a
// gradient background, a few solid-color UI-like panels, and a moving
// block simulating on-screen motion), not captured from a real DS/3DS/Cemu
// session. Real game/UI content compresses differently (text and sharp
// pixel-art edges in particular stress JPEG's block-DCT harder than this
// tool's smooth gradient does -- see net_server.cpp's own
// compressFrameBgraToJpeg() comment on exactly that finding from real
// DS/3DS testing). Treat this tool's numbers as a real, repeatable
// *relative* comparison between the two codecs on one machine, not an
// absolute prediction of Steam Deck / Bazzite performance -- re-run it
// there for that.

#include "host/h264_encoder.h"
#include "melonds_remote/protocol.h"

#include <turbojpeg.h>

#ifdef DUALDECK_HAVE_OPENH264
#include "h264_decoder.h"
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace melonds_remote;
using namespace melonds_remote::host;

namespace {

struct Resolution {
    const char* label;
    int width;
    int height;
};

// The three real surface sizes this project actually streams today (see
// docs/architecture.md / adapter_contract.h) -- DS's fixed native size,
// 3DS's fixed native size, and Cemu's default GamePad/DRC size (real
// per-title Wii U render targets can differ, but this is the documented
// common case, see CemuAdapter.h's kGamePadDefaultWidth/Height in
// host/cemu-patches/).
constexpr Resolution kResolutions[] = {
    {"DS (256x192)", 256, 192},
    {"3DS (320x240)", 320, 240},
    {"Cemu GamePad (854x480)", 854, 480},
};

constexpr int kFrameCount = 30; // one second at kAssumedCaptureFps=30, matching net_server.cpp's own assumption
constexpr int kAssumedFps = 30;

// Mirrors net_server.cpp's defaultVideoQualityForFrameSize() -- duplicated
// here (that one has internal linkage) rather than exposed, since it's a
// small, stable, already-documented formula; keep in sync if that
// function's own thresholds ever change.
int defaultVideoQualityForFrameSize(int width, int height) {
    constexpr int kLargeSurfacePixelThreshold = 150'000;
    constexpr int kLargeSurfaceQuality = 60;
    constexpr int kConfiguredDefault = 80;
    const int pixels = width * height;
    return pixels > kLargeSurfacePixelThreshold ? std::min(kConfiguredDefault, kLargeSurfaceQuality)
                                                 : kConfiguredDefault;
}

// Mirrors net_server.cpp's h264TargetBitrateBps() exactly -- same
// duplication rationale as defaultVideoQualityForFrameSize() above.
int h264TargetBitrateBps(int quality, int width, int height, int fps) {
    const double clampedQuality = std::clamp(quality, 1, 100) / 100.0;
    const double bitsPerPixelPerFrame = 0.05 + clampedQuality * 0.25;
    const double bitrate = static_cast<double>(width) * static_cast<double>(height) * bitsPerPixelPerFrame * fps;
    return static_cast<int>(std::clamp(bitrate, 250'000.0, 20'000'000.0));
}

// Generates frame `frameIndex` of a synthetic kFrameCount-frame sequence --
// see this file's own top comment for what it represents and why. Not a
// substitute for real captured content, just varied enough (a moving block
// plus a couple of static high-contrast panels) that P-frames have real
// work to do frame to frame instead of every one being a trivial skip.
std::vector<uint8_t> makeSyntheticFrame(int width, int height, int frameIndex) {
    std::vector<uint8_t> frame(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint8_t* px = &frame[(static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4];
            px[0] = static_cast<uint8_t>(x * 255 / std::max(width - 1, 1));       // B
            px[1] = static_cast<uint8_t>(y * 255 / std::max(height - 1, 1));      // G
            px[2] = static_cast<uint8_t>(((x + y) * 255) / std::max(width + height - 2, 1)); // R
            px[3] = 0xFF;
        }
    }

    // Two static high-contrast "UI panel" rectangles -- sharper edges than
    // the smooth gradient alone, closer (though still not equal) to real
    // GamePad UI content's compression behavior.
    auto fillRect = [&](int rx, int ry, int rw, int rh, uint8_t b, uint8_t g, uint8_t r) {
        for (int y = std::max(ry, 0); y < std::min(ry + rh, height); ++y) {
            for (int x = std::max(rx, 0); x < std::min(rx + rw, width); ++x) {
                uint8_t* px = &frame[(static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4];
                px[0] = b; px[1] = g; px[2] = r; px[3] = 0xFF;
            }
        }
    };
    fillRect(width / 20, height / 20, width / 4, height / 8, 0x20, 0x20, 0x20);
    fillRect(width - width / 4 - width / 20, height - height / 8 - height / 20, width / 4, height / 8, 0xF0, 0xF0, 0xF0);

    // A moving block simulating on-screen motion, so H.264's P-frames
    // aren't all trivial skips (bEnableFrameSkip could otherwise make this
    // benchmark unrealistically favorable to H.264 -- see h264_encoder.cpp's
    // own comment on why frame skip is enabled).
    int blockSize = std::max(width, height) / 8;
    int rangeX = std::max(width - blockSize, 1);
    int rangeY = std::max(height - blockSize, 1);
    int bx = (frameIndex * 7) % rangeX;
    int by = (frameIndex * 5) % rangeY;
    fillRect(bx, by, blockSize, blockSize, 0x30, 0xC0, 0xE0);

    return frame;
}

struct MinMaxAvg {
    uint64_t count = 0;
    double sum = 0.0;
    double minV = 1e18;
    double maxV = 0.0;
    void record(double v) {
        ++count;
        sum += v;
        minV = std::min(minV, v);
        maxV = std::max(maxV, v);
    }
    double avg() const { return count ? sum / static_cast<double>(count) : 0.0; }
};

double microsSince(std::chrono::steady_clock::time_point start) {
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
}

void runJpegBenchmark(const Resolution& res) {
    int quality = defaultVideoQualityForFrameSize(res.width, res.height);
    // Same subsampling rule net_server.cpp's compressFrameBgraToJpeg() uses.
    TJSAMP subsampling = quality >= 90 ? TJSAMP_444 : TJSAMP_420;

    tjhandle compressor = tjInitCompress();
    tjhandle decompressor = tjInitDecompress();

    MinMaxAvg compressUs, decompressUs, sizeBytes;
    for (int i = 0; i < kFrameCount; ++i) {
        auto frame = makeSyntheticFrame(res.width, res.height, i);

        unsigned char* jpegBuf = nullptr;
        unsigned long jpegSize = 0;
        auto t0 = std::chrono::steady_clock::now();
        tjCompress2(compressor, frame.data(), res.width, 0, res.height, TJPF_BGRA, &jpegBuf, &jpegSize, subsampling,
                    quality, TJFLAG_FASTDCT);
        compressUs.record(microsSince(t0));
        sizeBytes.record(static_cast<double>(jpegSize));

        std::vector<uint8_t> decoded(static_cast<size_t>(res.width) * res.height * 4);
        auto t1 = std::chrono::steady_clock::now();
        tjDecompress2(decompressor, jpegBuf, jpegSize, decoded.data(), res.width, 0, res.height, TJPF_BGRA,
                      TJFLAG_FASTDCT);
        decompressUs.record(microsSince(t1));

        tjFree(jpegBuf);
    }

    tjDestroy(compressor);
    tjDestroy(decompressor);

    std::printf("  JPEG  (quality=%d, %s): encode avg=%.0fus min=%.0fus max=%.0fus | decode avg=%.0fus "
                "min=%.0fus max=%.0fus | size avg=%.1fKB\n",
                quality, quality >= 90 ? "4:4:4" : "4:2:0", compressUs.avg(), compressUs.minV, compressUs.maxV,
                decompressUs.avg(), decompressUs.minV, decompressUs.maxV, sizeBytes.avg() / 1024.0);
}

#ifdef DUALDECK_HAVE_OPENH264
void runH264Benchmark(const Resolution& res) {
    int quality = defaultVideoQualityForFrameSize(res.width, res.height);
    int targetBitrate = h264TargetBitrateBps(quality, res.width, res.height, kAssumedFps);

    H264Encoder encoder;
    if (!encoder.initialize(res.width, res.height, kAssumedFps, targetBitrate)) {
        std::printf("  H264  (quality=%d): initialize() failed\n", quality);
        return;
    }
    melonds_remote::client::H264Decoder decoder;

    MinMaxAvg encodeUs, decodeUs, keyframeSizeBytes, deltaSizeBytes;
    int keyframeCount = 0, deltaFrameCount = 0, skippedCount = 0;

    for (int i = 0; i < kFrameCount; ++i) {
        auto frame = makeSyntheticFrame(res.width, res.height, i);

        ByteBuffer annexB;
        bool isKeyframe = false;
        auto t0 = std::chrono::steady_clock::now();
        bool ok = encoder.encodeFrame(frame.data(), res.width, res.height, annexB, isKeyframe);
        double encodeElapsed = microsSince(t0);
        if (!ok) {
            std::printf("  H264  (quality=%d): encodeFrame() failed on frame %d\n", quality, i);
            return;
        }
        if (annexB.empty()) {
            ++skippedCount; // rate-control skip -- see H264Encoder::encodeFrame()'s own comment
            continue;
        }
        encodeUs.record(encodeElapsed);
        if (isKeyframe) {
            ++keyframeCount;
            keyframeSizeBytes.record(static_cast<double>(annexB.size()));
        } else {
            ++deltaFrameCount;
            deltaSizeBytes.record(static_cast<double>(annexB.size()));
        }

        std::vector<uint8_t> decodedBgra;
        int decodedWidth = 0, decodedHeight = 0;
        bool hasFrame = false;
        auto t1 = std::chrono::steady_clock::now();
        bool decoded = decoder.decodeFrame(annexB.data(), annexB.size(), decodedBgra, decodedWidth, decodedHeight,
                                            hasFrame);
        if (decoded && !hasFrame) {
            // Same production drain fix net_client.cpp's videoReceiveLoop()
            // now applies -- see its own comment.
            decoded = decoder.decodeFrame(nullptr, 0, decodedBgra, decodedWidth, decodedHeight, hasFrame);
        }
        decodeUs.record(microsSince(t1));
        if (!decoded || !hasFrame) {
            std::printf("  H264  (quality=%d): decodeFrame() failed to produce a picture on frame %d\n", quality, i);
            return;
        }
    }

    double avgSizeBytes =
        (keyframeSizeBytes.sum + deltaSizeBytes.sum) / static_cast<double>(std::max<uint64_t>(1, keyframeCount + deltaFrameCount));
    std::printf("  H264  (quality=%d, target=%.1fMbps): encode avg=%.0fus min=%.0fus max=%.0fus | decode "
                "avg=%.0fus min=%.0fus max=%.0fus | size avg=%.1fKB (I-frame avg=%.1fKB x%d, P-frame "
                "avg=%.1fKB x%d, skipped=%d)\n",
                quality, targetBitrate / 1'000'000.0, encodeUs.avg(), encodeUs.minV, encodeUs.maxV, decodeUs.avg(),
                decodeUs.minV, decodeUs.maxV, avgSizeBytes / 1024.0, keyframeSizeBytes.avg() / 1024.0, keyframeCount,
                deltaSizeBytes.avg() / 1024.0, deltaFrameCount, skippedCount);
}
#endif // DUALDECK_HAVE_OPENH264

} // namespace

int main() {
    std::printf("DualDeck codec benchmark -- %d synthetic frames/resolution, matching this project's real "
                "quality/bitrate selection (net_server.cpp's defaultVideoQualityForFrameSize()/"
                "h264TargetBitrateBps()).\n",
                kFrameCount);
#ifndef DUALDECK_HAVE_OPENH264
    std::printf("(built without OpenH264 -- H.264 side will be skipped)\n");
#endif
    for (const auto& res : kResolutions) {
        std::printf("\n%s:\n", res.label);
        runJpegBenchmark(res);
#ifdef DUALDECK_HAVE_OPENH264
        runH264Benchmark(res);
#endif
    }
    return 0;
}
