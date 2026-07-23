#include "host/synthetic_frame_source.h"

#include <chrono>
#include <cmath>

namespace melonds_remote::host {

SyntheticFrameSource::SyntheticFrameSource(int targetFps) : targetFps_(targetFps) {}

SyntheticFrameSource::~SyntheticFrameSource() {
    stop();
}

void SyntheticFrameSource::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }
    thread_ = std::thread(&SyntheticFrameSource::generatorLoop, this);
}

void SyntheticFrameSource::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool SyntheticFrameSource::getLatestFrame(std::vector<uint8_t>& outFrame, uint64_t& outFrameIndex,
                                          uint16_t& outWidth, uint16_t& outHeight) {
    std::lock_guard<std::mutex> lock(bufferMutex_);
    if (!hasFrame_) {
        return false;
    }
    outFrame = latestFrame_;
    outFrameIndex = latestFrameIndex_;
    frameDimensions(outWidth, outHeight); // fixed size, matches this class's declared kFrameWidth/kFrameHeight
    return true;
}

void SyntheticFrameSource::renderPattern(std::vector<uint8_t>& buffer, uint64_t frameIndex) const {
    buffer.resize(kFrameSizeBytes);

    // A simple animated diagonal gradient plus a bouncing block, in BGRA
    // byte order to match melonDS's software renderer output format
    // (see docs/melonds-integration-analysis.md section 1.1).
    const int blockSize = 24;
    const int travelX = kFrameWidth - blockSize;
    const int travelY = kFrameHeight - blockSize;
    const int period = 2 * (travelX + travelY);
    int pos = static_cast<int>(frameIndex % static_cast<uint64_t>(period > 0 ? period : 1));

    int blockX, blockY;
    if (pos < travelX) {
        blockX = pos;
        blockY = 0;
    } else if (pos < travelX + travelY) {
        blockX = travelX;
        blockY = pos - travelX;
    } else if (pos < 2 * travelX + travelY) {
        blockX = travelX - (pos - travelX - travelY);
        blockY = travelY;
    } else {
        blockX = 0;
        blockY = travelY - (pos - 2 * travelX - travelY);
    }

    for (int y = 0; y < kFrameHeight; ++y) {
        for (int x = 0; x < kFrameWidth; ++x) {
            size_t offset = (static_cast<size_t>(y) * kFrameWidth + static_cast<size_t>(x)) *
                             kFrameBytesPerPixel;

            uint8_t b = static_cast<uint8_t>((x * 255) / kFrameWidth);
            uint8_t g = static_cast<uint8_t>((y * 255) / kFrameHeight);
            uint8_t r = static_cast<uint8_t>(((x + y + static_cast<int>(frameIndex)) % 256));

            bool inBlock = x >= blockX && x < blockX + blockSize &&
                           y >= blockY && y < blockY + blockSize;
            if (inBlock) {
                b = 255;
                g = 255;
                r = 255;
            }

            buffer[offset + 0] = b;
            buffer[offset + 1] = g;
            buffer[offset + 2] = r;
            buffer[offset + 3] = 255;
        }
    }
}

void SyntheticFrameSource::generatorLoop() {
    const auto frameInterval = std::chrono::microseconds(1'000'000 / (targetFps_ > 0 ? targetFps_ : 60));
    uint64_t frameIndex = 0;
    std::vector<uint8_t> scratch;

    while (running_.load()) {
        auto frameStart = std::chrono::steady_clock::now();

        renderPattern(scratch, frameIndex);
        {
            std::lock_guard<std::mutex> lock(bufferMutex_);
            latestFrame_.swap(scratch);
            latestFrameIndex_ = frameIndex;
            hasFrame_ = true;
        }
        ++frameIndex;

        auto elapsed = std::chrono::steady_clock::now() - frameStart;
        auto remaining = frameInterval - std::chrono::duration_cast<std::chrono::microseconds>(elapsed);
        if (remaining.count() > 0) {
            std::this_thread::sleep_for(remaining);
        }
    }
}

} // namespace melonds_remote::host
