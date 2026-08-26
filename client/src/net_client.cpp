#include "net_client.h"

#include "client_log.h"
#include "h264_decoder.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <turbojpeg.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>

namespace melonds_remote::client {

namespace {

// Wall-clock (epoch) microseconds -- comparable against
// VideoFramePayload::captureTimestampUs (protocol v10), which is also
// wall-clock, not steady_clock. Same "assumes synced clocks" caveat as
// ControllerState::clientTimestampUs already documents (see
// docs/known-limitations.md); duplicated here rather than shared with
// main.cpp's own wallClockNowUs() (internal linkage, file-local), same
// as net_server.cpp's nowMicrosEpoch() already duplicates it host-side.
uint64_t wallClockNowUs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<microseconds>(system_clock::now().time_since_epoch()).count());
}

// JPEG-decodes one video frame (protocol v8, see protocol.h's
// kProtocolVersion comment) back into raw BGRA8888 -- TJPF_BGRA as the
// output format means no manual channel reordering, matching this
// project's existing BGRA8888-everywhere convention. `outBgra` is resized
// to exactly outWidth*outHeight*4 regardless of its previous contents.
// Returns false (leaving outBgra/outWidth/outHeight unspecified) if `jpeg`
// isn't decodable at all.
//
// Decodes at the JPEG's own real embedded dimensions (outWidth/outHeight),
// not a caller-supplied expected size -- this used to require an exact
// match against the size HelloAck negotiated once at connection time and
// reject the frame otherwise, which broke the instant a host's real
// per-frame size legitimately differs from that one-time negotiated value
// (see adapter_contract.h's SurfaceFrame comment for why CemuAdapter's
// real capture size can't be known that early). The caller
// (videoReceiveLoop() below) is responsible for reacting to outWidth/
// outHeight changing between calls (see NetClient::hostNativeWidth()/
// hostNativeHeight()'s updated comment and main.cpp's per-frame texture
// resize check).
bool decompressJpegToBgra(tjhandle decompressor, const uint8_t* jpeg, size_t jpegSize,
                           std::vector<uint8_t>& outBgra, int& outWidth, int& outHeight) {
    int jpegSubsamp = 0, jpegColorspace = 0;
    if (tjDecompressHeader3(decompressor, jpeg, static_cast<unsigned long>(jpegSize), &outWidth,
                             &outHeight, &jpegSubsamp, &jpegColorspace) != 0) {
        logLine("video: tjDecompressHeader3 failed: %s\n", tjGetErrorStr2(decompressor));
        return false;
    }
    if (outWidth <= 0 || outHeight <= 0) {
        logLine("video: decoded JPEG has invalid dimensions %dx%d\n", outWidth, outHeight);
        return false;
    }

    outBgra.resize(static_cast<size_t>(outWidth) * outHeight * 4);
    if (tjDecompress2(decompressor, jpeg, static_cast<unsigned long>(jpegSize), outBgra.data(), outWidth,
                       /*pitch=*/0, outHeight, TJPF_BGRA, TJFLAG_FASTDCT) != 0) {
        logLine("video: tjDecompress2 failed: %s\n", tjGetErrorStr2(decompressor));
        return false;
    }
    return true;
}

bool sendAll(int fd, const uint8_t* data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        ssize_t n = ::send(fd, data + sent, size - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool recvExact(int fd, uint8_t* data, size_t size) {
    size_t received = 0;
    while (received < size) {
        ssize_t n = ::recv(fd, data + received, size - received, 0);
        if (n <= 0) return false;
        received += static_cast<size_t>(n);
    }
    return true;
}

// Real user report: a host that's reachable at the IP level but has the
// control/video ports firewalled shut with a DROP (not REJECT) rule --
// exactly what a fresh EmuDeck-installed patched AppImage looks like,
// since scripts/lib/apprun_templates.sh's ephemeral AppRun-spawned host-
// service never runs the firewall-port-opening step
// install-steam-shortcut.sh does -- made the raw blocking ::connect()
// below hang for the OS's own TCP SYN-retry timeout (100+ seconds, often
// longer). Worse, connect() holds connectMutex_ for its entire body, and
// disconnect() (called from main.cpp's exit/change-host path) needs that
// same mutex before it can even reach the shutdown() call that would
// otherwise unblock a stuck recv() -- so the whole client appeared to
// hang with no way out but killing it externally, exactly the reported
// "hangs when trying to change host or exit."
//
// Bounds the handshake specifically (not the persistent per-session
// receive loops below, which legitimately rely on blocking-forever recv()
// semantics for an otherwise-idle-but-healthy connection, e.g. HostControl
// mode) to kHandshakeTimeoutMs. SO_SNDTIMEO/SO_RCVTIMEO have no effect on
// the initial connect() syscall itself on Linux, so connectWithTimeout()
// below uses the standard non-blocking-connect-then-poll() pattern
// instead; SO_RCVTIMEO (set separately, around the Hello/HelloAck
// recvExact() calls) is sufficient to bound those.
constexpr int kHandshakeTimeoutMs = 5000;

bool connectWithTimeout(int fd, const sockaddr* addr, socklen_t addrLen, int timeoutMs) {
    int origFlags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, origFlags | O_NONBLOCK);

    bool ok = false;
    if (::connect(fd, addr, addrLen) == 0) {
        ok = true; // completed immediately (e.g. localhost)
    } else if (errno == EINPROGRESS) {
        pollfd pfd{fd, POLLOUT, 0};
        int pollResult = ::poll(&pfd, 1, timeoutMs);
        if (pollResult > 0) {
            int soError = 0;
            socklen_t len = sizeof(soError);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &len) == 0 && soError == 0) {
                ok = true;
            } else {
                errno = soError; // so the caller's strerror(errno) reflects the real failure
            }
        } else if (pollResult == 0) {
            errno = ETIMEDOUT; // vs. poll()'s own errno on a genuine poll() failure (rare)
        }
    }

    ::fcntl(fd, F_SETFL, origFlags); // restore blocking mode either way
    return ok;
}

