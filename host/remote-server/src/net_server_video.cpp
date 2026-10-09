#include "host/net_server.h"

#include "net_server_internal.h"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <optional>
#include <thread>
#include <utility>

#include "host/h264_encoder.h"
#include "host/pyrowave_encoder.h"

namespace dualdeck::host {

using namespace net_detail;

void NetServer::videoLoop() {
    const auto interval = std::chrono::microseconds(
        1'000'000 / (config_.videoSendFps > 0 ? config_.videoSendFps : 60));

    // One compressor handle for this whole thread's lifetime (protocol v8,
    // see protocol.h's kProtocolVersion comment and compressFrameBgraToJpeg()
    // above) -- tjInitCompress()/tjDestroy() do real setup/teardown work,
    // and this loop already runs at up to videoSendFps, so paying that cost
    // once here instead of per-frame (or even per-connection) matters.
    // Safe to reuse across reconnects since this thread never calls it
    // concurrently with itself.
    tjhandle jpegCompressor = tjInitCompress();
    if (!jpegCompressor) {
        std::fprintf(stderr, "NetServer: tjInitCompress failed, video streaming disabled\n");
        return;
    }

    // One H264Encoder for this whole thread's lifetime too, same
    // reasoning as jpegCompressor above -- real setup/teardown cost
    // (WelsCreateSVCEncoder/InitializeExt), reused across reconnects.
    // A no-op wrapper if this build has no OpenH264 (see
    // H264Encoder::isAvailable()); selectVideoCodec() already never
    // selects VideoCodec::H264 in that case, so this path is simply
    // never taken either way.
    host::H264Encoder h264Encoder;
    int h264InitializedWidth = 0;
    int h264InitializedHeight = 0;
    // Same once-per-thread lifetime as h264Encoder above -- its Vulkan
    // device in particular is real setup cost worth keeping across
    // reconnects. Only ever initialized once a session selects
    // VideoCodec::PyroWave (selectVideoCodec() already verified a capable
    // device exists by then).
    host::PyroWaveEncoder pyrowaveEncoder;
    int pyrowaveInitializedWidth = 0;
    int pyrowaveInitializedHeight = 0;
    int pyrowaveInitializedQuality = 0;

    while (running_.load()) {
        sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        int clientFd = ::accept(videoListenFd_, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
        if (clientFd < 0) {
            if (!running_.load()) break;
            continue;
        }

        int previous = videoClientFd_.exchange(clientFd);
        if (previous >= 0) {
            ::close(clientFd);
            videoClientFd_ = previous;
            continue;
        }

        if (!clientAuthenticated_.load() ||
            clientAddr.sin_addr.s_addr != authenticatedClientAddr_.load()) {
            std::fprintf(stderr, "NetServer: rejecting video connection (no authenticated session)\n");
            ::close(clientFd);
            videoClientFd_ = -1;
            continue;
        }

        int nodelay = 1;
        ::setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        // Bounds how long a stalled/slow reader can block this thread: without
        // this, a full TCP send buffer makes send() block indefinitely, which
        // is exactly the kind of unbounded network wait spec section 15 rules
        // out (it doesn't affect emulation directly since this is its own
        // thread, but it would otherwise hang this connection -- and thus
        // frame delivery to any well-behaved future reconnect -- forever).
        timeval sendTimeout{};
        sendTimeout.tv_sec = 1;
        sendTimeout.tv_usec = 0;
        ::setsockopt(clientFd, SOL_SOCKET, SO_SNDTIMEO, &sendTimeout, sizeof(sendTimeout));

        // Bounds how many whole frames the OS can buffer ahead of what's
        // actually gone out over the wire. Without this, a link too slow to
        // keep up with the raw (uncompressed) frame rate doesn't drop
        // frames -- TCP's in-order guarantee means every frame this loop
        // successfully hands to send() below still gets delivered
        // eventually, just increasingly late, since each new frame queues
        // up behind whatever stale ones are still sitting in the kernel's
        // send buffer. The loop below already only ever fetches the single
        // truest-latest frame each tick (see its own comment), so nothing
        // backs up at the application level -- but a large default
        // SO_SNDBUF (typically hundreds of KB to a few MB) can still let
        // several frames' worth of now-outdated bytes sit queued in the
        // kernel, and growing latency under a bandwidth-constrained link
        // (e.g. handheld Wi-Fi) is exactly what that produces. Sizing the
        // buffer to roughly two frames' worth means the kernel can accept
        // at most one frame ahead of what's in flight; once that fills,
        // send() blocks (bounded by SO_SNDTIMEO above) until the network
        // actually drains it, and by the time it does, the next loop
        // iteration reaches for whatever's truly latest rather than
        // whatever was queued -- so latency stays bounded to roughly one
        // frame's transmission time instead of growing without limit.
        uint16_t frameWidth = 0;
        uint16_t frameHeight = 0;
        {
            std::lock_guard<std::mutex> lock(targetMutex_);
            frameSource_->frameDimensions(frameWidth, frameHeight);
        }
        // Fixed for this connection's whole lifetime, same as
        // currentVideoQuality_'s per-session effective value -- codec
        // choice is only ever (re)negotiated at handshake time (protocol
        // v13), never mid-session.
        const bool useH264 = currentVideoCodec_.load() == VideoCodec::H264;
        const bool usePyroWave = currentVideoCodec_.load() == VideoCodec::PyroWave;
        // Forces the inner loop's "not yet initialized for this size"
        // check to (re)initialize the encoder on this connection's very
        // first frame, even if a previous connection already left it
        // initialized at the exact same resolution -- a freshly
        // (re)connected client has no prior decoder state of its own, so
        // it needs a real IDR as the first frame it ever receives
        // regardless of whether the encoder itself needed rebuilding.
        h264InitializedWidth = 0;
        h264InitializedHeight = 0;
        // 4 bytes/pixel raw (PixelFormat::Bgra8888 is the only pixel format
        // any adapter in this project uses) -- but what actually goes out
        // over the wire since protocol v8 is a JPEG-compressed frame (see
        // compressFrameBgraToJpeg() above), typically well under a quarter
        // of that raw size at the mid-range qualities a bandwidth-
        // constrained session (e.g. Cemu) would actually use. Sizing this
        // buffer off the true raw size would let the exact bufferbloat
        // this SO_SNDBUF sizing exists to prevent creep back in, just at a
        // smaller absolute scale -- so this estimates a conservative
        // fraction of raw size instead, floored so tiny (e.g. test-
        // fixture) frame sizes still get a workable buffer. Protocol v9's
        // per-session HelloPayload::videoQuality means that fraction can
        // no longer assume quality 80's compression ratio -- a
        // near-lossless request (>=90, see compressFrameBgraToJpeg()'s own
        // 4:4:4-subsampling threshold) compresses far less aggressively,
        // so this halves rather than quarters the raw estimate there to
        // avoid under-sizing the buffer for a case this code didn't need
        // to account for before per-session quality existed.
        {
            const size_t rawFrameBytes = static_cast<size_t>(frameWidth) * frameHeight * 4;
            const size_t rawFrameDivisor = currentVideoQuality_.load() >= 90 ? 2 : 4;
            const size_t estimatedCompressedFrameBytes =
                std::max<size_t>(rawFrameBytes / rawFrameDivisor, 16384);
            if (rawFrameBytes > 0) {
                int sndBuf = static_cast<int>(std::min<size_t>(estimatedCompressedFrameBytes * 2, 0x7fffffff));
                ::setsockopt(clientFd, SOL_SOCKET, SO_SNDBUF, &sndBuf, sizeof(sndBuf));
            }
        }

        std::vector<uint8_t> frame;
        ByteBuffer jpegFrame;
        ByteBuffer h264Frame;
        ByteBuffer pyrowaveFrame;
        std::optional<uint64_t> lastSentFrameIndex;
        while (running_.load()) {
            auto tickStart = std::chrono::steady_clock::now();

            uint64_t frameIndex = 0;
            // Real per-frame width/height, not the once-negotiated
            // `frameWidth`/`frameHeight` sampled before this loop started --
            // see AdapterBridge::getLatestFrame()'s comment for why that
            // once-negotiated value can never be trusted for the rest of a
            // Cemu session. Defaults to the once-negotiated value so a
            // source that never updates them (SyntheticFrameSource,
            // HostControlAdapter's always-false stub) keeps behaving
            // exactly as before.
            uint16_t currentFrameWidth = frameWidth;
            uint16_t currentFrameHeight = frameHeight;
            bool gotFrame;
            {
                std::lock_guard<std::mutex> lock(targetMutex_);
                gotFrame = frameSource_->getLatestFrame(frame, frameIndex, currentFrameWidth, currentFrameHeight);
            }
            // Skip re-sending a frame whose index hasn't changed since the
            // last tick: getLatestFrame() is "return the most recent one,
            // whatever it is" (latest-frame-wins), not "return a new one
            // if there is one" -- when this loop's own tick rate (up to
            // videoSendFps) outpaces the actual source frame rate (e.g.
            // AzaharAdapter's own ~30fps capture loop vs. this loop's
            // default 60Hz), sending unconditionally meant roughly half of
            // every video packet was a byte-for-byte duplicate of the one
            // before it -- pure wasted bandwidth, worth nothing to the
            // client (main.cpp/net_client.cpp just redraw the same texture
            // either way), and reducing the send budget actually available
            // for genuinely new frames.
            if (gotFrame && lastSentFrameIndex && frameIndex == *lastSentFrameIndex) {
                gotFrame = false;
            }
            // Taken right before encoding begins (protocol v10, see
            // protocol.h's kProtocolVersion comment and
            // VideoFramePayload) -- wall-clock, not steady_clock, so
            // it's directly comparable against the client's own
            // wall-clock receipt time the same way
            // ControllerState::clientTimestampUs already is for input
            // latency. Deliberately captured even on a tick that ends up
            // skipping the send below (dead code in that case) rather
            // than moved inside the `if (gotFrame)` block, so this stays
            // a single obvious "when did we start working on this
            // frame" read with no risk of drifting from what it's
            // actually meant to measure as the surrounding code changes.
            uint64_t captureTimestampUs = nowMicrosEpoch();
            // Latency-audit follow-up (see NetServerStats::recordVideoEncode's
            // own comment): steady_clock, not wall-clock, since this only
            // ever gets compared against another steady_clock read taken a
            // few lines below on this same thread -- immune to the same
            // wall-clock-jump risk captureTimestampUs above is fine
            // accepting only because *that* one is deliberately meant to
            // cross to the client's own clock instead. Only meaningful when
            // `gotFrame` was already true going in (an attempted encode/
            // compress actually happens below); recorded into stats_ only
            // on this tick's eventual successful send, matching
            // framesSent's own existing "success only" convention.
            const bool attemptingEncodeThisTick = gotFrame;
            const auto encodeStart = std::chrono::steady_clock::now();
            if (gotFrame && useH264) {
                if (currentFrameWidth != h264InitializedWidth || currentFrameHeight != h264InitializedHeight) {
                    // First frame of this connection (see the
                    // h264InitializedWidth/Height reset above), or a real
                    // mid-session resolution change -- Cemu's own
                    // per-title GamePad size already did this for real,
                    // see docs/history.md's "sheared/torn"
                    // entry. Either way the next encodeFrame() call below
                    // naturally produces a fresh IDR, which is exactly
                    // what a client with no prior decoder state (or a
                    // decoder state that just became the wrong
                    // resolution) needs.
                    // Deliberately NOT config_.videoSendFps: since the
                    // "Latency: tightened the two cheap-to-poll relay
                    // stages" pass (docs/history.md), that
                    // value is a polling-responsiveness tick rate (240
                    // by default) this loop's own outer interval uses,
                    // not a real content frame rate -- no adapter
                    // actually produces new frames anywhere near that
                    // often (Cemu's own CEMU_REMOTE_CAPTURE_FPS default
                    // is 30). Feeding 240 into fMaxFrameRate/uiIntraPeriod
                    // below would badly under-tune both (a keyframe only
                    // every ~16 real seconds at frame *count* 480, not
                    // the intended ~2). This constant is a reasonable
                    // assumption across today's real adapters, not a
                    // measured value -- worth revisiting once per-adapter
                    // real capture rate is actually plumbed through to
                    // NetServer.
                    constexpr int kAssumedCaptureFps = 30;
                    if (!h264Encoder.initialize(currentFrameWidth, currentFrameHeight, kAssumedCaptureFps,
                                                 h264TargetBitrateBps(currentVideoQuality_.load(), currentFrameWidth,
                                                                       currentFrameHeight, kAssumedCaptureFps))) {
                        std::fprintf(stderr, "NetServer: H264Encoder::initialize failed (%dx%d), skipping frame\n",
                                     currentFrameWidth, currentFrameHeight);
                        h264InitializedWidth = 0;
                        h264InitializedHeight = 0;
                        gotFrame = false;
                    } else {
                        h264InitializedWidth = currentFrameWidth;
                        h264InitializedHeight = currentFrameHeight;
                    }
                }
                bool isKeyframe = false;
                if (gotFrame && !h264Encoder.encodeFrame(frame.data(), currentFrameWidth, currentFrameHeight,
                                                          h264Frame, isKeyframe)) {
                    // Same "skip this tick, don't tear down the
                    // connection" treatment as a JPEG compress failure
                    // below -- a transient encode failure shouldn't be
                    // fatal.
                    gotFrame = false;
                }
                if (gotFrame && h264Frame.empty()) {
                    // The encoder's own rate control decided to skip
                    // this frame entirely to stay within the target
                    // bitrate (encodeFrame() still returns true for this
                    // -- see its own comment) -- nothing to send, same
                    // as "frame index unchanged" above, not an error.
                    gotFrame = false;
                }
            } else if (gotFrame && usePyroWave) {
                // Intra-only, so unlike H.264 above there's no "fresh
                // connection needs a keyframe" reset to force -- the
                // encoder only needs rebuilding on a real size change,
                // or when this session's negotiated quality (and so its
                // per-frame byte budget) differs from the last one's.
                const int quality = currentVideoQuality_.load();
                if (currentFrameWidth != pyrowaveInitializedWidth || currentFrameHeight != pyrowaveInitializedHeight ||
                    quality != pyrowaveInitializedQuality) {
                    if (!pyrowaveEncoder.initialize(currentFrameWidth, currentFrameHeight,
                                                    pyrowaveMaxFrameBytes(quality, currentFrameWidth,
                                                                          currentFrameHeight))) {
                        std::fprintf(stderr,
                                     "NetServer: PyroWaveEncoder::initialize failed (%dx%d), skipping frame\n",
                                     currentFrameWidth, currentFrameHeight);
                        pyrowaveInitializedWidth = 0;
                        pyrowaveInitializedHeight = 0;
                        gotFrame = false;
                    } else {
                        pyrowaveInitializedWidth = currentFrameWidth;
                        pyrowaveInitializedHeight = currentFrameHeight;
                        pyrowaveInitializedQuality = quality;
                    }
                }
                pyrowaveFrame.clear();
                if (gotFrame &&
                    !pyrowaveEncoder.encodeFrame(frame.data(), currentFrameWidth, currentFrameHeight, pyrowaveFrame)) {
                    // Same "skip this tick" treatment as the other codecs.
                    gotFrame = false;
                }
            } else if (gotFrame && !compressFrameBgraToJpeg(jpegCompressor, frame.data(), currentFrameWidth, currentFrameHeight,
                                                      currentVideoQuality_.load(), jpegFrame)) {
                // Logged inside compressFrameBgraToJpeg(); skip this tick
                // rather than tearing down the connection, same treatment
                // as "nothing new to send" below -- a transient encode
                // failure shouldn't be fatal.
                gotFrame = false;
            }
            const uint64_t encodeMicros = attemptingEncodeThisTick
                ? static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - encodeStart)
                                             .count())
                : 0;
            if (gotFrame) {
                VideoFramePayload videoPayload;
                videoPayload.captureTimestampUs = captureTimestampUs;
                videoPayload.jpeg = std::move(useH264 ? h264Frame : usePyroWave ? pyrowaveFrame : jpegFrame);
                ByteBuffer packet = buildVideoFramePacket(videoPayload);
                // Same steady_clock reasoning as encodeStart above --
                // isolates sendAll()'s own real cost (including any real
                // blocking against SO_SNDBUF/SO_SNDTIMEO backpressure, see
                // this connection's own setsockopt calls above) from
                // whatever the client-observed "network+encode+queue"
                // figure bundles together.
                const auto sendStart = std::chrono::steady_clock::now();
                const bool sendOk = sendAll(clientFd, packet.data(), packet.size());
                const uint64_t sendMicros = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - sendStart)
                        .count());
                if (!sendOk) {
                    break;
                }

                std::lock_guard<std::mutex> lock(statsMutex_);
                ++stats_.framesSent;
                stats_.recordVideoEncode(encodeMicros);
                stats_.recordVideoSend(sendMicros);
                if (lastSentFrameIndex && frameIndex > *lastSentFrameIndex + 1) {
                    stats_.framesDropped += frameIndex - *lastSentFrameIndex - 1;
                }
                lastSentFrameIndex = frameIndex;
            }

            auto elapsed = std::chrono::steady_clock::now() - tickStart;
            auto remaining = interval - std::chrono::duration_cast<std::chrono::microseconds>(elapsed);
            if (remaining.count() > 0) {
                std::this_thread::sleep_for(remaining);
            }
        }

        ::close(clientFd);
        videoClientFd_ = -1;
    }

    tjDestroy(jpegCompressor);
}

} // namespace dualdeck::host
