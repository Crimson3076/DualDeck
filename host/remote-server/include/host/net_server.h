#pragma once

// Minimal standalone host server: one TCP control connection (handshake +
// heartbeat + disconnect), one UDP input socket (ControllerState packets),
// and one TCP video connection (bottom-screen frames). This is the Phase 1
// "minimal host server" from SPEC.md section 24, built against IFrameSource
// / IEmulatorInputSink so it works today with a synthetic frame source and
// a logging input sink, and can be pointed at a real melonDS integration
// later without changing any network code.
//
// Security posture (spec section 13): binds explicitly to the given
// address (never INADDR_ANY implicitly), supports only one client at a
// time, validates every packet's magic/version/size before acting on it,
// and never accepts a file path or shell command from the client.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "host/emulator_input_sink.h"
#include "host/frame_source.h"
#include "melonds_remote/input_state_tracker.h"
#include "melonds_remote/rate_limiter.h"

namespace melonds_remote::host {

struct NetServerConfig {
    std::string bindAddress = "127.0.0.1";
    uint16_t controlPort = 8760;
    uint16_t inputPort = 8761;
    uint16_t videoPort = 8762;
    uint64_t inputTimeoutUs = 500'000; // 500ms, spec section 6.4 / 7.1
    int videoSendFps = 60;

    // Empty means authentication is disabled -- spec section 13 requires
    // this to be a conscious, warned-about choice, not a silent default.
    std::string authToken;

    // If a control-channel connection is silent (no heartbeat, no other
    // control traffic) for longer than this, it is dropped. Distinct from
    // inputTimeoutUs, which governs the UDP input stream.
    uint64_t controlHeartbeatTimeoutUs = 5'000'000; // 5s

    // Connection-attempt rate limiting (spec section 13).
    int maxConnectionAttemptsPerWindow = 5;
    uint64_t connectionAttemptWindowUs = 10'000'000; // 10s
};

class NetServer {
public:
    NetServer(NetServerConfig config, IEmulatorInputSink& inputSink, IFrameSource& frameSource);
    ~NetServer();

    NetServer(const NetServer&) = delete;
    NetServer& operator=(const NetServer&) = delete;

    // Starts all worker threads. Non-blocking; call stop() (or destroy the
    // object) to shut down.
    void start();
    void stop();

private:
    void controlLoop();
    void inputLoop();
    void videoLoop();
    void watchdogLoop();

    NetServerConfig config_;
    IEmulatorInputSink& inputSink_;
    IFrameSource& frameSource_;

    std::atomic<bool> running_{false};
    std::thread controlThread_;
    std::thread inputThread_;
    std::thread videoThread_;
    std::thread watchdogThread_;

    InputStateTracker inputTracker_;
    std::mutex trackerMutex_;

    ConnectionRateLimiter rateLimiter_;
    std::mutex rateLimiterMutex_;

    int controlListenFd_ = -1;
    int videoListenFd_ = -1;
    int inputFd_ = -1;

    std::atomic<int> controlClientFd_{-1};
    std::atomic<int> videoClientFd_{-1};

    // Set only once a client has completed the (possibly authenticated)
    // control-channel handshake, and cleared on disconnect/timeout.
    // inputLoop() uses this -- together with authenticatedClientAddr_ --
    // to refuse to act on ControllerState packets from anyone who hasn't
    // authenticated, and from any source address other than the
    // authenticated client's (spec section 13: don't accept arbitrary
    // unauthenticated input).
    std::atomic<bool> clientAuthenticated_{false};
    std::atomic<uint32_t> authenticatedClientAddr_{0}; // sockaddr_in::sin_addr.s_addr, network byte order
};

} // namespace melonds_remote::host
