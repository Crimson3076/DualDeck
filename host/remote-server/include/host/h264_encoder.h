#pragma once

// H.264 encoder wrapper (protocol v13's optional codec alongside JPEG --
// see docs/known-limitations.md's 2026-08-25 video-codec-negotiation
// entry, and NetServer::selectVideoCodec()'s own comment). Backed by
// OpenH264 (Cisco, BSD-licensed) when this host was built with it
// available -- see host/remote-server/CMakeLists.txt's optional-at-
// configure-time detection, the same pattern as X11/Wayland screen
// capture above it. Not yet wired into NetServer::videoLoop() -- the
// host still only ever advertises/selects VideoCodec::Jpeg, so this
// class exists as a real, independently-tested building block for that
// wiring (a later stage), not a live code path yet.

#include <cstdint>
#include <memory>

#include "melonds_remote/protocol.h"

namespace melonds_remote::host {

class H264Encoder {
public:
    H264Encoder();
    ~H264Encoder();

    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;

    // True if this build was compiled with OpenH264 available. Doesn't
    // depend on initialize() having been called -- safe to check before
    // ever constructing/using one of these, e.g. to decide whether to
    // include VideoCodecBit_H264 in the host's advertised codec set.
    static bool isAvailable();

    // (Re)initializes the encoder for the given frame size/target frame
    // rate/bitrate, tuned for low latency (single temporal/spatial
    // layer, frame skip enabled so the bitrate target is actually
    // enforced under sustained pressure instead of a slow link's queue
    // growing unbounded, a short periodic keyframe interval) rather than
    // maximum-compression offline encoding -- matching every other
    // latency-sensitive choice already made in this project's video
    // pipeline (see docs/known-limitations.md's latency entries).
    // Safe to call again with a different size -- e.g. Cemu's own
    // per-title GamePad resolution changing mid-session (see
    // docs/known-limitations.md's Cemu "sheared/torn" entry for why
    // that's a real, previously-hit case, not a hypothetical one) --
    // tears down and recreates the underlying encoder rather than
    // assuming a fixed size for this object's whole lifetime. Returns
    // false (previous state, if any, torn down either way) on failure,
    // including on a build with no OpenH264.
    bool initialize(int width, int height, int fps, int targetBitrateBps);

    // Encodes one BGRA8888 frame (`width`/`height` must match the size
    // passed to the most recent successful initialize()) into Annex-B
    // bytestream NAL units, appended to outAnnexB (not cleared first, so
    // a caller could batch multiple frames into one buffer, though
    // nothing does that today). outIsKeyframe reports whether this frame
    // included an IDR NAL (always true for the first frame after
    // initialize()/requestKeyframe()). Returns false (outAnnexB
    // untouched) on any encode failure, a width/height mismatch against
    // the initialized size, or if initialize() was never called/failed,
    // or this build has no OpenH264.
    bool encodeFrame(const uint8_t* bgra, int width, int height, ByteBuffer& outAnnexB,
                      bool& outIsKeyframe);

    // Forces the next encodeFrame() call to produce an IDR keyframe --
    // needed after a client (re)joins mid-session, since it can't decode
    // anything before the first IDR it receives. No-op if not
    // initialized or this build has no OpenH264.
    void requestKeyframe();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace melonds_remote::host
