#include "host/net_server.h"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <utility>

#include "dualdeck/protocol.h"

#include "net_server_internal.h"

namespace dualdeck::host {

using namespace net_detail;

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

SystemIdentity NetServer::currentSystemIdentity() const {
    std::lock_guard<std::mutex> lock(targetMutex_);
    return currentSystemIdentity_;
}

AdapterIdentity NetServer::currentAdapterIdentity() const {
    std::lock_guard<std::mutex> lock(targetMutex_);
    return currentAdapterIdentity_;
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

std::optional<size_t> NetServer::receiveSessionPacket(int fd, ByteBuffer& buf, PacketType type,
                                                     uint64_t NetServerStats::*malformed) {
    sockaddr_in fromAddr{};
    socklen_t fromLen = sizeof(fromAddr);
    ssize_t n = ::recvfrom(fd, buf.data(), buf.size(), 0, reinterpret_cast<sockaddr*>(&fromAddr), &fromLen);
    if (n <= 0) return std::nullopt;

    if (!clientAuthenticated_.load() || fromAddr.sin_addr.s_addr != authenticatedClientAddr_.load()) {
        return std::nullopt; // no authenticated session, or packet from an unrelated address
    }

    auto header = parseHeader(buf.data(), static_cast<size_t>(n));
    size_t payloadSize = static_cast<size_t>(n) - kPacketHeaderWireSize;
    if (!header || header->protocolVersion != kProtocolVersion || header->type != type ||
        payloadSize != header->payloadSize) {
        std::lock_guard<std::mutex> lock(statsMutex_);
        ++(stats_.*malformed);
        return std::nullopt; // reject malformed / mismatched packet, stay up
    }
    return payloadSize;
}

void NetServer::inputLoop() {
    ByteBuffer buf(kPacketHeaderWireSize + kControllerStateWireSize);

    while (running_.load()) {
        auto payloadSize = receiveSessionPacket(inputFd_, buf, PacketType::ControllerState,
                                                &NetServerStats::inputPacketsMalformed);
        if (!payloadSize) continue;
        if (*payloadSize != kControllerStateWireSize) {
            std::lock_guard<std::mutex> lock(statsMutex_);
            ++stats_.inputPacketsMalformed;
            continue;
        }

        auto state = parseControllerState(buf.data() + kPacketHeaderWireSize, *payloadSize);
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
        auto payloadSize = receiveSessionPacket(audioFd_, buf, PacketType::MicAudioFrame,
                                                &NetServerStats::micPacketsMalformed);
        if (!payloadSize) continue;

        auto frame = parseMicAudioFramePayload(buf.data() + kPacketHeaderWireSize, *payloadSize);
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

                // Latency-audit follow-up: split-out host-local encode/send
                // timing (see NetServerStats::recordVideoEncode's own
                // comment) -- a separate line, printed only once real video
                // frames were actually sent this window, rather than
                // reworking the two format strings above (which fire even
                // in mic/input-only windows with no video at all).
                if (snapshot.videoEncodeSampleCount > 0) {
                    std::fprintf(stderr,
                                  "NetServer: video timing -- encode avg=%.2fms min=%.2fms max=%.2fms "
                                  "(n=%llu) | send avg=%.2fms min=%.2fms max=%.2fms (n=%llu)\n",
                                  static_cast<double>(snapshot.videoEncodeSumUs) /
                                      static_cast<double>(snapshot.videoEncodeSampleCount) / 1000.0,
                                  static_cast<double>(snapshot.videoEncodeMinUs) / 1000.0,
                                  static_cast<double>(snapshot.videoEncodeMaxUs) / 1000.0,
                                  static_cast<unsigned long long>(snapshot.videoEncodeSampleCount),
                                  static_cast<double>(snapshot.videoSendSumUs) /
                                      static_cast<double>(snapshot.videoSendSampleCount) / 1000.0,
                                  static_cast<double>(snapshot.videoSendMinUs) / 1000.0,
                                  static_cast<double>(snapshot.videoSendMaxUs) / 1000.0,
                                  static_cast<unsigned long long>(snapshot.videoSendSampleCount));
                }
            }
        }
    }
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

} // namespace dualdeck::host
