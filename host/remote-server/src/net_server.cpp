#include "host/net_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>

#include "melonds_remote/protocol.h"

namespace melonds_remote::host {

namespace {

uint64_t nowMicros() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
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
    : config_(std::move(config)), inputSink_(inputSink), frameSource_(frameSource) {}

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

    controlThread_ = std::thread(&NetServer::controlLoop, this);
    inputThread_ = std::thread(&NetServer::inputLoop, this);
    videoThread_ = std::thread(&NetServer::videoLoop, this);
    watchdogThread_ = std::thread(&NetServer::watchdogLoop, this);
}

void NetServer::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    // Unblock accept()/recv() calls by closing the listening/connected fds.
    if (controlListenFd_ >= 0) ::shutdown(controlListenFd_, SHUT_RDWR);
    if (videoListenFd_ >= 0) ::shutdown(videoListenFd_, SHUT_RDWR);
    if (inputFd_ >= 0) ::shutdown(inputFd_, SHUT_RDWR);

    int controlClient = controlClientFd_.exchange(-1);
    if (controlClient >= 0) ::shutdown(controlClient, SHUT_RDWR);
    int videoClient = videoClientFd_.exchange(-1);
    if (videoClient >= 0) ::shutdown(videoClient, SHUT_RDWR);

    if (controlThread_.joinable()) controlThread_.join();
    if (inputThread_.joinable()) inputThread_.join();
    if (videoThread_.joinable()) videoThread_.join();
    if (watchdogThread_.joinable()) watchdogThread_.join();

    if (controlListenFd_ >= 0) ::close(controlListenFd_);
    if (videoListenFd_ >= 0) ::close(videoListenFd_);
    if (inputFd_ >= 0) ::close(inputFd_);
    controlListenFd_ = videoListenFd_ = inputFd_ = -1;
}

void NetServer::controlLoop() {
    while (running_.load()) {
        sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        int clientFd = ::accept(controlListenFd_, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
        if (clientFd < 0) {
            if (!running_.load()) break;
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

        char ipStr[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr));
        std::fprintf(stderr, "NetServer: control connection from %s\n", ipStr);

        uint8_t headerBuf[kPacketHeaderWireSize];
        ssize_t n = ::recv(clientFd, headerBuf, sizeof(headerBuf), MSG_WAITALL);
        bool handshakeOk = false;
        if (n == static_cast<ssize_t>(sizeof(headerBuf))) {
            auto header = parseHeader(headerBuf, sizeof(headerBuf));
            if (header && header->type == PacketType::Hello &&
                header->protocolVersion == kProtocolVersion) {
                handshakeOk = true;
            } else {
                std::fprintf(stderr, "NetServer: rejecting handshake (bad magic/type/version)\n");
            }
        }

        if (handshakeOk) {
            PacketHeader ackHeader;
            ackHeader.type = PacketType::HelloAck;
            ackHeader.payloadSize = 0;
            ByteBuffer ack;
            serializeHeader(ack, ackHeader);
            sendAll(clientFd, ack.data(), ack.size());

            // Keep reading (heartbeats / disconnect notices / garbage) until
            // the peer closes or sends something malformed enough to drop.
            while (running_.load()) {
                uint8_t buf[kPacketHeaderWireSize];
                ssize_t r = ::recv(clientFd, buf, sizeof(buf), MSG_WAITALL);
                if (r <= 0) {
                    break; // peer closed or link error
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
                // connection open; liveness is primarily tracked via the
                // UDP input stream (see watchdogLoop).
            }
        }

        std::fprintf(stderr, "NetServer: control connection closed\n");
        ::close(clientFd);
        controlClientFd_ = -1;

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

        auto header = parseHeader(buf.data(), static_cast<size_t>(n));
        if (!header || header->protocolVersion != kProtocolVersion ||
            header->type != PacketType::ControllerState) {
            continue; // reject malformed / mismatched packet, stay up
        }

        size_t payloadOffset = kPacketHeaderWireSize;
        size_t payloadSize = static_cast<size_t>(n) - payloadOffset;
        if (payloadSize != header->payloadSize || payloadSize != kControllerStateWireSize) {
            continue;
        }

        auto state = parseControllerState(buf.data() + payloadOffset, payloadSize);
        if (!state) {
            continue;
        }

        bool accepted;
        {
            std::lock_guard<std::mutex> lock(trackerMutex_);
            accepted = inputTracker_.onPacketReceived(*state, nowMicros());
        }
        if (accepted) {
            inputSink_.applyControllerState(*state);
        }
    }
}

void NetServer::watchdogLoop() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

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

        int nodelay = 1;
        ::setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        std::vector<uint8_t> frame;
        while (running_.load()) {
            auto tickStart = std::chrono::steady_clock::now();

            if (frameSource_.getLatestFrame(frame)) {
                ByteBuffer packet = buildPacket(PacketType::VideoFrame, frame);
                if (!sendAll(clientFd, packet.data(), packet.size())) {
                    break;
                }
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

} // namespace melonds_remote::host
