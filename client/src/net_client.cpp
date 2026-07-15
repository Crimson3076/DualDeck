#include "net_client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>

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

bool NetClient::connect() {
    controlFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (controlFd_ < 0) {
        std::perror("socket (control)");
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.controlPort);
    if (::inet_pton(AF_INET, config_.hostAddress.c_str(), &addr.sin_addr) != 1) {
        std::fprintf(stderr, "invalid host address: %s\n", config_.hostAddress.c_str());
        ::close(controlFd_);
        controlFd_ = -1;
        return false;
    }

    if (::connect(controlFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("connect (control)");
        ::close(controlFd_);
        controlFd_ = -1;
        return false;
    }

    ByteBuffer hello;
    serializeHeader(hello, PacketHeader{kPacketMagic, kProtocolVersion, PacketType::Hello, 0});
    if (!sendAll(controlFd_, hello.data(), hello.size())) {
        std::fprintf(stderr, "failed to send Hello\n");
        return false;
    }

    uint8_t ackBuf[kPacketHeaderWireSize];
    if (!recvExact(controlFd_, ackBuf, sizeof(ackBuf))) {
        std::fprintf(stderr, "failed to receive HelloAck\n");
        return false;
    }
    auto ackHeader = parseHeader(ackBuf, sizeof(ackBuf));
    if (!ackHeader || ackHeader->type != PacketType::HelloAck) {
        std::fprintf(stderr, "handshake rejected by host\n");
        return false;
    }

    // Video channel: a second TCP connection dedicated to frame streaming
    // (see docs/protocol.md -- Stage 1 keeps control and video separate
    // rather than multiplexing one socket).
    sockaddr_in videoAddr = addr;
    videoAddr.sin_port = htons(config_.videoPort);
    videoFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (videoFd_ < 0 || ::connect(videoFd_, reinterpret_cast<sockaddr*>(&videoAddr), sizeof(videoAddr)) < 0) {
        std::perror("connect (video)");
        return false;
    }

    udpFd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (udpFd_ < 0) {
        std::perror("socket (udp input)");
        return false;
    }
    sockaddr_in inputAddr = addr;
    inputAddr.sin_port = htons(config_.inputPort);
    if (::connect(udpFd_, reinterpret_cast<sockaddr*>(&inputAddr), sizeof(inputAddr)) < 0) {
        // connect() on a UDP socket just fixes the destination for later
        // send() calls; failure here means the address family/setup is
        // wrong, not an actual network condition.
        std::perror("connect (udp input)");
        return false;
    }

    connected_ = true;
    videoThread_ = std::thread(&NetClient::videoReceiveLoop, this);
    return true;
}

void NetClient::disconnect() {
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

    if (videoThread_.joinable()) videoThread_.join();

    if (controlFd_ >= 0) { ::close(controlFd_); controlFd_ = -1; }
    if (videoFd_ >= 0) { ::close(videoFd_); videoFd_ = -1; }
    if (udpFd_ >= 0) { ::close(udpFd_); udpFd_ = -1; }
}

void NetClient::sendControllerState(const ControllerState& state) {
    if (!connected_.load() || udpFd_ < 0) return;
    ByteBuffer packet = buildControllerStatePacket(state);
    ::send(udpFd_, packet.data(), packet.size(), MSG_NOSIGNAL);
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
