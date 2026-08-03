#include "host/net_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <turbojpeg.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <utility>

#include "melonds_remote/protocol.h"

namespace melonds_remote::host {

namespace {

// Monotonic clock: used for timeouts and sequence-number bookkeeping,
// where immunity to wall-clock jumps (NTP corrections, DST, manual clock
// changes) matters more than comparability with the client's clock.
uint64_t nowMicros() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

// Wall-clock (epoch) time: the only clock comparable with the client's
// ControllerState.clientTimestampUs (see wallClockNowUs() in
// client/src/main.cpp), so this is used only for the latency estimate in
// inputLoop()'s stats, never for timeout/ordering decisions.
uint64_t nowMicrosEpoch() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(system_clock::now().time_since_epoch()).count());
}

// Binds a TCP listening socket to config.bindAddress:port. Returns -1 on
// failure (logged); never falls back to binding all interfaces implicitly.
int makeTcpListener(const std::string& bindAddress, uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::perror("socket (tcp)");
        return -1;
    }

    int reuse = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, bindAddress.c_str(), &addr.sin_addr) != 1) {
        std::fprintf(stderr, "invalid bind address: %s\n", bindAddress.c_str());
        ::close(fd);
        return -1;
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind (tcp)");
        // Real user report, 2026-08-03: melonDS's own in-process
        // NetServer (this file, vendored into the melonDS patch) and the
        // separate persistent Host Control daemon (dualdeck-host-
        // control.service) are two independent processes that both bind
        // these exact same default client-facing ports -- melonDS isn't
        // wired into the shared-daemon/adapter-IPC model Azahar/Cemu use
        // (a known, deliberately-deferred larger rework, see
        // docs/known-limitations.md), so nothing today stops both from
        // trying to claim the same port at once. When that happens here,
        // this melonDS instance silently never starts a server at all
        // while the client, upon connecting, reaches whichever one *did*
        // win the bind -- typically the daemon, since it's usually
        // already running by the time melonDS launches -- landing the
        // client in Host Control mode instead of the melonDS session the
        // user actually wanted, with no obvious explanation from the
        // client side alone. EADDRINUSE specifically (not just any bind
        // failure) is exactly this scenario; call it out by name so
        // whoever reads this line (interactively, or via journalctl if
        // launched under systemd) doesn't have to guess.
        if (errno == EADDRINUSE) {
            std::fprintf(stderr,
                          "NetServer: port %u is already in use -- if the persistent Host Control "
                          "daemon (dualdeck-host-control.service) is running, stop it first "
                          "(systemctl --user stop dualdeck-host-control.service), then relaunch.\n",
                          port);
        }
        ::close(fd);
        return -1;
    }

    if (::listen(fd, 1) < 0) {
        std::perror("listen");
        ::close(fd);
        return -1;
    }

    return fd;
}

int makeUdpSocket(const std::string& bindAddress, uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        std::perror("socket (udp)");
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, bindAddress.c_str(), &addr.sin_addr) != 1) {
        std::fprintf(stderr, "invalid bind address: %s\n", bindAddress.c_str());
        ::close(fd);
        return -1;
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind (udp)");
        // See makeTcpListener()'s identical EADDRINUSE handling above --
        // same melonDS-vs-persistent-daemon port conflict, just on the
        // UDP input port instead of the TCP control/video ones.
        if (errno == EADDRINUSE) {
            std::fprintf(stderr,
                          "NetServer: port %u is already in use -- if the persistent Host Control "
                          "daemon (dualdeck-host-control.service) is running, stop it first "
                          "(systemctl --user stop dualdeck-host-control.service), then relaunch.\n",
                          port);
        }
        ::close(fd);
        return -1;
    }

    return fd;
}

// Constant-time-ish string comparison: always compares the same number of
// bytes (padding the shorter string's "missing" bytes into the mismatch
// accumulator) so a client can't use response timing to learn how many
// leading bytes of the auth token it guessed correctly.
bool constantTimeEquals(const std::string& a, const std::string& b) {
    size_t maxLen = std::max(a.size(), b.size());
    unsigned char diff = static_cast<unsigned char>(a.size() != b.size());
    for (size_t i = 0; i < maxLen; ++i) {
        unsigned char ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
        unsigned char cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
        diff = static_cast<unsigned char>(diff | (ca ^ cb));
    }
    return diff == 0;
}

// JPEG-compresses one raw BGRA8888 frame (protocol v8, see protocol.h's
// kProtocolVersion comment). `compressor` is reused across calls -- see
// videoLoop()'s comment on why -- so this function is only ever called
// from that one thread. TJPF_BGRA as both input format and (implicitly,
// via JCS_EXT_BGRA under the hood) how libjpeg-turbo reads it means no
// manual channel reordering is needed, matching this project's existing
// BGRA8888-everywhere convention. Returns false (leaving outJpeg
// untouched) on a compression failure, which should only happen for a
// malformed width/height.
bool compressFrameBgraToJpeg(tjhandle compressor, const uint8_t* bgra, int width, int height,
                              int quality, ByteBuffer& outJpeg) {
    // 4:2:0 chroma subsampling (half resolution in both chroma axes) is
    // invisible at typical photo/game content and quality settings, but
    // real-world feedback on DS/3DS -- small enough surfaces that
    // bandwidth was never the constraint compression exists for -- found
    // it visibly softens sharp pixel-art edges and text even at quality
    // 100, since subsampling is a structural choice independent of the
    // quality scalar. At a high requested quality (client is explicitly
    // asking for close-to-lossless, protocol v9's HelloPayload::
    // videoQuality), use full-resolution 4:4:4 chroma instead -- still
    // cheap at these frame sizes -- so "quality" actually reaches
    // full fidelity rather than being capped by subsampling artifacts.
    const TJSAMP subsampling = quality >= 90 ? TJSAMP_444 : TJSAMP_420;
    unsigned char* jpegBuf = nullptr;
    unsigned long jpegSize = 0;
    int result = tjCompress2(compressor, bgra, width, /*pitch=*/0, height, TJPF_BGRA,
                              &jpegBuf, &jpegSize, subsampling, quality, TJFLAG_FASTDCT);
    if (result != 0) {
        std::fprintf(stderr, "NetServer: tjCompress2 failed: %s\n", tjGetErrorStr2(compressor));
        return false;
    }
    outJpeg.assign(jpegBuf, jpegBuf + jpegSize);
    tjFree(jpegBuf);
    return true;
}

