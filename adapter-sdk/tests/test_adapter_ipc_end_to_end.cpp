// Real end-to-end tests of the adapter IPC channel (GitHub issue #28
// Phase 2): an actual AdapterIpcServer listening on an actual Unix
// domain socket, and an actual AdapterIpcClient connecting to it in the
// same test process -- not a mock of either side. Exercises the
// required testing-strategy items from issue #28 that apply to this
// layer: adapter registration/replacement/disconnect, capability
// negotiation (including version mismatch), and input-release paths.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

#include "melonds_remote/adapter/fake/fake_3ds_adapter.h"
#include "melonds_remote/adapter/fake/fake_ds_adapter.h"
#include "melonds_remote/adapter/ipc/adapter_ipc_client.h"
#include "melonds_remote/adapter/ipc/adapter_ipc_server.h"
#include "melonds_remote/adapter/ipc/ipc_protocol.h"
#include "test_framework.h"

using namespace melonds_remote;
using namespace melonds_remote::adapter;
using namespace melonds_remote::adapter::fake;
using namespace melonds_remote::adapter::ipc;

namespace {

// A fresh socket path per call, under /tmp -- deliberately not relying
// on defaultAdapterSocketPath()/$XDG_RUNTIME_DIR so these tests are
// hermetic regardless of what environment they run in (this sandbox,
// CI, a developer machine).
std::string uniqueSocketPath() {
    static std::atomic<int> counter{0};
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return "/tmp/melonds-remote-adapter-ipc-test-" + std::to_string(::getpid()) + "-" +
           std::to_string(counter++) + "-" + std::to_string(now) + "/adapter.sock";
}

// Polls `predicate` until it returns true or `timeoutMs` elapses.
// Returns whether it succeeded -- used instead of a fixed sleep so
// these tests are both fast in the common case and not flaky under
// load.
bool waitUntil(const std::function<bool()>& predicate, int timeoutMs = 2000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

} // namespace

MDR_TEST(ipc_client_connects_and_server_reports_capabilities) {
    std::string path = uniqueSocketPath();
    AdapterIpcServer server(path);
    MDR_CHECK(server.start());
    MDR_CHECK(!server.hasConnectedAdapter());

    FakeDsAdapter ds;
    AdapterIpcClient client(ds, path);
    MDR_CHECK(client.connect());
    MDR_CHECK(client.isConnected());

    MDR_CHECK(waitUntil([&] { return server.hasConnectedAdapter(); }));
    AdapterCapabilities caps = server.capabilities();
    MDR_CHECK(caps.system.systemId == "nds");
    MDR_CHECK(caps.adapter.adapterId == "fake-ds");
    MDR_CHECK_EQ(caps.surfaces.size(), 1u);

    client.disconnect();
    server.stop();
}

MDR_TEST(ipc_input_relayed_from_server_to_adapter) {
    std::string path = uniqueSocketPath();
    AdapterIpcServer server(path);
    MDR_CHECK(server.start());

    FakeDsAdapter ds;
    AdapterIpcClient client(ds, path);
    MDR_CHECK(client.connect());
    MDR_CHECK(waitUntil([&] { return server.hasConnectedAdapter(); }));

    // server.applyGenericInput() is the Host-Service-facing call; it
    // must reach the real, separate FakeDsAdapter object on the other
    // side of the socket, not just update AdapterIpcServer's own state.
    GenericInputState state;
    state.buttons = GenericButton_East;
    state.sequence = 7;
    server.applyGenericInput(state);

    MDR_CHECK(waitUntil([&] { return !ds.isReleased(); }));
    MDR_CHECK_EQ(ds.lastAppliedInput().buttons, static_cast<uint32_t>(GenericButton_East));
    MDR_CHECK_EQ(ds.lastAppliedInput().sequence, 7u);

    server.releaseAllInputs();
    MDR_CHECK(waitUntil([&] { return ds.isReleased(); }));

    client.disconnect();
    server.stop();
}

MDR_TEST(ipc_frame_relayed_from_adapter_to_server) {
    std::string path = uniqueSocketPath();
    AdapterIpcServer server(path);
    MDR_CHECK(server.start());

    FakeDsAdapter ds;
    AdapterIpcClient client(ds, path);
    MDR_CHECK(client.connect());
    MDR_CHECK(waitUntil([&] { return server.hasConnectedAdapter(); }));

    ds.pushFrame("bottom", {9, 8, 7});

    SurfaceFrame outFrame;
    MDR_CHECK(waitUntil([&] { return server.latestFrame("bottom", outFrame) && !outFrame.pixels.empty(); }));
    MDR_CHECK(outFrame.pixels == std::vector<uint8_t>({9, 8, 7}));

    client.disconnect();
    server.stop();
}

MDR_TEST(ipc_state_changed_relayed_from_adapter_to_server) {
    std::string path = uniqueSocketPath();
    AdapterIpcServer server(path);
    MDR_CHECK(server.start());

    FakeDsAdapter ds;
    AdapterIpcClient client(ds, path);
    MDR_CHECK(client.connect());
    MDR_CHECK(waitUntil([&] { return server.hasConnectedAdapter(); }));

    MDR_CHECK(ds.setState(SessionState::Starting));
    MDR_CHECK(ds.setState(SessionState::Running));

    MDR_CHECK(waitUntil([&] { return server.currentState() == SessionState::Running; }));

    client.disconnect();
    server.stop();
}

MDR_TEST(ipc_disconnect_then_reconnect_replaces_session) {
    std::string path = uniqueSocketPath();
    AdapterIpcServer server(path);
    MDR_CHECK(server.start());

    FakeDsAdapter firstAdapter;
    {
        AdapterIpcClient client(firstAdapter, path);
        MDR_CHECK(client.connect());
        MDR_CHECK(waitUntil([&] { return server.hasConnectedAdapter(); }));
        client.disconnect();
    }
    MDR_CHECK(waitUntil([&] { return !server.hasConnectedAdapter(); }));

    // A second, different adapter registers after the first one left --
    // this is the "replacement" behavior from GitHub issue #28's
    // required testing-strategy list, falling out of "one at a time,
    // slot frees on disconnect" rather than needing separate logic (see
    // adapter_ipc_server.h's class comment).
    FakeThreeDsAdapter secondAdapter;
    AdapterIpcClient client2(secondAdapter, path);
    MDR_CHECK(client2.connect());
    MDR_CHECK(waitUntil([&] { return server.hasConnectedAdapter(); }));
    MDR_CHECK(server.capabilities().system.systemId == "3ds");

    client2.disconnect();
    server.stop();
}

MDR_TEST(ipc_second_simultaneous_connection_not_served_until_first_leaves) {
    // AdapterIpcServer serves exactly one connection at a time (see its
    // class comment) -- while serveConnection() is blocked handling
    // client1, a second connection attempt sits in the listen backlog
    // unserved rather than being admitted early. client2.connect()
    // therefore blocks (waiting on a HelloAck that can't arrive yet)
    // for as long as client1 stays connected, so it's run on its own
    // thread here rather than inline -- calling it directly on the test
    // thread would deadlock the test, since nothing would ever run the
    // client1.disconnect() below to free the accept loop up again.
    std::string path = uniqueSocketPath();
    AdapterIpcServer server(path);
    MDR_CHECK(server.start());

    FakeDsAdapter first;
    AdapterIpcClient client1(first, path);
    MDR_CHECK(client1.connect());
    MDR_CHECK(waitUntil([&] { return server.hasConnectedAdapter(); }));

    FakeThreeDsAdapter second;
    AdapterIpcClient client2(second, path);
    std::atomic<bool> client2Connected{false};
    std::atomic<bool> client2Done{false};
    std::thread client2Thread([&] {
        client2Connected = client2.connect();
        client2Done = true;
    });

    // While client1 is still connected, the server must keep reporting
    // it -- client2's still-pending connection attempt must not have
    // silently taken over.
    MDR_CHECK(server.capabilities().system.systemId == "nds");
    MDR_CHECK(!client2Done.load());

    client1.disconnect();

    // Now that client1 left, the accept loop is free to reach client2's
    // backlogged connection -- "replacement" (GitHub issue #28's
    // required testing-strategy list) completing once the slot frees.
    MDR_CHECK(waitUntil([&] { return client2Done.load(); }, 8000));
    MDR_CHECK(client2Connected.load());

    client2Thread.join();
    client2.disconnect();
    server.stop();
}

MDR_TEST(ipc_malformed_hello_rejected_without_crashing_server) {
    std::string path = uniqueSocketPath();
    AdapterIpcServer server(path);
    MDR_CHECK(server.start());

    // A raw socket sending garbage instead of a real Hello message --
    // the server must reject it and keep running, not crash (GitHub
    // issue #28: "A malformed or incompatible adapter must not crash
    // the Host Service").
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    MDR_CHECK(fd >= 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    MDR_CHECK(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

    const uint8_t garbage[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    MDR_CHECK(::send(fd, garbage, sizeof(garbage), 0) == static_cast<ssize_t>(sizeof(garbage)));
    ::close(fd);

    // Server must still be usable afterward -- prove it by having a
    // real, well-formed adapter connect successfully right after.
    FakeDsAdapter ds;
    AdapterIpcClient client(ds, path);
    MDR_CHECK(waitUntil([&] { return client.connect(); }));
    MDR_CHECK(waitUntil([&] { return server.hasConnectedAdapter(); }));

    client.disconnect();
    server.stop();
}

MDR_TEST(ipc_contract_version_mismatch_rejected) {
    std::string path = uniqueSocketPath();
    AdapterIpcServer server(path);
    MDR_CHECK(server.start());

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    MDR_CHECK(fd >= 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    MDR_CHECK(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

    // Hand-craft a Hello with a well-formed payload but a wrong
    // contractVersion in the header -- must be rejected the same way a
    // wrong kProtocolVersion is rejected on the client<->host channel.
    ByteBuffer payload;
    serializeAdapterCapabilities(payload, FakeDsAdapter{}.capabilities());
    IpcHeader header;
    header.type = IpcMessageType::Hello;
    header.contractVersion = static_cast<uint16_t>(kAdapterContractVersion + 1);
    header.payloadSize = static_cast<uint32_t>(payload.size());
    ByteBuffer packet;
    serializeIpcHeader(packet, header);
    packet.insert(packet.end(), payload.begin(), payload.end());
    MDR_CHECK(::send(fd, packet.data(), packet.size(), 0) == static_cast<ssize_t>(packet.size()));

    uint8_t ackHeaderBuf[kIpcHeaderWireSize];
    ssize_t n = ::recv(fd, ackHeaderBuf, sizeof(ackHeaderBuf), MSG_WAITALL);
    MDR_CHECK_EQ(n, static_cast<ssize_t>(sizeof(ackHeaderBuf)));
    auto ackHeader = parseIpcHeader(ackHeaderBuf, sizeof(ackHeaderBuf));
    MDR_CHECK(ackHeader.has_value());
    MDR_CHECK(ackHeader->type == IpcMessageType::HelloAck);

    ByteBuffer ackPayload(ackHeader->payloadSize);
    n = ::recv(fd, ackPayload.data(), ackPayload.size(), MSG_WAITALL);
    MDR_CHECK_EQ(n, static_cast<ssize_t>(ackPayload.size()));
    auto ack = parseAdapterHelloAckPayload(ackPayload.data(), ackPayload.size());
    MDR_CHECK(ack.has_value());
    MDR_CHECK_EQ(ack->accepted, 0);
    MDR_CHECK(ack->reason == AdapterHelloAckReason::ContractVersionMismatch);

    ::close(fd);
    server.stop();
}
