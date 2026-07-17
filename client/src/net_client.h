#pragma once

// Client-side counterpart to host/remote-server/include/host/net_server.h:
// TCP control handshake, UDP ControllerState sending, TCP video-frame
// receiving. Pure sockets + protocol/, no SDL, so the networking logic is
// testable independently of the windowing/input code in main.cpp.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "melonds_remote/protocol.h"

namespace melonds_remote::client {

struct NetClientConfig {
    std::string hostAddress = "127.0.0.1";
    uint16_t controlPort = 8760;
    uint16_t inputPort = 8761;
    uint16_t videoPort = 8762;
    uint16_t audioPort = 8765; // MicAudioFrame packets, GitHub issue #2

    std::string clientName = "SteamDeck";
    std::string clientPlatform = "linux";
    uint16_t displayWidth = 1280;
    uint16_t displayHeight = 800;

    // Must match the host's --auth-token if it has one configured
    // (static pre-shared secret, spec section 13). Otherwise, this is the
    // client's own persistent device identity (see device_identity.h) --
    // the same value on every connection attempt, to every host, set
    // once at startup and never changed for the life of the process; a
    // human at the host approves or denies it (see
    // host/remote-server/include/host/device_approval_manager.h).
    std::string authToken;

    // This client's own release version string (e.g. "v0.1.24"), sent to
    // the host in Hello. See melonds_remote::HelloPayload::appVersion for
    // the full explanation; empty (the default) means "unknown/dev
    // build", which disables the app-version check on the host side for
    // this connection regardless of what the host itself is running.
    std::string appVersion;

    // How often to send a Heartbeat packet on the control channel while
    // otherwise idle, so the host's control-channel timeout doesn't fire
    // on a live-but-quiet connection.
    uint32_t heartbeatIntervalMs = 1000;
};

class NetClient {
public:
    explicit NetClient(NetClientConfig config);
    ~NetClient();

    NetClient(const NetClient&) = delete;
    NetClient& operator=(const NetClient&) = delete;

    // Performs the control handshake and starts the background video
    // receive thread. Returns false if the control connection or
    // handshake fails.
    bool connect();
    void disconnect();
    bool isConnected() const { return connected_.load(); }

    // Sends one ControllerState packet over UDP. Fire-and-forget, matching
    // the "send full state at a fixed rate" model (spec section 6.3) --
    // the caller is expected to call this at ~120Hz.
    void sendControllerState(const ControllerState& state);

    // Sends one MicAudioFrame packet over its own UDP socket (issue #2),
    // fire-and-forget like sendControllerState() above. No-op if not
    // connected or the host didn't advertise micSupported in its
    // HelloAck -- callers should check hostMicSupported() before ever
    // opening a capture device in the first place, but this is a safe
    // no-op regardless.
    void sendMicAudioFrame(const MicAudioFramePayload& frame);

    // Whether the host's most recent HelloAck reported it can accept and
    // inject microphone audio. Only meaningful once connected (false
    // otherwise, same convention as sessionId()/hostAppVersion()).
    bool hostMicSupported() const { return hostMicSupported_.load(); }

    // Copies the most recently received video frame (BGRA8888,
    // host::kFrameSizeBytes long) into `outFrame` and returns true, or
    // returns false if no frame has arrived yet. Never blocks.
    bool getLatestFrame(std::vector<uint8_t>& outFrame);

    // Session ID assigned by the host in HelloAck, or 0 if not connected.
    // Informational only (e.g. for logging); not currently used to
    // validate anything client-side.
    uint32_t sessionId() const { return sessionId_.load(); }

    // Meaningful only right after a connect() call returns false: why the
    // host rejected the handshake. In particular, ApprovalRequired means
    // the device identity in NetClientConfig::authToken hasn't been
    // approved by a human at the host yet -- there's nothing to do on the
    // client side but keep retrying (the caller's existing reconnect/
    // backoff loop already does this), unlike the old 6-digit-code flow
    // this replaced.
    HelloRejectReason lastRejectReason() const;

    // The host's own release version string as reported in its most
    // recent HelloAck, regardless of whether that handshake was accepted
    // -- meaningful right after any connect() call that got as far as
    // receiving a HelloAck (including an AppVersionMismatch rejection, so
    // the caller can display e.g. "host is on vX, you're on vY"). Empty
    // if no HelloAck has been received yet, or if the host itself didn't
    // report a version.
    std::string hostAppVersion() const;

private:
    void videoReceiveLoop();
    void heartbeatLoop();
    void closePartialConnection();

    NetClientConfig config_;

    // atomic because connect()/disconnect() may run on a different thread
    // (the auto-reconnect loop in main.cpp) than sendControllerState(),
    // which reads udpFd_ on every call from the render/input thread.
    std::atomic<int> controlFd_{-1};
    std::atomic<int> videoFd_{-1};
    std::atomic<int> udpFd_{-1};
    std::atomic<int> udpAudioFd_{-1};
    std::atomic<uint32_t> sessionId_{0};
    std::atomic<bool> hostMicSupported_{false};

    // Serializes connect()/disconnect() against each other -- callers may
    // run reconnect-on-a-background-thread while the main thread can
    // still call disconnect() at shutdown. connectionAttemptInProgress_
    // prevents a second caller from queueing another connect behind the
    // first one while the handshake is still blocking.
    std::mutex connectMutex_;
    std::atomic<bool> connectionAttemptInProgress_{false};

    std::atomic<bool> connected_{false};
    std::thread videoThread_;
    std::thread heartbeatThread_;

    std::mutex frameMutex_;
    std::vector<uint8_t> latestFrame_;
    bool hasFrame_ = false;

    // Guards lastRejectReason_; connect() holds connectMutex_ for its
    // whole body anyway, but lastRejectReason() may be called from a
    // different thread (e.g. the render thread reacting to connect()'s
    // return value while a background reconnect thread also calls
    // connect()), so it gets its own narrower lock.
    mutable std::mutex handshakeResultMutex_;
    HelloRejectReason lastRejectReason_ = HelloRejectReason::None;
    std::string hostAppVersion_;
};

} // namespace melonds_remote::client
