#include "host/net_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <optional>

#include "melonds_remote/protocol.h"

namespace melonds_remote::host {

namespace {

// Monotonic clock: used for timeouts and sequence-number bookkeeping,
// where immunity to wall-clock jumps (NTP corrections, DST, manual clock
// changes) matters more than comparability with the client's clock.
uint64_t nowMicros() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

// Wall-clock (epoch) time: the only clock comparable with the client's
// ControllerState.clientTimestampUs (see wallClockNowUs() in
// client/src/main.cpp), so this is used only for the latency estimate in
// inputLoop()'s stats, never for timeout/ordering decisions.
uint64_t nowMicrosEpoch() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(system_clock::now().time_since_epoch()).count());
}

// Binds a TCP listening socket to config.bindAddress:port. Returns -1 on
// failure (logged); never falls back to binding all interfaces implicitly.
int makeTcpListener(const std::string& bindAddress, uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::perror("socket (tcp)");
        return -1;
    }

    int reuse = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, bindAddress.c_str(), &addr.sin_addr) != 1) {
        std::fprintf(stderr, "invalid bind address: %s\n", bindAddress.c_str());
        ::close(fd);
        return -1;
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind (tcp)");
        ::close(fd);
        return -1;
    }

    if (::listen(fd, 1) < 0) {
        std::perror("listen");
        ::close(fd);
        return -1;
    }

    return fd;
}

int makeUdpSocket(const std::string& bindAddress, uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        std::perror("socket (udp)");
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, bindAddress.c_str(), &addr.sin_addr) != 1) {
        std::fprintf(stderr, "invalid bind address: %s\n", bindAddress.c_str());
        ::close(fd);
        return -1;
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind (udp)");
        ::close(fd);
        return -1;
    }

    return fd;
}

// Constant-time-ish string comparison: always compares the same number of
// bytes (padding the shorter string's "missing" bytes into the mismatch
// accumulator) so a client can't use response timing to learn how many
// leading bytes of the auth token it guessed correctly.
bool constantTimeEquals(const std::string& a, const std::string& b) {
    size_t maxLen = std::max(a.size(), b.size());
    unsigned char diff = static_cast<unsigned char>(a.size() != b.size());
    for (size_t i = 0; i < maxLen; ++i) {
        unsigned char ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
        unsigned char cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
        diff = static_cast<unsigned char>(diff | (ca ^ cb));
    }
    return diff == 0;
}

