#include "melonds_remote/adapter/ipc/adapter_ipc_server.h"

#include <sys/socket.h>
#include <sys/stat.h>
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

// Without this, a peer that stops sending mid-connection (crashed,
// hung, or simply never got around to accept()ing a backlogged
// connection while a prior one was still being served -- see
// AdapterIpcServer's "one at a time" policy) leaves recv() blocking
// forever, since none of the recv() calls on this channel otherwise
// specify a timeout. Matches NetServer::controlLoop()'s existing
// SO_RCVTIMEO precedent for the client<->host control channel, applied
// here to the local adapter channel instead. A connected-but-silent
// adapter is expected to send at least a Heartbeat well within this
// window (see AdapterIpcClient::writeLoop()'s ~1s heartbeat interval).
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

// Reads one full message's header+payload. Returns std::nullopt on
// disconnect, error, or a malformed header/oversized payload -- the
// caller treats any of those as "this connection is over," matching
// NetServer::controlLoop()'s existing "malformed control packet ->
// drop the connection" policy rather than trying to resync a stream.
std::optional<std::pair<IpcHeader, melonds_remote::ByteBuffer>> recvMessage(int fd) {
    uint8_t headerBuf[kIpcHeaderWireSize];
    ssize_t n = ::recv(fd, headerBuf, sizeof(headerBuf), MSG_WAITALL);
    if (n != static_cast<ssize_t>(sizeof(headerBuf))) return std::nullopt;

    auto header = parseIpcHeader(headerBuf, sizeof(headerBuf));
    if (!header) return std::nullopt;

    // Same bound as the largest single payload this channel ever
    // legitimately carries (a Frame message) -- rejects a hostile/buggy
    // declared payloadSize before ever allocating for it.
    constexpr uint32_t kMaxPayloadSize = kMaxIpcFramePixelBytes + 256;
    if (header->payloadSize > kMaxPayloadSize) return std::nullopt;

    melonds_remote::ByteBuffer payload(header->payloadSize);
    if (!payload.empty()) {
        ssize_t got = ::recv(fd, payload.data(), payload.size(), MSG_WAITALL);
        if (got != static_cast<ssize_t>(payload.size())) return std::nullopt;
    }
    return std::make_pair(*header, std::move(payload));
}

// GitHub issue #28: "How only the current user may register an
// adapter" -- the socket's own filesystem permissions (0700 dir, and
// implicitly the socket file itself inherits the umask-restricted mode
// set at bind time below) already scope this to processes that can
// even open the path, but SO_PEERCRED is a second, cheap, explicit
// check that doesn't depend on getting every permission bit right
// elsewhere: reject a connection outright if the connecting process's
// real UID doesn't match ours, before any handshake bytes are even
// read.
bool peerIsSameUser(int fd) {
    struct ucred cred{};
    socklen_t len = sizeof(cred);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
        return false; // can't verify -- fail closed
    }
    return cred.uid == ::geteuid();
}

} // namespace

AdapterIpcServer::AdapterIpcServer(std::string socketPath)
    : socketPath_(socketPath.empty() ? defaultAdapterSocketPath() : std::move(socketPath)) {}

AdapterIpcServer::~AdapterIpcServer() {
    stop();
}

bool AdapterIpcServer::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return false;
    }

    if (socketPath_.empty() || !ensureSocketDirectory(socketPath_)) {
        std::fprintf(stderr, "AdapterIpcServer: no usable socket path, not starting\n");
        running_ = false;
        return false;
    }

    listenFd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        std::perror("AdapterIpcServer: socket");
        running_ = false;
        return false;
    }

    // A stale socket file from a previous, uncleanly-terminated run
    // must not block bind() -- unlink it first (bind() on an
    // AF_UNIX path fails with EADDRINUSE if the path already exists,
    // even if nothing is listening on it anymore).
    ::unlink(socketPath_.c_str());

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socketPath_.size() >= sizeof(addr.sun_path)) {
        std::fprintf(stderr, "AdapterIpcServer: socket path too long: %s\n", socketPath_.c_str());
        ::close(listenFd_);
        listenFd_ = -1;
        running_ = false;
        return false;
    }
    std::strncpy(addr.sun_path, socketPath_.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("AdapterIpcServer: bind");
        ::close(listenFd_);
        listenFd_ = -1;
        running_ = false;
        return false;
    }

    // Belt-and-suspenders alongside the parent directory's 0700 mode
    // and the SO_PEERCRED check in serveConnection() -- explicitly set
    // the socket file itself to 0600 regardless of umask.
    ::chmod(socketPath_.c_str(), 0600);

    if (::listen(listenFd_, 1) < 0) {
        std::perror("AdapterIpcServer: listen");
        ::close(listenFd_);
        listenFd_ = -1;
        running_ = false;
        return false;
    }

    acceptThread_ = std::thread(&AdapterIpcServer::acceptLoop, this);
    return true;
}

