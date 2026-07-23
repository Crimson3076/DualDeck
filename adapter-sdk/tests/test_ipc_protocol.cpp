#include "melonds_remote/adapter/ipc/ipc_protocol.h"
#include "test_framework.h"

using namespace melonds_remote;
using namespace melonds_remote::adapter;
using namespace melonds_remote::adapter::ipc;

namespace {
AdapterCapabilities sampleCapabilities() {
    AdapterCapabilities caps;
    caps.system = SystemIdentity{"3ds", "Nintendo 3DS"};
    caps.adapter = AdapterIdentity{"fake-3ds", "Fake 3DS Adapter", "0.0.1"};

    VideoSurfaceDescriptor top;
    top.surfaceId = "top";
    top.role = SurfaceRole::Top;
    top.width = 400;
    top.height = 240;
    top.locallyDisplayed = true;

    VideoSurfaceDescriptor bottom;
    bottom.surfaceId = "bottom";
    bottom.role = SurfaceRole::Bottom;
    bottom.width = 320;
    bottom.height = 240;
    bottom.touchSupported = true;
    bottom.touchRangeX = 319;
    bottom.touchRangeY = 239;
    bottom.remotelyDisplayed = true;

    caps.surfaces = {top, bottom};
    caps.supportsMicrophone = true;
    return caps;
}
} // namespace

