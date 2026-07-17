#include "melonds_remote/adapter/fake/fake_adapter_base.h"

namespace melonds_remote::adapter::fake {

FakeAdapterBase::FakeAdapterBase(AdapterCapabilities capabilities)
    : capabilities_(std::move(capabilities)) {}

AdapterCapabilities FakeAdapterBase::capabilities() const {
    return capabilities_;
}

SessionState FakeAdapterBase::currentState() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

void FakeAdapterBase::applyGenericInput(const GenericInputState& state) {
    std::lock_guard<std::mutex> lock(mutex_);
    lastInput_ = state;
    released_ = false;
}

void FakeAdapterBase::releaseAllInputs() {
    std::lock_guard<std::mutex> lock(mutex_);
    lastInput_ = GenericInputState{};
    released_ = true;
}

bool FakeAdapterBase::latestFrame(const std::string& surfaceId, SurfaceFrame& outFrame) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = latestFrames_.find(surfaceId);
    if (it == latestFrames_.end()) {
        return false;
    }
    outFrame = it->second;
    return true;
}

void FakeAdapterBase::pushFrame(const std::string& surfaceId, std::vector<uint8_t> pixels) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t index = nextFrameIndex_[surfaceId]++;
    SurfaceFrame frame;
    frame.surfaceId = surfaceId;
    frame.frameIndex = index;
    frame.pixels = std::move(pixels);
    latestFrames_[surfaceId] = std::move(frame);
}

bool FakeAdapterBase::setState(SessionState state) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!isValidTransition(state_, state)) {
        return false;
    }
    state_ = state;
    return true;
}

GenericInputState FakeAdapterBase::lastAppliedInput() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastInput_;
}

bool FakeAdapterBase::isReleased() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return released_;
}

} // namespace melonds_remote::adapter::fake