bool sendAll(int fd, const uint8_t* data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        ssize_t n = ::send(fd, data + sent, size - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

} // namespace

// Real, live tuning gap this closes: `NetServerConfig::videoJpegQuality`
// (default 80) is the *only* value ever actually used for a client that
// doesn't send its own HelloPayload::videoQuality override -- there is
// no `--video-quality` CLI flag on this binary at all, so every one of
// run-host.sh/run-host-azahar.sh/run-host-cemu.sh launches with the
// exact same compiled-in default regardless of which adapter is
// actually driving the session. That default was picked (see
// videoJpegQuality's own comment) for DS/3DS-sized surfaces (49k-77k
// pixels); applying it unchanged to Cemu's much larger GamePad surface
// (854x480, ~410k pixels -- ~5-8x more pixels than DS/3DS) produces
// proportionally larger JPEGs at the same quality, directly adding to
// per-frame encode time, network transmit time, and how often
// SO_SNDBUF's backpressure (see videoLoop()'s own comment) has to
// actually kick in -- a real, concrete contributor to reported
// "latency is poor" complaints on Wii U sessions specifically. Resolved
// once per connection, from that connection's own real negotiated
// frame size (HelloAck's nativeWidth/nativeHeight), not the emulated
// system's identity -- correct for any future adapter with an
// unusually large or small surface too, not just today's known ones.
int defaultVideoQualityForFrameSize(int configuredDefault, uint16_t width, uint16_t height) {
    constexpr int kLargeSurfacePixelThreshold = 150'000; // comfortably above 3DS's 76,800, below Cemu's 409,920
    constexpr int kLargeSurfaceQuality = 60; // still legible on Cemu's GamePad UI text at typical viewing distance
    const int pixels = static_cast<int>(width) * height;
    return pixels > kLargeSurfacePixelThreshold ? std::min(configuredDefault, kLargeSurfaceQuality)
                                                 : configuredDefault;
}

NetServer::NetServer(NetServerConfig config, IEmulatorInputSink& inputSink, IFrameSource& frameSource,
                     IMicAudioSink& micSink)
    : config_(std::move(config)), inputSink_(&inputSink), frameSource_(&frameSource),
      currentSystemIdentity_(config_.systemIdentity), currentAdapterIdentity_(config_.adapterIdentity),
      micSink_(micSink),
      deviceApproval_(config_.approvalStateFilePath, config_.pendingRequestTtl),
      rateLimiter_(config_.maxConnectionAttemptsPerWindow, config_.connectionAttemptWindowUs),
      currentVideoQuality_(config_.videoJpegQuality) {
    if (!config_.authToken.empty()) {
        std::fprintf(stderr,
                      "NetServer: static auth token configured -- device-approval flow is disabled, "
                      "the exact token is required.\n");
    } else {
        std::fprintf(stderr,
                      "NetServer: no static auth token configured; using device-approval mode. An "
                      "unrecognized client's connection request will be queued here for you to "
                      "approve or deny -- see this process's stdin/console.\n");
    }

    deviceApproval_.setOnPendingRequestsChanged(
        [this](std::vector<DeviceApprovalManager::PendingRequest> requests) {
            // DeviceApprovalManager serializes every call to this callback
            // under its own internal mutex (see notifyChangedLocked), so
            // mutating notifiedPendingIds_ here without a separate lock of
            // our own is safe -- two invocations can never run concurrently.
            std::unordered_set<std::string> currentIds;
            for (const auto& r : requests) {
                currentIds.insert(r.deviceId);
                if (notifiedPendingIds_.count(r.deviceId) == 0) {
                    std::fprintf(stderr,
                                  "NetServer: pending connection request from '%s' at %s "
                                  "(device %s) -- type 'approve %s' or 'deny %s' to respond\n",
                                  r.clientName.c_str(), r.address.c_str(), r.deviceId.c_str(),
                                  r.deviceId.substr(0, 8).c_str(), r.deviceId.substr(0, 8).c_str());
                }
            }
            notifiedPendingIds_ = std::move(currentIds);

            if (config_.onPendingRequestsChanged) {
                config_.onPendingRequestsChanged(requests);
            }
        });
}

NetServer::~NetServer() {
    stop();
}

void NetServer::setTarget(IEmulatorInputSink& inputSink, IFrameSource& frameSource, HostMode mode,
                           SystemIdentity systemIdentity, AdapterIdentity adapterIdentity) {
    IEmulatorInputSink* previousSink;
    {
        std::lock_guard<std::mutex> lock(targetMutex_);
        previousSink = inputSink_;
        inputSink_ = &inputSink;
        frameSource_ = &frameSource;
        currentMode_ = mode;
        currentSystemIdentity_ = std::move(systemIdentity);
        currentAdapterIdentity_ = std::move(adapterIdentity);
    }

    // Release outside the lock (and unconditionally, even on the very
    // first setTarget() call from a constructor -- releaseAll() must
    // already be safe to call on an idle sink): a button/touch the old
    // target thought was still held must not carry over to whatever's
    // driving the session now.
    if (previousSink) {
        previousSink->releaseAll();
    }

    int fd = controlClientFd_.load();
    if (fd >= 0 && clientAuthenticated_.load()) {
        ModeChangedPayload payload;
        payload.mode = mode;
        {
            // Re-read under the lock rather than reusing the (now-moved-
            // from) parameters above: another setTarget() call could
            // have already raced ahead of this one between the unlock
            // above and here, and the notification should always
            // reflect whatever is actually current, not stale local
            // state.
            std::lock_guard<std::mutex> lock(targetMutex_);
            payload.system = currentSystemIdentity_;
            payload.adapter = currentAdapterIdentity_;
        }
        ByteBuffer packet = buildModeChangedPacket(payload);
        // Best-effort: a failed send here just means this client learns
        // the new state on its next reconnect via HelloAck instead,
        // same fallback as any other transient control-channel hiccup.
        sendAll(fd, packet.data(), packet.size());
    }
}

HostMode NetServer::currentMode() const {
    std::lock_guard<std::mutex> lock(targetMutex_);
    return currentMode_;
}

bool NetServer::approveDevice(const std::string& deviceIdOrPrefix) {
    return deviceApproval_.approve(deviceIdOrPrefix);
}

bool NetServer::denyDevice(const std::string& deviceIdOrPrefix) {
    return deviceApproval_.deny(deviceIdOrPrefix);
}

std::vector<DeviceApprovalManager::PendingRequest> NetServer::pendingRequests() {
    return deviceApproval_.pendingRequests();
}

void NetServer::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }

    controlListenFd_ = makeTcpListener(config_.bindAddress, config_.controlPort);
    videoListenFd_ = makeTcpListener(config_.bindAddress, config_.videoPort);
    inputFd_ = makeUdpSocket(config_.bindAddress, config_.inputPort);

    if (controlListenFd_ < 0 || videoListenFd_ < 0 || inputFd_ < 0) {
        std::fprintf(stderr, "NetServer: failed to bind one or more sockets, not starting\n");
        running_ = false;
        return;
    }

    std::fprintf(stderr,
                  "NetServer: listening on %s (control=%u, input(udp)=%u, video=%u)\n",
                  config_.bindAddress.c_str(), config_.controlPort, config_.inputPort,
                  config_.videoPort);

    controlThread_ = std::thread(&NetServer::controlLoop, this);
    inputThread_ = std::thread(&NetServer::inputLoop, this);
    videoThread_ = std::thread(&NetServer::videoLoop, this);
    watchdogThread_ = std::thread(&NetServer::watchdogLoop, this);

    if (config_.micSupported) {
        // Failure to bind here is not fatal to the rest of the server,
        // same treatment as a discovery bind failure -- mic is an
        // optional feature, not load-bearing for the core session.
        audioFd_ = makeUdpSocket(config_.bindAddress, config_.audioPort);
        if (audioFd_ < 0) {
            std::fprintf(stderr,
                          "NetServer: microphone support disabled (failed to bind UDP port %u)\n",
                          config_.audioPort);
        } else {
            std::fprintf(stderr, "NetServer: microphone audio listening on %s:%u\n",
                          config_.bindAddress.c_str(), config_.audioPort);
            audioThread_ = std::thread(&NetServer::audioLoop, this);
        }
    }

    if (config_.discoveryEnabled) {
        // Bound to "0.0.0.0", not config_.bindAddress -- see the comment
        // on NetServerConfig::discoveryEnabled for why. A bind failure
        // here (e.g. port already in use) is not fatal to the rest of
        // the server -- discovery is a convenience, not load-bearing;
        // clients can still be given a host address directly.
        discoveryFd_ = makeUdpSocket("0.0.0.0", config_.discoveryPort);
        if (discoveryFd_ < 0) {
            std::fprintf(stderr,
                          "NetServer: discovery disabled (failed to bind UDP port %u) -- "
                          "clients must be given the host address directly\n",
                          config_.discoveryPort);
        } else {
            int broadcast = 1;
            ::setsockopt(discoveryFd_, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
            std::fprintf(stderr, "NetServer: LAN discovery listening on 0.0.0.0:%u\n",
                          config_.discoveryPort);
            discoveryThread_ = std::thread(&NetServer::discoveryLoop, this);
        }
    }
}

