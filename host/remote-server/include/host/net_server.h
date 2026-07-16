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
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "host/emulator_input_sink.h"
#include "host/frame_source.h"
#include "host/pairing_manager.h"
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
    //
    // If non-empty, this is a static pre-shared token checked before
    // anything else (legacy/CI-friendly path, e.g. tests/smoke_test.py):
    // an exact match is accepted outright and the pairing-code flow below
    // never runs. Leave empty to use pairing mode instead (recommended
    // for real use): unrecognized clients are shown a 6-digit code (see
    // pairingStateFilePath / onPairingCodeChanged) instead of a fixed
    // secret you have to configure and distribute yourself.
    std::string authToken;

    // Where issued pairing tokens are persisted so a paired client stays
    // paired across host restarts (see PairingManager). Empty disables
    // persistence -- pairing still works for the life of the process,
    // it's just forgotten on restart. Ignored entirely when authToken is
    // set (static-token mode bypasses pairing).
    std::string pairingStateFilePath;

    // How long a generated pairing code stays valid before a fresh one
    // is needed.
    std::chrono::seconds pairingCodeTtl{300};

    // Optional hook for surfacing the pairing code somewhere beyond the
    // console (e.g. melonDS's own window). Called with the new code when
    // one is generated, and with std::nullopt when it's consumed/expires.
    // Invoked synchronously on NetServer's control-connection thread --
    // if the callback touches UI state, it must marshal to the UI thread
    // itself (e.g. Qt::QueuedConnection).
    std::function<void(std::optional<std::string>)> onPairingCodeChanged;

    // If a control-channel connection is silent (no heartbeat, no other
    // control traffic) for longer than this, it is dropped. Distinct from
    // inputTimeoutUs, which governs the UDP input stream.
    uint64_t controlHeartbeatTimeoutUs = 5'000'000; // 5s

    // Connection-attempt rate limiting (spec section 13).
    int maxConnectionAttemptsPerWindow = 5;
    uint64_t connectionAttemptWindowUs = 10'000'000; // 10s

    // How often to log aggregated diagnostics (spec sections 8.5 and 14:
    // frame rate, dropped frames, input packet rate, out-of-order
    // packets, and latency instrumentation).
    uint64_t statsLoggingIntervalUs = 5'000'000; // 5s

    // LAN discovery (spec section 8.1's deferred "future versions" item,
    // now implemented): a client that doesn't already know a host address
    // can broadcast a DiscoveryRequest and get a unicast DiscoveryResponse
    // back from every host listening, without any auth -- it only ever
    // reveals a host name and port numbers, never anything sensitive; the
    // actual control/input/video ports still require the full
    // pairing/token handshake regardless. Deliberately bound to
    // "0.0.0.0" (not `bindAddress`) since receiving a broadcast requires
    // it -- this is the one socket in this class that doesn't obey
    // `bindAddress`, and is the reason discovery can be disabled
    // (`discoveryEnabled = false`) for anyone who'd rather not have this
    // host answer broadcasts at all.
    bool discoveryEnabled = true;
    uint16_t discoveryPort = 8763;
    // Friendly name returned in DiscoveryResponse; empty means "ask the
    // OS for the hostname at start() time" (see NetServer::discoveryLoop).
    std::string hostName;
};

// Aggregated counters reset each time they're logged. Guarded by
// NetServer::statsMutex_ -- deliberately a single mutex-protected struct
// rather than several independent atomics, since it's read/reset as one
// unit on the ~5s logging cadence and written at most once per received
// packet or sent frame (not a hot enough path to need lock-free counters).
struct NetServerStats {
    uint64_t inputPacketsAccepted = 0;
    uint64_t inputPacketsOutOfOrder = 0;
    uint64_t inputPacketsMalformed = 0;
    uint64_t framesSent = 0;
    uint64_t framesDropped = 0;
    uint64_t latencySampleCount = 0;
    uint64_t latencySumUs = 0;
    uint64_t latencyMinUs = UINT64_MAX;
    uint64_t latencyMaxUs = 0;

    void recordLatency(uint64_t latencyUs) {
        ++latencySampleCount;
        latencySumUs += latencyUs;
        if (latencyUs < latencyMinUs) latencyMinUs = latencyUs;
        if (latencyUs > latencyMaxUs) latencyMaxUs = latencyUs;
    }
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
    void discoveryLoop();

    NetServerConfig config_;
    IEmulatorInputSink& inputSink_;
    IFrameSource& frameSource_;
    PairingManager pairingManager_;

    std::atomic<bool> running_{false};
    std::thread controlThread_;
    std::thread inputThread_;
    std::thread videoThread_;
    std::thread watchdogThread_;
    std::thread discoveryThread_;

    InputStateTracker inputTracker_;
    std::mutex trackerMutex_;

    ConnectionRateLimiter rateLimiter_;
    std::mutex rateLimiterMutex_;

    NetServerStats stats_;
    std::mutex statsMutex_;

    int controlListenFd_ = -1;
    int videoListenFd_ = -1;
    int inputFd_ = -1;
    int discoveryFd_ = -1;

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