bool sendAll(int fd, const uint8_t* data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        ssize_t n = ::send(fd, data + sent, size - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

} // namespace

NetServer::NetServer(NetServerConfig config, IEmulatorInputSink& inputSink, IFrameSource& frameSource)
    : config_(std::move(config)), inputSink_(inputSink), frameSource_(frameSource),
      pairingManager_(config_.pairingStateFilePath, config_.pairingCodeTtl),
      rateLimiter_(config_.maxConnectionAttemptsPerWindow, config_.connectionAttemptWindowUs) {
    if (!config_.authToken.empty()) {
        std::fprintf(stderr,
                      "NetServer: static auth token configured -- pairing-code flow is disabled, "
                      "the exact token is required.\n");
    } else {
        std::fprintf(stderr,
                      "NetServer: no static auth token configured; using pairing-code mode. An "
                      "unrecognized connection attempt will display a 6-digit code here to enter "
                      "on the client (spec section 13).\n");
    }

    pairingManager_.setOnCodeChanged([this](std::optional<std::string> code) {
        if (code) {
            std::fprintf(stderr,
                          "NetServer: pairing code: %s (enter this on the client; expires in %lds)\n",
                          code->c_str(), static_cast<long>(config_.pairingCodeTtl.count()));
        } else {
            std::fprintf(stderr, "NetServer: pairing code cleared\n");
        }
        if (config_.onPairingCodeChanged) {
            config_.onPairingCodeChanged(code);
        }
    });
}

NetServer::~NetServer() {
    stop();
}

void NetServer::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }

    controlListenFd_ = makeTcpListener(config_.bindAddress, config_.controlPort);
    videoListenFd_ = makeTcpListener(config_.bindAddress, config_.videoPort);
    inputFd_ = makeUdpSocket(config_.bindAddress, config_.inputPort);

    if (controlListenFd_ < 0 || videoListenFd_ < 0 || inputFd_ < 0) {
        std::fprintf(stderr, "NetServer: failed to bind one or more sockets, not starting\n");
        running_ = false;
        return;
    }

    std::fprintf(stderr,
                  "NetServer: listening on %s (control=%u, input(udp)=%u, video=%u)\n",
                  config_.bindAddress.c_str(), config_.controlPort, config_.inputPort,
                  config_.videoPort);

    // Show a pairing code immediately, rather than only the first time
    // some client's Hello gets rejected -- otherwise launching the host
    // and looking at it before any client has tried connecting shows
    // nothing (see docs/known-limitations.md).
    if (config_.authToken.empty()) {
        pairingManager_.ensureCodeActive();
    }

    controlThread_ = std::thread(&NetServer::controlLoop, this);
    inputThread_ = std::thread(&NetServer::inputLoop, this);
    videoThread_ = std::thread(&NetServer::videoLoop, this);
    watchdogThread_ = std::thread(&NetServer::watchdogLoop, this);

    if (config_.discoveryEnabled) {
        // Bound to "0.0.0.0", not config_.bindAddress -- see the comment
        // on NetServerConfig::discoveryEnabled for why. A bind failure
        // here (e.g. port already in use) is not fatal to the rest of
        // the server -- discovery is a convenience, not load-bearing;
        // clients can still be given a host address directly.
        discoveryFd_ = makeUdpSocket("0.0.0.0", config_.discoveryPort);
        if (discoveryFd_ < 0) {
            std::fprintf(stderr,
                          "NetServer: discovery disabled (failed to bind UDP port %u) -- "
                          "clients must be given the host address directly\n",
                          config_.discoveryPort);
        } else {
            int broadcast = 1;
            ::setsockopt(discoveryFd_, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
            std::fprintf(stderr, "NetServer: LAN discovery listening on 0.0.0.0:%u\n",
                          config_.discoveryPort);
            discoveryThread_ = std::thread(&NetServer::discoveryLoop, this);
        }
    }
}

void NetServer::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    // Unblock accept()/recv() calls by closing the listening/connected fds.
    if (controlListenFd_ >= 0) ::shutdown(controlListenFd_, SHUT_RDWR);
    if (videoListenFd_ >= 0) ::shutdown(videoListenFd_, SHUT_RDWR);
    if (inputFd_ >= 0) ::shutdown(inputFd_, SHUT_RDWR);
    if (discoveryFd_ >= 0) ::shutdown(discoveryFd_, SHUT_RDWR);

    int controlClient = controlClientFd_.exchange(-1);
    if (controlClient >= 0) ::shutdown(controlClient, SHUT_RDWR);
    int videoClient = videoClientFd_.exchange(-1);
    if (videoClient >= 0) ::shutdown(videoClient, SHUT_RDWR);

    if (controlThread_.joinable()) controlThread_.join();
    if (inputThread_.joinable()) inputThread_.join();
    if (videoThread_.joinable()) videoThread_.join();
    if (watchdogThread_.joinable()) watchdogThread_.join();
    if (discoveryThread_.joinable()) discoveryThread_.join();

    if (controlListenFd_ >= 0) ::close(controlListenFd_);
    if (videoListenFd_ >= 0) ::close(videoListenFd_);
    if (inputFd_ >= 0) ::close(inputFd_);
    if (discoveryFd_ >= 0) ::close(discoveryFd_);
    controlListenFd_ = videoListenFd_ = inputFd_ = discoveryFd_ = -1;
}

namespace {
// Upper bound on a Hello payload's declared size, well above what any
// legitimate client name/platform/token combination needs, so a hostile
// payloadSize value can't be used to make the host allocate/read
// arbitrarily large amounts of data (spec section 13).
constexpr uint32_t kMaxHelloPayloadSize = 512;
} // namespace

