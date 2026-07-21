#include "melonds_remote/protocol.h"
#include "test_framework.h"

using namespace melonds_remote;

MDR_TEST(mode_changed_payload_round_trip_emulation) {
    ModeChangedPayload payload;
    payload.mode = HostMode::Emulation;
    payload.system = {"nds", "Nintendo DS"};
    payload.adapter = {"melonds", "melonDS", "1.1"};

    ByteBuffer buf;
    serializeModeChangedPayload(buf, payload);
    auto parsed = parseModeChangedPayload(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK(parsed->mode == HostMode::Emulation);
    MDR_CHECK(parsed->system.systemId == "nds");
    MDR_CHECK(parsed->system.systemName == "Nintendo DS");
    MDR_CHECK(parsed->adapter.adapterId == "melonds");
    MDR_CHECK(parsed->adapter.adapterName == "melonDS");
    MDR_CHECK(parsed->adapter.adapterVersion == "1.1");
}

MDR_TEST(mode_changed_payload_round_trip_host_control) {
    ModeChangedPayload payload;
    payload.mode = HostMode::HostControl;
    payload.system = {"host", "Host Menu"};
    payload.adapter = {"host-control", "Host Control", ""};

    ByteBuffer buf;
    serializeModeChangedPayload(buf, payload);
    auto parsed = parseModeChangedPayload(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK(parsed->mode == HostMode::HostControl);
    MDR_CHECK(parsed->system.systemId == "host");
    MDR_CHECK(parsed->adapter.adapterId == "host-control");
}

MDR_TEST(mode_changed_payload_rejects_unrecognized_mode_byte) {
    ByteBuffer buf;
    buf.push_back(2); // neither Emulation (0) nor HostControl (1)
    appendSystemIdentity(buf, {"nds", "Nintendo DS"});
    appendAdapterIdentity(buf, {"melonds", "melonDS", "1.1"});

    MDR_CHECK(!parseModeChangedPayload(buf.data(), buf.size()).has_value());
}

MDR_TEST(mode_changed_payload_rejects_empty_buffer) {
    ByteBuffer buf;
    MDR_CHECK(!parseModeChangedPayload(buf.data(), buf.size()).has_value());
}

MDR_TEST(mode_changed_payload_rejects_truncated_buffer) {
    ModeChangedPayload payload;
    payload.mode = HostMode::Emulation;
    payload.system = {"nds", "Nintendo DS"};
    payload.adapter = {"melonds", "melonDS", "1.1"};

    ByteBuffer buf;
    serializeModeChangedPayload(buf, payload);
    buf.resize(buf.size() - 1);

    MDR_CHECK(!parseModeChangedPayload(buf.data(), buf.size()).has_value());
}

MDR_TEST(mode_changed_payload_rejects_trailing_garbage) {
    ModeChangedPayload payload;
    payload.mode = HostMode::Emulation;
    payload.system = {"nds", "Nintendo DS"};
    payload.adapter = {"melonds", "melonDS", "1.1"};

    ByteBuffer buf;
    serializeModeChangedPayload(buf, payload);
    buf.push_back(0xFF);

    MDR_CHECK(!parseModeChangedPayload(buf.data(), buf.size()).has_value());
}

MDR_TEST(build_mode_changed_packet_has_correct_type) {
    ModeChangedPayload payload;
    payload.mode = HostMode::HostControl;
    payload.system = {"host", "Host Menu"};
    payload.adapter = {"host-control", "Host Control", ""};
    ByteBuffer packet = buildModeChangedPacket(payload);

    auto header = parseHeader(packet.data(), packet.size());
    MDR_CHECK(header.has_value());
    MDR_CHECK(header->type == PacketType::ModeChanged);

    auto parsed = parseModeChangedPayload(packet.data() + kPacketHeaderWireSize,
                                           packet.size() - kPacketHeaderWireSize);
    MDR_CHECK(parsed.has_value());
    MDR_CHECK(parsed->mode == HostMode::HostControl);
}

// Adding this packet type deliberately did not bump kProtocolVersion --
// see protocol.h's PacketType::ModeChanged comment for why (unsolicited,
// not part of Hello/HelloAck/DiscoveryResponse negotiation, and an older
// client that never reads from the control channel post-handshake simply
// never consumes these bytes). Guards against an accidental bump being
// added here under the mistaken assumption every new packet type needs one.
MDR_TEST(mode_changed_did_not_require_a_protocol_version_bump) {
    MDR_CHECK_EQ(kProtocolVersion, 6);
}
