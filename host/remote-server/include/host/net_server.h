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
// address rather than some hardcoded surprise value, supports only one
// client at a time, validates every packet's magic/version/size before
// acting on it, and never accepts a file path or shell command from the
// client. Defaults to "0.0.0.0" (all interfaces) rather than loopback --
// this is a LAN remote-play tool, so out-of-the-box reachability from
// another machine is the expected behavior, not an opt-in; the
// device-approval/token handshake (not the bind address) is what's
// supposed to keep that safe. This also has to match what LAN discovery already
// promises: discovery (below) answers broadcasts on every interface by
// default and hands back a real, externally-reachable address, so a
// bindAddress default of "127.0.0.1" would make discovery advertise a
// host that then refuses every connection attempt -- exactly the
// "discovers fine, then 'connection refused' forever" bug this comment
// is here to prevent regressing.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "host/device_approval_manager.h"
#include "host/emulator_input_sink.h"
#include "host/frame_source.h"
#include "host/mic_audio_sink.h"
#include "melonds_remote/input_state_tracker.h"
#include "melonds_remote/rate_limiter.h"

namespace melonds_remote::host {

struct NetServerConfig {
    // See the file-level "Security posture" comment above for why this
    // is "0.0.0.0" and not "127.0.0.1" -- pass --bind 127.0.0.1
    // explicitly for same-machine-only testing.
    std::string bindAddress = "0.0.0.0";
    uint16_t controlPort = 8760;
    uint16_t inputPort = 8761;
    uint16_t videoPort = 8762;
    uint64_t inputTimeoutUs = 500'000; // 500ms, spec section 6.4 / 7.1
    // How often videoLoop() checks frameSource_->getLatestFrame() for a
    // new frame index -- NOT a bandwidth cap. Since net_server.cpp's
    // dedup guard skips re-sending an unchanged frameIndex, a tick that
    // finds nothing new costs one mutex-guarded comparison and never
    // touches the socket. Raising this only shrinks the worst-case delay
    // between a new frame becoming available and this loop noticing it
    // (relay latency); it does not raise actual send rate, which is
    // still bounded by how fast frameSource_ itself produces new frames.
    int videoSendFps = 240;

    // JPEG quality (1-100, libjpeg-turbo's tjCompress2 scale) videoLoop()
    // compresses every frame at before sending (protocol v8 -- see
    // protocol.h's kProtocolVersion comment for why: raw BGRA8888 frames
    // made streaming unusable once Cemu's much larger GamePad surface was
    // added). 80 is a middle-of-the-road tradeoff between bandwidth and
    // visible compression artifacts on typical game content; lower this
    // further for a slower link, or raise it for a fast wired one where
    // artifacts matter more than bandwidth.
    int videoJpegQuality = 80;

    // Whether this host accepts MicAudioFrame packets at all (GitHub
    // issue #2) -- an explicit host-side on/off switch (like
    // discoveryEnabled below), not just "does a sink object happen to
    // exist," since injecting audio into a running session is a
    // meaningfully different privacy posture than video/input and some
    // hosts may want it off entirely. Reported to the client as
    // HelloAckPayload::micSupported; when false, audioLoop() isn't even
    // started (see start()).
    bool micSupported = true;
    uint16_t audioPort = 8765;

    // Empty means authentication is disabled -- spec section 13 requires
    // this to be a conscious, warned-about choice, not a silent default.
    //
    // If non-empty, this is a static pre-shared token checked before
    // anything else (legacy/CI-friendly path, e.g. tests/smoke_test.py):
    // an exact match is accepted outright and the device-approval flow
    // below never runs. Leave empty to use device-approval mode instead
    // (recommended for real use): an unrecognized client's connection
    // request is queued for a human at the host to approve or deny (see
    // approvalStateFilePath / onPendingRequestsChanged) rather than
    // requiring a fixed secret configured and distributed by hand, or a
    // code typed on the client (which Steam Input's virtual keyboard
    // doesn't reliably bring up -- see docs/known-limitations.md).
    std::string authToken;

    // Where approved device identities are persisted so an approved
    // client stays approved across host restarts (see
    // DeviceApprovalManager). Empty disables persistence -- approval
    // still works for the life of the process, it's just forgotten on
    // restart. Ignored entirely when authToken is set (static-token mode
    // bypasses device approval).
    std::string approvalStateFilePath;

