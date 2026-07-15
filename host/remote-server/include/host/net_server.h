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

namespace melonds_remote::host {

struct NetServerConfig {
    std::string bindAddress = "127.0.0.1";
    uint16_t controlPort = 8760;
    uint16_t inputPort = 8761;
    uint16_t videoPort = 8762;
    uint64_t inputTimeoutUs = 500'000; // 500ms, spec section 6.4 / 7.1
    int videoSendFps = 60;
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

    int controlListenFd_ = -1;
    int videoListenFd_ = -1;
    int inputFd_ = -1;

    std::atomic<int> controlClientFd_{-1};
    std::atomic<int> videoClientFd_{-1};
};

} // namespace melonds_remote::host
