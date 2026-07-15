#pragma once

// Seam between the network layer and wherever the bottom-screen pixels come
// from. Today this is SyntheticFrameSource (a generated test pattern); a
// future MelonDSFrameSource will read GPU::GetFramebuffers() from the
// emulation thread, copy the bottom buffer into its own bounded slot, and
// expose it here (see docs/melonds-integration-analysis.md section 1).

#include <cstddef>
#include <cstdint>
#include <vector>

namespace melonds_remote::host {

inline constexpr int kFrameWidth = 256;
inline constexpr int kFrameHeight = 192;
inline constexpr int kFrameBytesPerPixel = 4; // BGRA8888, matching melonDS's software renderer output
inline constexpr size_t kFrameSizeBytes =
    static_cast<size_t>(kFrameWidth) * static_cast<size_t>(kFrameHeight) * kFrameBytesPerPixel;

class IFrameSource {
public:
    virtual ~IFrameSource() = default;

    // Fills `outFrame` with the most recent available frame (resized to
    // kFrameSizeBytes if needed) and returns true, or returns false if no
    // frame is available yet. Never blocks -- this exists so a network
    // thread can poll it without stalling on the frame producer, per the
    // "latest-frame-wins" / bounded-queue requirement (spec section 15/16).
    virtual bool getLatestFrame(std::vector<uint8_t>& outFrame) = 0;
};

} // namespace melonds_remote::host
