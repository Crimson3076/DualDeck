#include "melonds_remote/protocol.h"
#include "test_framework.h"

using namespace melonds_remote;

MDR_TEST(header_round_trip) {
    PacketHeader header;
    header.type = PacketType::Heartbeat;
    header.payloadSize = 42;

    ByteBuffer buf;
    serializeHeader(buf, header);
    MDR_CHECK_EQ(buf.size(), kPacketHeaderWireSize);

    auto parsed = parseHeader(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK_EQ(parsed->magic, kPacketMagic);
    MDR_CHECK_EQ(parsed->protocolVersion, kProtocolVersion);
    MDR_CHECK(parsed->type == PacketType::Heartbeat);
    MDR_CHECK_EQ(parsed->payloadSize, 42u);
}

MDR_TEST(header_rejects_bad_magic) {
    PacketHeader header;
    ByteBuffer buf;
    serializeHeader(buf, header);

    // Corrupt the magic number.
    buf[0] ^= 0xFF;

    auto parsed = parseHeader(buf.data(), buf.size());
    MDR_CHECK(!parsed.has_value());
}

MDR_TEST(header_rejects_short_buffer) {
    ByteBuffer buf(kPacketHeaderWireSize - 1, 0);
    auto parsed = parseHeader(buf.data(), buf.size());
    MDR_CHECK(!parsed.has_value());
}

MDR_TEST(header_rejects_null_data) {
    auto parsed = parseHeader(nullptr, kPacketHeaderWireSize);
    MDR_CHECK(!parsed.has_value());
}

MDR_TEST(build_packet_sets_correct_payload_size) {
    ByteBuffer payload{1, 2, 3, 4, 5};
    ByteBuffer packet = buildPacket(PacketType::Hello, payload);
    MDR_CHECK_EQ(packet.size(), kPacketHeaderWireSize + payload.size());

    auto header = parseHeader(packet.data(), packet.size());
    MDR_CHECK(header.has_value());
    MDR_CHECK_EQ(header->payloadSize, payload.size());
    MDR_CHECK(header->type == PacketType::Hello);
}