void NetServer::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    // Unblock accept()/recv() calls by closing the listening/connected fds.
    if (controlListenFd_ >= 0) ::shutdown(controlListenFd_, SHUT_RDWR);
    if (videoListenFd_ >= 0) ::shutdown(videoListenFd_, SHUT_RDWR);
    if (inputFd_ >= 0) ::shutdown(inputFd_, SHUT_RDWR);
    if (discoveryFd_ >= 0) ::shutdown(discoveryFd_, SHUT_RDWR);
    if (audioFd_ >= 0) ::shutdown(audioFd_, SHUT_RDWR);

    int controlClient = controlClientFd_.exchange(-1);
    if (controlClient >= 0) ::shutdown(controlClient, SHUT_RDWR);
    int videoClient = videoClientFd_.exchange(-1);
    if (videoClient >= 0) ::shutdown(videoClient, SHUT_RDWR);

    if (controlThread_.joinable()) controlThread_.join();
    if (inputThread_.joinable()) inputThread_.join();
    if (videoThread_.joinable()) videoThread_.join();
    if (watchdogThread_.joinable()) watchdogThread_.join();
    if (discoveryThread_.joinable()) discoveryThread_.join();
    if (audioThread_.joinable()) audioThread_.join();

    if (controlListenFd_ >= 0) ::close(controlListenFd_);
    if (videoListenFd_ >= 0) ::close(videoListenFd_);
    if (inputFd_ >= 0) ::close(inputFd_);
    if (discoveryFd_ >= 0) ::close(discoveryFd_);
    if (audioFd_ >= 0) ::close(audioFd_);
    controlListenFd_ = videoListenFd_ = inputFd_ = discoveryFd_ = audioFd_ = -1;
}

namespace {
// Upper bound on a Hello payload's declared size, well above what any
// legitimate client name/platform/token combination needs, so a hostile
// payloadSize value can't be used to make the host allocate/read
// arbitrarily large amounts of data (spec section 13).
constexpr uint32_t kMaxHelloPayloadSize = 512;

// runSelfUpdateCommand <command>
//
// See NetServerConfig::selfUpdateCommand's own comment for the real user
// request behind this and why it's gated to already-approved devices
// only. `command` is never attacker- or network-controlled -- it only
// ever comes from main.cpp's own hardcoded "<host_root>/internal/
// apply-update.sh" path when --self-update is passed, never from
// anything a connecting client sends -- so shelling out to it directly
// is safe.
//
// Fire-and-forget: `apply-update.sh` itself can take well over a minute
// (a real download), and this is called from inside the same
// controlLoop() accept-handling that must send a HelloAck back promptly
// -- blocking the handshake response on that would make an already-slow
// update look like a hung/broken host on top of being out of date.
// `nohup ... &` backgrounds the actual work; std::system() itself only
// waits for the shell to fork it off, not for it to finish.
void runSelfUpdateCommand(const std::string& command) {
    std::string shellCommand = "nohup " + command + " >/dev/null 2>&1 &";
    if (std::system(shellCommand.c_str()) != 0) {
        std::fprintf(stderr, "NetServer: failed to launch self-update command (%s)\n", command.c_str());
    }
}
} // namespace