// Sets (timeoutMs > 0) or clears (timeoutMs == 0, "block forever") this
// socket's receive timeout. Cleared once the handshake below actually
// succeeds, right before controlReceiveLoop()/videoReceiveLoop() start
// relying on it -- see kHandshakeTimeoutMs's comment for why those two
// must keep today's blocking-forever behavior.
void setRecvTimeoutMs(int fd, int timeoutMs) {
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

} // namespace

NetClient::NetClient(NetClientConfig config) : config_(std::move(config)) {}

NetClient::~NetClient() {
    disconnect();
}

void NetClient::closePartialConnection() {
    // Closing the fds first unblocks any recv()/send() a still-running
    // videoReceiveLoop()/heartbeatLoop() from a previous session is
    // stuck in, so the joins below complete promptly instead of hanging.
    // This matters for reconnect: connect() calls this at its start, and
    // assigning a new std::thread over a still-joinable one (without
    // joining first) would call std::terminate.
    if (controlFd_ >= 0) { ::close(controlFd_); controlFd_ = -1; }
    if (videoFd_ >= 0) { ::close(videoFd_); videoFd_ = -1; }
    if (udpFd_ >= 0) { ::close(udpFd_); udpFd_ = -1; }
    if (udpAudioFd_ >= 0) { ::close(udpAudioFd_); udpAudioFd_ = -1; }
    sessionId_ = 0;
    hostMicSupported_ = false;
    hostMode_ = HostMode::Emulation;

    if (videoThread_.joinable()) videoThread_.join();
    if (controlThread_.joinable()) controlThread_.join();
    if (heartbeatThread_.joinable()) heartbeatThread_.join();
}