void NetServer::controlLoop() {
    while (running_.load()) {
        sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        int clientFd = ::accept(controlListenFd_, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
        if (clientFd < 0) {
            if (!running_.load()) break;
            continue;
        }

        char ipStr[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr));

        bool rateOk;
        {
            std::lock_guard<std::mutex> lock(rateLimiterMutex_);
            rateOk = rateLimiter_.allowAttempt(ipStr, nowMicros());
        }
        if (!rateOk) {
            std::fprintf(stderr, "NetServer: rate-limiting connection attempts from %s\n", ipStr);
            ::close(clientFd);
            continue;
        }

        // Only one client at a time (spec section 7.1 initial scope).
        int previous = controlClientFd_.exchange(clientFd);
        if (previous >= 0) {
            std::fprintf(stderr, "NetServer: rejecting extra control connection\n");
            ::close(clientFd);
            controlClientFd_ = previous;
            continue;
        }

        std::fprintf(stderr, "NetServer: control connection from %s\n", ipStr);

        // Bound how long a client may take to complete the handshake and
        // how long the connection may sit idle afterward (spec section 8.1
        // heartbeat/keepalive).
        timeval recvTimeout{};
        recvTimeout.tv_sec = static_cast<time_t>(config_.controlHeartbeatTimeoutUs / 1'000'000);
        recvTimeout.tv_usec = static_cast<suseconds_t>(config_.controlHeartbeatTimeoutUs % 1'000'000);
        ::setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &recvTimeout, sizeof(recvTimeout));

        bool handshakeOk = false;
        HelloRejectReason rejectReason = HelloRejectReason::ProtocolVersionMismatch;
        std::string issuedPairingToken;

        uint8_t headerBuf[kPacketHeaderWireSize];
        ssize_t n = ::recv(clientFd, headerBuf, sizeof(headerBuf), MSG_WAITALL);
        if (n == static_cast<ssize_t>(sizeof(headerBuf))) {
            auto header = parseHeader(headerBuf, sizeof(headerBuf));
            if (header && header->type == PacketType::Hello &&
                header->protocolVersion == kProtocolVersion &&
                header->payloadSize <= kMaxHelloPayloadSize) {
                ByteBuffer payloadBuf(header->payloadSize);
                ssize_t got = header->payloadSize == 0
                                  ? 0
                                  : ::recv(clientFd, payloadBuf.data(), payloadBuf.size(), MSG_WAITALL);
                if (got == static_cast<ssize_t>(payloadBuf.size())) {
                    auto hello = parseHelloPayload(payloadBuf.data(), payloadBuf.size());
                    if (hello) {
                        if (!config_.authToken.empty()) {
                            // Legacy/CI-friendly static-token mode: exact match or reject,
                            // pairing-code flow is bypassed entirely.
                            if (constantTimeEquals(hello->authToken, config_.authToken)) {
                                handshakeOk = true;
                            } else {
                                std::fprintf(stderr,
                                              "NetServer: rejecting handshake from %s (bad auth token)\n",
                                              ipStr);
                                rejectReason = HelloRejectReason::AuthenticationFailed;
                            }
                        } else {
                            switch (pairingManager_.check(hello->authToken)) {
                                case PairingManager::CheckResult::ValidExistingToken:
                                    handshakeOk = true;
                                    break;
                                case PairingManager::CheckResult::ValidCode:
                                    handshakeOk = true;
                                    issuedPairingToken = pairingManager_.lastIssuedToken();
                                    std::fprintf(stderr,
                                                  "NetServer: %s paired successfully with a new device token\n",
                                                  ipStr);
                                    break;
                                case PairingManager::CheckResult::NeedsCode:
                                case PairingManager::CheckResult::WrongCode:
                                    std::fprintf(stderr,
                                                  "NetServer: rejecting handshake from %s (pairing required)\n",
                                                  ipStr);
                                    rejectReason = HelloRejectReason::PairingRequired;
                                    break;
                            }
                        }
                    } else {
                        std::fprintf(stderr, "NetServer: rejecting handshake (malformed Hello payload)\n");
                    }
                } else {
                    std::fprintf(stderr, "NetServer: rejecting handshake (short Hello payload)\n");
                }
            } else {
                std::fprintf(stderr, "NetServer: rejecting handshake (bad magic/type/version)\n");
            }
        }

        HelloAckPayload ack;
        ack.accepted = handshakeOk ? 1 : 0;
        ack.rejectReason = handshakeOk ? HelloRejectReason::None : rejectReason;
        ack.sessionId = handshakeOk ? static_cast<uint32_t>(nowMicros()) : 0;
        ack.pairingToken = issuedPairingToken;
        ByteBuffer ackPacket = buildHelloAckPacket(ack);
        sendAll(clientFd, ackPacket.data(), ackPacket.size());

        if (handshakeOk) {
            // Only from this point on will inputLoop() act on
            // ControllerState packets, and only from this same source
            // address (spec section 13).
            authenticatedClientAddr_ = clientAddr.sin_addr.s_addr;
            clientAuthenticated_ = true;

            // Keep reading (heartbeats / disconnect notices / garbage) until
            // the peer closes, goes silent past controlHeartbeatTimeoutUs
            // (the recv() above times out and returns -1/EAGAIN), or sends
            // something malformed enough to drop.
            while (running_.load()) {
                uint8_t buf[kPacketHeaderWireSize];
                ssize_t r = ::recv(clientFd, buf, sizeof(buf), MSG_WAITALL);
                if (r <= 0) {
                    break; // peer closed, link error, or heartbeat timeout
                }
                auto hdr = parseHeader(buf, static_cast<size_t>(r));
                if (!hdr) {
                    std::fprintf(stderr, "NetServer: dropping malformed control packet\n");
                    break;
                }
                if (hdr->type == PacketType::Disconnect) {
                    break;
                }
                // Heartbeat or unrecognized-but-valid type: keep the
                // connection open.
            }
        }

        std::fprintf(stderr, "NetServer: control connection closed\n");
        ::close(clientFd);
        controlClientFd_ = -1;
        clientAuthenticated_ = false;
        authenticatedClientAddr_ = 0;

        {
            std::lock_guard<std::mutex> lock(trackerMutex_);
            inputTracker_.reset();
        }
        inputSink_.releaseAll();
    }
}