void AdapterIpcServer::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    if (listenFd_ >= 0) ::shutdown(listenFd_, SHUT_RDWR);
    int client = clientFd_.exchange(-1);
    if (client >= 0) ::shutdown(client, SHUT_RDWR);

    if (acceptThread_.joinable()) acceptThread_.join();

    if (listenFd_ >= 0) ::close(listenFd_);
    listenFd_ = -1;
    ::unlink(socketPath_.c_str());
}

void AdapterIpcServer::acceptLoop() {
    while (running_.load()) {
        int clientFd = ::accept(listenFd_, nullptr, nullptr);
        if (clientFd < 0) {
            if (!running_.load()) break;
            continue;
        }

        if (!peerIsSameUser(clientFd)) {
            std::fprintf(stderr, "AdapterIpcServer: rejecting connection from a different user\n");
            ::close(clientFd);
            continue;
        }

        setRecvTimeout(clientFd, kRecvTimeoutSeconds);
        serveConnection(clientFd);
    }
}

void AdapterIpcServer::serveConnection(int clientFd) {
    auto helloMsg = recvMessage(clientFd);
    bool handshakeOk = false;
    AdapterHelloAckReason rejectReason = AdapterHelloAckReason::ContractVersionMismatch;
    AdapterCapabilities negotiatedCaps;

    if (helloMsg && helloMsg->first.type == IpcMessageType::Hello &&
        helloMsg->first.contractVersion == kAdapterContractVersion) {
        auto caps = parseAdapterCapabilities(helloMsg->second.data(), helloMsg->second.size());
        if (caps) {
            handshakeOk = true;
            negotiatedCaps = std::move(*caps);
        }
    }

    AdapterHelloAckPayload ack;
    ack.accepted = handshakeOk ? 1 : 0;
    ack.reason = handshakeOk ? AdapterHelloAckReason::None : rejectReason;
    melonds_remote::ByteBuffer ackPayload;
    serializeAdapterHelloAckPayload(ackPayload, ack);
    sendMessage(clientFd, IpcMessageType::HelloAck, ackPayload);

    if (!handshakeOk) {
        ::close(clientFd);
        return;
    }

    bool clientAlreadyConnected = false;
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        capabilities_ = negotiatedCaps;
        state_ = SessionState::Available;
        latestFrames_.clear();
        connected_ = true;
        clientAlreadyConnected = lastKnownClientConnected_;
    }
    clientFd_ = clientFd;

    // Catch this newly-connected adapter up to the real, current remote-
    // client-connection state -- see lastKnownClientConnected_'s own
    // comment for the real bug this fixes (a client that connected
    // before this adapter finished its own handshake would otherwise
    // never be reported to it at all, until the next connect/disconnect
    // transition). A "false" is just as worth sending as a "true": either
    // way this adapter's own state (e.g. Azahar's remote_client_have_
    // saved_layout) should reflect reality from the moment it's ready to
    // receive messages, not assume a default.
    {
        melonds_remote::ByteBuffer payload;
        serializeClientConnectionChanged(payload, clientAlreadyConnected);
        std::lock_guard<std::mutex> lock(writeMutex_);
        sendMessage(clientFd, IpcMessageType::ClientConnectionChanged, payload);
    }

    // Sends a periodic heartbeat to the connected adapter so its own
    // recv() (5s SO_RCVTIMEO -- see AdapterIpcClient::readLoop()) never
    // idle-times-out just because no real input happened to arrive
    // recently. This server otherwise only ever sends anything to the
    // adapter in direct response to real client traffic
    // (applyGenericInput()/releaseAllInputs()), so with no client
    // connected -- or a connected-but-quiet one -- there can genuinely
    // be zero outbound bytes for 5+ seconds, which the adapter would
    // otherwise wrongly read as a dead connection and tear the whole
    // session down (GitHub issue #4's out-of-process melonDS mode is
    // what first exercised this: melonDS reconnected in a loop with no
    // client ever attached, since there was never anything to relay).
    // Mirrors AdapterIpcClient::writeLoop()'s own heartbeat-when-idle
    // logic, just in the other direction.
    std::atomic<bool> heartbeatStop{false};
    std::thread heartbeatThread([this, clientFd, &heartbeatStop]() {
        while (!heartbeatStop.load()) {
            for (int i = 0; i < 10 && !heartbeatStop.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (heartbeatStop.load()) break;
            bool sent;
            {
                std::lock_guard<std::mutex> lock(writeMutex_);
                sent = sendMessage(clientFd, IpcMessageType::Heartbeat, {});
            }
            if (!sent) break;
        }
    });

    bool explicitDisconnect = false;
    while (running_.load()) {
        auto msg = recvMessage(clientFd);
        if (!msg) break; // disconnect, error, or malformed -- end this session

        switch (msg->first.type) {
            case IpcMessageType::Frame: {
                auto frame = parseSurfaceFrame(msg->second.data(), msg->second.size());
                if (frame) {
                    std::lock_guard<std::mutex> lock(sessionMutex_);
                    latestFrames_[frame->surfaceId] = std::move(*frame);
                }
                break;
            }
            case IpcMessageType::StateChanged: {
                auto state = parseSessionState(msg->second.data(), msg->second.size());
                if (state) {
                    std::lock_guard<std::mutex> lock(sessionMutex_);
                    state_ = *state;
                }
                break;
            }
            case IpcMessageType::Heartbeat:
                break; // keeps the connection open, nothing else to do
            case IpcMessageType::Disconnect:
                explicitDisconnect = true;
                break;
            default:
                break; // unrecognized-but-parsed message type: ignore, stay connected
        }
        if (explicitDisconnect) break;
    }

    heartbeatStop = true;
    if (heartbeatThread.joinable()) heartbeatThread.join();
    ::close(clientFd);
    resetSessionLocked();
}

