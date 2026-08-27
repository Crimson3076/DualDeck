// Real end-to-end tests of NetClient's control-channel read loop (GitHub
// issue #4 Phase E): a real NetClient connecting to a real NetServer over
// real loopback sockets -- not mocks of either side, matching this
// project's established end-to-end test style (see e.g.
// host/remote-server/tests/test_net_server_mode_switch.cpp, which this
// file's fixture setup deliberately mirrors). Exercises the three things
// the control-channel read loop exists for: a fresh handshake already
// reflects whatever mode the host is in, a live ModeChanged packet
// updates hostMode()/hostSystemIdentity()/hostAdapterIdentity() while
// connected, and a dead server connection is actually detected (isConnected()
// becomes false) rather than hanging forever -- the gap this phase closed
// (see net_client.h's hostMode() doc comment and docs/known-limitations.md).

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

#include "host/logging_input_sink.h"
#include "host/logging_mic_audio_sink.h"
#include "host/net_server.h"
#include "melonds_remote/protocol.h"
#include "net_client.h"
#include "test_framework.h"

using namespace melonds_remote;
using namespace melonds_remote::host;
using namespace melonds_remote::client;

namespace {

// No video content is needed by anything in this file -- every test here
// is about the control channel, not the video channel -- so both of
// NetServer's swappable targets just use this rather than
// host::SyntheticFrameSource's real generator thread.
class NullFrameSource : public IFrameSource {
public:
    bool getLatestFrame(std::vector<uint8_t>&, uint64_t&, uint16_t&, uint16_t&) override { return false; }
};

// Real bug this exists to catch: AzaharAdapter's actual 320x240 bottom
// screen used to have no way to tell a connecting client it wasn't DS's
// fixed 256x192, so every frame it produced was silently dropped by the
// client's own hardcoded size check -- see net_client.cpp's
// videoReceiveLoop() and net_server.cpp's HelloAck construction.
// Declares an arbitrary non-DS size and can optionally hand back a real
// frame of exactly that size, to prove the whole pipeline (HelloAck's
// nativeWidth/nativeHeight -> NetClient's hostNativeWidth_/Height_ ->
// videoReceiveLoop()'s payload-size check) actually delivers a
// non-DS-sized frame end to end, not just that the reported dimensions
// look right in isolation.
class SizedFrameSource : public IFrameSource {
public:
    SizedFrameSource(uint16_t width, uint16_t height) : width_(width), height_(height) {}

    void frameDimensions(uint16_t& outWidth, uint16_t& outHeight) const override {
        outWidth = width_;
        outHeight = height_;
    }

    bool getLatestFrame(std::vector<uint8_t>& outFrame, uint64_t& outFrameIndex,
                        uint16_t& outWidth, uint16_t& outHeight) override {
        if (!hasFrame_) return false;
        outFrame = frame_;
        outFrameIndex = 0;
        outWidth = width_;
        outHeight = height_;
        return true;
    }

