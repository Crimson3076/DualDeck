#include "melonds_remote/adapter/ipc/adapter_ipc_client.h"

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <utility>

#include "melonds_remote/adapter/ipc/ipc_protocol.h"
#include "melonds_remote/adapter/ipc/socket_path.h"

namespace melonds_remote::adapter::ipc {

namespace {

// See adapter_ipc_server.cpp's identical constant/rationale -- applied
// here so this side's own recv() calls (the HelloAck wait in connect(),
// and every subsequent read in readLoop()) can't block forever either,
// e.g. if the Host Service is still busy serving a previous adapter's
// connection and never gets around to accepting this one (see
// AdapterIpcServer's "one at a time" policy) or has otherwise stopped
// responding.
constexpr int kRecvTimeoutSeconds = 5;

void setRecvTimeout(int fd, int seconds) {
    timeval tv{};
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
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

bool sendMessage(int fd, IpcMessageType type, const melonds_remote::ByteBuffer& payload) {
    melonds_remote::ByteBuffer packet = buildIpcMessage(type, payload);
    return sendAll(fd, packet.data(), packet.size());
}

// Mirrors adapter_ipc_server.cpp's recvMessage() exactly (same
// bounded-read, reject-rather-than-resync policy) -- kept as a separate
// copy rather than a shared header function since each side's error
// handling differs slightly (the server drops the whole session on any
// malformed message from a possibly-hostile local peer; the client
// below does the same toward a Host Service it trusts by construction,
// but keeping the logic duplicated once each keeps both files
// independently readable without an extra shared internal header for
// two ~15-line functions).
std::optional<std::pair<IpcHeader, melonds_remote::ByteBuffer>> recvMessage(int fd) {
    uint8_t headerBuf[kIpcHeaderWireSize];
    ssize_t n = ::recv(fd, headerBuf, sizeof(headerBuf), MSG_WAITALL);
    if (n != static_cast<ssize_t>(sizeof(headerBuf))) return std::nullopt;

    auto header = parseIpcHeader(headerBuf, sizeof(headerBuf));
    if (!header) return std::nullopt;

    constexpr uint32_t kMaxPayloadSize = kMaxIpcFramePixelBytes + 256;
    if (header->payloadSize > kMaxPayloadSize) return std::nullopt;

    melonds_remote::ByteBuffer payload(header->payloadSize);
    if (!payload.empty()) {
        ssize_t got = ::recv(fd, payload.data(), payload.size(), MSG_WAITALL);
        if (got != static_cast<ssize_t>(payload.size())) return std::nullopt;
    }
    return std::make_pair(*header, std::move(payload));
}

} // namespace

AdapterIpcClient::AdapterIpcClient(IEmulatorAdapter& localAdapter, std::string socketPath)
    : localAdapter_(localAdapter),
      socketPath_(socketPath.empty() ? defaultAdapterSocketPath() : std::move(socketPath)) {}

AdapterIpcClient::~AdapterIpcClient() {
    disconnect();
}

void AdapterIpcClient::setConnectionStateCallback(std::function<void(bool)> callback) {
    connectionStateCallback_ = std::move(callback);
}

bool AdapterIpcClient::connect() {
    if (socketPath_.empty()) {
        std::fprintf(stderr, "AdapterIpcClient: no usable socket path\n");
        return false;
    }

    // A previous connection may have ended on its own -- readLoop()/
    // writeLoop() clear connected_ themselves on a recv timeout or send
    // failure -- without the caller ever calling disconnect(). Join any
    // such leftover threads before spawning new ones below: std::thread's
    // move-assign terminates the process if the target is still
    // joinable, and a caller reconnecting on this same instance after a
    // connection drop (see RemoteServerBridge's out-of-process mode,
    // GitHub issue #4) is exactly a caller who wouldn't otherwise know
    // to call disconnect() first. No-op on the first connect() call, or
    // when the caller already did call disconnect().
    if (readThread_.joinable()) readThread_.join();
    if (writeThread_.joinable()) writeThread_.join();

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        std::perror("AdapterIpcClient: socket");
        return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socketPath_.size() >= sizeof(addr.sun_path)) {
        std::fprintf(stderr, "AdapterIpcClient: socket path too long: %s\n", socketPath_.c_str());
        ::close(fd);
        return false;
    }
    std::strncpy(addr.sun_path, socketPath_.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("AdapterIpcClient: connect");
        ::close(fd);
        return false;
    }
    setRecvTimeout(fd, kRecvTimeoutSeconds);

    melonds_remote::ByteBuffer helloPayload;
    serializeAdapterCapabilities(helloPayload, localAdapter_.capabilities());
    if (!sendMessage(fd, IpcMessageType::Hello, helloPayload)) {
        ::close(fd);
        return false;
    }

    auto ackMsg = recvMessage(fd);
    if (!ackMsg || ackMsg->first.type != IpcMessageType::HelloAck) {
        std::fprintf(stderr, "AdapterIpcClient: no HelloAck received\n");
        ::close(fd);
        return false;
    }
    auto ack = parseAdapterHelloAckPayload(ackMsg->second.data(), ackMsg->second.size());
    if (!ack || !ack->accepted) {
        std::fprintf(stderr, "AdapterIpcClient: handshake rejected (reason=%d)\n",
                      ack ? static_cast<int>(ack->reason) : -1);
        ::close(fd);
        return false;
    }

