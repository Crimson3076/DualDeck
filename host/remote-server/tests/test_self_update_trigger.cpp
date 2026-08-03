// Real end-to-end tests of the client-triggers-host-update feature (real
// user request, 2026-08-03: "if the client connects to the host and the
// host is on an older version, it should try to trigger an update if
// possible" -- scoped, per the user's own explicit choice via
// AskUserQuestion, to devices already in the host's approved-devices set;
// see NetServerConfig::selfUpdateCommand's comment in net_server.h and
// the handleHelloLocked() version-mismatch block in net_server.cpp for
// the full design). Uses a real NetServer on real loopback sockets and a
// real DeviceApprovalManager backed by a real temp state file -- not
// mocks of either -- plus a real (harmless) shell command as the
// "self-update" trigger, observed via a marker file it touches, since
// runSelfUpdateCommand() is deliberately fire-and-forget with no
// programmatic completion signal.
//
// Deliberately duplicates the small raw-socket Hello/HelloAck helpers
// from test_net_server_mode_switch.cpp's anonymous-namespace TestClient
// rather than sharing them -- that struct is intentionally file-local,
// and this file additionally needs to inspect a *rejected* handshake's
// HelloAck (TestClient::connectAndHandshake() already stores it before
// bailing out on !accepted, but exposing that cleanly plus a
// per-connection appVersion override was judged not worth coupling the
// two files over; see net_server.cpp's own recvMessage() duplication
// comment for this project's established precedent on this tradeoff).

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>

#include "host/device_approval_manager.h"
#include "host/logging_input_sink.h"
#include "host/logging_mic_audio_sink.h"
#include "host/net_server.h"
#include "melonds_remote/protocol.h"
#include "test_framework.h"

using namespace melonds_remote;
using namespace melonds_remote::host;

namespace {

class FakeFrameSource : public IFrameSource {
public:
    bool getLatestFrame(std::vector<uint8_t>&, uint64_t&, uint16_t&, uint16_t&) override { return false; }
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

bool waitUntil(const std::function<bool()>& predicate, int timeoutMs = 3000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
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

// Connects, sends a real Hello with the given deviceId/appVersion, and
// returns the parsed HelloAck regardless of accept/reject (unlike
// test_net_server_mode_switch.cpp's TestClient, which bails out on
// !accepted -- this file's whole point is inspecting rejections).
bool handshakeAndGetAck(uint16_t controlPort, const std::string& deviceId, const std::string& appVersion,
                         HelloAckPayload& outAck) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(controlPort);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return false;
    }

    HelloPayload hello;
    hello.clientName = "test-client";
    hello.clientPlatform = "linux";
    hello.displayWidth = 1280;
    hello.displayHeight = 800;
    hello.authToken = deviceId;
    hello.appVersion = appVersion;
    ByteBuffer packet = buildHelloPacket(hello);
    bool ok = sendAllRaw(fd, packet.data(), packet.size());

    uint8_t headerBuf[kPacketHeaderWireSize];
    ok = ok && recvExactRaw(fd, headerBuf, sizeof(headerBuf));
    if (!ok) {
        ::close(fd);
        return false;
    }
    auto header = parseHeader(headerBuf, sizeof(headerBuf));
    if (!header || header->type != PacketType::HelloAck) {
        ::close(fd);
        return false;
    }
    ByteBuffer body(header->payloadSize);
    if (!body.empty() && !recvExactRaw(fd, body.data(), body.size())) {
        ::close(fd);
        return false;
    }
    auto parsed = parseHelloAckPayload(body.data(), body.size());
    ::close(fd);
    if (!parsed) return false;
    outAck = *parsed;
    return true;
}

// Unique-per-call temp path under the OS temp dir -- no two tests in this
// file share a marker or state file, so a leftover from one can't make
// another spuriously pass.
std::string uniqueTempPath(const std::string& label) {
    static std::atomic<int> counter{0};
    std::string path = "/tmp/dualdeck_self_update_test_" + label + "_" +
                        std::to_string(::getpid()) + "_" + std::to_string(counter++);
    ::unlink(path.c_str());
    return path;
}

struct TempFileCleanup {
    std::string path;
    ~TempFileCleanup() { ::unlink(path.c_str()); }
};

} // namespace

// DeviceApprovalManager::isApproved() (device_approval_manager.h/.cpp) --
// the new read-only lookup this feature relies on, verified independent
// of any NetServer/socket plumbing.
MDR_TEST(is_approved_is_false_for_an_unknown_device) {
    std::string statePath = uniqueTempPath("dam_unknown");
    TempFileCleanup cleanup{statePath};
    DeviceApprovalManager manager(statePath);
    MDR_CHECK(!manager.isApproved("some-device-id"));
}

