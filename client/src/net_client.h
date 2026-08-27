#pragma once

// Client-side counterpart to host/remote-server/include/host/net_server.h:
// TCP control handshake, UDP ControllerState sending, TCP video-frame
// receiving. Pure sockets + protocol/, no SDL, so the networking logic is
// testable independently of the windowing/input code in main.cpp.

#include <atomic>
#include <cstdint>
#include <deque>
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

    // JPEG quality (1-100) to request for this session's video, or 0 to
    // defer to the host's own configured default. See
    // melonds_remote::HelloPayload::videoQuality's comment; set from
    // ClientSettings::videoQuality in main.cpp.
    uint8_t videoQuality = 0;

    // Protocol v13: whether to also advertise VideoCodecBit_H264 (in
    // addition to the always-advertised VideoCodecBit_Jpeg) in
    // HelloPayload.supportedVideoCodecs. Set from ClientSettings::
    // videoCodecH264Experimental in main.cpp -- see that field's own
    // comment for why this defaults off. The host still has the final
    // say (NetServer::selectVideoCodec()); this only ever raises the
    // possibility, never forces H.264.
    bool preferH264 = false;

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

    // Bumped every time latestFrame_'s content or validity actually
    // changes -- a new frame swapped in by videoReceiveLoop(), or hasFrame_
    // reset by controlReceiveLoop() on a mode change (see that call site's
    // own comment). Lets a caller like main.cpp's render loop tell "is
    // there anything new to show" apart from "is there still nothing new"
    // without paying getLatestFrame()'s full-vector-copy cost just to find
    // out -- real fix for the render loop redundantly copying+re-uploading
    // the same unchanged frame to the GPU every single iteration
    // regardless of whether a new one had actually arrived. Never
    // decreases, never resets to 0 (including across a reconnect on the
    // same NetClient instance) -- callers should only ever compare it for
    // inequality against a previously-observed value, never for a specific
    // number.
    uint64_t latestFrameGeneration() const { return latestFrameGeneration_.load(); }

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

    // Which emulated system / adapter the host reported in its most
    // recent HelloAck (GitHub issue #28), regardless of whether that
    // handshake was accepted -- same availability convention as
    // hostAppVersion() above. Default-constructed (empty strings) if no
    // HelloAck has been received yet.
    SystemIdentity hostSystemIdentity() const;
    AdapterIdentity hostAdapterIdentity() const;

    // The most recently decoded video frame's real dimensions. Originally
    // just HelloAck's one-time-negotiated value (GitHub issue: "no video
    // on real hardware" for Azahar/3DS -- the host used to always default
    // to DS's 256x192 in HelloAck regardless of what it actually streamed,
    // so a 320x240 3DS frame was silently rejected). Now also updated by
    // videoReceiveLoop() after every successfully decoded frame (real bug
    // this fixes: CemuAdapter's actual capture resolution isn't known at
    // Hello time and can differ from whatever was negotiated then -- see
    // adapter_contract.h's SurfaceFrame comment) -- so, like hostMode()
    // below, callers should poll this every frame rather than only once
    // per connection, to react to a real size change without a reconnect.
    // Defaults to 256x192 if no frame has been decoded yet.
    uint16_t hostNativeWidth() const;
    uint16_t hostNativeHeight() const;

    // Which mode the host is currently in (GitHub issue #4 Phase E):
    // Emulation (streaming a real session, the only mode that ever
    // existed before issue #4) or HostControl (no emulator running --
    // the client should show a host-control screen instead of the video
    // texture, while still sending ControllerState as normal so
    // HostControlAdapter's virtual gamepad on the host side works).
    // Reflects the host's most recent HelloAck right after connect(),
    // and is kept live afterward by controlReceiveLoop() reacting to
    // ModeChanged packets -- callers should poll this every frame rather
    // than only reading it once per connection, since the host can
    // switch modes mid-session (an adapter connecting/disconnecting, or
    // a manual override at the host). Defaults to Emulation when not
    // connected, matching every pre-issue-#4 host's only behavior.
    HostMode hostMode() const { return hostMode_.load(); }

    // Which VideoCodec the host actually selected for this session (see
    // NetServer::selectVideoCodec()) -- JPEG unless both sides advertised
    // and negotiated H264 (protocol v13; see HelloPayload::
    // supportedVideoCodecs and h264_decoder.h). Set once at connect()
    // time; unlike hostMode() there's no mid-session change to react to,
    // since codec choice is fixed for a session's whole lifetime (same
    // as HelloPayload::videoQuality).
    VideoCodec negotiatedVideoCodec() const { return negotiatedVideoCodec_.load(); }

    // Debug-overlay stats (GitHub/real user request, 2026-08-26: "show
    // the user what resolution is being streamed, what fps, codec, etc
    // as a debug overlay"). All updated by videoReceiveLoop() after every
    // successfully decoded frame, atomically, safe to poll from any
    // thread (main.cpp's render loop) at any rate -- same convention as
    // hostNativeWidth()/hostMode() above, not a snapshot taken once.
    //
    // Frames actually decoded and displayed since connect() -- not a
    // rate on its own; callers wanting an FPS figure should sample this
    // twice a known time apart and divide, same as receivedFps() below
    // does internally (kept separate so a caller that wants raw frame
    // count for something else, e.g. correlating with a log line, isn't
    // forced to also care about timing).
    uint64_t receivedFrameCount() const { return receivedFrameCount_.load(); }
    // Frames/sec actually received+decoded, recomputed once/sec inside
    // videoReceiveLoop() from its own wall-clock-timestamped window --
    // this is real receive throughput, not a render-loop rate (which
    // could differ if the render loop is vsync-limited or uncapped) and
    // not NetServerConfig::videoSendFps (a host-side polling-tick-rate
    // ceiling, not a guarantee every tick produces a distinct frame).
    // 0 until the first full one-second window completes after connect.
    uint32_t receivedFps() const { return receivedFps_.load(); }
    // Compressed size of the most recently received frame's wire payload
    // (VideoFramePayload::jpeg -- JPEG or H264 Annex-B bytes depending on
    // negotiatedVideoCodec(), see that field's own comment in protocol.h)
    // in bytes, before decode. A rough, single-frame bandwidth indicator,
    // not an average -- deliberately not smoothed, so a debug overlay
    // showing it live reflects real per-frame variance (an I-frame vs. a
    // P-frame's very different size under H264, in particular) rather
    // than hiding it behind an average that would make bitrate spikes
    // invisible.
    uint32_t lastFrameCompressedBytes() const { return lastFrameCompressedBytes_.load(); }
    // Wall-clock micros this specific frame took to decode (JPEG:
    // libjpeg-turbo; H264: H264Decoder) -- the same measurement
    // decodeStats below already accumulates min/max/avg of for its own
    // periodic log line, exposed here as the single latest sample for a
    // live overlay instead of a periodically-reset accumulator.
    uint32_t lastDecodeMicros() const { return lastDecodeMicros_.load(); }
    // Network+encode+queue latency for this specific frame (captureTimestampUs
    // to receipt, corrected for host/client clock offset -- protocol v14,
    // see connect()'s own comment) -- same source measurement as
    // networkStats' own min/max/avg log line, again exposed as the
    // latest single sample. 0 if this particular frame's corrected
    // latency came out at or below zero (measurement noise, clamped --
    // see videoReceiveLoop()'s own comment); not filtered by
    // kMaxPlausibleLatencyUs, unlike networkStats -- an implausibly
    // large value still shows here, since that's itself a useful signal
    // something is wrong rather than something to hide.
    uint32_t lastLatencyMicros() const { return lastLatencyMicros_.load(); }

    // Enqueues `line` (already formatted -- see client_log.h) to be
    // forwarded to the host as a ClientLog packet on the control
    // channel, for host-side debugging/app development. Best-effort and
    // never blocks: silently dropped if not currently connected or the
    // queue is already at kMaxQueuedLogLines -- losing a diagnostic
    // message is a vastly smaller problem than this call stalling
    // whatever hot path just tried to log something, or an unbounded
    // queue building up while disconnected. Actually sent by
    // heartbeatLoop(), which already wakes every 50ms regardless of
    // connection activity.
    void sendClientLog(const std::string& line);

