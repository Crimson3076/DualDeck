#pragma once

#include <atomic>
#include <mutex>
#include <thread>

#include "host/frame_source.h"

namespace melonds_remote::host {

// Generates a moving test pattern at a fixed rate on its own thread and
// exposes the latest completed frame through a single-slot, mutex-protected
// buffer -- the same "one-frame queue, latest wins, never block the
// producer" shape a real melonDS frame source will use (spec section 16).
// This lets the rest of the host (and the client) be developed and tested
// before a melonDS fork is wired in.
class SyntheticFrameSource : public IFrameSource {
public:
    explicit SyntheticFrameSource(int targetFps = 60);
    ~SyntheticFrameSource() override;

    SyntheticFrameSource(const SyntheticFrameSource&) = delete;
    SyntheticFrameSource& operator=(const SyntheticFrameSource&) = delete;

    void start();
    void stop();

    bool getLatestFrame(std::vector<uint8_t>& outFrame) override;

private:
    void generatorLoop();
    void renderPattern(std::vector<uint8_t>& buffer, uint64_t frameIndex) const;

    int targetFps_;
    std::atomic<bool> running_{false};
    std::thread thread_;

    std::mutex bufferMutex_;
    std::vector<uint8_t> latestFrame_;
    bool hasFrame_ = false;
};

} // namespace melonds_remote::host