    void setFrame(std::vector<uint8_t> frame) {
        frame_ = std::move(frame);
        hasFrame_ = true;
    }

private:
    uint16_t width_;
    uint16_t height_;
    std::vector<uint8_t> frame_;
    bool hasFrame_ = false;
};

uint16_t freePort() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    uint16_t port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

bool waitUntil(const std::function<bool()>& predicate, int timeoutMs = 2000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

// RAII wrapper: a real NetServer on hermetically-allocated loopback ports,
// static-auth-token mode, always torn down even if a CHECK fails partway
// through. Mirrors host/remote-server/tests/test_net_server_mode_switch.cpp's
// ServerFixture.
struct ServerFixture {
    LoggingInputSink sinkA;
    NullFrameSource frameA;
    LoggingInputSink sinkB;
    NullFrameSource frameB;
    LoggingMicAudioSink micSink;
    NetServerConfig config;
    NetServer server;

    ServerFixture()
        : config([] {
              NetServerConfig cfg;
              cfg.bindAddress = "127.0.0.1";
              cfg.controlPort = freePort();
              cfg.inputPort = freePort();
              cfg.videoPort = freePort();
              cfg.discoveryEnabled = false;
              cfg.micSupported = false;
              cfg.authToken = "test-token";
              cfg.inputTimeoutUs = 5'000'000; // generous -- irrelevant to these tests
              return cfg;
          }()),
          server(config, sinkA, frameA, micSink) {
        server.start();
    }

    ~ServerFixture() { server.stop(); }

    NetClientConfig clientConfig() const {
        NetClientConfig cfg;
        cfg.hostAddress = "127.0.0.1";
        cfg.controlPort = config.controlPort;
        cfg.inputPort = config.inputPort;
        cfg.videoPort = config.videoPort;
        cfg.authToken = config.authToken;
        cfg.heartbeatIntervalMs = 200; // short, so heartbeat-loop-related tests don't wait long
        return cfg;
    }
};

} // namespace

MDR_TEST(net_client_defaults_to_emulation_mode) {
    ServerFixture fixture;
    NetClient client(fixture.clientConfig());
    MDR_CHECK(client.connect());
    MDR_CHECK(client.hostMode() == HostMode::Emulation);
    client.disconnect();
}

MDR_TEST(net_client_reports_host_control_mode_from_a_fresh_handshake) {
    ServerFixture fixture;
    // Swap into HostControl before any client ever connects -- a brand
    // new handshake must see this from HelloAck::mode directly, not wait
    // for a ModeChanged that (per NetServer::setTarget()) is only ever
    // sent to an *already-connected* client and so would never arrive.
    fixture.server.setTarget(fixture.sinkB, fixture.frameB, HostMode::HostControl, SystemIdentity{"host", "Host Menu"},
                              AdapterIdentity{"host-control", "Host Control", ""});

    NetClient client(fixture.clientConfig());
    MDR_CHECK(client.connect());
    MDR_CHECK(client.hostMode() == HostMode::HostControl);
    MDR_CHECK(client.hostSystemIdentity().systemId == "host");
    MDR_CHECK(client.hostAdapterIdentity().adapterId == "host-control");
    client.disconnect();
}

MDR_TEST(net_client_receives_mode_changed_while_connected) {
    ServerFixture fixture;
    NetClient client(fixture.clientConfig());
    MDR_CHECK(client.connect());
    MDR_CHECK(client.hostMode() == HostMode::Emulation);

    fixture.server.setTarget(fixture.sinkB, fixture.frameB, HostMode::HostControl, SystemIdentity{"host", "Host Menu"},
                              AdapterIdentity{"host-control", "Host Control", ""});

    MDR_CHECK(waitUntil([&] { return client.hostMode() == HostMode::HostControl; }));
    MDR_CHECK(client.hostSystemIdentity().systemId == "host");
    MDR_CHECK(client.hostAdapterIdentity().adapterId == "host-control");
    // The control connection must still be alive -- ModeChanged is not a
    // disconnect signal.
    MDR_CHECK(client.isConnected());

    client.disconnect();
}

MDR_TEST(net_client_receives_mode_changed_back_to_emulation) {
    ServerFixture fixture;
    NetClient client(fixture.clientConfig());
    MDR_CHECK(client.connect());

    fixture.server.setTarget(fixture.sinkB, fixture.frameB, HostMode::HostControl, SystemIdentity{"host", "Host Menu"},
                              AdapterIdentity{"host-control", "Host Control", ""});
    MDR_CHECK(waitUntil([&] { return client.hostMode() == HostMode::HostControl; }));

    fixture.server.setTarget(fixture.sinkA, fixture.frameA, HostMode::Emulation, SystemIdentity{"nds", "Nintendo DS"},
                              AdapterIdentity{"melonds", "melonDS", "1.1"});
    MDR_CHECK(waitUntil([&] { return client.hostMode() == HostMode::Emulation; }));
    MDR_CHECK(client.hostSystemIdentity().systemId == "nds");
    MDR_CHECK(client.hostAdapterIdentity().adapterId == "melonds");

    client.disconnect();
}

MDR_TEST(net_client_reports_host_native_dimensions_from_hello_ack) {
    ServerFixture fixture;
    // Swap in a non-DS-sized source before any client connects -- a
    // fresh handshake's HelloAck must reflect it directly, matching
    // net_client_reports_host_control_mode_from_a_fresh_handshake above.
    SizedFrameSource threeDsFrame(320, 240);
    fixture.server.setTarget(fixture.sinkA, threeDsFrame, HostMode::Emulation,
                              SystemIdentity{"n3ds", "Nintendo 3DS"}, AdapterIdentity{"azahar", "Azahar", "1.0"});

    NetClient client(fixture.clientConfig());
    MDR_CHECK(client.connect());
    MDR_CHECK_EQ(client.hostNativeWidth(), 320);
    MDR_CHECK_EQ(client.hostNativeHeight(), 240);
    client.disconnect();
}

MDR_TEST(net_client_receives_a_non_ds_sized_video_frame) {
    ServerFixture fixture;
    SizedFrameSource threeDsFrame(320, 240);
    std::vector<uint8_t> realFrame(static_cast<size_t>(320) * 240 * 4, 0x7A);
    threeDsFrame.setFrame(realFrame);
    fixture.server.setTarget(fixture.sinkA, threeDsFrame, HostMode::Emulation,
                              SystemIdentity{"n3ds", "Nintendo 3DS"}, AdapterIdentity{"azahar", "Azahar", "1.0"});

    NetClient client(fixture.clientConfig());
    MDR_CHECK(client.connect());

    // Before the fix, videoReceiveLoop() always expected a 256x192-sized
    // (196608-byte) payload -- a 320x240 (307200-byte) frame would fail
    // that check, log "dropping unexpected packet", and close the video
    // connection entirely, so getLatestFrame() would never return true.
    //
    // Not a byte-for-byte comparison against realFrame: protocol v8
    // JPEG-compresses every frame (see protocol.h's kProtocolVersion
    // comment), which is lossy by design, so net_server.cpp's encode ->
    // net_client.cpp's decode round trip is not guaranteed to reproduce
    // the exact input bytes even for this flat, single-color frame. What
    // this test actually needs to prove -- a non-DS-sized frame survives
    // the whole pipeline, decoded back to the right size with recognizably
    // the same content -- is checked instead via size plus a small
    // per-channel tolerance on the B/G/R bytes.
    //
    // The 4th byte of every BGRA pixel (alpha) is skipped entirely rather
    // than compared: JPEG has no alpha channel at all, so
    // compressFrameBgraToJpeg()/decompressJpegToBgra() only ever round-trip
    // B/G/R through it -- decoding always produces 0xFF there regardless
    // of the original byte. That's fine in practice (main.cpp always sets
    // SDL_BLENDMODE_NONE on the video texture, so alpha is never actually
    // used), but means alpha can't be part of this equality check.
    std::vector<uint8_t> received;
    MDR_CHECK(waitUntil([&] {
        if (!client.getLatestFrame(received)) return false;
        if (received.size() != realFrame.size()) return false;
        for (size_t i = 0; i < received.size(); ++i) {
            if (i % 4 == 3) continue; // alpha -- see comment above
            int diff = static_cast<int>(received[i]) - static_cast<int>(realFrame[i]);
            if (diff < -8 || diff > 8) return false;
        }
        return true;
    }));
    MDR_CHECK(client.isConnected());

    client.disconnect();
}

// Protocol v14: real user report, LATENCY read exactly 0US for a whole
// session (see protocol.h's kProtocolVersion v14 comment and
// NetClient::connect()'s own comment for the full clock-skew story).
// ServerFixture runs host and client in the same process on the same
// machine, so the real clock-offset estimate connect() computes should
// land near zero here -- this doesn't exercise actual cross-machine
// skew (there is none to exercise on loopback), but it does prove the
// new HelloAckPayload::hostTimeUs plumbing and offset arithmetic run
// end-to-end without crashing, rejecting the handshake, or producing a
// wildly wrong (e.g. multi-second) corrected latency for a frame that
// really did arrive within this test's own runtime.
MDR_TEST(net_client_reports_a_plausible_latency_on_loopback) {
    ServerFixture fixture;
    SizedFrameSource dsFrame(256, 192);
    std::vector<uint8_t> realFrame(static_cast<size_t>(256) * 192 * 4, 0x33);
    dsFrame.setFrame(realFrame);
    fixture.server.setTarget(fixture.sinkA, dsFrame, HostMode::Emulation, SystemIdentity{"nds", "Nintendo DS"},
                              AdapterIdentity{"melonds", "melonDS", "1.1"});

    NetClient client(fixture.clientConfig());
    MDR_CHECK(client.connect());

    std::vector<uint8_t> received;
    MDR_CHECK(waitUntil([&] { return client.getLatestFrame(received); }));
    // Generous upper bound (1 second) -- this is a same-process loopback
    // connection, real latency should be microseconds to low
    // milliseconds; a value anywhere near this ceiling would mean the
    // offset estimate itself is badly wrong, not just noisy.
    MDR_CHECK(client.lastLatencyMicros() < 1'000'000);

    client.disconnect();
}

// Real user report, 2026-08-03: "when exiting an emulator...the last
// frame that was sent to the client persists on screen until I change
// client and reinitialize the connection." Root cause: NetClient's
// hasFrame_ was set true the first time a frame was ever decoded and
// never reset, so getLatestFrame() kept handing back the same stale
// buffer forever, even long after the mode that produced it had changed.
// See net_client.cpp's ModeChanged handling for the fix.
MDR_TEST(net_client_clears_stale_frame_on_mode_change) {
    ServerFixture fixture;
    SizedFrameSource dsFrame(256, 192);
    std::vector<uint8_t> realFrame(static_cast<size_t>(256) * 192 * 4, 0x5A);
    dsFrame.setFrame(realFrame);
    fixture.server.setTarget(fixture.sinkA, dsFrame, HostMode::Emulation, SystemIdentity{"nds", "Nintendo DS"},
                              AdapterIdentity{"melonds", "melonDS", "1.0"});

    NetClient client(fixture.clientConfig());
    MDR_CHECK(client.connect());

    std::vector<uint8_t> received;
    MDR_CHECK(waitUntil([&] { return client.getLatestFrame(received); }));

    // The adapter disconnects (e.g. the emulator exits) -- the host falls
    // back to HostControl with nothing to stream. Before the fix,
    // getLatestFrame() would keep returning the frame captured above
    // forever, regardless of this mode change.
    fixture.server.setTarget(fixture.sinkB, fixture.frameB, HostMode::HostControl, SystemIdentity{"host", "Host Menu"},
                              AdapterIdentity{"host-control", "Host Control", ""});
    MDR_CHECK(waitUntil([&] { return client.hostMode() == HostMode::HostControl; }));
    MDR_CHECK(waitUntil([&] { return !client.getLatestFrame(received); }));

    client.disconnect();
}

#ifdef DUALDECK_HAVE_OPENH264
// Latency-audit follow-up: real end-to-end proof that videoReceiveLoop()'s
// decoder-drain fix (see its own comment) actually works against this
// project's real encoder/decoder pair over a real socket, not just in
// isolation (test_h264_decoder.cpp already covers the decoder alone).
// OpenH264's documented no-delay-decode semantics can defer a frame's
// output to a follow-up call fed no new data -- a real, reproducible
// behavior with this build's OpenH264, not hypothetical (see
// test_h264_decoder.cpp's own tests, which already have to handle it
// explicitly). SizedFrameSource below never changes its frame content or
// index, and NetServer::videoLoop() deliberately skips re-sending an
// unchanged frame index (see its own comment) -- so exactly one
// VideoFrame packet is ever sent for this whole connection. Without the
// drain fix, a deferred first frame would mean this test's waitUntil()
// times out completely (there is no second packet coming to flush it via
// fresh data), not just arrives late -- a strong, non-flaky regression
// guard rather than a timing-dependent one.
MDR_TEST(net_client_receives_an_h264_video_frame_without_needing_a_second_frame) {
    ServerFixture fixture;
    SizedFrameSource dsFrame(256, 192);
    std::vector<uint8_t> realFrame(static_cast<size_t>(256) * 192 * 4, 0x5A);
    dsFrame.setFrame(realFrame);
    fixture.server.setTarget(fixture.sinkA, dsFrame, HostMode::Emulation, SystemIdentity{"nds", "Nintendo DS"},
                              AdapterIdentity{"melonds", "melonDS", "1.0"});

    NetClientConfig clientConfig = fixture.clientConfig();
    clientConfig.preferH264 = true;
    NetClient client(clientConfig);
    MDR_CHECK(client.connect());
    // Confirms the negotiation actually landed on H264 -- if it hadn't,
    // the wait below would still pass (JPEG never defers output at all)
    // without proving anything about the drain fix.
    MDR_CHECK(client.negotiatedVideoCodec() == VideoCodec::H264);

    std::vector<uint8_t> received;
    MDR_CHECK(waitUntil([&] {
        if (!client.getLatestFrame(received)) return false;
        return received.size() == realFrame.size();
    }));
    MDR_CHECK(client.isConnected());

    client.disconnect();
}
#endif // DUALDECK_HAVE_OPENH264

MDR_TEST(net_client_detects_a_dead_server_connection) {
    ServerFixture fixture;
    NetClient client(fixture.clientConfig());
    MDR_CHECK(client.connect());
    MDR_CHECK(client.isConnected());

    // No graceful Disconnect packet -- server.stop() just tears the
    // listener/connections down, matching an unplugged host or a crash
    // more than a clean shutdown. Before this phase, nothing read the
    // control socket at all, so a control-channel-only failure (as
    // opposed to a video-channel failure, which videoReceiveLoop() has
    // always caught) would never have been noticed.
    fixture.server.stop();

    MDR_CHECK(waitUntil([&] { return !client.isConnected(); }, 5000));

    client.disconnect();
}

// Real user report, 2026-08-01: a host reachable at the TCP level but
// that never actually replies to Hello -- exactly what a firewalled
// port with a silent DROP rule looks like from the client's side once
// the raw TCP handshake happens to complete anyway (e.g. a listening
// backlog accepting the connection at the kernel level before an
// application-level firewall rule blocks the subsequent data) -- used to
// make NetClient::connect() block forever inside a blocking recvExact(),
// and since connect() holds connectMutex_ for its entire body, even
// disconnect() from another thread couldn't unblock it: the whole client
// appeared to hang with no way out but killing it externally (reported
// as "hangs on connecting to host, and now hangs when trying to change
// host or exit"). See net_client.cpp's kHandshakeTimeoutMs/
// connectWithTimeout()/setRecvTimeoutMs() for the fix. This test
// simulates the "TCP connects, then silence" half of that scenario (the
// half practical to reproduce hermetically); the raw-connect()-level
// half (a SYN that's silently dropped rather than accepted) isn't
// simulated the same way here since it needs an actual unresponsive
// route, not just a cooperating local listener.
MDR_TEST(net_client_connect_times_out_when_host_never_replies) {
    int listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    MDR_CHECK(listenFd >= 0);
    int reuse = 1;
    ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    MDR_CHECK(::bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    ::getsockname(listenFd, reinterpret_cast<sockaddr*>(&addr), &len);
    uint16_t port = ntohs(addr.sin_port);
    MDR_CHECK(::listen(listenFd, 1) == 0);

    // Accepts the connection (so the client's raw TCP connect() succeeds
    // quickly, exercising the recvExact()-timeout half of the fix
    // specifically) but never reads or writes anything -- Hello arrives
    // and just sits unread in the kernel's receive buffer, and no
    // HelloAck is ever sent back.
    int acceptedFd = -1;
    std::thread acceptThread([&] { acceptedFd = ::accept(listenFd, nullptr, nullptr); });

    NetClientConfig clientConfig;
    clientConfig.hostAddress = "127.0.0.1";
    clientConfig.controlPort = port;
    clientConfig.inputPort = freePort();
    clientConfig.videoPort = freePort();
    clientConfig.authToken = "test-token";
    NetClient client(clientConfig);

    auto start = std::chrono::steady_clock::now();
    bool connected = client.connect();
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - start)
                         .count();

    acceptThread.join();

    MDR_CHECK(!connected);
    // Before the fix, this line would never be reached at all. 7000ms is
    // a generous margin over kHandshakeTimeoutMs's 5000ms bound
    // (net_client.cpp) -- proving this returns promptly, not asserting a
    // tight timing budget.
    MDR_CHECK(elapsedMs < 7000);

    if (acceptedFd >= 0) ::close(acceptedFd);
    ::close(listenFd);
}

// Real end-to-end proof of the client log-forwarding feature
// (client/src/client_log.h): a real NetClient::sendClientLog() call
// reaches a real NetServer over the real control-channel socket and gets
// parsed back into the exact same line -- not just that
// serializeClientLogPayload()/parseClientLogPayload() round-trip in
// isolation (see protocol/tests/test_client_log.cpp for that). Doesn't
// reuse ServerFixture above since none of its other tests need
// onClientLogLine wired up.
MDR_TEST(net_client_forwards_a_log_line_to_the_host) {
    std::mutex receivedMutex;
    std::vector<std::pair<std::string, std::string>> received;

    LoggingInputSink sink;
    NullFrameSource frame;
    LoggingMicAudioSink micSink;
    NetServerConfig config;
    config.bindAddress = "127.0.0.1";
    config.controlPort = freePort();
    config.inputPort = freePort();
    config.videoPort = freePort();
    config.discoveryEnabled = false;
    config.micSupported = false;
    config.authToken = "test-token";
    config.onClientLogLine = [&](const std::string& address, const std::string& line) {
        std::lock_guard<std::mutex> lock(receivedMutex);
        received.emplace_back(address, line);
    };
    NetServer server(config, sink, frame, micSink);
    server.start();

    NetClientConfig clientConfig;
    clientConfig.hostAddress = "127.0.0.1";
    clientConfig.controlPort = config.controlPort;
    clientConfig.inputPort = config.inputPort;
    clientConfig.videoPort = config.videoPort;
    clientConfig.authToken = config.authToken;
    clientConfig.heartbeatIntervalMs = 200;
    NetClient client(clientConfig);
    MDR_CHECK(client.connect());

    client.sendClientLog("hello from a test client\n");

    MDR_CHECK(waitUntil([&] {
        std::lock_guard<std::mutex> lock(receivedMutex);
        return !received.empty();
    }));
    {
        std::lock_guard<std::mutex> lock(receivedMutex);
        MDR_CHECK(received.size() == 1);
        MDR_CHECK(received[0].first == "127.0.0.1");
        MDR_CHECK(received[0].second == "hello from a test client\n");
    }

    client.disconnect();
    server.stop();
}

// A line enqueued before connect() (or after disconnect()) is silently
// dropped, per sendClientLog()'s own documented best-effort contract --
// there is no connection to forward it over, and it must never be
// queued up to send "whenever a connection eventually exists."
MDR_TEST(net_client_drops_a_log_line_when_not_connected) {
    NetClient client(NetClientConfig{});
    client.sendClientLog("this should be dropped, not queued\n");
    // No observable effect to assert beyond "this doesn't crash or
    // block" -- there is no server to receive anything from, by design.
}