    fd_ = fd;
    connected_ = true;
    lastSentFrameIndex_.clear();
    lastSentState_ = SessionState::Available;
    readThread_ = std::thread(&AdapterIpcClient::readLoop, this);
    writeThread_ = std::thread(&AdapterIpcClient::writeLoop, this);
    return true;
}

void AdapterIpcClient::disconnect() {
    bool wasConnected = connected_.exchange(false);
    if (wasConnected) {
        int fd = fd_.load();
        if (fd >= 0) {
            sendMessage(fd, IpcMessageType::Disconnect, {});
            ::shutdown(fd, SHUT_RDWR);
        }
    }

    // Always join, regardless of wasConnected -- readLoop()/writeLoop()
    // clear connected_ themselves on a recv timeout or send failure
    // (e.g. the Host Service went away, or a heartbeat send failed), so
    // by the time a caller notices via isConnected()==false and calls
    // this, connected_ may already be false while the threads are still
    // mid-exit. Skipping the join in that case (as an earlier version of
    // this function did) left readThread_/writeThread_ still joinable;
    // a caller that reconnects on this same instance (see
    // RemoteServerBridge's out-of-process mode, GitHub issue #4) would
    // then have connect() assign a new std::thread over one that's still
    // joinable, which terminates the process (std::thread's move-assign
    // requires the target not be joinable). Joining unconditionally here
    // is what makes reconnecting on the same AdapterIpcClient safe.
    if (readThread_.joinable()) readThread_.join();
    if (writeThread_.joinable()) writeThread_.join();

    int fd = fd_.exchange(-1);
    if (fd >= 0) ::close(fd);
}

void AdapterIpcClient::readLoop() {
    int fd = fd_.load();
    while (connected_.load()) {
        auto msg = recvMessage(fd);
        if (!msg) {
            connected_ = false;
            break;
        }

        switch (msg->first.type) {
            case IpcMessageType::InputState: {
                auto state = parseGenericInputState(msg->second.data(), msg->second.size());
                if (state) {
                    localAdapter_.applyGenericInput(*state);
                }
                break;
            }
            case IpcMessageType::ReleaseInputs:
                localAdapter_.releaseAllInputs();
                break;
            case IpcMessageType::ClientConnectionChanged: {
                auto connected = parseClientConnectionChanged(msg->second.data(), msg->second.size());
                if (connected && connectionStateCallback_) {
                    connectionStateCallback_(*connected);
                }
                break;
            }
            case IpcMessageType::Heartbeat:
                break;
            case IpcMessageType::Disconnect:
                connected_ = false;
                return;
            default:
                break;
        }
    }
}

void AdapterIpcClient::writeLoop() {
    // ~250Hz poll -- not tied to any one declared surface's own
    // nominalFps/maxFps (an adapter may have surfaces at different
    // rates); this just governs how often writeLoop() checks whether
    // there's anything new to send, not how often frames actually
    // change. Each iteration that finds nothing new (the per-surface
    // lastSentFrameIndex_ check below) is just a mutex-guarded
    // struct/vector copy and comparison -- no message is sent -- so
    // polling this often costs CPU wakeups, not bandwidth. Kept well
    // below the video-loop's own tick rate on the host side
    // (NetServerConfig::videoSendFps) so this stage is never the
    // bottleneck in the capture -> IPC -> relay -> send latency chain.
    constexpr auto kPollInterval = std::chrono::milliseconds(4);
    constexpr auto kHeartbeatInterval = std::chrono::seconds(1);
    auto lastHeartbeat = std::chrono::steady_clock::now();

    while (connected_.load()) {
        int fd = fd_.load();
        if (fd < 0) break;
        bool sentSomething = false;

        SessionState state = localAdapter_.currentState();
        if (state != lastSentState_) {
            melonds_remote::ByteBuffer payload;
            serializeSessionState(payload, state);
            if (!sendMessage(fd, IpcMessageType::StateChanged, payload)) {
                connected_ = false;
                break;
            }
            lastSentState_ = state;
            sentSomething = true;
        }

        for (const auto& surface : localAdapter_.capabilities().surfaces) {
            SurfaceFrame frame;
            if (!localAdapter_.latestFrame(surface.surfaceId, frame)) continue;

            auto it = lastSentFrameIndex_.find(surface.surfaceId);
            if (it != lastSentFrameIndex_.end() && it->second == frame.frameIndex) {
                continue; // nothing new for this surface since last tick
            }

            melonds_remote::ByteBuffer payload;
            serializeSurfaceFrame(payload, frame);
            if (!sendMessage(fd, IpcMessageType::Frame, payload)) {
                connected_ = false;
                break;
            }
            lastSentFrameIndex_[surface.surfaceId] = frame.frameIndex;
            sentSomething = true;
        }
        if (!connected_.load()) break;

        auto now = std::chrono::steady_clock::now();
        if (!sentSomething && now - lastHeartbeat >= kHeartbeatInterval) {
            if (!sendMessage(fd, IpcMessageType::Heartbeat, {})) {
                connected_ = false;
                break;
            }
        }
        if (sentSomething || now - lastHeartbeat >= kHeartbeatInterval) {
            lastHeartbeat = now;
        }

        std::this_thread::sleep_for(kPollInterval);
    }
}

} // namespace melonds_remote::adapter::ipc