bool NetClient::connect() {
    // A second connect() should not wait behind an in-flight handshake and
    // then make a duplicate attempt with the same user action. This is a
    // last line of defense for callers in addition to main.cpp giving its
    // reconnect thread sole ownership of connection attempts.
    bool expected = false;
    if (!connectionAttemptInProgress_.compare_exchange_strong(expected, true)) {
        logLine("connection attempt already in progress; ignoring duplicate request\n");
        return false;
    }
    struct AttemptGuard {
        std::atomic<bool>& inProgress;
        ~AttemptGuard() { inProgress = false; }
    } attemptGuard{connectionAttemptInProgress_};

    // Serializes against disconnect() -- e.g. the reconnect-on-a-background
    // thread pattern in client/src/main.cpp may race with a main-thread
    // disconnect() at shutdown.
    std::lock_guard<std::mutex> lock(connectMutex_);

    // Reset first: lastRejectReason_ is only ever written when a real
    // HelloAck is actually parsed below, so without this, a caller
    // checking lastRejectReason() after this attempt fails at an earlier
    // stage (e.g. the raw TCP connect() call itself, meaning the host is
    // simply unreachable) would otherwise still see a stale reason left
    // over from a *previous* attempt's real rejection -- misleading for
    // anything that shows a distinct status per attempt (the setup
    // wizard's connect/approval step in particular).
    {
        std::lock_guard<std::mutex> resultLock(handshakeResultMutex_);
        lastRejectReason_ = HelloRejectReason::None;
        hostAppVersion_.clear();
        // Deliberately NOT reset here (GitHub issue #28: "preserve [the
        // active emulator] on reconnect/error screens where practical")
        // -- hostSystemIdentity_/hostAdapterIdentity_ keep whatever was
        // last learned from a real HelloAck across a failed reconnect
        // attempt, so a caller's "reconnecting to <last known
        // system/adapter>" status text doesn't blank out just because
        // this one attempt hasn't gotten far enough to receive a fresh
        // one yet. They're only ever overwritten below, once an actual
        // HelloAck is parsed.
    }

    // A previous connect() attempt may have failed partway through (or a
    // prior session may have just ended); never layer a new attempt on
    // top of stale file descriptors.
    closePartialConnection();

    controlFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (controlFd_ < 0) {
        logLine("socket (control): %s\n", std::strerror(errno));
        closePartialConnection();
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.controlPort);
    if (::inet_pton(AF_INET, config_.hostAddress.c_str(), &addr.sin_addr) != 1) {
        logLine("invalid host address: %s\n", config_.hostAddress.c_str());
        closePartialConnection();
        return false;
    }

    if (!connectWithTimeout(controlFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr), kHandshakeTimeoutMs)) {
        logLine("connect (control): %s\n", std::strerror(errno));
        closePartialConnection();
        return false;
    }
    // Bounds the two recvExact() calls below (Hello/HelloAck) -- cleared
    // back to "block forever" once the handshake fully succeeds, a few
    // lines before controlThread_/videoThread_ start. See
    // kHandshakeTimeoutMs's comment above.
    setRecvTimeoutMs(controlFd_, kHandshakeTimeoutMs);

    HelloPayload helloPayload;
    helloPayload.clientName = config_.clientName;
    helloPayload.clientPlatform = config_.clientPlatform;
    helloPayload.displayWidth = config_.displayWidth;
    helloPayload.displayHeight = config_.displayHeight;
    helloPayload.authToken = config_.authToken;
    helloPayload.appVersion = config_.appVersion;
    helloPayload.videoQuality = config_.videoQuality;
    // VideoCodecBit_Jpeg is always advertised (no external dependency,
    // decode always available); VideoCodecBit_H264 only if config_.
    // preferH264 (ClientSettings::videoCodecH264Experimental, defaults
    // off -- see that field's own comment) opted in. See
    // NetServer::selectVideoCodec() for the host's side of this
    // negotiation -- it has the final say either way.
    helloPayload.supportedVideoCodecs =
        kVideoCodecBit_Jpeg | (config_.preferH264 ? kVideoCodecBit_H264 : 0);
    ByteBuffer hello = buildHelloPacket(helloPayload);
    if (!sendAll(controlFd_, hello.data(), hello.size())) {
        logLine("failed to send Hello\n");
        closePartialConnection();
        return false;
    }

    uint8_t ackHeaderBuf[kPacketHeaderWireSize];
    if (!recvExact(controlFd_, ackHeaderBuf, sizeof(ackHeaderBuf))) {
        logLine("failed to receive HelloAck\n");
        closePartialConnection();
        return false;
    }
    auto ackHeader = parseHeader(ackHeaderBuf, sizeof(ackHeaderBuf));
    if (!ackHeader || ackHeader->type != PacketType::HelloAck || ackHeader->payloadSize > 256) {
        logLine("handshake rejected by host (bad response header)\n");
        closePartialConnection();
        return false;
    }

    ByteBuffer ackPayloadBuf(ackHeader->payloadSize);
    if (!ackPayloadBuf.empty() && !recvExact(controlFd_, ackPayloadBuf.data(), ackPayloadBuf.size())) {
        logLine("failed to receive HelloAck payload\n");
        closePartialConnection();
        return false;
    }
    auto ack = parseHelloAckPayload(ackPayloadBuf.data(), ackPayloadBuf.size());
    if (!ack) {
        logLine("handshake rejected by host (malformed HelloAck payload)\n");
        closePartialConnection();
        return false;
    }
    {
        std::lock_guard<std::mutex> resultLock(handshakeResultMutex_);
        lastRejectReason_ = ack->rejectReason;
        hostAppVersion_ = ack->appVersion;
        hostSystemIdentity_ = ack->system;
        hostAdapterIdentity_ = ack->adapter;
    }
    // Stored even on a rejected handshake, same availability convention
    // as hostAppVersion_/hostSystemIdentity_ above -- and unconditionally
    // (not inside the `if (!ack->accepted)` early-return below), so
    // videoReceiveLoop()'s payload-size check always reflects the most
    // recent HelloAck this host actually sent, not a stale value from
    // a previous, possibly different, host.
    hostNativeWidth_ = ack->nativeWidth;
    hostNativeHeight_ = ack->nativeHeight;
    if (!ack->accepted) {
        if (ack->rejectReason == HelloRejectReason::ApprovalRequired) {
            logLine("handshake rejected by host: awaiting approval -- a human at the host "
                    "needs to approve this device, no action needed here\n");
        } else if (ack->rejectReason == HelloRejectReason::AppVersionMismatch) {
            logLine("handshake rejected by host: version mismatch (host is %s, this client is "
                    "%s) -- update one side to match the other\n",
                    ack->appVersion.c_str(), config_.appVersion.c_str());
        } else {
            logLine("handshake rejected by host (reason code %d)\n",
                          static_cast<int>(ack->rejectReason));
        }
        closePartialConnection();
        return false;
    }
    sessionId_ = ack->sessionId;
    hostMicSupported_ = ack->micSupported != 0;
    hostMode_ = ack->mode;
    // Always VideoCodec::Jpeg today -- see NetServer::selectVideoCodec()'s
    // comment. Stored (not just read once here) so videoReceiveLoop() has
    // a real value to switch its decode path on once a second codec
    // exists, rather than that loop needing its own separate plumbing.
    negotiatedVideoCodec_ = ack->selectedVideoCodec;

    // Video channel: a second TCP connection dedicated to frame streaming
    // (see docs/protocol.md -- Stage 1 keeps control and video separate
    // rather than multiplexing one socket).
    sockaddr_in videoAddr = addr;
    videoAddr.sin_port = htons(config_.videoPort);
    videoFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (videoFd_ < 0 ||
        !connectWithTimeout(videoFd_, reinterpret_cast<sockaddr*>(&videoAddr), sizeof(videoAddr), kHandshakeTimeoutMs)) {
        logLine("connect (video): %s\n", std::strerror(errno));
        closePartialConnection();
        return false;
    }

    udpFd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (udpFd_ < 0) {
        logLine("socket (udp input): %s\n", std::strerror(errno));
        closePartialConnection();
        return false;
    }
    sockaddr_in inputAddr = addr;
    inputAddr.sin_port = htons(config_.inputPort);
    if (::connect(udpFd_, reinterpret_cast<sockaddr*>(&inputAddr), sizeof(inputAddr)) < 0) {
        // connect() on a UDP socket just fixes the destination for later
        // send() calls; failure here means the address family/setup is
        // wrong, not an actual network condition.
        logLine("connect (udp input): %s\n", std::strerror(errno));
        closePartialConnection();
        return false;
    }

    udpAudioFd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (udpAudioFd_ < 0) {
        logLine("socket (udp audio): %s\n", std::strerror(errno));
        closePartialConnection();
        return false;
    }
    sockaddr_in audioAddr = addr;
    audioAddr.sin_port = htons(config_.audioPort);
    if (::connect(udpAudioFd_, reinterpret_cast<sockaddr*>(&audioAddr), sizeof(audioAddr)) < 0) {
        logLine("connect (udp audio): %s\n", std::strerror(errno));
        closePartialConnection();
        return false;
    }

    // Handshake succeeded -- restore blocking-forever recv() semantics
    // before controlReceiveLoop() starts relying on it (see
    // kHandshakeTimeoutMs's comment above).
    setRecvTimeoutMs(controlFd_, 0);

    connected_ = true;
    videoThread_ = std::thread(&NetClient::videoReceiveLoop, this);
    controlThread_ = std::thread(&NetClient::controlReceiveLoop, this);
    heartbeatThread_ = std::thread(&NetClient::heartbeatLoop, this);
    return true;
}

