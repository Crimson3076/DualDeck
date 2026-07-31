// Tests for GitHub issue #4 Phase D: computeDesiredMode()'s pure
// decision logic, plus a real end-to-end test of ModeCoordinator itself
// -- a real AdapterIpcServer, a real SyntheticEmulatorAdapter connected
// to it over a real Unix-domain-socket AdapterIpcClient (not mocks of
// either side, matching adapter-sdk's own established e2e test style),
// and a real NetServer whose currentMode() is observed directly (no
// need for a real client connection just to read it).

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

#include "host/adapter_bridge.h"
#include "host/logging_input_sink.h"
#include "host/logging_mic_audio_sink.h"
#include "host/mode_coordinator.h"
#include "host/net_server.h"
#include "melonds_remote/adapter/ipc/adapter_ipc_client.h"
#include "melonds_remote/adapter/ipc/adapter_ipc_server.h"
#include "melonds_remote/adapter/synthetic/synthetic_emulator_adapter.h"
#include "melonds_remote/protocol.h"
#include "test_framework.h"

using namespace melonds_remote;
using namespace melonds_remote::host;
using namespace melonds_remote::adapter::ipc;
using namespace melonds_remote::adapter::synthetic;

namespace {

bool waitUntil(const std::function<bool()>& predicate, int timeoutMs = 3000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

std::string uniqueSocketPath() {
    static std::atomic<int> counter{0};
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return "/tmp/melonds-remote-mode-coordinator-test-" + std::to_string(::getpid()) + "-" +
           std::to_string(counter++) + "-" + std::to_string(now) + "/adapter.sock";
}

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

bool sendAllRaw(int fd, const uint8_t* data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        ssize_t n = ::send(fd, data + sent, size - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool recvExactRaw(int fd, uint8_t* data, size_t size) {
    size_t got = 0;
    while (got < size) {
        ssize_t n = ::recv(fd, data + got, size - got, 0);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

// Real Hello/HelloAck handshake against a real control port -- used to
// confirm what NetServer actually reports to a connecting client, not
// just what some internal object happens to hold.
bool handshakeAndGetAck(uint16_t controlPort, HelloAckPayload& outAck) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(controlPort);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;

    HelloPayload hello;
    hello.clientName = "test-client";
    hello.clientPlatform = "linux";
    ByteBuffer packet = buildHelloPacket(hello);
    if (!sendAllRaw(fd, packet.data(), packet.size())) return false;

    uint8_t headerBuf[kPacketHeaderWireSize];
    if (!recvExactRaw(fd, headerBuf, sizeof(headerBuf))) return false;
    auto header = parseHeader(headerBuf, sizeof(headerBuf));
    if (!header || header->type != PacketType::HelloAck) return false;

    ByteBuffer body(header->payloadSize);
    if (!body.empty() && !recvExactRaw(fd, body.data(), body.size())) return false;
    auto parsed = parseHelloAckPayload(body.data(), body.size());
    ::close(fd);
    if (!parsed) return false;
    outAck = *parsed;
    return true;
}

// Bundles everything a ModeCoordinator test needs: a real AdapterIpcServer
// (the emulation-side target's data source), a fake host-control target
// (LoggingInputSink -- HostControlAdapter itself needs real uinput
// access this sandbox doesn't have, and ModeCoordinator is generic over
// "some IEmulatorInputSink/IFrameSource pair" regardless of which
// concrete class it is), and a real NetServer + ModeCoordinator wired
// together. Torn down in reverse construction order automatically.
struct Fixture {
    LoggingMicAudioSink micSink;
    LoggingInputSink hostControlInputSink;
    class NullFrameSource : public IFrameSource {
    public:
        bool getLatestFrame(std::vector<uint8_t>&, uint64_t&, uint16_t&, uint16_t&) override { return false; }
    } hostControlFrameSource;

    std::string socketPath = uniqueSocketPath();
    AdapterIpcServer adapterServer{socketPath};
    AdapterBridge bridge{adapterServer};

    NetServerConfig config;
    NetServer server;
    ModeCoordinator coordinator;

    Fixture()
        : config([] {
              NetServerConfig cfg;
              cfg.bindAddress = "127.0.0.1";
              cfg.controlPort = freePort();
              cfg.inputPort = freePort();
              cfg.videoPort = freePort();
              cfg.discoveryEnabled = false;
              cfg.micSupported = false;
              cfg.authToken = "test-token";
              return cfg;
          }()),
          server(config, hostControlInputSink, hostControlFrameSource, micSink),
          coordinator(server, adapterServer, bridge, bridge, hostControlInputSink, hostControlFrameSource,
                      /*systemIdentityExplicit=*/false, /*adapterIdentityExplicit=*/false,
                      SystemIdentity{}, AdapterIdentity{}) {
        MDR_CHECK(adapterServer.start());
        server.start();
        coordinator.start();
    }

    ~Fixture() {
        coordinator.stop();
        server.stop();
        adapterServer.stop();
    }
};

} // namespace

MDR_TEST(compute_desired_mode_no_adapter_no_override_is_host_control) {
    MDR_CHECK(computeDesiredMode(/*adapterConnected=*/false, /*manualHostControlOverride=*/false) ==
              HostMode::HostControl);
}

MDR_TEST(compute_desired_mode_adapter_connected_no_override_is_emulation) {
    MDR_CHECK(computeDesiredMode(/*adapterConnected=*/true, /*manualHostControlOverride=*/false) ==
              HostMode::Emulation);
}

MDR_TEST(compute_desired_mode_override_wins_even_with_adapter_connected) {
    MDR_CHECK(computeDesiredMode(/*adapterConnected=*/true, /*manualHostControlOverride=*/true) ==
              HostMode::HostControl);
}

MDR_TEST(compute_desired_mode_override_with_no_adapter_is_still_host_control) {
    MDR_CHECK(computeDesiredMode(/*adapterConnected=*/false, /*manualHostControlOverride=*/true) ==
              HostMode::HostControl);
}

MDR_TEST(compute_desired_host_session_state_no_adapter_no_override_is_connected) {
    MDR_CHECK(computeDesiredHostSessionState(/*adapterConnected=*/false, melonds_remote::adapter::SessionState::Error,
                                              /*manualHostControlOverride=*/false) == HostSessionState::Connected);
}

MDR_TEST(compute_desired_host_session_state_override_wins_even_with_adapter_connected) {
    MDR_CHECK(computeDesiredHostSessionState(/*adapterConnected=*/true,
                                              melonds_remote::adapter::SessionState::Running,
                                              /*manualHostControlOverride=*/true) ==
              HostSessionState::CompanionModeActive);
}

MDR_TEST(compute_desired_host_session_state_ignores_stale_adapter_state_once_disconnected) {
    // AdapterIpcServer deliberately leaves the last-known SessionState in
    // place across a disconnect (see adapter_ipc_server.cpp) -- a stale
    // Running here must not leak into the result once adapterConnected
    // is false.
    MDR_CHECK(computeDesiredHostSessionState(/*adapterConnected=*/false,
                                              melonds_remote::adapter::SessionState::Running,
                                              /*manualHostControlOverride=*/false) == HostSessionState::Connected);
}

MDR_TEST(compute_desired_host_session_state_maps_every_adapter_session_state) {
    using melonds_remote::adapter::SessionState;
    MDR_CHECK(computeDesiredHostSessionState(true, SessionState::Available, false) == HostSessionState::Connected);
    MDR_CHECK(computeDesiredHostSessionState(true, SessionState::Starting, false) == HostSessionState::Launching);
    MDR_CHECK(computeDesiredHostSessionState(true, SessionState::Running, false) ==
              HostSessionState::EmulatorRunning);
    MDR_CHECK(computeDesiredHostSessionState(true, SessionState::Paused, false) ==
              HostSessionState::EmulatorRunning);
    MDR_CHECK(computeDesiredHostSessionState(true, SessionState::Stopped, false) == HostSessionState::Connected);
    MDR_CHECK(computeDesiredHostSessionState(true, SessionState::Error, false) == HostSessionState::Error);
}

MDR_TEST(coordinator_starts_in_host_control_mode) {
    Fixture fixture;
    MDR_CHECK(fixture.server.currentMode() == HostMode::HostControl);
    MDR_CHECK(!fixture.coordinator.isOverridden());
}

MDR_TEST(coordinator_switches_to_emulation_when_an_adapter_connects) {
    Fixture fixture;
    MDR_CHECK(fixture.server.currentMode() == HostMode::HostControl);

    SyntheticEmulatorAdapter syntheticAdapter;
    AdapterIpcClient client(syntheticAdapter, fixture.socketPath);
    MDR_CHECK(client.connect());

    MDR_CHECK(waitUntil([&] { return fixture.server.currentMode() == HostMode::Emulation; }));

    client.disconnect();
}

MDR_TEST(coordinator_switches_back_to_host_control_when_the_adapter_disconnects) {
    Fixture fixture;

    SyntheticEmulatorAdapter syntheticAdapter;
    AdapterIpcClient client(syntheticAdapter, fixture.socketPath);
    MDR_CHECK(client.connect());
    MDR_CHECK(waitUntil([&] { return fixture.server.currentMode() == HostMode::Emulation; }));

    client.disconnect();
    MDR_CHECK(waitUntil([&] { return fixture.server.currentMode() == HostMode::HostControl; }));
}

MDR_TEST(coordinator_manual_override_forces_host_control_even_while_adapter_connected) {
    Fixture fixture;

    SyntheticEmulatorAdapter syntheticAdapter;
    AdapterIpcClient client(syntheticAdapter, fixture.socketPath);
    MDR_CHECK(client.connect());
    MDR_CHECK(waitUntil([&] { return fixture.server.currentMode() == HostMode::Emulation; }));

    fixture.coordinator.forceHostControl();
    MDR_CHECK(fixture.coordinator.isOverridden());
    MDR_CHECK(waitUntil([&] { return fixture.server.currentMode() == HostMode::HostControl; }));

    // The adapter is still connected the whole time -- confirms this is
    // a real override, not just "no adapter" being (mis)detected.
    MDR_CHECK(fixture.adapterServer.hasConnectedAdapter());

    client.disconnect();
}

MDR_TEST(coordinator_clearing_override_resumes_auto_detection) {
    Fixture fixture;

    SyntheticEmulatorAdapter syntheticAdapter;
    AdapterIpcClient client(syntheticAdapter, fixture.socketPath);
    MDR_CHECK(client.connect());
    MDR_CHECK(waitUntil([&] { return fixture.server.currentMode() == HostMode::Emulation; }));

    fixture.coordinator.forceHostControl();
    MDR_CHECK(waitUntil([&] { return fixture.server.currentMode() == HostMode::HostControl; }));

    fixture.coordinator.clearOverride();
    MDR_CHECK(!fixture.coordinator.isOverridden());
    // Adapter is still connected -- clearing the override should swap
    // straight back to Emulation without needing a reconnect.
    MDR_CHECK(waitUntil([&] { return fixture.server.currentMode() == HostMode::Emulation; }));

    client.disconnect();
}

MDR_TEST(fresh_handshake_reports_host_control_identity_before_any_adapter_connects) {
    Fixture fixture;
    HelloAckPayload ack;
    MDR_CHECK(handshakeAndGetAck(fixture.config.controlPort, ack));
    MDR_CHECK(ack.system.systemId == kHostControlSystemIdentity.systemId);
    MDR_CHECK(ack.adapter.adapterId == kHostControlAdapterIdentity.adapterId);
    MDR_CHECK(ack.mode == HostMode::HostControl);
}

MDR_TEST(fresh_handshake_reports_the_connected_adapters_own_identity_in_emulation_mode) {
    Fixture fixture;

    SyntheticEmulatorAdapter syntheticAdapter;
    AdapterIpcClient client(syntheticAdapter, fixture.socketPath);
    MDR_CHECK(client.connect());
    MDR_CHECK(waitUntil([&] { return fixture.server.currentMode() == HostMode::Emulation; }));

    HelloAckPayload ack;
    MDR_CHECK(handshakeAndGetAck(fixture.config.controlPort, ack));
    MDR_CHECK(ack.system.systemId == "synthetic");
    MDR_CHECK(ack.adapter.adapterId == "synthetic-ipc");
    MDR_CHECK(ack.mode == HostMode::Emulation);

    client.disconnect();
}

MDR_TEST(coordinator_current_session_state_reflects_a_real_connected_adapter) {
    // SyntheticEmulatorAdapter always reports SessionState::Running once
    // connected (see its own header comment) -- confirms
    // ModeCoordinator::currentSessionState() actually reads through the
    // real AdapterIpcServer::currentState() proxy end to end, not just
    // that the pure computeDesiredHostSessionState() function is correct
    // in isolation (already covered above).
    Fixture fixture;
    MDR_CHECK(fixture.coordinator.currentSessionState() == HostSessionState::Connected);

    SyntheticEmulatorAdapter syntheticAdapter;
    AdapterIpcClient client(syntheticAdapter, fixture.socketPath);
    MDR_CHECK(client.connect());
    MDR_CHECK(waitUntil(
        [&] { return fixture.coordinator.currentSessionState() == HostSessionState::EmulatorRunning; }));

    client.disconnect();
    MDR_CHECK(waitUntil([&] { return fixture.coordinator.currentSessionState() == HostSessionState::Connected; }));
}