void NetServer::controlLoop() {
    while (running_.load()) {
        sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        int clientFd = ::accept(controlListenFd_, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
        if (clientFd < 0) {
            if (!running_.load()) break;
            continue;
        }

        char ipStr[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr));

        bool rateOk;
        {
            std::lock_guard<std::mutex> lock(rateLimiterMutex_);
            rateOk = rateLimiter_.allowAttempt(ipStr, nowMicros());
        }
        if (!rateOk) {
            std::fprintf(stderr, "NetServer: rate-limiting connection attempts from %s\n", ipStr);
            ::close(clientFd);
            continue;
        }

        // Only one client at a time (spec section 7.1 initial scope).
        int previous = controlClientFd_.exchange(clientFd);
        if (previous >= 0) {
            std::fprintf(stderr, "NetServer: rejecting extra control connection\n");
            ::close(clientFd);
            controlClientFd_ = previous;
            continue;
        }

        std::fprintf(stderr, "NetServer: control connection from %s\n", ipStr);

        // Bound how long a client may take to complete the handshake and
        // how long the connection may sit idle afterward (spec section 8.1
        // heartbeat/keepalive).
        timeval recvTimeout{};
        recvTimeout.tv_sec = static_cast<time_t>(config_.controlHeartbeatTimeoutUs / 1'000'000);
        recvTimeout.tv_usec = static_cast<suseconds_t>(config_.controlHeartbeatTimeoutUs % 1'000'000);
        ::setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &recvTimeout, sizeof(recvTimeout));

        bool handshakeOk = false;
        HelloRejectReason rejectReason = HelloRejectReason::ProtocolVersionMismatch;
        bool clientRequestedExplicitVideoQuality = false;

        uint8_t headerBuf[kPacketHeaderWireSize];
        ssize_t n = ::recv(clientFd, headerBuf, sizeof(headerBuf), MSG_WAITALL);
        if (n == static_cast<ssize_t>(sizeof(headerBuf))) {
            auto header = parseHeader(headerBuf, sizeof(headerBuf));
            // Real user report, 2026-08-03: "Client -> Host update
            // notifier seemingly broke in this version." Root cause: this
            // used to require header->protocolVersion == kProtocolVersion
            // just to attempt parsing Hello at all -- meaning a genuine
            // wire-format bump (like this same release's protocol v12,
            // which grew ControllerState) short-circuited straight to the
            // default ProtocolVersionMismatch rejection below, never
            // reaching the appVersion-mismatch/self-update-trigger logic
            // beneath it. That logic's whole point is "an already-
            // approved device is running a different release than this
            // host -- self-update," which is if anything *more* true, not
            // less, when the two sides don't even agree on the wire
            // format itself. HelloPayload's own byte layout is
            // independent of kProtocolVersion (confirmed by reading
            // parseHelloPayload() -- it's driven entirely by length-
            // prefixed strings read from `data`/`size`, never consults
            // the packet header), so it's safe to attempt parsing
            // regardless of a version mismatch here; a genuinely
            // incompatible future HelloPayload layout still fails safely
            // via parseHelloPayload()'s own strict size/trailing-byte
            // checks, landing in the existing "malformed Hello payload"
            // branch below rather than misparsing.
            bool protocolVersionMismatch = header && header->protocolVersion != kProtocolVersion;
            if (header && header->type == PacketType::Hello && header->payloadSize <= kMaxHelloPayloadSize) {
                ByteBuffer payloadBuf(header->payloadSize);
                ssize_t got = header->payloadSize == 0
                                  ? 0
                                  : ::recv(clientFd, payloadBuf.data(), payloadBuf.size(), MSG_WAITALL);
                if (got == static_cast<ssize_t>(payloadBuf.size())) {
                    auto hello = parseHelloPayload(payloadBuf.data(), payloadBuf.size());
                    if (!hello) {
                        std::fprintf(stderr, "NetServer: rejecting handshake (malformed Hello payload)\n");
                    } else if (protocolVersionMismatch ||
                               (!config_.appVersion.empty() && !hello->appVersion.empty() &&
                                hello->appVersion != config_.appVersion)) {
                        // Wire-format-compatible (same kProtocolVersion) but a
                        // different release -- checked before authentication so
                        // a stale client never even learns whether its stale
                        // credentials would have worked. A real protocol-
                        // version mismatch (protocolVersionMismatch above)
                        // takes the same path -- see this block's own
                        // real-user-report comment above.
                        if (protocolVersionMismatch) {
                            std::fprintf(stderr,
                                          "NetServer: rejecting handshake from %s (protocol version "
                                          "mismatch: host is v%u (app %s), client is v%u (app %s))\n",
                                          ipStr, static_cast<unsigned>(kProtocolVersion),
                                          config_.appVersion.c_str(), static_cast<unsigned>(header->protocolVersion),
                                          hello->appVersion.c_str());
                        } else {
                            std::fprintf(stderr,
                                          "NetServer: rejecting handshake from %s (app version mismatch: "
                                          "host is %s, client is %s)\n",
                                          ipStr, config_.appVersion.c_str(), hello->appVersion.c_str());
                        }
                        rejectReason = protocolVersionMismatch ? HelloRejectReason::ProtocolVersionMismatch
                                                                : HelloRejectReason::AppVersionMismatch;
                        // Real user request, 2026-08-03 (see
                        // NetServerConfig::selfUpdateCommand's own
                        // comment for the full design): only for a
                        // device identity already in the approved set --
                        // config_.authToken.empty() also excludes
                        // static-token deployments, where deviceApproval_
                        // is never populated at all and hello->authToken
                        // means something else entirely (a shared
                        // secret, not a persistent device id).
                        if (config_.authToken.empty() && !config_.selfUpdateCommand.empty() &&
                            deviceApproval_.isApproved(hello->authToken) && !selfUpdateTriggered_.exchange(true)) {
                            std::fprintf(stderr,
                                          "NetServer: %s is an already-approved device -- triggering "
                                          "self-update (%s)\n",
                                          ipStr, config_.selfUpdateCommand.c_str());
                            runSelfUpdateCommand(config_.selfUpdateCommand);
                            rejectReason = HelloRejectReason::AppVersionMismatchUpdateTriggered;
                        }
                    } else if (!config_.authToken.empty()) {
                        // Legacy/CI-friendly static-token mode: exact match or reject,
                        // device-approval flow is bypassed entirely.
                        if (constantTimeEquals(hello->authToken, config_.authToken)) {
                            handshakeOk = true;
                        } else {
                            std::fprintf(stderr,
                                          "NetServer: rejecting handshake from %s (bad auth token)\n",
                                          ipStr);
                            rejectReason = HelloRejectReason::AuthenticationFailed;
                        }
                    } else {
                        switch (deviceApproval_.check(hello->authToken, hello->clientName, ipStr)) {
                            case DeviceApprovalManager::CheckResult::Approved:
                                handshakeOk = true;
                                break;
                            case DeviceApprovalManager::CheckResult::Pending:
                                std::fprintf(stderr,
                                              "NetServer: rejecting handshake from %s (device not "
                                              "yet approved -- see the pending-request log above)\n",
                                              ipStr);
                                rejectReason = HelloRejectReason::ApprovalRequired;
                                break;
                        }
                    }

                    if (handshakeOk) {
                        // Protocol v9: an in-range client request always
                        // wins. Otherwise, the actual fallback is resolved
                        // below (see defaultVideoQualityForFrameSize()),
                        // once this connection's real frame dimensions are
                        // known -- deferred rather than just using
                        // config_.videoJpegQuality unconditionally here.
                        if (hello->videoQuality >= 1 && hello->videoQuality <= 100) {
                            currentVideoQuality_ = hello->videoQuality;
                            clientRequestedExplicitVideoQuality = true;
                        }
                    }
                } else {
                    std::fprintf(stderr, "NetServer: rejecting handshake (short Hello payload)\n");
                }
            } else {
                std::fprintf(stderr, "NetServer: rejecting handshake (bad magic/type/version)\n");
            }
        }

        HelloAckPayload ack;
        ack.accepted = handshakeOk ? 1 : 0;
        ack.rejectReason = handshakeOk ? HelloRejectReason::None : rejectReason;
        ack.sessionId = handshakeOk ? static_cast<uint32_t>(nowMicros()) : 0;
        ack.appVersion = config_.appVersion;
        // Reflects whether the audio socket actually bound at start() --
        // not just config_.micSupported, so a bind failure (e.g. the port
        // was already in use) is honestly reported rather than promising
        // a feature that silently won't work.
        ack.micSupported = audioFd_ >= 0 ? 1 : 0;
        {
            // Whatever setTarget() (GitHub issue #4 Phase B) most
            // recently set, not just the construction-time default --
            // a brand-new connection should never see stale identity.
            std::lock_guard<std::mutex> lock(targetMutex_);
            ack.system = currentSystemIdentity_;
            ack.adapter = currentAdapterIdentity_;
            ack.mode = currentMode_;
            // See IFrameSource::frameDimensions()'s comment -- reports the
            // connected source's real dimensions (e.g. AzaharAdapter's
            // 320x240) instead of always claiming DS's 256x192, so the
            // client can size its receive buffer/texture correctly.
            frameSource_->frameDimensions(ack.nativeWidth, ack.nativeHeight);
        }
        if (handshakeOk && !clientRequestedExplicitVideoQuality) {
            currentVideoQuality_ =
                defaultVideoQualityForFrameSize(config_.videoJpegQuality, ack.nativeWidth, ack.nativeHeight);
        }
        ByteBuffer ackPacket = buildHelloAckPacket(ack);
        sendAll(clientFd, ackPacket.data(), ackPacket.size());

        if (handshakeOk) {
            // Only from this point on will inputLoop() act on
            // ControllerState packets, and only from this same source
            // address (spec section 13).
            authenticatedClientAddr_ = clientAddr.sin_addr.s_addr;
            clientAuthenticated_ = true;
            if (config_.onClientConnectionChanged) {
                config_.onClientConnectionChanged(true);
            }

            // Keep reading (heartbeats / disconnect notices / garbage) until
            // the peer closes, goes silent past controlHeartbeatTimeoutUs
            // (the recv() above times out and returns -1/EAGAIN), or sends
            // something malformed enough to drop.
            while (running_.load()) {
                uint8_t buf[kPacketHeaderWireSize];
                ssize_t r = ::recv(clientFd, buf, sizeof(buf), MSG_WAITALL);
                if (r <= 0) {
                    break; // peer closed, link error, or heartbeat timeout
                }
                auto hdr = parseHeader(buf, static_cast<size_t>(r));
                if (!hdr) {
                    std::fprintf(stderr, "NetServer: dropping malformed control packet\n");
                    break;
                }
                if (hdr->type == PacketType::Disconnect) {
                    break;
                }

                // Read off (and act on) this packet's payload before
                // looping back to read the next header -- ClientLog
                // (client log-forwarding) is the first packet a client
                // sends on this post-handshake loop that actually
                // carries one; skipping it would desync the stream,
                // since its bytes would otherwise be misread as the
                // start of the next packet's header. Bounded well below
                // any sane control-channel payload to reject an
                // implausible/corrupt declared size before ever
                // allocating for it (spec section 13).
                constexpr uint32_t kMaxControlPayloadSize = 4096;
                if (hdr->payloadSize > kMaxControlPayloadSize) {
                    std::fprintf(stderr,
                                  "NetServer: dropping control packet with implausible payload size\n");
                    break;
                }
                ByteBuffer payload(hdr->payloadSize);
                if (!payload.empty()) {
                    ssize_t got = ::recv(clientFd, payload.data(), payload.size(), MSG_WAITALL);
                    if (got != static_cast<ssize_t>(payload.size())) {
                        break; // peer closed, link error, or heartbeat timeout mid-payload
                    }
                }

                if (hdr->type == PacketType::ClientLog) {
                    auto log = parseClientLogPayload(payload.data(), payload.size());
                    // No trailing "\n" added here -- every client_log.h
                    // logLine() call already ends its format string with
                    // one, so `line` arrives with it intact.
                    if (log) {
                        std::fprintf(stderr, "[client %s] %s", ipStr, log->line.c_str());
                        if (config_.onClientLogLine) {
                            config_.onClientLogLine(ipStr, log->line);
                        }
                    } else {
                        std::fprintf(stderr, "NetServer: dropping malformed ClientLog packet\n");
                    }
                }
                // Heartbeat or any other recognized-but-payload-free/
                // unrecognized type: keep the connection open, the
                // payload (if any) already consumed above.
            }
        }

        std::fprintf(stderr, "NetServer: control connection closed\n");
        ::close(clientFd);
        controlClientFd_ = -1;
        clientAuthenticated_ = false;
        authenticatedClientAddr_ = 0;
        if (handshakeOk && config_.onClientConnectionChanged) {
            config_.onClientConnectionChanged(false);
        }

        {
            std::lock_guard<std::mutex> lock(trackerMutex_);
            inputTracker_.reset();
        }
        {
            std::lock_guard<std::mutex> lock(targetMutex_);
            inputSink_->releaseAll();
        }
        micSink_.releaseAudio();
    }
}