void NetServer::inputLoop() {
    ByteBuffer buf(kPacketHeaderWireSize + kControllerStateWireSize);

    while (running_.load()) {
        sockaddr_in fromAddr{};
        socklen_t fromLen = sizeof(fromAddr);
        ssize_t n = ::recvfrom(inputFd_, buf.data(), buf.size(), 0,
                                reinterpret_cast<sockaddr*>(&fromAddr), &fromLen);
        if (n <= 0) {
            if (!running_.load()) break;
            continue;
        }

        if (!clientAuthenticated_.load() ||
            fromAddr.sin_addr.s_addr != authenticatedClientAddr_.load()) {
            continue; // no authenticated session, or packet from an unrelated address
        }

        auto header = parseHeader(buf.data(), static_cast<size_t>(n));
        if (!header || header->protocolVersion != kProtocolVersion ||
            header->type != PacketType::ControllerState) {
            std::lock_guard<std::mutex> lock(statsMutex_);
            ++stats_.inputPacketsMalformed;
            continue; // reject malformed / mismatched packet, stay up
        }

        size_t payloadOffset = kPacketHeaderWireSize;
        size_t payloadSize = static_cast<size_t>(n) - payloadOffset;
        if (payloadSize != header->payloadSize || payloadSize != kControllerStateWireSize) {
            std::lock_guard<std::mutex> lock(statsMutex_);
            ++stats_.inputPacketsMalformed;
            continue;
        }

        auto state = parseControllerState(buf.data() + payloadOffset, payloadSize);
        if (!state) {
            std::lock_guard<std::mutex> lock(statsMutex_);
            ++stats_.inputPacketsMalformed;
            continue;
        }

        bool accepted;
        {
            std::lock_guard<std::mutex> lock(trackerMutex_);
            accepted = inputTracker_.onPacketReceived(*state, nowMicros());
        }

        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            if (accepted) {
                ++stats_.inputPacketsAccepted;
                // Latency estimate needs a shared time base with the
                // client, unlike the steady_clock used for
                // timeout/sequence bookkeeping above -- see the comment
                // on wallClockNowUs() in client/src/main.cpp. Skip
                // clearly-bogus deltas (clock not synced, or a client
                // that hasn't been updated to send wall-clock time yet)
                // rather than polluting the average with them.
                uint64_t nowWallUs = nowMicrosEpoch();
                if (nowWallUs >= state->clientTimestampUs) {
                    uint64_t latencyUs = nowWallUs - state->clientTimestampUs;
                    constexpr uint64_t kMaxPlausibleLatencyUs = 10'000'000; // 10s
                    if (latencyUs <= kMaxPlausibleLatencyUs) {
                        stats_.recordLatency(latencyUs);
                    }
                }
            } else {
                ++stats_.inputPacketsOutOfOrder;
            }
        }

        if (accepted) {
            inputSink_.applyControllerState(*state);
        }
    }
}