private:
    void videoReceiveLoop();
    void controlReceiveLoop();
    void heartbeatLoop();
    // Drains the queue sendClientLog() fills and sends each line as its
    // own ClientLog packet. Returns false the instant a send fails (the
    // connection is dead), matching how a Heartbeat send failure is
    // already treated in heartbeatLoop() -- losing the rest of a batch
    // of queued log lines to a dead socket is not worth a partial retry.
    bool sendQueuedLogLines();
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
    std::atomic<HostMode> hostMode_{HostMode::Emulation};
    std::atomic<VideoCodec> negotiatedVideoCodec_{VideoCodec::Jpeg};
    // Read on every received video packet by videoReceiveLoop() (see
    // hostNativeWidth()/hostNativeHeight() below), so atomic like
    // sessionId_/hostMode_ above rather than mutex-guarded like
    // hostSystemIdentity_/hostAdapterIdentity_, which main.cpp only reads
    // once per connect edge.
    std::atomic<uint16_t> hostNativeWidth_{256};
    std::atomic<uint16_t> hostNativeHeight_{192};
    // Protocol v14 clock-offset estimate (host clock minus this client's
    // own clock, in microseconds -- may be negative), computed once per
    // connect() and applied to every video frame's latency calculation
    // in videoReceiveLoop() -- see connect()'s own comment for how it's
    // derived and why it exists. Signed (unlike hostNativeWidth_ above)
    // since either clock can legitimately be ahead of the other.
    std::atomic<int64_t> clockOffsetUs_{0};

    // Debug-overlay stats -- see the public getters' own comments above
    // for what each one means. All written only by videoReceiveLoop(),
    // same single-writer/many-reader atomic convention as
    // hostNativeWidth_ above.
    std::atomic<uint64_t> receivedFrameCount_{0};
    std::atomic<uint32_t> receivedFps_{0};
    std::atomic<uint32_t> lastFrameCompressedBytes_{0};
    std::atomic<uint32_t> lastDecodeMicros_{0};
    std::atomic<uint32_t> lastLatencyMicros_{0};

    // Serializes connect()/disconnect() against each other -- callers may
    // run reconnect-on-a-background-thread while the main thread can
    // still call disconnect() at shutdown. connectionAttemptInProgress_
    // prevents a second caller from queueing another connect behind the
    // first one while the handshake is still blocking.
    std::mutex connectMutex_;
    std::atomic<bool> connectionAttemptInProgress_{false};

    std::atomic<bool> connected_{false};
    std::thread videoThread_;
    std::thread controlThread_;
    std::thread heartbeatThread_;

    std::mutex frameMutex_;
    std::vector<uint8_t> latestFrame_;
    bool hasFrame_ = false;
    // See latestFrameGeneration()'s own comment. Written by both
    // videoReceiveLoop() (new frame) and controlReceiveLoop() (hasFrame_
    // reset on mode change) -- two threads, but each only ever increments,
    // never reads-then-conditionally-writes, so a plain atomic fetch_add is
    // sufficient; no separate lock needed beyond frameMutex_ already
    // serializing the hasFrame_/latestFrame_ mutation each bump
    // accompanies.
    std::atomic<uint64_t> latestFrameGeneration_{0};

    // Guards logQueue_ -- filled by sendClientLog() (any thread), drained
    // by heartbeatLoop() (its own thread) every ~50ms. Capped at
    // kMaxQueuedLogLines (see sendClientLog()'s own comment for why an
    // over-cap line is simply dropped rather than queued or blocked on).
    std::mutex logQueueMutex_;
    std::deque<std::string> logQueue_;

    // Guards lastRejectReason_; connect() holds connectMutex_ for its
    // whole body anyway, but lastRejectReason() may be called from a
    // different thread (e.g. the render thread reacting to connect()'s
    // return value while a background reconnect thread also calls
    // connect()), so it gets its own narrower lock.
    mutable std::mutex handshakeResultMutex_;
    HelloRejectReason lastRejectReason_ = HelloRejectReason::None;
    std::string hostAppVersion_;
    SystemIdentity hostSystemIdentity_;
    AdapterIdentity hostAdapterIdentity_;
};

} // namespace melonds_remote::client
