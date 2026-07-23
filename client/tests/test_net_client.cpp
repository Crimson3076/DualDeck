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
#include <thread>

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
