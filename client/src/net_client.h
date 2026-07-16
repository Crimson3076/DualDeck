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

    std::string clientName = "SteamDeck";
    std::string clientPlatform = "linux";
    uint16_t displayWidth = 1280;
    uint16_t displayHeight = 800;

    // Must match the host's --auth-token, if it has one configured; empty
    // if the host has authentication disabled (spec section 13).
    std::string authToken;

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

    // Copies the most recently received video frame (BGRA8888,
    // host::kFrameSizeBytes long) into `outFrame` and returns true, or
    // returns false if no frame has arrived yet. Never blocks.
    bool getLatestFrame(std::vector<uint8_t>& outFrame);

    // Session ID assigned by the host in HelloAck, or 0 if not connected.
    // Informational only (e.g. for logging); not currently used to
    // validate anything client-side.
    uint32_t sessionId() const { return sessionId_.load(); }

    // Meaningful only right after a connect() call returns false: why the
    // host rejected the handshake. In particular, PairingRequired means
    // the caller should prompt the user for the 6-digit code currently
    // displayed on the host (spec section 13) and retry via
    // setAuthToken() + connect(), rather than just keep blindly retrying
    // with the same (rejected) token.
    HelloRejectReason lastRejectReason() const;

    // Non-empty only right after a connect() call returns true where the
    // handshake just consumed a fresh pairing code: the persistent token
    // the caller must save (e.g. to disk, keyed by host address) and pass
    // to setAuthToken() on all future runs, so the user isn't prompted
    // for a code again.
    std::string lastPairingToken() const;

    // Changes the value sent as HelloPayload::authToken on the next
    // connect() call (a pairing code the user just entered, or a
    // previously-saved persistent pairing token). Safe to call whether or
    // not currently connected.
    void setAuthToken(std::string token);

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
    std::atomic<uint32_t> sessionId_{0};

    // Serializes connect()/disconnect() against each other -- callers may
    // run reconnect-on-a-background-thread while the main thread can
    // still call disconnect() at shutdown.
    std::mutex connectMutex_;

    std::atomic<bool> connected_{false};
    std::thread videoThread_;
    std::thread heartbeatThread_;

    std::mutex frameMutex_;
    std::vector<uint8_t> latestFrame_;
    bool hasFrame_ = false;

    // Guards config_.authToken (mutable via setAuthToken()) and the two
    // handshake-result fields below; connect() holds connectMutex_ for
    // its whole body anyway, but setAuthToken()/the getters may be called
    // from a different thread (e.g. the render thread reacting to
    // connect()'s return value while a background reconnect thread also
    // touches config_), so these get their own narrower lock.
    mutable std::mutex handshakeResultMutex_;
    HelloRejectReason lastRejectReason_ = HelloRejectReason::None;
    std::string lastPairingToken_;
};

} // namespace melonds_remote::client
