// Real end-to-end tests of NetServer::setTarget() (GitHub issue #4 Phase
// B): an actual NetServer listening on real sockets, an actual raw-socket
// test client doing the real Hello/HelloAck handshake and sending real
// UDP ControllerState packets -- not mocks of either side. Exercises the
// three things setTarget() promises: input/frame routing actually moves
// to the new target, the previous target is released (no held button
// carries over), and an already-connected client gets a ModeChanged
// notification (or, with nobody connected, a safe no-op).

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <turbojpeg.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <thread>

#include "host/logging_input_sink.h"
#include "host/logging_mic_audio_sink.h"
#include "host/net_server.h"
#include "melonds_remote/protocol.h"
#include "test_framework.h"

using namespace melonds_remote;
using namespace melonds_remote::host;

namespace {

// Deterministic IFrameSource test double -- fills every pixel with a
// single distinct byte per instance so a captured VideoFrame packet
// unambiguously identifies which source produced it, without needing
// SyntheticFrameSource's own generator thread/timing.
class FakeFrameSource : public IFrameSource {
public:
    explicit FakeFrameSource(uint8_t fillByte) : fillByte_(fillByte) {}

    bool getLatestFrame(std::vector<uint8_t>& outFrame, uint64_t& outFrameIndex,
                        uint16_t& outWidth, uint16_t& outHeight) override {
        outFrame.assign(kFrameSizeBytes, fillByte_);
        outFrameIndex = ++frameIndex_;
        outWidth = static_cast<uint16_t>(kFrameWidth);
        outHeight = static_cast<uint16_t>(kFrameHeight);
        return true;
    }

private:
    uint8_t fillByte_;
    uint64_t frameIndex_ = 0;
};

uint16_t freePort() {
    // Bind to port 0, let the OS assign one, read it back, close --
    // small race window (another process could grab it before NetServer
    // binds) but this is the same approach the rest of this project's
    // test suites use for hermetic port allocation.
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

// Minimal raw-socket test client: does a real Hello/HelloAck handshake,
// then leaves the control/input/video sockets open for the test to drive
// directly (send ControllerState, read a VideoFrame or a ModeChanged
// packet off the control channel) -- deliberately not NetClient itself,
// since NetClient never reads from the control channel post-handshake
// today and this file needs to.
struct TestClient {
    int controlFd = -1;
    uint16_t inputPort = 0;
    int videoFd = -1;
    HelloAckPayload ack;

    bool connectAndHandshake(uint16_t controlPort, uint16_t inputPortIn, uint16_t videoPort,
                              const std::string& authToken) {
        inputPort = inputPortIn;

        controlFd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(controlPort);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (::connect(controlFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;

        HelloPayload hello;
        hello.clientName = "test-client";
        hello.clientPlatform = "linux";
        hello.displayWidth = 1280;
        hello.displayHeight = 800;
        hello.authToken = authToken;
        ByteBuffer packet = buildHelloPacket(hello);
        if (!sendAllRaw(controlFd, packet.data(), packet.size())) return false;

        uint8_t headerBuf[kPacketHeaderWireSize];
        if (!recvExactRaw(controlFd, headerBuf, sizeof(headerBuf))) return false;
        auto header = parseHeader(headerBuf, sizeof(headerBuf));
        if (!header || header->type != PacketType::HelloAck) return false;

        ByteBuffer body(header->payloadSize);
        if (!body.empty() && !recvExactRaw(controlFd, body.data(), body.size())) return false;
        auto parsed = parseHelloAckPayload(body.data(), body.size());
        if (!parsed) return false;
        ack = *parsed;
        if (!ack.accepted) return false;

        videoFd = ::socket(AF_INET, SOCK_STREAM, 0);
        addr.sin_port = htons(videoPort);
        if (::connect(videoFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;

        return true;
    }

    void sendControllerState(uint32_t sequence, uint16_t dsButtons) {
        ControllerState state;
        state.sequence = sequence;
        state.dsButtons = dsButtons;
        ByteBuffer packet = buildControllerStatePacket(state);

        int udpFd = ::socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(inputPort);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        ::sendto(udpFd, packet.data(), packet.size(), 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ::close(udpFd);
    }

    // Reads one VideoFrame packet and decodes its first pixel byte (enough
    // to identify which FakeFrameSource produced it, since each fills
    // every pixel with one distinct, widely-separated byte: 0x11 vs 0x22).
    // The wire payload is an 8-byte host capture timestamp (protocol v10,
    // see protocol.h's kProtocolVersion comment and VideoFramePayload)
    // followed by JPEG-compressed image bytes (protocol v8) rather than
    // the raw fill byte itself, so this strips the timestamp and decodes
    // the JPEG portion -- lossy compression means the decoded byte is
    // only guaranteed to be *close* to the original fill byte, which is
    // exactly why 0x11/0x22 (a difference of 17) were chosen: no
    // JPEG-quality-80 rounding error gets anywhere near confusing the two.
    bool readVideoFrameFirstByte(uint8_t& outByte) {
        uint8_t headerBuf[kPacketHeaderWireSize];
        if (!recvExactRaw(videoFd, headerBuf, sizeof(headerBuf))) return false;
        auto header = parseHeader(headerBuf, sizeof(headerBuf));
        if (!header || header->type != PacketType::VideoFrame) return false;
        ByteBuffer body(header->payloadSize);
        if (!recvExactRaw(videoFd, body.data(), body.size())) return false;
        if (body.empty()) return false;

        auto videoFrame = parseVideoFramePayload(body.data(), body.size());
        if (!videoFrame || videoFrame->jpeg.empty()) return false;

        tjhandle decompressor = tjInitDecompress();
        if (!decompressor) return false;
        std::vector<uint8_t> pixels(static_cast<size_t>(kFrameWidth) * kFrameHeight * 4);
        int result = tjDecompress2(decompressor, videoFrame->jpeg.data(),
                                    static_cast<unsigned long>(videoFrame->jpeg.size()), pixels.data(), kFrameWidth,
                                    /*pitch=*/0, kFrameHeight, TJPF_BGRA, TJFLAG_FASTDCT);
        tjDestroy(decompressor);
        if (result != 0) return false;

        outByte = pixels[0];
        return true;
    }

    // Reads exactly one packet off the control channel and parses it as
    // ModeChanged. Blocks (with the test's own overall timeout as the
    // only backstop) -- used only after the test has already confirmed a
    // ModeChanged should be arriving.
    bool readModeChanged(ModeChangedPayload& outPayload) {
        uint8_t headerBuf[kPacketHeaderWireSize];
        if (!recvExactRaw(controlFd, headerBuf, sizeof(headerBuf))) return false;
        auto header = parseHeader(headerBuf, sizeof(headerBuf));
        if (!header || header->type != PacketType::ModeChanged) return false;
        ByteBuffer body(header->payloadSize);
        if (!body.empty() && !recvExactRaw(controlFd, body.data(), body.size())) return false;
        auto parsed = parseModeChangedPayload(body.data(), body.size());
        if (!parsed) return false;
        outPayload = *parsed;
        return true;
    }

    void close() {
        if (controlFd >= 0) ::close(controlFd);
        if (videoFd >= 0) ::close(videoFd);
    }
};

// RAII wrapper: every test gets a real, freshly-started NetServer on
// hermetically-allocated loopback ports, static-auth-token mode
// (device-approval's console-input flow is irrelevant to what this file
// tests), always torn down even if a CHECK fails partway through.
struct ServerFixture {
    LoggingInputSink sinkA;
    FakeFrameSource frameA{0x11};
    LoggingInputSink sinkB;
    FakeFrameSource frameB{0x22};
    LoggingMicAudioSink micSink;
    NetServerConfig config;
    NetServer server;

    explicit ServerFixture(NetServerConfig cfg)
        : config([&] {
              cfg.bindAddress = "127.0.0.1";
              cfg.controlPort = freePort();
              cfg.inputPort = freePort();
              cfg.videoPort = freePort();
              cfg.discoveryEnabled = false;
              cfg.micSupported = false;
              cfg.authToken = "test-token";
              cfg.inputTimeoutUs = 5'000'000; // generous -- these tests don't exercise the fail-safe timeout
              return cfg;
          }()),
          server(config, sinkA, frameA, micSink) {
        server.start();
    }

    ~ServerFixture() { server.stop(); }

    bool connectClient(TestClient& client) {
        return client.connectAndHandshake(config.controlPort, config.inputPort, config.videoPort,
                                           config.authToken);
    }
};

// JPEG compression (protocol v8) is lossy, so a decoded fill byte is only
// guaranteed to land close to the original, not identical to it -- see
// TestClient::readVideoFrameFirstByte()'s comment on why frameA/frameB's
// 0x11/0x22 fill bytes are still unambiguous under this tolerance.
bool closeTo(uint8_t actual, uint8_t expected) {
    int diff = static_cast<int>(actual) - static_cast<int>(expected);
    return diff >= -8 && diff <= 8;
}

} // namespace

MDR_TEST(set_target_routes_new_input_to_the_new_sink) {
    ServerFixture fixture{NetServerConfig{}};
    TestClient client;
    MDR_CHECK(fixture.connectClient(client));

    client.sendControllerState(1, DSButton_A);
    MDR_CHECK(waitUntil([&] { return fixture.sinkA.lastState().dsButtons == DSButton_A; }));
    MDR_CHECK_EQ(fixture.sinkB.lastState().dsButtons, 0u);

    fixture.server.setTarget(fixture.sinkB, fixture.frameB, HostMode::HostControl, {"host", "Host Menu"},
                              {"host-control", "Host Control", ""});

    client.sendControllerState(2, DSButton_B);
    MDR_CHECK(waitUntil([&] { return fixture.sinkB.lastState().dsButtons == DSButton_B; }));

    client.close();
}

MDR_TEST(set_target_releases_the_previous_sink) {
    ServerFixture fixture{NetServerConfig{}};
    TestClient client;
    MDR_CHECK(fixture.connectClient(client));

    client.sendControllerState(1, DSButton_A);
    MDR_CHECK(waitUntil([&] { return fixture.sinkA.lastState().dsButtons == DSButton_A; }));

    fixture.server.setTarget(fixture.sinkB, fixture.frameB, HostMode::HostControl, {"host", "Host Menu"},
                              {"host-control", "Host Control", ""});

    // The old sink must reflect an all-released state immediately after
    // the swap -- not still showing DSButton_A held, which would mean
    // whatever's now driving the *new* target's session inherited a
    // press it never actually saw.
    MDR_CHECK(waitUntil([&] { return fixture.sinkA.lastState().dsButtons == 0; }));

    client.close();
}

MDR_TEST(set_target_routes_video_to_the_new_frame_source) {
    ServerFixture fixture{NetServerConfig{}};
    TestClient client;
    MDR_CHECK(fixture.connectClient(client));

    uint8_t firstByte = 0;
    MDR_CHECK(client.readVideoFrameFirstByte(firstByte));
    MDR_CHECK(closeTo(firstByte, 0x11)); // frameA's fill byte

    fixture.server.setTarget(fixture.sinkB, fixture.frameB, HostMode::HostControl, {"host", "Host Menu"},
                              {"host-control", "Host Control", ""});

    // Drain a few frames -- one already in flight from frameA could have
    // been queued right before the swap.
    bool sawFrameB = false;
    for (int i = 0; i < 20 && !sawFrameB; ++i) {
        if (!client.readVideoFrameFirstByte(firstByte)) break;
        if (closeTo(firstByte, 0x22)) sawFrameB = true;
    }
    MDR_CHECK(sawFrameB);

    client.close();
}

MDR_TEST(set_target_notifies_an_already_connected_client) {
    ServerFixture fixture{NetServerConfig{}};
    TestClient client;
    MDR_CHECK(fixture.connectClient(client));

    fixture.server.setTarget(fixture.sinkB, fixture.frameB, HostMode::HostControl,
                              SystemIdentity{"host", "Host Menu"},
                              AdapterIdentity{"host-control", "Host Control", ""});

    ModeChangedPayload payload;
    MDR_CHECK(client.readModeChanged(payload));
    MDR_CHECK(payload.mode == HostMode::HostControl);
    MDR_CHECK(payload.system.systemId == "host");
    MDR_CHECK(payload.adapter.adapterId == "host-control");

    client.close();
}

MDR_TEST(set_target_with_no_connected_client_is_a_safe_noop) {
    ServerFixture fixture{NetServerConfig{}};
    // Deliberately no connectClient() call -- setTarget() must not
    // crash, hang, or otherwise misbehave with nobody connected at all.
    fixture.server.setTarget(fixture.sinkB, fixture.frameB, HostMode::HostControl, {"host", "Host Menu"},
                              {"host-control", "Host Control", ""});
    MDR_CHECK(fixture.server.currentMode() == HostMode::HostControl);
}

MDR_TEST(current_mode_reflects_the_most_recent_set_target) {
    ServerFixture fixture{NetServerConfig{}};
    MDR_CHECK(fixture.server.currentMode() == HostMode::Emulation);

    fixture.server.setTarget(fixture.sinkB, fixture.frameB, HostMode::HostControl, {"host", "Host Menu"},
                              {"host-control", "Host Control", ""});
    MDR_CHECK(fixture.server.currentMode() == HostMode::HostControl);

    fixture.server.setTarget(fixture.sinkA, fixture.frameA, HostMode::Emulation, {"nds", "Nintendo DS"},
                              {"melonds", "melonDS", "1.1"});
    MDR_CHECK(fixture.server.currentMode() == HostMode::Emulation);
}

MDR_TEST(hello_ack_reflects_identity_from_a_set_target_call_made_before_any_client_connected) {
    ServerFixture fixture{NetServerConfig{}};

    // Swap before anyone has ever connected -- a brand-new handshake
    // must see the current state, not the construction-time default.
    fixture.server.setTarget(fixture.sinkB, fixture.frameB, HostMode::HostControl,
                              SystemIdentity{"host", "Host Menu"},
                              AdapterIdentity{"host-control", "Host Control", ""});

    TestClient client;
    MDR_CHECK(fixture.connectClient(client));
    MDR_CHECK(client.ack.system.systemId == "host");
    MDR_CHECK(client.ack.adapter.adapterId == "host-control");
    MDR_CHECK(client.ack.mode == HostMode::HostControl);

    client.close();
}

// defaultVideoQualityForFrameSize() (net_server.cpp) -- pure decision
// logic, unit-tested directly without any real connection, same pattern
// as mode_coordinator.h's computeDesiredMode(). Real bug/gap this
// exists to catch: applying a DS/3DS-tuned JPEG quality default
// unchanged to Cemu's much larger GamePad surface (see the function's
// own comment) directly worsens video latency on Wii U sessions.
MDR_TEST(default_video_quality_keeps_the_configured_default_for_ds_sized_frames) {
    MDR_CHECK(defaultVideoQualityForFrameSize(80, 256, 192) == 80);
}

MDR_TEST(default_video_quality_keeps_the_configured_default_for_3ds_sized_frames) {
    MDR_CHECK(defaultVideoQualityForFrameSize(80, 320, 240) == 80);
}

MDR_TEST(default_video_quality_is_lowered_for_cemu_sized_frames) {
    int quality = defaultVideoQualityForFrameSize(80, 854, 480);
    MDR_CHECK(quality < 80);
    MDR_CHECK(quality > 0);
}

MDR_TEST(default_video_quality_never_exceeds_an_already_low_configured_default) {
    // A host operator who already configured a lower-than-60 default
    // (e.g. for a known-slow link) must never see it raised back up
    // just because a large surface connected.
    MDR_CHECK(defaultVideoQualityForFrameSize(40, 854, 480) == 40);
}