MDR_TEST(is_approved_is_false_for_an_empty_device_id) {
    std::string statePath = uniqueTempPath("dam_empty");
    TempFileCleanup cleanup{statePath};
    DeviceApprovalManager manager(statePath);
    MDR_CHECK(!manager.isApproved(""));
}

MDR_TEST(is_approved_is_true_after_a_real_approve) {
    std::string statePath = uniqueTempPath("dam_approved");
    TempFileCleanup cleanup{statePath};
    DeviceApprovalManager manager(statePath);
    manager.check("device-123", "test-client", "127.0.0.1"); // records pending
    MDR_CHECK(!manager.isApproved("device-123"));
    MDR_CHECK(manager.approve("device-123"));
    MDR_CHECK(manager.isApproved("device-123"));
}

// Real bug this guards against: check() has a documented side effect of
// registering/refreshing a pending-approval entry for an unrecognized
// device -- isApproved() must never do that (see its own header comment:
// a client whose app version doesn't even match yet has no business
// cluttering the pending-approval queue). Verified by confirming a
// pending request list stays empty across repeated isApproved() calls.
MDR_TEST(is_approved_never_registers_a_pending_request) {
    std::string statePath = uniqueTempPath("dam_no_pending");
    TempFileCleanup cleanup{statePath};
    DeviceApprovalManager manager(statePath);
    manager.isApproved("never-seen-device");
    manager.isApproved("never-seen-device");
    MDR_CHECK(manager.pendingRequests().empty());
}

// End-to-end: an already-approved device presenting a mismatched
// appVersion gets AppVersionMismatchUpdateTriggered (not plain
// AppVersionMismatch), and the configured selfUpdateCommand actually
// runs -- observed via a marker file, since runSelfUpdateCommand() is
// intentionally fire-and-forget with no other completion signal.
MDR_TEST(approved_device_with_version_mismatch_triggers_self_update) {
    std::string statePath = uniqueTempPath("e2e_state");
    TempFileCleanup cleanupState{statePath};
    std::string markerPath = uniqueTempPath("e2e_marker");
    TempFileCleanup cleanupMarker{markerPath};

    // Pre-approve "device-abc" via a standalone manager pointed at the
    // same state file NetServer will load from at construction.
    {
        DeviceApprovalManager preApproval(statePath);
        preApproval.check("device-abc", "test-client", "127.0.0.1");
        MDR_CHECK(preApproval.approve("device-abc"));
    }

    LoggingInputSink sink;
    FakeFrameSource frameSource;
    LoggingMicAudioSink micSink;
    NetServerConfig config;
    config.bindAddress = "127.0.0.1";
    config.controlPort = freePort();
    config.inputPort = freePort();
    config.videoPort = freePort();
    config.discoveryEnabled = false;
    config.micSupported = false;
    config.authToken = ""; // device-approval mode, required for this feature
    config.approvalStateFilePath = statePath;
    config.appVersion = "v-host";
    config.selfUpdateCommand = "touch " + markerPath;

    NetServer server(config, sink, frameSource, micSink);
    server.start();
    MDR_CHECK(server.isRunning());

    HelloAckPayload ack;
    MDR_CHECK(handshakeAndGetAck(config.controlPort, "device-abc", "v-client-old", ack));
    MDR_CHECK(!ack.accepted);
    MDR_CHECK(ack.rejectReason == HelloRejectReason::AppVersionMismatchUpdateTriggered);

    MDR_CHECK(waitUntil([&] {
        FILE* f = std::fopen(markerPath.c_str(), "r");
        if (!f) return false;
        std::fclose(f);
        return true;
    }));

    server.stop();
}

// An *unapproved* device must never trigger the update, and must see the
// plain AppVersionMismatch reason -- this is the security-relevant
// boundary the user explicitly chose (AskUserQuestion: "Auto-trigger for
// already-approved devices"), not "any connecting client."
MDR_TEST(unapproved_device_with_version_mismatch_does_not_trigger_self_update) {
    std::string statePath = uniqueTempPath("e2e_state_unapproved");
    TempFileCleanup cleanupState{statePath};
    std::string markerPath = uniqueTempPath("e2e_marker_unapproved");
    TempFileCleanup cleanupMarker{markerPath};

    LoggingInputSink sink;
    FakeFrameSource frameSource;
    LoggingMicAudioSink micSink;
    NetServerConfig config;
    config.bindAddress = "127.0.0.1";
    config.controlPort = freePort();
    config.inputPort = freePort();
    config.videoPort = freePort();
    config.discoveryEnabled = false;
    config.micSupported = false;
    config.authToken = "";
    config.approvalStateFilePath = statePath;
    config.appVersion = "v-host";
    config.selfUpdateCommand = "touch " + markerPath;

    NetServer server(config, sink, frameSource, micSink);
    server.start();
    MDR_CHECK(server.isRunning());

    HelloAckPayload ack;
    MDR_CHECK(handshakeAndGetAck(config.controlPort, "device-never-approved", "v-client-old", ack));
    MDR_CHECK(!ack.accepted);
    MDR_CHECK(ack.rejectReason == HelloRejectReason::AppVersionMismatch);

    // Give a spurious background command a real chance to appear before
    // declaring victory -- an absent marker after a real wait is a much
    // stronger negative result than checking once immediately.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    FILE* f = std::fopen(markerPath.c_str(), "r");
    MDR_CHECK(f == nullptr);

    server.stop();
}

