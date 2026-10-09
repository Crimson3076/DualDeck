#include "host/net_server.h"

#include "net_server_internal.h"

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

#include "host/h264_encoder.h"
#include "host/pyrowave_encoder.h"
#include "melonds_remote/protocol.h"

namespace melonds_remote::host {

using namespace net_detail;

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
//
// Real user report, 2026-08-03 (Bazzite): this backgrounded, detached
// invocation has no controlling terminal at all, so when apply-update.sh
// hands off to install-steam-shortcut.sh --force on an immutable
// (rpm-ostree) system, its ostree-booted branch used to unconditionally
// delegate to install-host-distrobox.sh --install-only -- which needs an
// unattended `sudo dnf install` to succeed before it will activate the
// staged files (by design, so a failed package install never activates
// unverified files). With no TTY to authenticate sudo, that step
// silently fails, the activation swap never runs, and install/
// (including internal/lib/libturbojpeg.so.0) is left stuck on whatever
// was staged before this update -- explaining a self-update that appears
// to succeed (readlink still points at the correct, un-swapped install/)
// while the host binary keeps failing to start. `--self-update` is only
// ever wired to this exact persistent Host Control daemon process (see
// this function's own header comment above, and main.cpp's --self-update
// help text: "never set this for melonDS's in-process integration"), so
// it's always correct -- not just for this one call -- to tell the
// update chain it's a Host-Control-only update: install-steam-
// shortcut.sh's ostree branch now skips the Distrobox/dnf provisioning
// step entirely when DUALDECK_HOST_CONTROL=1 and does the same
// lightweight, always-succeeds file swap the non-immutable branch
// already uses (dualdeck-host-control.service never launches
// melonDS/Azahar/Cemu, so there's no Distrobox container use to keep in
// sync in the first place).
void runSelfUpdateCommand(const std::string& command) {
    std::string shellCommand = "DUALDECK_HOST_CONTROL=1 nohup " + command + " >/dev/null 2>&1 &";
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
        // Captured here (not read directly from `hello` further down)
        // because `hello` goes out of scope well before the
        // frameSource_->setTargetDisplaySize() call site below needs
        // it -- see that call site's own comment.
        uint16_t clientDisplayWidth = 0;
        uint16_t clientDisplayHeight = 0;
        // Same "hello goes out of scope early" reasoning as
        // clientDisplayWidth/Height above -- see selectVideoCodec()'s own
        // comment for how this gets computed.
        VideoCodec selectedVideoCodec = VideoCodec::Jpeg;

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
                    if (hello) {
                        clientDisplayWidth = hello->displayWidth;
                        clientDisplayHeight = hello->displayHeight;
                    }
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
                        selectedVideoCodec = selectVideoCodec(hello->supportedVideoCodecs);
                        currentVideoCodec_ = selectedVideoCodec;
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
            ack.selectedVideoCodec = selectedVideoCodec;
            // See IFrameSource::setTargetDisplaySize()'s own comment --
            // only for an actually-accepted handshake, so a rejected/
            // malformed connection's reported size (clientDisplayWidth/
            // Height default to 0 either way, so this is mostly a
            // belt-and-suspenders guard against re-arming with stale
            // values from a since-rejected reconnect attempt) never
            // affects a source shared with whatever session is already
            // active.
            if (handshakeOk) {
                frameSource_->setTargetDisplaySize(clientDisplayWidth, clientDisplayHeight);
            }
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
        // Protocol v14: taken as close to the actual send below as
        // possible (same nowMicrosEpoch() clock VideoFramePayload::
        // captureTimestampUs already uses) -- see kProtocolVersion's v14
        // comment in protocol.h and NetClient::connect()'s own comment
        // for why this exists and how the client turns it into a clock-
        // offset estimate.
        ack.hostTimeUs = nowMicrosEpoch();
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

} // namespace melonds_remote::host