void AdapterIpcServer::resetSessionLocked() {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    connected_ = false;
    clientFd_ = -1;
    latestFrames_.clear();
    // capabilities_/state_ deliberately left as last-known rather than
    // cleared -- mirrors how the client keeps showing "last known
    // system/adapter identity" across a drop (see client/src/main.cpp's
    // identityLine()) rather than blanking UI the instant a connection
    // drops.
}

bool AdapterIpcServer::hasConnectedAdapter() const {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    return connected_;
}

AdapterCapabilities AdapterIpcServer::capabilities() const {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    return capabilities_;
}

SessionState AdapterIpcServer::currentState() const {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    return state_;
}

void AdapterIpcServer::applyGenericInput(const GenericInputState& state) {
    int fd = clientFd_.load();
    if (fd < 0) return; // no adapter connected -- safe no-op, matches NetClient::sendControllerState()
    melonds_remote::ByteBuffer payload;
    serializeGenericInputState(payload, state);
    std::lock_guard<std::mutex> lock(writeMutex_);
    sendMessage(fd, IpcMessageType::InputState, payload);
}

void AdapterIpcServer::releaseAllInputs() {
    int fd = clientFd_.load();
    if (fd < 0) return; // safe no-op if nothing is connected to release
    std::lock_guard<std::mutex> lock(writeMutex_);
    sendMessage(fd, IpcMessageType::ReleaseInputs, {});
}

void AdapterIpcServer::notifyClientConnectionChanged(bool connected) {
    // Recorded unconditionally, even with no adapter currently connected
    // to actually tell -- see lastKnownClientConnected_'s own comment for
    // why: whichever adapter connects next needs to learn the real
    // current state instead of assuming "not connected" by default.
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        lastKnownClientConnected_ = connected;
    }
    int fd = clientFd_.load();
    if (fd < 0) return; // no adapter connected -- safe no-op, matches releaseAllInputs()'s same pattern
    melonds_remote::ByteBuffer payload;
    serializeClientConnectionChanged(payload, connected);
    std::lock_guard<std::mutex> lock(writeMutex_);
    sendMessage(fd, IpcMessageType::ClientConnectionChanged, payload);
}

bool AdapterIpcServer::latestFrame(const std::string& surfaceId, SurfaceFrame& outFrame) {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    auto it = latestFrames_.find(surfaceId);
    if (it == latestFrames_.end()) return false;
    outFrame = it->second;
    return true;
}

} // namespace melonds_remote::adapter::ipc