void NetServer::watchdogLoop() {
    uint64_t lastPruneUs = nowMicros();
    uint64_t lastStatsLogUs = nowMicros();
    uint64_t lastPairingCheckUs = nowMicros();
    constexpr uint64_t kPairingCheckIntervalUs = 5'000'000; // 5s

    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Keeps a pairing code visible even if it expires (default 5min
        // TTL) with nobody having attempted to pair yet -- without this,
        // an expired-and-never-replaced code would just sit there
        // looking valid until the next connection attempt.
        if (config_.authToken.empty()) {
            uint64_t now = nowMicros();
            if (now - lastPairingCheckUs >= kPairingCheckIntervalUs) {
                pairingManager_.ensureCodeActive();
                lastPairingCheckUs = now;
            }
        }

        bool timedOut;
        {
            std::lock_guard<std::mutex> lock(trackerMutex_);
            timedOut = inputTracker_.isTimedOut(nowMicros(), config_.inputTimeoutUs);
            if (timedOut) {
                inputTracker_.reset();
            }
        }
        if (timedOut) {
            std::fprintf(stderr, "NetServer: input timeout, releasing all inputs\n");
            inputSink_.releaseAll();
        }

        uint64_t now = nowMicros();
        if (now - lastPruneUs > config_.connectionAttemptWindowUs) {
            std::lock_guard<std::mutex> lock(rateLimiterMutex_);
            rateLimiter_.pruneStaleEntries(now);
            lastPruneUs = now;
        }

        if (now - lastStatsLogUs >= config_.statsLoggingIntervalUs) {
            NetServerStats snapshot;
            {
                std::lock_guard<std::mutex> lock(statsMutex_);
                snapshot = stats_;
                stats_ = NetServerStats{};
            }
            double windowSec = static_cast<double>(now - lastStatsLogUs) / 1'000'000.0;
            lastStatsLogUs = now;

            if (snapshot.inputPacketsAccepted || snapshot.inputPacketsOutOfOrder ||
                snapshot.inputPacketsMalformed || snapshot.framesSent || snapshot.framesDropped) {
                double inputRate = windowSec > 0 ? static_cast<double>(snapshot.inputPacketsAccepted) / windowSec : 0.0;
                double frameRate = windowSec > 0 ? static_cast<double>(snapshot.framesSent) / windowSec : 0.0;

                if (snapshot.latencySampleCount > 0) {
                    double avgLatencyMs =
                        static_cast<double>(snapshot.latencySumUs) / static_cast<double>(snapshot.latencySampleCount) / 1000.0;
                    std::fprintf(stderr,
                                  "NetServer: stats -- input: accepted=%llu outOfOrder=%llu malformed=%llu "
                                  "(%.1f/s) | video: sent=%llu (%.1f fps) dropped=%llu | latency: avg=%.1fms "
                                  "min=%.1fms max=%.1fms (n=%llu)\n",
                                  static_cast<unsigned long long>(snapshot.inputPacketsAccepted),
                                  static_cast<unsigned long long>(snapshot.inputPacketsOutOfOrder),
                                  static_cast<unsigned long long>(snapshot.inputPacketsMalformed), inputRate,
                                  static_cast<unsigned long long>(snapshot.framesSent), frameRate,
                                  static_cast<unsigned long long>(snapshot.framesDropped), avgLatencyMs,
                                  static_cast<double>(snapshot.latencyMinUs) / 1000.0,
                                  static_cast<double>(snapshot.latencyMaxUs) / 1000.0,
                                  static_cast<unsigned long long>(snapshot.latencySampleCount));
                } else {
                    std::fprintf(stderr,
                                  "NetServer: stats -- input: accepted=%llu outOfOrder=%llu malformed=%llu "
                                  "(%.1f/s) | video: sent=%llu (%.1f fps) dropped=%llu\n",
                                  static_cast<unsigned long long>(snapshot.inputPacketsAccepted),
                                  static_cast<unsigned long long>(snapshot.inputPacketsOutOfOrder),
                                  static_cast<unsigned long long>(snapshot.inputPacketsMalformed), inputRate,
                                  static_cast<unsigned long long>(snapshot.framesSent), frameRate,
                                  static_cast<unsigned long long>(snapshot.framesDropped));
                }
            }
        }
    }
}