    // How long a pending approval request is kept without a fresh retry
    // before being silently evicted from the queue (a client that gave
    // up shouldn't leave a permanent stale entry).
    std::chrono::seconds pendingRequestTtl{60};

    // Optional hook for surfacing pending approval requests somewhere
    // beyond the console (e.g. melonDS's own window, as a Yes/No
    // dialog). Called with the current full list of pending requests
    // whenever it changes (a new arrival, an eviction, or an
    // approve/deny) -- see DeviceApprovalManager::setOnPendingRequestsChanged.
    // Invoked synchronously on NetServer's control-connection thread (or
    // the watchdog thread, for stale evictions) -- if the callback
    // touches UI state, it must marshal to the UI thread itself (e.g.
    // Qt::QueuedConnection).
    std::function<void(std::vector<DeviceApprovalManager::PendingRequest>)> onPendingRequestsChanged;

    // Fired with `true` once a client's handshake completes (control
    // channel authenticated and accepted), and with `false` when that
    // same client's control connection ends (graceful disconnect,
    // malformed packet, or heartbeat timeout) -- never fired for a
    // rejected handshake attempt. Lets a host-side UI react to "someone
    // is actively streaming right now" (e.g. the melonDS integration uses
    // this to show only the top screen locally while a client has the
    // bottom screen, per SPEC.md's "Wii U GamePad" model, restoring
    // whatever screen layout was configured before once the client
    // disconnects). Invoked synchronously on NetServer's
    // control-connection thread -- if the callback touches UI state, it
    // must marshal to the UI thread itself (e.g. Qt::QueuedConnection).
    std::function<void(bool)> onClientConnectionChanged;

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
    // device-approval/token handshake regardless. Deliberately bound to
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

    // This host's own release version string (e.g. "v0.1.24"), sent back
    // in every HelloAck regardless of accept/reject so the client can
    // always show what the host is running. If both this and the
    // connecting client's HelloPayload::appVersion are non-empty and
    // differ, the handshake is rejected with
    // HelloRejectReason::AppVersionMismatch before authentication is even
    // checked -- see protocol.h's HelloPayload::appVersion comment for
    // why this is separate from kProtocolVersion. Left empty (the
    // default) to disable this check entirely, e.g. for a from-source dev
    // build with no meaningful version string -- run-host.sh/main.cpp set
    // this from the packaged archive's VERSION file.
    std::string appVersion;

    // Emulator-independent identity (GitHub issue #28's architecture
    // foundation milestone: decouple DualDeck from melonDS), sent back
    // in every HelloAck and DiscoveryResponse -- see
    // melonds_remote::SystemIdentity/AdapterIdentity in protocol.h for
    // the field-by-field meaning. Defaults to a clearly-labeled
    // synthetic/test identity rather than empty strings, so a host that
    // never overrides this (the standalone host/remote-server binary
    // itself, or a test harness constructing NetServerConfig directly)
    // is never mistaken in client UI for a real DS/melonDS session. The
    // melonDS integration (RemoteServerBridge) overrides both to the
    // real "nds"/"Nintendo DS" and "melonds"/"melonDS" identity in its
    // own constructor -- see docs/architecture.md's "Emulator identity
    // model" section for why that override lives there rather than as a
    // NetServer constructor parameter.
    SystemIdentity systemIdentity{"synthetic", "Synthetic Test System"};
    AdapterIdentity adapterIdentity{"synthetic-test", "Synthetic Test Adapter", ""};
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
    uint64_t micPacketsAccepted = 0;
    uint64_t micPacketsMalformed = 0;
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
    // `micSink` receives MicAudioFrame packets (GitHub issue #2). Pass a
    // LoggingMicAudioSink when no real audio destination exists yet, same
    // as `inputSink` before a real melonDS integration existed.
    // `config.systemIdentity`/`adapterIdentity` become the initial values
    // reported to clients -- see setTarget() below for changing them (and
    // the target itself) after start().
    NetServer(NetServerConfig config, IEmulatorInputSink& inputSink, IFrameSource& frameSource,
               IMicAudioSink& micSink);
    ~NetServer();

    NetServer(const NetServer&) = delete;
    NetServer& operator=(const NetServer&) = delete;