void NetServer::inputLoop() {
    ByteBuffer buf(kPacketHeaderWireSize + kControllerStateWireSize);

    while (running_.load()) {
        sockaddr_in fromAddr{};
        socklen_t fromLen = sizeof(fromAddr);
        ssize_t n = ::recvfrom(inputFd_, buf.data(), buf.size(), 0,
                                reinterpret_cast<sockaddr*>(&fromAddr), &fromLen);
        if (n <= 0) {
            if (!running_.load()) break;
            continue;
        }

        if (!clientAuthenticated_.load() ||
            fromAddr.sin_addr.s_addr != authenticatedClientAddr_.load()) {
            continue; // no authenticated session, or packet from an unrelated address
        }

        auto header = parseHeader(buf.data(), static_cast<size_t>(n));
        if (!header || header->protocolVersion != kProtocolVersion ||
            header->type != PacketType::ControllerState) {
            std::lock_guard<std::mutex> lock(statsMutex_);
            ++stats_.inputPacketsMalformed;
            continue; // reject malformed / mismatched packet, stay up
        }

        size_t payloadOffset = kPacketHeaderWireSize;
        size_t payloadSize = static_cast<size_t>(n) - payloadOffset;
        if (payloadSize != header->payloadSize || payloadSize != kControllerStateWireSize) {
            std::lock_guard<std::mutex> lock(statsMutex_);
            ++stats_.inputPacketsMalformed;
            continue;
        }

        auto state = parseControllerState(buf.data() + payloadOffset, payloadSize);
        if (!state) {
            std::lock_guard<std::mutex> lock(statsMutex_);
            ++stats_.inputPacketsMalformed;
            continue;
        }

        bool accepted;
        {
            std::lock_guard<std::mutex> lock(trackerMutex_);
            accepted = inputTracker_.onPacketReceived(*state, nowMicros());
        }

        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            if (accepted) {
                ++stats_.inputPacketsAccepted;
                // Latency estimate needs a shared time base with the
                // client, unlike the steady_clock used for
                // timeout/sequence bookkeeping above -- see the comment
                // on wallClockNowUs() in client/src/main.cpp. Skip
                // clearly-bogus deltas (clock not synced, or a client
                // that hasn't been updated to send wall-clock time yet)
                // rather than polluting the average with them.
                uint64_t nowWallUs = nowMicrosEpoch();
                if (nowWallUs >= state->clientTimestampUs) {
                    uint64_t latencyUs = nowWallUs - state->clientTimestampUs;
                    constexpr uint64_t kMaxPlausibleLatencyUs = 10'000'000; // 10s
                    if (latencyUs <= kMaxPlausibleLatencyUs) {
                        stats_.recordLatency(latencyUs);
                    }
                }
            } else {
                ++stats_.inputPacketsOutOfOrder;
            }
        }

        if (accepted) {
            std::lock_guard<std::mutex> lock(targetMutex_);
            inputSink_->applyControllerState(*state);
        }
    }
}

