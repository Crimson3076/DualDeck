#include "net_client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <thread>

namespace melonds_remote::client {

namespace {

// 256 * 192 * 4, kept in sync with host::kFrameSizeBytes by
// docs/protocol.md's fixed Stage-1 video format rather than a shared
// header, since the client does not link against host/remote-server.
constexpr size_t kExpectedFrameBytes = 256u * 192u * 4u;

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

    if (videoThread_.joinable()) videoThread_.join();
    if (heartbeatThread_.joinable()) heartbeatThread_.join();
}

bool NetClient::connect() {
    // A second connect() should not wait behind an in-flight handshake and
    // then make a duplicate attempt with the same user action. This is a
    // last line of defense for callers in addition to main.cpp giving its
    // reconnect thread sole ownership of connection attempts.
    bool expected = false;
    if (!connectionAttemptInProgress_.compare_exchange_strong(expected, true)) {
        std::fprintf(stderr, "connection attempt already in progress; ignoring duplicate request\n");
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
        std::perror("socket (control)");
        closePartialConnection();
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.controlPort);
    if (::inet_pton(AF_INET, config_.hostAddress.c_str(), &addr.sin_addr) != 1) {
        std::fprintf(stderr, "invalid host address: %s\n", config_.hostAddress.c_str());
        closePartialConnection();
        return false;
    }

    if (::connect(controlFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("connect (control)");
        closePartialConnection();
        return false;
    }

    HelloPayload helloPayload;
    helloPayload.clientName = config_.clientName;
    helloPayload.clientPlatform = config_.clientPlatform;
    helloPayload.displayWidth = config_.displayWidth;
    helloPayload.displayHeight = config_.displayHeight;
    helloPayload.authToken = config_.authToken;
    helloPayload.appVersion = config_.appVersion;
    ByteBuffer hello = buildHelloPacket(helloPayload);
    if (!sendAll(controlFd_, hello.data(), hello.size())) {
        std::fprintf(stderr, "failed to send Hello\n");
        closePartialConnection();
        return false;
    }

    uint8_t ackHeaderBuf[kPacketHeaderWireSize];
    if (!recvExact(controlFd_, ackHeaderBuf, sizeof(ackHeaderBuf))) {
        std::fprintf(stderr, "failed to receive HelloAck\n");
        closePartialConnection();
        return false;
    }
    auto ackHeader = parseHeader(ackHeaderBuf, sizeof(ackHeaderBuf));
    if (!ackHeader || ackHeader->type != PacketType::HelloAck || ackHeader->payloadSize > 256) {
        std::fprintf(stderr, "handshake rejected by host (bad response header)\n");
        closePartialConnection();
        return false;
    }

    ByteBuffer ackPayloadBuf(ackHeader->payloadSize);
    if (!ackPayloadBuf.empty() && !recvExact(controlFd_, ackPayloadBuf.data(), ackPayloadBuf.size())) {
        std::fprintf(stderr, "failed to receive HelloAck payload\n");
        closePartialConnection();
        return false;
    }
    auto ack = parseHelloAckPayload(ackPayloadBuf.data(), ackPayloadBuf.size());
    if (!ack) {
        std::fprintf(stderr, "handshake rejected by host (malformed HelloAck payload)\n");
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
    if (!ack->accepted) {
        if (ack->rejectReason == HelloRejectReason::ApprovalRequired) {
            std::fprintf(stderr,
                          "handshake rejected by host: awaiting approval -- a human at the host "
                          "needs to approve this device, no action needed here\n");
        } else if (ack->rejectReason == HelloRejectReason::AppVersionMismatch) {
            std::fprintf(stderr,
                          "handshake rejected by host: version mismatch (host is %s, this client is "
                          "%s) -- update one side to match the other\n",
                          ack->appVersion.c_str(), config_.appVersion.c_str());
        } else {
            std::fprintf(stderr, "handshake rejected by host (reason code %d)\n",
                          static_cast<int>(ack->rejectReason));
        }
        closePartialConnection();
        return false;
    }
    sessionId_ = ack->sessionId;
    hostMicSupported_ = ack->micSupported != 0;

    // Video channel: a second TCP connection dedicated to frame streaming
    // (see docs/protocol.md -- Stage 1 keeps control and video separate
    // rather than multiplexing one socket).
    sockaddr_in videoAddr = addr;
    videoAddr.sin_port = htons(config_.videoPort);
    videoFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (videoFd_ < 0 || ::connect(videoFd_, reinterpret_cast<sockaddr*>(&videoAddr), sizeof(videoAddr)) < 0) {
        std::perror("connect (video)");
        closePartialConnection();
        return false;
    }

    udpFd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (udpFd_ < 0) {
        std::perror("socket (udp input)");
        closePartialConnection();
        return false;
    }
    sockaddr_in inputAddr = addr;
    inputAddr.sin_port = htons(config_.inputPort);
    if (::connect(udpFd_, reinterpret_cast<sockaddr*>(&inputAddr), sizeof(inputAddr)) < 0) {
        // connect() on a UDP socket just fixes the destination for later
        // send() calls; failure here means the address family/setup is
        // wrong, not an actual network condition.
        std::perror("connect (udp input)");
        closePartialConnection();
        return false;
    }

    udpAudioFd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (udpAudioFd_ < 0) {
        std::perror("socket (udp audio)");
        closePartialConnection();
        return false;
    }
    sockaddr_in audioAddr = addr;
    audioAddr.sin_port = htons(config_.audioPort);
    if (::connect(udpAudioFd_, reinterpret_cast<sockaddr*>(&audioAddr), sizeof(audioAddr)) < 0) {
        std::perror("connect (udp audio)");
        closePartialConnection();
        return false;
    }

    connected_ = true;
    videoThread_ = std::thread(&NetClient::videoReceiveLoop, this);
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
            break;
        }
        for (uint32_t waited = 0; waited < config_.heartbeatIntervalMs && connected_.load(); waited += 50) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
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

bool NetClient::getLatestFrame(std::vector<uint8_t>& outFrame) {
    std::lock_guard<std::mutex> lock(frameMutex_);
    if (!hasFrame_) return false;
    outFrame = latestFrame_;
    return true;
}

void NetClient::videoReceiveLoop() {
    std::vector<uint8_t> headerBuf(kPacketHeaderWireSize);
    std::vector<uint8_t> payloadBuf;

    while (connected_.load()) {
        if (!recvExact(videoFd_, headerBuf.data(), headerBuf.size())) {
            break;
        }
        auto header = parseHeader(headerBuf.data(), headerBuf.size());
        if (!header || header->type != PacketType::VideoFrame ||
            header->payloadSize != kExpectedFrameBytes) {
            std::fprintf(stderr, "video: dropping unexpected packet, closing connection\n");
            break;
        }

        payloadBuf.resize(header->payloadSize);
        if (!recvExact(videoFd_, payloadBuf.data(), payloadBuf.size())) {
            break;
        }

        std::lock_guard<std::mutex> lock(frameMutex_);
        latestFrame_.swap(payloadBuf);
        hasFrame_ = true;
    }

    connected_ = false;
}

} // namespace melonds_remote::client