    // Starts all worker threads. Non-blocking; call stop() (or destroy the
    // object) to shut down.
    void start();
    void stop();

    // Atomically swaps which IEmulatorInputSink/IFrameSource this server
    // is driven by, and updates the identity it reports, without
    // disturbing any already-connected client's control/input/video
    // sockets (GitHub issue #4 Phase B). This is what lets a Host
    // Service outlive a single emulator process: it can start pointed at
    // a HostControlAdapter (`mode` == HostMode::HostControl, no emulator
    // running yet), then swap to a real adapter once one connects
    // (`mode` == HostMode::Emulation), and swap back when it disconnects
    // -- all while a client stays connected throughout instead of having
    // to reconnect.
    //
    // The previous target's releaseAll() is called (after the swap, not
    // under any internal lock) so a button/touch held at the moment of a
    // swap can never carry over onto the new target. If a client is
    // currently authenticated, a ModeChanged packet is sent to it on the
    // control channel; a safe no-op (state applies to the next connecting
    // client's HelloAck instead) if nobody is connected right now, or if
    // the send itself fails (the client learns the new state on its next
    // reconnect regardless).
    //
    // Safe to call from any thread, including before start() (e.g. to
    // establish the initial target/identity the same way the constructor
    // parameters do -- construction-time and post-construction swaps go
    // through the exact same code path).
    void setTarget(IEmulatorInputSink& inputSink, IFrameSource& frameSource, HostMode mode,
                    SystemIdentity systemIdentity, AdapterIdentity adapterIdentity);

    // The mode set by the most recent setTarget() call (or the
    // construction-time default, HostMode::Emulation, if setTarget() was
    // never called). Safe from any thread.
    HostMode currentMode() const;

    // Approves/denies a pending connection request by exact device id or
    // unambiguous prefix (see DeviceApprovalManager::approve/deny).
    // Returns false if nothing pending matches. Safe to call from any
    // thread (e.g. a console-input thread in main.cpp, or a Qt UI-thread
    // slot in the melonDS integration).
    bool approveDevice(const std::string& deviceIdOrPrefix);
    bool denyDevice(const std::string& deviceIdOrPrefix);

    // Current pending connection requests -- e.g. for a UI/console loop
    // to list on demand rather than only reacting to onPendingRequestsChanged.
    std::vector<DeviceApprovalManager::PendingRequest> pendingRequests();

private:
    void controlLoop();
    void inputLoop();
    void videoLoop();
    void watchdogLoop();
    void discoveryLoop();
    void audioLoop();

    NetServerConfig config_;

    // The active target and reported identity, swappable at runtime via
    // setTarget() (GitHub issue #4 Phase B) -- guarded together so
    // inputLoop()/videoLoop()/watchdogLoop() and a fresh handshake's
    // HelloAck never see inputSink_/frameSource_ paired with a mismatched
    // identity mid-swap. Never null after construction.
    mutable std::mutex targetMutex_;
    IEmulatorInputSink* inputSink_;
    IFrameSource* frameSource_;
    HostMode currentMode_ = HostMode::Emulation;
    SystemIdentity currentSystemIdentity_;
    AdapterIdentity currentAdapterIdentity_;

    IMicAudioSink& micSink_;
    DeviceApprovalManager deviceApproval_;
    // Only ever touched from inside the onPendingRequestsChanged callback
    // above (see the comment there for why that's safe without its own lock).
    std::unordered_set<std::string> notifiedPendingIds_;

    std::atomic<bool> running_{false};
    std::thread controlThread_;
    std::thread inputThread_;
    std::thread videoThread_;
    std::thread watchdogThread_;
    std::thread discoveryThread_;
    std::thread audioThread_;

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
    int audioFd_ = -1;

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

    // The effective JPEG quality for the current/most recent session --
    // either config_.videoJpegQuality, or the connecting client's own
    // HelloPayload::videoQuality if it sent a nonzero one (protocol v9).
    // Set once at handshake time, alongside clientAuthenticated_ above;
    // videoLoop() reads it when a new video connection arrives. Same
    // single-active-client assumption as the rest of this class (see
    // clientAuthenticated_'s comment) -- there is no per-connection state
    // here, just the most recent handshake's outcome.
    std::atomic<int> currentVideoQuality_{80};
};

} // namespace melonds_remote::host