void NetClient::disconnect() {
    std::lock_guard<std::mutex> lock(connectMutex_);

    // Always fall through to close sockets below, even if connect() only
    // partially succeeded or the video thread already noticed a dropped
    // connection and cleared connected_ itself.
    connected_ = false;

    if (controlFd_ >= 0) {
        ByteBuffer bye;
        serializeHeader(bye, PacketHeader{kPacketMagic, kProtocolVersion, PacketType::Disconnect, 0});
        sendAll(controlFd_, bye.data(), bye.size());
        ::shutdown(controlFd_, SHUT_RDWR);
    }
    if (videoFd_ >= 0) ::shutdown(videoFd_, SHUT_RDWR);

    // Also joins videoThread_/heartbeatThread_ and closes udpFd_.
    closePartialConnection();
}

void NetClient::heartbeatLoop() {
    ByteBuffer heartbeat;
    serializeHeader(heartbeat, PacketHeader{kPacketMagic, kProtocolVersion, PacketType::Heartbeat, 0});

    while (connected_.load()) {
        if (!sendAll(controlFd_, heartbeat.data(), heartbeat.size())) {
            // A send failure here (broken pipe, reset connection) means
            // the link is dead even if controlReceiveLoop()/
            // videoReceiveLoop() haven't noticed yet (e.g. in
            // HostMode::HostControl, where no video frames ever arrive
            // to surface a dropped connection on that thread) -- without
            // this, the caller's isConnected() would keep reporting true
            // indefinitely with a socket that can never recover.
            connected_ = false;
            break;
        }
        for (uint32_t waited = 0; waited < config_.heartbeatIntervalMs && connected_.load(); waited += 50) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            // Drained every 50ms tick, not just once per full
            // heartbeatIntervalMs, so a forwarded log line reaches the
            // host promptly enough to actually be useful for live
            // debugging rather than arriving in a burst up to a second
            // late.
            if (!sendQueuedLogLines()) {
                connected_ = false;
                break;
            }
        }
    }
}

