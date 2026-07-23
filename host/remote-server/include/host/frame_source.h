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
    // kFrameSizeBytes if needed), `outFrameIndex` with a monotonically
    // increasing index identifying it (starting at 0 for the first frame
    // ever produced), and `outWidth`/`outHeight` with THIS frame's actual
    // pixel dimensions, and returns true; or returns false if no frame is
    // available yet. Never blocks -- this exists so a network thread can
    // poll it without stalling on the frame producer, per the
    // "latest-frame-wins" / bounded-queue requirement (spec section
    // 15/16). The frame index lets a caller that polls slower than frames
    // are produced compute how many frames it skipped (spec section 14:
    // log dropped frames), without needing the frames themselves.
    //
    // outWidth/outHeight (added alongside AdapterBridge's real fix for a
    // shipped bug -- see SurfaceFrame's comment in adapter_contract.h)
    // are the authoritative per-frame size a caller must use to interpret
    // `outFrame`'s bytes (e.g. as the width/pitch passed to a JPEG
    // encoder) -- NOT necessarily the same value frameDimensions() below
    // reports, which is only a coarse, once-negotiated estimate. Sources
    // with a genuinely fixed frame size (this default, SyntheticFrameSource,
    // NoAdapterFrameSource) just echo frameDimensions() here every call.
    virtual bool getLatestFrame(std::vector<uint8_t>& outFrame, uint64_t& outFrameIndex,
                                uint16_t& outWidth, uint16_t& outHeight) = 0;

    // The pixel dimensions of frames this source produces, reported to a
    // connecting client in HelloAck (see net_server.cpp) so it can size its
    // receive buffer/texture correctly instead of assuming DS's fixed
    // 256x192 -- a real gap found when AzaharAdapter (320x240, GitHub
    // issue #28's 3DS follow-on) was first run on real hardware: every
    // frame it produced was silently rejected downstream because nothing
    // ever told the client it wasn't DS-sized. Defaults to
    // kFrameWidth/kFrameHeight so every pre-existing source (DS-only, and
    // genuinely fixed at that resolution) keeps working unchanged.
    virtual void frameDimensions(uint16_t& outWidth, uint16_t& outHeight) const {
        outWidth = static_cast<uint16_t>(kFrameWidth);
        outHeight = static_cast<uint16_t>(kFrameHeight);
    }
};

} // namespace melonds_remote::host
