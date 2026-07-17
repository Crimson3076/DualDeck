#include "melonds_remote/adapter/synthetic/synthetic_emulator_adapter.h"

#include <chrono>

namespace melonds_remote::adapter::synthetic {

namespace {
constexpr int kWidth = 256;
constexpr int kHeight = 192;
constexpr size_t kFrameSizeBytes = static_cast<size_t>(kWidth) * static_cast<size_t>(kHeight) * 4;
constexpr const char* kSurfaceId = "synthetic";

AdapterCapabilities buildCapabilities() {
    AdapterCapabilities caps;
    caps.system = melonds_remote::SystemIdentity{"synthetic", "Synthetic Test System"};
    caps.adapter =
        melonds_remote::AdapterIdentity{"synthetic-ipc", "Synthetic IPC Adapter (test fixture)", "0.0.1"};

    VideoSurfaceDescriptor surface;
    surface.surfaceId = kSurfaceId;
    surface.role = SurfaceRole::Bottom;
    surface.width = kWidth;
    surface.height = kHeight;
    surface.touchSupported = true;
    surface.touchRangeX = kWidth - 1;
    surface.touchRangeY = kHeight - 1;
    surface.required = true;
    surface.remotelyDisplayed = true;
    caps.surfaces = {surface};

    return caps;
}
} // namespace

SyntheticEmulatorAdapter::SyntheticEmulatorAdapter(int targetFps) : targetFps_(targetFps) {}

SyntheticEmulatorAdapter::~SyntheticEmulatorAdapter() {
    stop();
}

void SyntheticEmulatorAdapter::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    thread_ = std::thread(&SyntheticEmulatorAdapter::generatorLoop, this);
}

void SyntheticEmulatorAdapter::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

void SyntheticEmulatorAdapter::renderPattern(std::vector<uint8_t>& pixels, uint64_t frameIndex) const {
    pixels.resize(kFrameSizeBytes);

    // A moving diagonal gradient, in BGRA byte order (matching
    // protocol.h's existing VideoFrame convention, though this frame
    // never actually reaches that channel in this phase -- it's only
    // ever consumed over the new adapter IPC channel). Simple and
    // cheap; the point is "a real, changing frame is being produced,"
    // not visual fidelity.
    int shift = static_cast<int>(frameIndex % 256);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            size_t offset = (static_cast<size_t>(y) * kWidth + static_cast<size_t>(x)) * 4;
            pixels[offset + 0] = static_cast<uint8_t>((x + shift) & 0xFF);       // B
            pixels[offset + 1] = static_cast<uint8_t>((y + shift) & 0xFF);       // G
            pixels[offset + 2] = static_cast<uint8_t>((x + y + shift) & 0xFF);   // R
            pixels[offset + 3] = 0xFF;                                          // A/unused
        }
    }
}

void SyntheticEmulatorAdapter::generatorLoop() {
    uint64_t frameIndex = 0;
    const auto interval = std::chrono::microseconds(1'000'000 / (targetFps_ > 0 ? targetFps_ : 60));

    while (running_.load()) {
        auto tickStart = std::chrono::steady_clock::now();

        std::vector<uint8_t> pixels;
        renderPattern(pixels, frameIndex);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            latestFrame_.surfaceId = kSurfaceId;
            latestFrame_.frameIndex = frameIndex;
            latestFrame_.pixels = std::move(pixels);
            hasFrame_ = true;
        }
        ++frameIndex;

        auto elapsed = std::chrono::steady_clock::now() - tickStart;
        auto remaining = interval - std::chrono::duration_cast<std::chrono::microseconds>(elapsed);
        if (remaining.count() > 0) {
            std::this_thread::sleep_for(remaining);
        }
    }
}

AdapterCapabilities SyntheticEmulatorAdapter::capabilities() const {
    static const AdapterCapabilities caps = buildCapabilities();
    return caps;
}

SessionState SyntheticEmulatorAdapter::currentState() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

void SyntheticEmulatorAdapter::applyGenericInput(const GenericInputState& state) {
    std::lock_guard<std::mutex> lock(mutex_);
    lastInput_ = state;
}

void SyntheticEmulatorAdapter::releaseAllInputs() {
    std::lock_guard<std::mutex> lock(mutex_);
    lastInput_ = GenericInputState{};
}

bool SyntheticEmulatorAdapter::latestFrame(const std::string& surfaceId, SurfaceFrame& outFrame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!hasFrame_ || surfaceId != kSurfaceId) return false;
    outFrame = latestFrame_;
    return true;
}

} // namespace melonds_remote::adapter::synthetic