bool NetClient::sendQueuedLogLines() {
    std::deque<std::string> toSend;
    {
        std::lock_guard<std::mutex> lock(logQueueMutex_);
        toSend.swap(logQueue_);
    }
    for (const auto& line : toSend) {
        ClientLogPayload payload;
        payload.line = line;
        ByteBuffer packet = buildClientLogPacket(payload);
        if (!sendAll(controlFd_, packet.data(), packet.size())) {
            return false;
        }
    }
    return true;
}

namespace {
// See sendClientLog()'s header comment for why an over-cap line is
// dropped rather than queued or blocked on.
constexpr size_t kMaxQueuedLogLines = 64;
} // namespace

void NetClient::sendClientLog(const std::string& line) {
    if (!connected_.load()) return;
    std::lock_guard<std::mutex> lock(logQueueMutex_);
    if (logQueue_.size() >= kMaxQueuedLogLines) return;
    logQueue_.push_back(line);
}

void NetClient::sendControllerState(const ControllerState& state) {
    if (!connected_.load() || udpFd_ < 0) return;
    ByteBuffer packet = buildControllerStatePacket(state);
    ::send(udpFd_, packet.data(), packet.size(), MSG_NOSIGNAL);
}

void NetClient::sendMicAudioFrame(const MicAudioFramePayload& frame) {
    if (!connected_.load() || udpAudioFd_ < 0 || !hostMicSupported_.load()) return;
    ByteBuffer packet = buildMicAudioFramePacket(frame);
    ::send(udpAudioFd_, packet.data(), packet.size(), MSG_NOSIGNAL);
}

HelloRejectReason NetClient::lastRejectReason() const {
    std::lock_guard<std::mutex> lock(handshakeResultMutex_);
    return lastRejectReason_;
}

std::string NetClient::hostAppVersion() const {
    std::lock_guard<std::mutex> lock(handshakeResultMutex_);
    return hostAppVersion_;
}

SystemIdentity NetClient::hostSystemIdentity() const {
    std::lock_guard<std::mutex> lock(handshakeResultMutex_);
    return hostSystemIdentity_;
}

AdapterIdentity NetClient::hostAdapterIdentity() const {
    std::lock_guard<std::mutex> lock(handshakeResultMutex_);
    return hostAdapterIdentity_;
}

uint16_t NetClient::hostNativeWidth() const {
    return hostNativeWidth_.load();
}

uint16_t NetClient::hostNativeHeight() const {
    return hostNativeHeight_.load();
}

bool NetClient::getLatestFrame(std::vector<uint8_t>& outFrame) {
    std::lock_guard<std::mutex> lock(frameMutex_);
    if (!hasFrame_) return false;
    outFrame = latestFrame_;
    return true;
}

namespace {
// How often videoReceiveLoop() below logs an accumulated video-latency/
// decode-time snapshot, matching the cadence NetServer's own periodic
// stats logging uses host-side (default statsLoggingIntervalUs).
constexpr uint64_t kVideoStatsLogIntervalUs = 5'000'000; // 5s

// Same threshold net_server.cpp's inputLoop() already uses to skip a
// clearly-bogus latency delta (clock not synced, or a peer that hasn't
// been updated to send a comparable timestamp yet) rather than letting
// it dominate the running average/max.
constexpr uint64_t kMaxPlausibleLatencyUs = 10'000'000; // 10s

struct MinMaxAvgUs {
    uint64_t sampleCount = 0;
    uint64_t sumUs = 0;
    uint64_t minUs = std::numeric_limits<uint64_t>::max();
    uint64_t maxUs = 0;

    void record(uint64_t us) {
        ++sampleCount;
        sumUs += us;
        minUs = std::min(minUs, us);
        maxUs = std::max(maxUs, us);
    }
};
} // namespace