void NetServer::audioLoop() {
    // Fixed header + MicAudioFramePayload's fixed prefix + the largest
    // possible sample payload (kMicAudioSamplesPerPacket samples).
    ByteBuffer buf(kPacketHeaderWireSize + 4 + 8 + 2 +
                   static_cast<size_t>(kMicAudioSamplesPerPacket) * 2);

    while (running_.load()) {
        sockaddr_in fromAddr{};
        socklen_t fromLen = sizeof(fromAddr);
        ssize_t n = ::recvfrom(audioFd_, buf.data(), buf.size(), 0,
                                reinterpret_cast<sockaddr*>(&fromAddr), &fromLen);
        if (n <= 0) {
            if (!running_.load()) break;
            continue;
        }

        if (!clientAuthenticated_.load() ||
            fromAddr.sin_addr.s_addr != authenticatedClientAddr_.load()) {
            continue; // no authenticated session, or packet from an unrelated address
        }

        auto header = parseHeader(buf.data(), static_cast<size_t>(n));
        if (!header || header->protocolVersion != kProtocolVersion ||
            header->type != PacketType::MicAudioFrame) {
            std::lock_guard<std::mutex> lock(statsMutex_);
            ++stats_.micPacketsMalformed;
            continue; // reject malformed / mismatched packet, stay up
        }

        size_t payloadOffset = kPacketHeaderWireSize;
        size_t payloadSize = static_cast<size_t>(n) - payloadOffset;
        if (payloadSize != header->payloadSize) {
            std::lock_guard<std::mutex> lock(statsMutex_);
            ++stats_.micPacketsMalformed;
            continue;
        }

        auto frame = parseMicAudioFramePayload(buf.data() + payloadOffset, payloadSize);
        if (!frame) {
            std::lock_guard<std::mutex> lock(statsMutex_);
            ++stats_.micPacketsMalformed;
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            ++stats_.micPacketsAccepted;
        }
        micSink_.applyMicAudio(*frame);
    }
}

void NetServer::watchdogLoop() {
    uint64_t lastPruneUs = nowMicros();
    uint64_t lastStatsLogUs = nowMicros();
    uint64_t lastApprovalSweepUs = nowMicros();
    constexpr uint64_t kApprovalSweepIntervalUs = 5'000'000; // 5s

    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // check() already evicts stale pending requests as a side effect,
        // but a host nobody is actively retrying against (its one pending
        // client gave up) would otherwise never get its queue swept.
        if (config_.authToken.empty()) {
            uint64_t now = nowMicros();
            if (now - lastApprovalSweepUs >= kApprovalSweepIntervalUs) {
                deviceApproval_.evictStale();
                lastApprovalSweepUs = now;
            }
        }

        bool timedOut;
        {
            std::lock_guard<std::mutex> lock(trackerMutex_);
            timedOut = inputTracker_.isTimedOut(nowMicros(), config_.inputTimeoutUs);
            if (timedOut) {
                inputTracker_.reset();
            }
        }
        if (timedOut) {
            std::fprintf(stderr, "NetServer: input timeout, releasing all inputs\n");
            std::lock_guard<std::mutex> lock(targetMutex_);
            inputSink_->releaseAll();
        }

        uint64_t now = nowMicros();
        if (now - lastPruneUs > config_.connectionAttemptWindowUs) {
            std::lock_guard<std::mutex> lock(rateLimiterMutex_);
            rateLimiter_.pruneStaleEntries(now);
            lastPruneUs = now;
        }

        if (now - lastStatsLogUs >= config_.statsLoggingIntervalUs) {
            NetServerStats snapshot;
            {
                std::lock_guard<std::mutex> lock(statsMutex_);
                snapshot = stats_;
                stats_ = NetServerStats{};
            }
            double windowSec = static_cast<double>(now - lastStatsLogUs) / 1'000'000.0;
            lastStatsLogUs = now;

            if (snapshot.inputPacketsAccepted || snapshot.inputPacketsOutOfOrder ||
                snapshot.inputPacketsMalformed || snapshot.framesSent || snapshot.framesDropped ||
                snapshot.micPacketsAccepted || snapshot.micPacketsMalformed) {
                double inputRate = windowSec > 0 ? static_cast<double>(snapshot.inputPacketsAccepted) / windowSec : 0.0;
                double frameRate = windowSec > 0 ? static_cast<double>(snapshot.framesSent) / windowSec : 0.0;
                double micRate = windowSec > 0 ? static_cast<double>(snapshot.micPacketsAccepted) / windowSec : 0.0;

                if (snapshot.latencySampleCount > 0) {
                    double avgLatencyMs =
                        static_cast<double>(snapshot.latencySumUs) / static_cast<double>(snapshot.latencySampleCount) / 1000.0;
                    std::fprintf(stderr,
                                  "NetServer: stats -- input: accepted=%llu outOfOrder=%llu malformed=%llu "
                                  "(%.1f/s) | video: sent=%llu (%.1f fps) dropped=%llu | mic: accepted=%llu "
                                  "malformed=%llu (%.1f/s) | latency: avg=%.1fms "
                                  "min=%.1fms max=%.1fms (n=%llu)\n",
                                  static_cast<unsigned long long>(snapshot.inputPacketsAccepted),
                                  static_cast<unsigned long long>(snapshot.inputPacketsOutOfOrder),
                                  static_cast<unsigned long long>(snapshot.inputPacketsMalformed), inputRate,
                                  static_cast<unsigned long long>(snapshot.framesSent), frameRate,
                                  static_cast<unsigned long long>(snapshot.framesDropped),
                                  static_cast<unsigned long long>(snapshot.micPacketsAccepted),
                                  static_cast<unsigned long long>(snapshot.micPacketsMalformed), micRate,
                                  avgLatencyMs,
                                  static_cast<double>(snapshot.latencyMinUs) / 1000.0,
                                  static_cast<double>(snapshot.latencyMaxUs) / 1000.0,
                                  static_cast<unsigned long long>(snapshot.latencySampleCount));
                } else {
                    std::fprintf(stderr,
                                  "NetServer: stats -- input: accepted=%llu outOfOrder=%llu malformed=%llu "
                                  "(%.1f/s) | video: sent=%llu (%.1f fps) dropped=%llu | mic: accepted=%llu "
                                  "malformed=%llu (%.1f/s)\n",
                                  static_cast<unsigned long long>(snapshot.inputPacketsAccepted),
                                  static_cast<unsigned long long>(snapshot.inputPacketsOutOfOrder),
                                  static_cast<unsigned long long>(snapshot.inputPacketsMalformed), inputRate,
                                  static_cast<unsigned long long>(snapshot.framesSent), frameRate,
                                  static_cast<unsigned long long>(snapshot.framesDropped),
                                  static_cast<unsigned long long>(snapshot.micPacketsAccepted),
                                  static_cast<unsigned long long>(snapshot.micPacketsMalformed), micRate);
                }
            }
        }
    }
}

