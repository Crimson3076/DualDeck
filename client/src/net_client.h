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

private:
    void videoReceiveLoop();

    NetClientConfig config_;
    int controlFd_ = -1;
    int videoFd_ = -1;
    int udpFd_ = -1;

    std::atomic<bool> connected_{false};
    std::thread videoThread_;

    std::mutex frameMutex_;
    std::vector<uint8_t> latestFrame_;
    bool hasFrame_ = false;
};

} // namespace melonds_remote::client