void NetClient::videoReceiveLoop() {
    std::vector<uint8_t> headerBuf(kPacketHeaderWireSize);
    std::vector<uint8_t> payloadBuf;
    std::vector<uint8_t> decodedFrame;

    // Protocol v8 (see protocol.h's kProtocolVersion comment): VideoFrame's
    // payload is now a JPEG-compressed image, decoded back to raw BGRA8888
    // here before latestFrame_ is ever touched -- everything downstream
    // (getLatestFrame(), main.cpp's texture upload) still sees exactly the
    // same raw format it always has. One decompressor handle for this
    // thread's whole lifetime, same reasoning as net_server.cpp's
    // jpegCompressor: tjInitDecompress()/tjDestroy() aren't free, and this
    // loop runs once per incoming frame.
    tjhandle jpegDecompressor = tjInitDecompress();
    if (!jpegDecompressor) {
        logLine("video: tjInitDecompress failed, cannot receive video\n");
        connected_ = false;
        return;
    }

    // Protocol v13: constructed unconditionally, same as jpegDecompressor
    // above, but only actually used below when negotiatedVideoCodec()
    // reports H264 -- which itself only ever happens if this build has
    // real OpenH264 decode available (this project's own client never
    // advertises VideoCodecBit_H264 in HelloPayload yet -- see
    // connect()'s own comment -- so a live session negotiating H.264
    // isn't reachable from a shipped client today; this exists as the
    // building block for turning that on).
    H264Decoder h264Decoder;

    // Video-latency instrumentation: `networkStats` is
    // receipt-wall-clock-time minus VideoFramePayload::captureTimestampUs
    // (protocol v10) -- network transit + host-side encode + send-queue
    // time, the video-path counterpart to NetServer's own input-latency
    // stat, with the same clock-sync assumption. `decodeStats` is purely
    // local wall-clock-free timing (steady_clock) around
    // decompressJpegToBgra() itself -- no clock-sync caveat applies to it
    // at all, since both ends of that measurement happen on this same
    // machine. Logged via logLine() (which also forwards to the host, see
    // client_log.h) every kVideoStatsLogIntervalUs rather than per-frame,
    // matching NetServer's own periodic-stats-not-per-packet approach.
    MinMaxAvgUs networkStats;
    MinMaxAvgUs decodeStats;
    uint64_t lastStatsLogUs = wallClockNowUs();

    // Debug-overlay FPS window (receivedFps() above) -- a simple once/sec
    // recompute against this loop's own wall-clock timestamps, not tied
    // to lastStatsLogUs/kVideoStatsLogIntervalUs above (that interval is
    // tuned for how often a human wants a log line, not for how often an
    // on-screen counter should refresh).
    uint64_t fpsWindowStartUs = wallClockNowUs();
    uint32_t framesThisFpsWindow = 0;

    while (connected_.load()) {
        if (!recvExact(videoFd_, headerBuf.data(), headerBuf.size())) {
            break;
        }
        // hostNativeWidth_/hostNativeHeight_ (this connection's own
        // HelloAck-reported dimensions, not a fixed DS-sized constant --
        // see the AzaharAdapter/3DS note this comment used to carry)
        // bound how large a compressed payload could legitimately be:
        // real JPEG output at any quality is essentially never larger
        // than the equivalent raw frame (plus VideoFramePayload's fixed
        // 8-byte timestamp prefix, protocol v10), so this is a generous
        // sanity ceiling against a corrupt/bogus size field, not a tight
        // expected-size check like before compression (payload size now
        // varies frame to frame with scene content).
        const uint32_t rawFrameBytes =
            static_cast<uint32_t>(hostNativeWidth_.load()) * hostNativeHeight_.load() * 4u +
            kVideoFrameTimestampWireSize;
        auto header = parseHeader(headerBuf.data(), headerBuf.size());
        if (!header || header->type != PacketType::VideoFrame || header->payloadSize == 0 ||
            header->payloadSize > rawFrameBytes) {
            logLine("video: dropping unexpected packet, closing connection\n");
            break;
        }

        payloadBuf.resize(header->payloadSize);
        if (!recvExact(videoFd_, payloadBuf.data(), payloadBuf.size())) {
            break;
        }

        auto videoFrame = parseVideoFramePayload(payloadBuf.data(), payloadBuf.size());
        if (!videoFrame) {
            logLine("video: dropping malformed frame (short of even the timestamp prefix), "
                    "closing connection\n");
            break;
        }

        uint64_t nowWallUs = wallClockNowUs();
        // Hoisted out of the `if` below (rather than left scoped to just
        // the networkStats.record() call, as before) so it can also seed
        // lastLatencyMicros_ once this frame is confirmed decoded -- 0
        // stays the value if this frame's latency couldn't be computed at
        // all (clock skew putting captureTimestampUs in the future), same
        // "not counted" meaning lastLatencyMicros_'s own comment
        // documents.
        uint64_t latencyUs = 0;
        if (nowWallUs >= videoFrame->captureTimestampUs) {
            latencyUs = nowWallUs - videoFrame->captureTimestampUs;
            if (latencyUs <= kMaxPlausibleLatencyUs) {
                networkStats.record(latencyUs);
            }
        }

        auto decodeStart = std::chrono::steady_clock::now();
        int decodedWidth = 0, decodedHeight = 0;
        bool decoded;
        bool hasFrame = true;
        if (negotiatedVideoCodec() == VideoCodec::H264) {
            decoded = h264Decoder.decodeFrame(videoFrame->jpeg.data(), videoFrame->jpeg.size(), decodedFrame,
                                               decodedWidth, decodedHeight, hasFrame);
            if (decoded && !hasFrame) {
                // Latency-audit follow-up: OpenH264's documented no-delay-
                // decode semantics (codec_api.h's own decode-loop example)
                // can defer a frame's output to a follow-up call fed no
                // new data, rather than the call that actually fed the
                // bitstream in -- the exact pattern
                // test_h264_decoder.cpp already exercises against this
                // same decoder (see its own comment). Production code
                // used to just fall through to the `continue` below and
                // wait for the *next* network packet to arrive instead --
                // that usually still works (feeding fresh data also
                // flushes a pending picture), but silently costs up to a
                // full extra inter-frame interval on exactly the frame
                // that matters most for perceived responsiveness: the
                // first one after a (re)connect. Draining immediately
                // here removes that dependency on timing instead of
                // hoping the next packet arrives soon enough.
                decoded = h264Decoder.decodeFrame(nullptr, 0, decodedFrame, decodedWidth, decodedHeight, hasFrame);
            }
        } else {
            decoded = decompressJpegToBgra(jpegDecompressor, videoFrame->jpeg.data(), videoFrame->jpeg.size(),
                                            decodedFrame, decodedWidth, decodedHeight);
        }
        const uint64_t decodeMicros = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - decodeStart)
                .count());
        decodeStats.record(decodeMicros);
        if (!decoded) {
            logLine("video: dropping undecodable frame, closing connection\n");
            break;
        }
        if (!hasFrame) {
            // H264Decoder-only case (decompressJpegToBgra() always either
            // fails or produces a frame) -- reached only when the drain
            // attempt above (or JPEG's own single decode call) still
            // didn't produce a picture: a genuine SPS/PPS-only access
            // unit with nothing to output at all, not a deferred frame
            // that draining could have recovered. Nothing to display yet,
            // not an error -- wait for the next packet.
            continue;
        }
        // Real bug this fixes: hostNativeWidth_/hostNativeHeight_ used to
        // only ever be set once, from HelloAck -- if a frame's real
        // decoded size ever differed (see decompressJpegToBgra()'s
        // comment), every subsequent frame was rejected outright and the
        // connection torn down. Now kept in sync with whatever the most
        // recently decoded frame's real size actually is; main.cpp's
        // render loop checks these every frame (not just on connect) and
        // resizes its texture the moment they change.
        hostNativeWidth_ = static_cast<uint16_t>(decodedWidth);
        hostNativeHeight_ = static_cast<uint16_t>(decodedHeight);

        // Debug-overlay stats (see each getter's own comment in
        // net_client.h) -- updated for every genuinely displayed frame,
        // i.e. exactly the frames that reach the swap into latestFrame_
        // just below, not every packet this loop merely receives (an
        // SPS/PPS-only H264 access unit above already `continue`d past
        // this point without incrementing anything here).
        ++receivedFrameCount_;
        lastFrameCompressedBytes_ = static_cast<uint32_t>(videoFrame->jpeg.size());
        lastDecodeMicros_ = static_cast<uint32_t>(decodeMicros);
        lastLatencyMicros_ = static_cast<uint32_t>(latencyUs);
        ++framesThisFpsWindow;
        if (nowWallUs - fpsWindowStartUs >= 1'000'000) {
            receivedFps_ = framesThisFpsWindow;
            framesThisFpsWindow = 0;
            fpsWindowStartUs = nowWallUs;
        }

        {
            std::lock_guard<std::mutex> lock(frameMutex_);
            latestFrame_.swap(decodedFrame);
            hasFrame_ = true;
        }
        ++latestFrameGeneration_;

        if (nowWallUs - lastStatsLogUs >= kVideoStatsLogIntervalUs) {
            if (networkStats.sampleCount > 0) {
                logLine("video: latency (network+encode+queue) avg=%.1fms min=%.1fms max=%.1fms "
                        "(n=%llu) | decode avg=%.1fms min=%.1fms max=%.1fms (n=%llu)\n",
                        static_cast<double>(networkStats.sumUs) / static_cast<double>(networkStats.sampleCount) /
                            1000.0,
                        static_cast<double>(networkStats.minUs) / 1000.0,
                        static_cast<double>(networkStats.maxUs) / 1000.0,
                        static_cast<unsigned long long>(networkStats.sampleCount),
                        static_cast<double>(decodeStats.sumUs) / static_cast<double>(decodeStats.sampleCount) /
                            1000.0,
                        static_cast<double>(decodeStats.minUs) / 1000.0,
                        static_cast<double>(decodeStats.maxUs) / 1000.0,
                        static_cast<unsigned long long>(decodeStats.sampleCount));
            }
            networkStats = MinMaxAvgUs{};
            decodeStats = MinMaxAvgUs{};
            lastStatsLogUs = nowWallUs;
        }
    }

    tjDestroy(jpegDecompressor);
    connected_ = false;
}