// The update is only ever launched once per NetServer lifetime
// (selfUpdateTriggered_ is deliberately never reset -- see net_server.h's
// comment: re-running the update command on every retry from a client
// that's still on the old version while the update is already in flight
// would be wasteful/racy). A second Hello from the same approved device
// while already-triggered sees plain AppVersionMismatch, not a second
// AppVersionMismatchUpdateTriggered -- documented behavior, not a bug:
// the client's auto-reconnect loop keeps retrying regardless of which of
// the two rejection reasons it sees.
MDR_TEST(self_update_is_only_triggered_once) {
    std::string statePath = uniqueTempPath("e2e_state_once");
    TempFileCleanup cleanupState{statePath};
    std::string markerPath = uniqueTempPath("e2e_marker_once");
    TempFileCleanup cleanupMarker{markerPath};

    {
        DeviceApprovalManager preApproval(statePath);
        preApproval.check("device-xyz", "test-client", "127.0.0.1");
        MDR_CHECK(preApproval.approve("device-xyz"));
    }

    LoggingInputSink sink;
    FakeFrameSource frameSource;
    LoggingMicAudioSink micSink;
    NetServerConfig config;
    config.bindAddress = "127.0.0.1";
    config.controlPort = freePort();
    config.inputPort = freePort();
    config.videoPort = freePort();
    config.discoveryEnabled = false;
    config.micSupported = false;
    config.authToken = "";
    config.approvalStateFilePath = statePath;
    config.appVersion = "v-host";
    config.selfUpdateCommand = "touch " + markerPath;

    NetServer server(config, sink, frameSource, micSink);
    server.start();
    MDR_CHECK(server.isRunning());

    HelloAckPayload firstAck;
    MDR_CHECK(handshakeAndGetAck(config.controlPort, "device-xyz", "v-client-old", firstAck));
    MDR_CHECK(firstAck.rejectReason == HelloRejectReason::AppVersionMismatchUpdateTriggered);
    MDR_CHECK(waitUntil([&] {
        FILE* f = std::fopen(markerPath.c_str(), "r");
        if (!f) return false;
        std::fclose(f);
        return true;
    }));

    HelloAckPayload secondAck;
    MDR_CHECK(handshakeAndGetAck(config.controlPort, "device-xyz", "v-client-old", secondAck));
    MDR_CHECK(!secondAck.accepted);
    MDR_CHECK(secondAck.rejectReason == HelloRejectReason::AppVersionMismatch);

    server.stop();
}

// Static-auth-token deployments never populate DeviceApprovalManager's
// approved set at all (hello->authToken means "shared secret" there, not
// "persistent device identity") -- selfUpdateCommand must be a no-op in
// that mode even if someone sets it by mistake, never a footgun that
// fires for literally any correctly-token-guessing client.
MDR_TEST(self_update_never_triggers_in_static_auth_token_mode) {
    std::string markerPath = uniqueTempPath("e2e_marker_static_token");
    TempFileCleanup cleanupMarker{markerPath};

    LoggingInputSink sink;
    FakeFrameSource frameSource;
    LoggingMicAudioSink micSink;
    NetServerConfig config;
    config.bindAddress = "127.0.0.1";
    config.controlPort = freePort();
    config.inputPort = freePort();
    config.videoPort = freePort();
    config.discoveryEnabled = false;
    config.micSupported = false;
    config.authToken = "static-secret";
    config.appVersion = "v-host";
    config.selfUpdateCommand = "touch " + markerPath;

    NetServer server(config, sink, frameSource, micSink);
    server.start();
    MDR_CHECK(server.isRunning());

    HelloAckPayload ack;
    MDR_CHECK(handshakeAndGetAck(config.controlPort, "static-secret", "v-client-old", ack));
    MDR_CHECK(!ack.accepted);
    MDR_CHECK(ack.rejectReason == HelloRejectReason::AppVersionMismatch);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    FILE* f = std::fopen(markerPath.c_str(), "r");
    MDR_CHECK(f == nullptr);

    server.stop();
}