void NetServer::videoLoop() {
    const auto interval = std::chrono::microseconds(
        1'000'000 / (config_.videoSendFps > 0 ? config_.videoSendFps : 60));

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

        std::vector<uint8_t> frame;
        std::optional<uint64_t> lastSentFrameIndex;
        while (running_.load()) {
            auto tickStart = std::chrono::steady_clock::now();

            uint64_t frameIndex = 0;
            if (frameSource_.getLatestFrame(frame, frameIndex)) {
                ByteBuffer packet = buildPacket(PacketType::VideoFrame, frame);
                if (!sendAll(clientFd, packet.data(), packet.size())) {
                    break;
                }

                std::lock_guard<std::mutex> lock(statsMutex_);
                ++stats_.framesSent;
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
}

void NetServer::discoveryLoop() {
    std::string hostName = config_.hostName;
    if (hostName.empty()) {
        char hostnameBuf[256] = {0};
        if (::gethostname(hostnameBuf, sizeof(hostnameBuf) - 1) == 0) {
            hostName = hostnameBuf;
        } else {
            hostName = "melonds-remote-host";
        }
    }
    if (hostName.size() > kMaxProtocolStringLength) {
        hostName.resize(kMaxProtocolStringLength);
    }

    ByteBuffer buf(kPacketHeaderWireSize);

    while (running_.load()) {
        sockaddr_in fromAddr{};
        socklen_t fromLen = sizeof(fromAddr);
        ssize_t n = ::recvfrom(discoveryFd_, buf.data(), buf.size(), 0,
                                reinterpret_cast<sockaddr*>(&fromAddr), &fromLen);
        if (n <= 0) {
            if (!running_.load()) break;
            continue;
        }

        auto header = parseHeader(buf.data(), static_cast<size_t>(n));
        if (!header || header->protocolVersion != kProtocolVersion ||
            header->type != PacketType::DiscoveryRequest) {
            continue; // not a well-formed DiscoveryRequest -- ignore silently
        }

        DiscoveryResponsePayload response;
        response.hostName = hostName;
        response.controlPort = config_.controlPort;
        response.inputPort = config_.inputPort;
        response.videoPort = config_.videoPort;
        ByteBuffer packet = buildDiscoveryResponsePacket(response);
        // Unicast back to the specific sender -- never a broadcast reply.
        ::sendto(discoveryFd_, packet.data(), packet.size(), 0,
                 reinterpret_cast<sockaddr*>(&fromAddr), fromLen);
    }
}

} // namespace melonds_remote::host