// GitHub issue #4 Phase E: the control channel is no longer write-only
// from the client's side. This is the client-side counterpart to the
// host's controlLoop() (host/remote-server/src/net_server.cpp) tolerant-
// parsing convention: keep the connection open for any recognized-but-
// unhandled packet type, and only drop the connection on a real
// transport failure or a header/payload that fails to parse at all
// (which -- unlike an unrecognized *type* -- means the two sides have
// lost byte-alignment on the stream and cannot safely continue).
void NetClient::controlReceiveLoop() {
    std::vector<uint8_t> headerBuf(kPacketHeaderWireSize);
    std::vector<uint8_t> payloadBuf;

    while (connected_.load()) {
        if (!recvExact(controlFd_, headerBuf.data(), headerBuf.size())) {
            break;
        }
        auto header = parseHeader(headerBuf.data(), headerBuf.size());
        // Same bound the host's own controlLoop() applies to an incoming
        // control-channel payload (spec section 13: validate declared
        // sizes before allocating for them) -- ModeChanged is the
        // largest payload ever expected here (mode byte + two identity
        // structs), well under this.
        if (!header || header->payloadSize > 512) {
            logLine("control: dropping malformed packet, closing connection\n");
            break;
        }

        payloadBuf.resize(header->payloadSize);
        if (!payloadBuf.empty() && !recvExact(controlFd_, payloadBuf.data(), payloadBuf.size())) {
            break;
        }

        if (header->type == PacketType::ModeChanged) {
            auto modeChanged = parseModeChangedPayload(payloadBuf.data(), payloadBuf.size());
            if (!modeChanged) {
                logLine("control: dropping malformed ModeChanged packet\n");
                continue;
            }
            // Real user report, 2026-08-03: "when exiting an emulator...
            // the last frame that was sent to the client persists on
            // screen until I change client and reinitialize the
            // connection." Root cause: hasFrame_ (set true the first time
            // videoReceiveLoop() ever decodes a frame) was never reset
            // anywhere -- getLatestFrame() keeps handing back the same
            // stale latestFrame_ buffer forever, to any caller, regardless
            // of mode changes or the adapter that produced it having long
            // since disconnected. main.cpp's render loop only stops
            // calling getLatestFrame() once hostMode_ reads HostControl
            // *and* mirrorHostScreen is off; otherwise (mirroring on, or a
            // brief window before this ModeChanged packet is even
            // processed) it kept redrawing the last real frame
            // indefinitely, exactly matching the report. A mode change is
            // exactly the signal that whatever video state existed before
            // is no longer valid -- reset it here so the render loop falls
            // through to the test pattern / host-control placeholder
            // immediately instead of only on a fresh reconnect (a new
            // NetClient instance, which starts with hasFrame_ already
            // false -- the reason "exit and reopen the client" already
            // masked this).
            if (modeChanged->mode != hostMode_) {
                {
                    std::lock_guard<std::mutex> lock(frameMutex_);
                    hasFrame_ = false;
                }
                ++latestFrameGeneration_;
            }
            hostMode_ = modeChanged->mode;
            {
                std::lock_guard<std::mutex> lock(handshakeResultMutex_);
                hostSystemIdentity_ = modeChanged->system;
                hostAdapterIdentity_ = modeChanged->adapter;
            }
        }
        // Anything else (a Heartbeat echoed back, or a future packet
        // type this client build predates) is simply ignored -- the
        // connection stays open either way.
    }

    connected_ = false;
}

} // namespace melonds_remote::client