void NetServer::videoLoop() {
    const auto interval = std::chrono::microseconds(
        1'000'000 / (config_.videoSendFps > 0 ? config_.videoSendFps : 60));

    // One compressor handle for this whole thread's lifetime (protocol v8,
    // see protocol.h's kProtocolVersion comment and compressFrameBgraToJpeg()
    // above) -- tjInitCompress()/tjDestroy() do real setup/teardown work,
    // and this loop already runs at up to videoSendFps, so paying that cost
    // once here instead of per-frame (or even per-connection) matters.
    // Safe to reuse across reconnects since this thread never calls it
    // concurrently with itself.
    tjhandle jpegCompressor = tjInitCompress();
    if (!jpegCompressor) {
        std::fprintf(stderr, "NetServer: tjInitCompress failed, video streaming disabled\n");
        return;
    }

    while (running_.load()) {
        sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        int clientFd = ::accept(videoListenFd_, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
        if (clientFd < 0) {
            if (!running_.load()) break;
            continue;
        }

        int previous = videoClientFd_.exchange(clientFd);
        if (previous >= 0) {
            ::close(clientFd);
            videoClientFd_ = previous;
            continue;
        }

        if (!clientAuthenticated_.load() ||
            clientAddr.sin_addr.s_addr != authenticatedClientAddr_.load()) {
            std::fprintf(stderr, "NetServer: rejecting video connection (no authenticated session)\n");
            ::close(clientFd);
            videoClientFd_ = -1;
            continue;
        }

        int nodelay = 1;
        ::setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        // Bounds how long a stalled/slow reader can block this thread: without
        // this, a full TCP send buffer makes send() block indefinitely, which
        // is exactly the kind of unbounded network wait spec section 15 rules
        // out (it doesn't affect emulation directly since this is its own
        // thread, but it would otherwise hang this connection -- and thus
        // frame delivery to any well-behaved future reconnect -- forever).
        timeval sendTimeout{};
        sendTimeout.tv_sec = 1;
        sendTimeout.tv_usec = 0;
        ::setsockopt(clientFd, SOL_SOCKET, SO_SNDTIMEO, &sendTimeout, sizeof(sendTimeout));

        // Bounds how many whole frames the OS can buffer ahead of what's
        // actually gone out over the wire. Without this, a link too slow to
        // keep up with the raw (uncompressed) frame rate doesn't drop
        // frames -- TCP's in-order guarantee means every frame this loop
        // successfully hands to send() below still gets delivered
        // eventually, just increasingly late, since each new frame queues
        // up behind whatever stale ones are still sitting in the kernel's
        // send buffer. The loop below already only ever fetches the single
        // truest-latest frame each tick (see its own comment), so nothing
        // backs up at the application level -- but a large default
        // SO_SNDBUF (typically hundreds of KB to a few MB) can still let
        // several frames' worth of now-outdated bytes sit queued in the
        // kernel, and growing latency under a bandwidth-constrained link
        // (e.g. handheld Wi-Fi) is exactly what that produces. Sizing the
        // buffer to roughly two frames' worth means the kernel can accept
        // at most one frame ahead of what's in flight; once that fills,
        // send() blocks (bounded by SO_SNDTIMEO above) until the network
        // actually drains it, and by the time it does, the next loop
        // iteration reaches for whatever's truly latest rather than
        // whatever was queued -- so latency stays bounded to roughly one
        // frame's transmission time instead of growing without limit.
        uint16_t frameWidth = 0;
        uint16_t frameHeight = 0;
        {
            std::lock_guard<std::mutex> lock(targetMutex_);
            frameSource_->frameDimensions(frameWidth, frameHeight);
        }
        // 4 bytes/pixel raw (PixelFormat::Bgra8888 is the only pixel format
        // any adapter in this project uses) -- but what actually goes out
        // over the wire since protocol v8 is a JPEG-compressed frame (see
        // compressFrameBgraToJpeg() above), typically well under a quarter
        // of that raw size at the mid-range qualities a bandwidth-
        // constrained session (e.g. Cemu) would actually use. Sizing this
        // buffer off the true raw size would let the exact bufferbloat
        // this SO_SNDBUF sizing exists to prevent creep back in, just at a
        // smaller absolute scale -- so this estimates a conservative
        // fraction of raw size instead, floored so tiny (e.g. test-
        // fixture) frame sizes still get a workable buffer. Protocol v9's
        // per-session HelloPayload::videoQuality means that fraction can
        // no longer assume quality 80's compression ratio -- a
        // near-lossless request (>=90, see compressFrameBgraToJpeg()'s own
        // 4:4:4-subsampling threshold) compresses far less aggressively,
        // so this halves rather than quarters the raw estimate there to
        // avoid under-sizing the buffer for a case this code didn't need
        // to account for before per-session quality existed.
        {
            const size_t rawFrameBytes = static_cast<size_t>(frameWidth) * frameHeight * 4;
            const size_t rawFrameDivisor = currentVideoQuality_.load() >= 90 ? 2 : 4;
            const size_t estimatedCompressedFrameBytes =
                std::max<size_t>(rawFrameBytes / rawFrameDivisor, 16384);
            if (rawFrameBytes > 0) {
                int sndBuf = static_cast<int>(std::min<size_t>(estimatedCompressedFrameBytes * 2, 0x7fffffff));
                ::setsockopt(clientFd, SOL_SOCKET, SO_SNDBUF, &sndBuf, sizeof(sndBuf));
            }
        }

        std::vector<uint8_t> frame;
        ByteBuffer jpegFrame;
        std::optional<uint64_t> lastSentFrameIndex;
        while (running_.load()) {
            auto tickStart = std::chrono::steady_clock::now();

            uint64_t frameIndex = 0;
            // Real per-frame width/height, not the once-negotiated
            // `frameWidth`/`frameHeight` sampled before this loop started --
            // see AdapterBridge::getLatestFrame()'s comment for why that
            // once-negotiated value can never be trusted for the rest of a
            // Cemu session. Defaults to the once-negotiated value so a
            // source that never updates them (SyntheticFrameSource,
            // HostControlAdapter's always-false stub) keeps behaving
            // exactly as before.
            uint16_t currentFrameWidth = frameWidth;
            uint16_t currentFrameHeight = frameHeight;
            bool gotFrame;
            {
                std::lock_guard<std::mutex> lock(targetMutex_);
                gotFrame = frameSource_->getLatestFrame(frame, frameIndex, currentFrameWidth, currentFrameHeight);
            }
            // Skip re-sending a frame whose index hasn't changed since the
            // last tick: getLatestFrame() is "return the most recent one,
            // whatever it is" (latest-frame-wins), not "return a new one
            // if there is one" -- when this loop's own tick rate (up to
            // videoSendFps) outpaces the actual source frame rate (e.g.
            // AzaharAdapter's own ~30fps capture loop vs. this loop's
            // default 60Hz), sending unconditionally meant roughly half of
            // every video packet was a byte-for-byte duplicate of the one
            // before it -- pure wasted bandwidth, worth nothing to the
            // client (main.cpp/net_client.cpp just redraw the same texture
            // either way), and reducing the send budget actually available
            // for genuinely new frames.
            if (gotFrame && lastSentFrameIndex && frameIndex == *lastSentFrameIndex) {
                gotFrame = false;
            }
            // Taken right before encoding begins (protocol v10, see
            // protocol.h's kProtocolVersion comment and
            // VideoFramePayload) -- wall-clock, not steady_clock, so
            // it's directly comparable against the client's own
            // wall-clock receipt time the same way
            // ControllerState::clientTimestampUs already is for input
            // latency. Deliberately captured even on a tick that ends up
            // skipping the send below (dead code in that case) rather
            // than moved inside the `if (gotFrame)` block, so this stays
            // a single obvious "when did we start working on this
            // frame" read with no risk of drifting from what it's
            // actually meant to measure as the surrounding code changes.
            uint64_t captureTimestampUs = nowMicrosEpoch();
            if (gotFrame && !compressFrameBgraToJpeg(jpegCompressor, frame.data(), currentFrameWidth, currentFrameHeight,
                                                      currentVideoQuality_.load(), jpegFrame)) {
                // Logged inside compressFrameBgraToJpeg(); skip this tick
                // rather than tearing down the connection, same treatment
                // as "nothing new to send" below -- a transient encode
                // failure shouldn't be fatal.
                gotFrame = false;
            }
            if (gotFrame) {
                VideoFramePayload videoPayload;
                videoPayload.captureTimestampUs = captureTimestampUs;
                videoPayload.jpeg = std::move(jpegFrame);
                ByteBuffer packet = buildVideoFramePacket(videoPayload);
                if (!sendAll(clientFd, packet.data(), packet.size())) {
                    break;
                }

                std::lock_guard<std::mutex> lock(statsMutex_);
                ++stats_.framesSent;
                if (lastSentFrameIndex && frameIndex > *lastSentFrameIndex + 1) {
                    stats_.framesDropped += frameIndex - *lastSentFrameIndex - 1;
                }
                lastSentFrameIndex = frameIndex;
            }

            auto elapsed = std::chrono::steady_clock::now() - tickStart;
            auto remaining = interval - std::chrono::duration_cast<std::chrono::microseconds>(elapsed);
            if (remaining.count() > 0) {
                std::this_thread::sleep_for(remaining);
            }
        }

        ::close(clientFd);
        videoClientFd_ = -1;
    }

    tjDestroy(jpegCompressor);
}

void NetServer::discoveryLoop() {
    std::string hostName = config_.hostName;
    if (hostName.empty()) {
        char hostnameBuf[256] = {0};
        if (::gethostname(hostnameBuf, sizeof(hostnameBuf) - 1) == 0) {
            hostName = hostnameBuf;
        } else {
            hostName = "dualdeck-host";
        }
    }
    if (hostName.size() > kMaxProtocolStringLength) {
        hostName.resize(kMaxProtocolStringLength);
    }

    ByteBuffer buf(kPacketHeaderWireSize);

    while (running_.load()) {
        sockaddr_in fromAddr{};
        socklen_t fromLen = sizeof(fromAddr);
        ssize_t n = ::recvfrom(discoveryFd_, buf.data(), buf.size(), 0,
                                reinterpret_cast<sockaddr*>(&fromAddr), &fromLen);
        if (n <= 0) {
            if (!running_.load()) break;
            continue;
        }

        auto header = parseHeader(buf.data(), static_cast<size_t>(n));
        // Deliberately NOT checking header->protocolVersion here (real-usage
        // bug: a client left on an older/newer build than a freshly-updated
        // host silently saw zero hosts in its discovery scan, with no error
        // anywhere -- this check made a mismatched-but-otherwise-well-formed
        // DiscoveryRequest indistinguishable from noise on the wire).
        // Discovery only needs to answer "is there a host here to try" --
        // actual version compatibility is already enforced, with a real
        // AppVersionMismatch error shown to the user, by the Hello/HelloAck
        // handshake a client performs after picking this host from the list.
        if (!header || header->type != PacketType::DiscoveryRequest) {
            continue; // not a well-formed DiscoveryRequest -- ignore silently
        }

        DiscoveryResponsePayload response;
        response.hostName = hostName;
        response.controlPort = config_.controlPort;
        response.inputPort = config_.inputPort;
        response.videoPort = config_.videoPort;
        response.audioPort = config_.audioPort;
        {
            // Same reasoning as controlLoop()'s HelloAck above: reflect
            // whatever setTarget() most recently set, so the
            // host-selection list shows the current mode/identity even
            // before a client attempts a handshake.
            std::lock_guard<std::mutex> lock(targetMutex_);
            response.system = currentSystemIdentity_;
            response.adapter = currentAdapterIdentity_;
        }
        ByteBuffer packet = buildDiscoveryResponsePacket(response);
        // Unicast back to the specific sender -- never a broadcast reply.
        ::sendto(discoveryFd_, packet.data(), packet.size(), 0,
                 reinterpret_cast<sockaddr*>(&fromAddr), fromLen);
    }
}

} // namespace melonds_remote::host