MDR_TEST(ipc_header_round_trip) {
    IpcHeader header;
    header.type = IpcMessageType::Heartbeat;
    header.payloadSize = 7;

    ByteBuffer buf;
    serializeIpcHeader(buf, header);
    MDR_CHECK_EQ(buf.size(), kIpcHeaderWireSize);

    auto parsed = parseIpcHeader(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK_EQ(parsed->magic, kAdapterIpcMagic);
    MDR_CHECK_EQ(parsed->contractVersion, kAdapterContractVersion);
    MDR_CHECK(parsed->type == IpcMessageType::Heartbeat);
    MDR_CHECK_EQ(parsed->payloadSize, 7u);
}

MDR_TEST(ipc_header_rejects_client_host_protocol_magic) {
    // A stray melonds_remote::PacketHeader (the client<->host wire
    // protocol, "DMR1") landing on this channel by mistake must be
    // rejected outright, not misparsed as an IPC header -- the two
    // channels' magic numbers must never collide.
    ByteBuffer buf;
    melonds_remote::PacketHeader clientHostHeader;
    melonds_remote::serializeHeader(buf, clientHostHeader);
    MDR_CHECK(!parseIpcHeader(buf.data(), buf.size()).has_value());
}

MDR_TEST(build_ipc_message_sets_correct_payload_size) {
    ByteBuffer payload{1, 2, 3};
    ByteBuffer packet = buildIpcMessage(IpcMessageType::Frame, payload);
    MDR_CHECK_EQ(packet.size(), kIpcHeaderWireSize + payload.size());

    auto header = parseIpcHeader(packet.data(), packet.size());
    MDR_CHECK(header.has_value());
    MDR_CHECK(header->type == IpcMessageType::Frame);
    MDR_CHECK_EQ(header->payloadSize, 3u);
}

MDR_TEST(adapter_capabilities_round_trip) {
    AdapterCapabilities caps = sampleCapabilities();
    ByteBuffer buf;
    serializeAdapterCapabilities(buf, caps);

    auto parsed = parseAdapterCapabilities(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK(parsed->system.systemId == "3ds");
    MDR_CHECK(parsed->adapter.adapterId == "fake-3ds");
    MDR_CHECK_EQ(parsed->surfaces.size(), 2u);
    MDR_CHECK(parsed->surfaces[0].surfaceId == "top");
    MDR_CHECK_EQ(parsed->surfaces[0].width, 400);
    MDR_CHECK(parsed->surfaces[1].surfaceId == "bottom");
    MDR_CHECK(parsed->surfaces[1].touchSupported);
    MDR_CHECK_EQ(parsed->surfaces[1].touchRangeX, 319);
    MDR_CHECK(parsed->supportsMicrophone);
    MDR_CHECK(!parsed->supportsMotion);
}

MDR_TEST(adapter_capabilities_rejects_surface_count_over_bound) {
    ByteBuffer buf;
    appendSystemIdentity(buf, SystemIdentity{"nds", "Nintendo DS"});
    appendAdapterIdentity(buf, AdapterIdentity{"melonds", "melonDS", "1.0"});
    appendU16(buf, static_cast<uint16_t>(kMaxIpcSurfaceCount + 1)); // declared surface count, but none follow
    MDR_CHECK(!parseAdapterCapabilities(buf.data(), buf.size()).has_value());
}

MDR_TEST(hello_ack_payload_round_trip_accepted) {
    AdapterHelloAckPayload ack;
    ack.accepted = 1;
    ack.reason = AdapterHelloAckReason::None;
    ByteBuffer buf;
    serializeAdapterHelloAckPayload(buf, ack);

    auto parsed = parseAdapterHelloAckPayload(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK_EQ(parsed->accepted, 1);
    MDR_CHECK(parsed->reason == AdapterHelloAckReason::None);
}

MDR_TEST(hello_ack_payload_round_trip_version_mismatch) {
    AdapterHelloAckPayload ack;
    ack.accepted = 0;
    ack.reason = AdapterHelloAckReason::ContractVersionMismatch;
    ByteBuffer buf;
    serializeAdapterHelloAckPayload(buf, ack);

    auto parsed = parseAdapterHelloAckPayload(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK_EQ(parsed->accepted, 0);
    MDR_CHECK(parsed->reason == AdapterHelloAckReason::ContractVersionMismatch);
}

MDR_TEST(generic_input_state_round_trip_with_touches) {
    GenericInputState state;
    state.sequence = 99;
    state.clientTimestampUs = 12345;
    state.buttons = GenericButton_South | GenericButton_L1;
    state.leftStickX = -1000;
    state.leftStickY = 2000;
    state.rightStickX = 3000;
    state.rightStickY = -4000;
    state.leftTrigger = 128;
    state.rightTrigger = 255;
    state.touches = {TouchContact{"bottom", 10, 20}, TouchContact{"top", 30, 40}};
    state.micActive = true;
    state.emulatorActions = GenericAction_PauseResume;

    ByteBuffer buf;
    serializeGenericInputState(buf, state);
    auto parsed = parseGenericInputState(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK_EQ(parsed->sequence, 99u);
    MDR_CHECK_EQ(parsed->buttons, state.buttons);
    MDR_CHECK_EQ(parsed->leftStickX, -1000);
    MDR_CHECK_EQ(parsed->leftTrigger, 128);
    MDR_CHECK_EQ(parsed->touches.size(), 2u);
    MDR_CHECK(parsed->touches[0].surfaceId == "bottom");
    MDR_CHECK_EQ(parsed->touches[0].x, 10);
    MDR_CHECK(parsed->touches[1].surfaceId == "top");
    MDR_CHECK(parsed->micActive);
    MDR_CHECK_EQ(parsed->emulatorActions, static_cast<uint32_t>(GenericAction_PauseResume));
}

MDR_TEST(generic_input_state_empty_touches_round_trip) {
    GenericInputState state; // defaults, no touches
    ByteBuffer buf;
    serializeGenericInputState(buf, state);
    auto parsed = parseGenericInputState(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK(parsed->touches.empty());
}

MDR_TEST(surface_frame_round_trip) {
    SurfaceFrame frame;
    frame.surfaceId = "bottom";
    frame.frameIndex = 42;
    frame.width = 320;
    frame.height = 240;
    frame.pixels = {1, 2, 3, 4, 5};

    ByteBuffer buf;
    serializeSurfaceFrame(buf, frame);
    auto parsed = parseSurfaceFrame(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK(parsed->surfaceId == "bottom");
    MDR_CHECK_EQ(parsed->frameIndex, 42u);
    MDR_CHECK_EQ(parsed->width, 320);
    MDR_CHECK_EQ(parsed->height, 240);
    MDR_CHECK(parsed->pixels == std::vector<uint8_t>({1, 2, 3, 4, 5}));
}

MDR_TEST(surface_frame_rejects_oversized_declared_pixel_count) {
    // A declared pixel count above kMaxIpcFramePixelBytes must be
    // rejected before any allocation is attempted for it, matching
    // protocol.h's own "validate every declared size before trusting
    // it" discipline (spec section 13) applied to this channel.
    ByteBuffer buf;
    appendString(buf, "bottom");
    appendU64(buf, 0);
    appendU16(buf, 0); // width (contract v2)
    appendU16(buf, 0); // height (contract v2)
    appendU32(buf, kMaxIpcFramePixelBytes + 1);
    // No actual pixel bytes follow -- the declared count alone must be
    // enough to reject this before parseSurfaceFrame() would ever try
    // to read that many bytes.
    MDR_CHECK(!parseSurfaceFrame(buf.data(), buf.size()).has_value());
}

MDR_TEST(surface_frame_rejects_pixel_count_size_mismatch) {
    SurfaceFrame frame;
    frame.surfaceId = "bottom";
    frame.frameIndex = 1;
    frame.pixels = {1, 2, 3};
    ByteBuffer buf;
    serializeSurfaceFrame(buf, frame);
    buf.pop_back(); // declared count now says 3 but only 2 bytes follow
    MDR_CHECK(!parseSurfaceFrame(buf.data(), buf.size()).has_value());
}

MDR_TEST(session_state_round_trip_every_value) {
    const SessionState allStates[] = {
        SessionState::Available, SessionState::Starting, SessionState::Running,
        SessionState::Paused,    SessionState::Stopped,  SessionState::Error,
    };
    for (SessionState s : allStates) {
        ByteBuffer buf;
        serializeSessionState(buf, s);
        auto parsed = parseSessionState(buf.data(), buf.size());
        MDR_CHECK(parsed.has_value());
        MDR_CHECK(*parsed == s);
    }
}

MDR_TEST(session_state_rejects_out_of_range_value) {
    ByteBuffer buf{200};
    MDR_CHECK(!parseSessionState(buf.data(), buf.size()).has_value());
}

MDR_TEST(client_connection_changed_round_trip) {
    for (bool connected : {true, false}) {
        ByteBuffer buf;
        serializeClientConnectionChanged(buf, connected);
        auto parsed = parseClientConnectionChanged(buf.data(), buf.size());
        MDR_CHECK(parsed.has_value());
        MDR_CHECK(*parsed == connected);
    }
}

MDR_TEST(client_connection_changed_rejects_wrong_size) {
    ByteBuffer buf{1, 1};
    MDR_CHECK(!parseClientConnectionChanged(buf.data(), buf.size()).has_value());
}
