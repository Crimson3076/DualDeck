#include "net_client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <turbojpeg.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <thread>

namespace melonds_remote::client {

namespace {

// JPEG-decodes one video frame (protocol v8, see protocol.h's
// kProtocolVersion comment) back into raw BGRA8888 -- TJPF_BGRA as the
// output format means no manual channel reordering, matching this
// project's existing BGRA8888-everywhere convention. `outBgra` is resized
// to exactly width*height*4 regardless of its previous contents. Returns
// false (leaving outBgra unspecified) if `jpeg` isn't a valid JPEG image of
// exactly width x height -- a mismatched size is treated as a decode
// failure rather than silently cropped/padded, since it would otherwise
// silently corrupt the frame the caller renders.
bool decompressJpegToBgra(tjhandle decompressor, const uint8_t* jpeg, size_t jpegSize, int width,
                           int height, std::vector<uint8_t>& outBgra) {
    int jpegWidth = 0, jpegHeight = 0, jpegSubsamp = 0, jpegColorspace = 0;
    if (tjDecompressHeader3(decompressor, jpeg, static_cast<unsigned long>(jpegSize), &jpegWidth,
                             &jpegHeight, &jpegSubsamp, &jpegColorspace) != 0) {
        std::fprintf(stderr, "video: tjDecompressHeader3 failed: %s\n", tjGetErrorStr2(decompressor));
        return false;
    }
    if (jpegWidth != width || jpegHeight != height) {
        std::fprintf(stderr, "video: decoded JPEG is %dx%d, expected %dx%d\n", jpegWidth, jpegHeight,
                     width, height);
        return false;
    }

    outBgra.resize(static_cast<size_t>(width) * height * 4);
    if (tjDecompress2(decompressor, jpeg, static_cast<unsigned long>(jpegSize), outBgra.data(), width,
                       /*pitch=*/0, height, TJPF_BGRA, TJFLAG_FASTDCT) != 0) {
        std::fprintf(stderr, "video: tjDecompress2 failed: %s\n", tjGetErrorStr2(decompressor));
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
    helloPayload.videoQuality = config_.videoQuality;
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
    hostMode_ = ack->mode;

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
        std::fprintf(stderr, "video: tjInitDecompress failed, cannot receive video\n");
        connected_ = false;
        return;
    }

    while (connected_.load()) {
        if (!recvExact(videoFd_, headerBuf.data(), headerBuf.size())) {
            break;
        }
        // hostNativeWidth_/hostNativeHeight_ (this connection's own
        // HelloAck-reported dimensions, not a fixed DS-sized constant --
        // see the AzaharAdapter/3DS note this comment used to carry)
        // bound how large a compressed payload could legitimately be:
        // real JPEG output at any quality is essentially never larger
        // than the equivalent raw frame, so this is a generous sanity
        // ceiling against a corrupt/bogus size field, not a tight
        // expected-size check like before compression (payload size now
        // varies frame to frame with scene content).
        const uint32_t rawFrameBytes =
            static_cast<uint32_t>(hostNativeWidth_.load()) * hostNativeHeight_.load() * 4u;
        auto header = parseHeader(headerBuf.data(), headerBuf.size());
        if (!header || header->type != PacketType::VideoFrame || header->payloadSize == 0 ||
            header->payloadSize > rawFrameBytes) {
            std::fprintf(stderr, "video: dropping unexpected packet, closing connection\n");
            break;
        }

        payloadBuf.resize(header->payloadSize);
        if (!recvExact(videoFd_, payloadBuf.data(), payloadBuf.size())) {
            break;
        }

        if (!decompressJpegToBgra(jpegDecompressor, payloadBuf.data(), payloadBuf.size(),
                                   hostNativeWidth_.load(), hostNativeHeight_.load(), decodedFrame)) {
            std::fprintf(stderr, "video: dropping undecodable frame, closing connection\n");
            break;
        }

        std::lock_guard<std::mutex> lock(frameMutex_);
        latestFrame_.swap(decodedFrame);
        hasFrame_ = true;
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
            std::fprintf(stderr, "control: dropping malformed packet, closing connection\n");
            break;
        }

        payloadBuf.resize(header->payloadSize);
        if (!payloadBuf.empty() && !recvExact(controlFd_, payloadBuf.data(), payloadBuf.size())) {
            break;
        }

        if (header->type == PacketType::ModeChanged) {
            auto modeChanged = parseModeChangedPayload(payloadBuf.data(), payloadBuf.size());
            if (!modeChanged) {
                std::fprintf(stderr, "control: dropping malformed ModeChanged packet\n");
                continue;
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
