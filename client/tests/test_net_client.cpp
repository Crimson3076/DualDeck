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
    bool getLatestFrame(std::vector<uint8_t>&, uint64_t&) override { return false; }
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
