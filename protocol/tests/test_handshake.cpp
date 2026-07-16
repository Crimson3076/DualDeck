#include "melonds_remote/protocol.h"
#include "test_framework.h"

using namespace melonds_remote;

MDR_TEST(hello_payload_round_trip) {
    HelloPayload hello;
    hello.clientName = "SteamDeck-01";
    hello.clientPlatform = "linux-steamos";
    hello.displayWidth = 1280;
    hello.displayHeight = 800;
    hello.authToken = "s3cr3t";

    ByteBuffer buf;
    serializeHelloPayload(buf, hello);

    auto parsed = parseHelloPayload(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK(parsed->clientName == hello.clientName);
    MDR_CHECK(parsed->clientPlatform == hello.clientPlatform);
    MDR_CHECK_EQ(parsed->displayWidth, hello.displayWidth);
    MDR_CHECK_EQ(parsed->displayHeight, hello.displayHeight);
    MDR_CHECK(parsed->authToken == hello.authToken);
}

MDR_TEST(hello_payload_empty_fields_round_trip) {
    HelloPayload hello; // all default/empty
    ByteBuffer buf;
    serializeHelloPayload(buf, hello);

    auto parsed = parseHelloPayload(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK(parsed->clientName.empty());
    MDR_CHECK(parsed->authToken.empty());
}

MDR_TEST(hello_payload_rejects_truncated_buffer) {
    HelloPayload hello;
    hello.clientName = "abc";
    ByteBuffer buf;
    serializeHelloPayload(buf, hello);

    buf.resize(buf.size() - 1); // chop off the last byte
    MDR_CHECK(!parseHelloPayload(buf.data(), buf.size()).has_value());
}

MDR_TEST(hello_payload_rejects_trailing_garbage) {
    HelloPayload hello;
    ByteBuffer buf;
    serializeHelloPayload(buf, hello);
    buf.push_back(0xFF); // extra byte the parser didn't consume

    MDR_CHECK(!parseHelloPayload(buf.data(), buf.size()).has_value());
}

MDR_TEST(read_string_rejects_declared_length_exceeding_max) {
    ByteBuffer buf;
    appendU16(buf, static_cast<uint16_t>(kMaxProtocolStringLength + 1));
    for (size_t i = 0; i < kMaxProtocolStringLength + 1; ++i) buf.push_back('a');

    size_t offset = 0;
    auto s = readString(buf.data(), buf.size(), offset);
    MDR_CHECK(!s.has_value());
}

MDR_TEST(read_string_rejects_declared_length_exceeding_buffer) {
    ByteBuffer buf;
    appendU16(buf, 10); // claims 10 bytes follow
    buf.push_back('a'); // but only provides 1

    size_t offset = 0;
    auto s = readString(buf.data(), buf.size(), offset);
    MDR_CHECK(!s.has_value());
}

MDR_TEST(hello_ack_payload_round_trip_accepted) {
    HelloAckPayload ack;
    ack.accepted = 1;
    ack.rejectReason = HelloRejectReason::None;
    ack.sessionId = 0xCAFEBABE;
    ack.nativeWidth = 256;
    ack.nativeHeight = 192;

    ByteBuffer buf;
    serializeHelloAckPayload(buf, ack);
    auto parsed = parseHelloAckPayload(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK_EQ(parsed->accepted, 1);
    MDR_CHECK(parsed->rejectReason == HelloRejectReason::None);
    MDR_CHECK_EQ(parsed->sessionId, ack.sessionId);
    MDR_CHECK_EQ(parsed->nativeWidth, 256);
    MDR_CHECK_EQ(parsed->nativeHeight, 192);
    MDR_CHECK(parsed->pairingToken.empty());
}

MDR_TEST(hello_ack_payload_round_trip_with_pairing_token) {
    HelloAckPayload ack;
    ack.accepted = 1;
    ack.rejectReason = HelloRejectReason::None;
    ack.sessionId = 42;
    ack.pairingToken = "a1b2c3d4e5f6";

    ByteBuffer buf;
    serializeHelloAckPayload(buf, ack);
    auto parsed = parseHelloAckPayload(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK(parsed->pairingToken == ack.pairingToken);
}

MDR_TEST(hello_ack_payload_round_trip_pairing_required) {
    HelloAckPayload ack;
    ack.accepted = 0;
    ack.rejectReason = HelloRejectReason::PairingRequired;

    ByteBuffer buf;
    serializeHelloAckPayload(buf, ack);
    auto parsed = parseHelloAckPayload(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK_EQ(parsed->accepted, 0);
    MDR_CHECK(parsed->rejectReason == HelloRejectReason::PairingRequired);
}

MDR_TEST(hello_ack_payload_rejects_trailing_garbage) {
    HelloAckPayload ack;
    ack.accepted = 1;
    ByteBuffer buf;
    serializeHelloAckPayload(buf, ack);
    buf.push_back(0xFF); // extra byte the parser didn't consume

    MDR_CHECK(!parseHelloAckPayload(buf.data(), buf.size()).has_value());
}

MDR_TEST(hello_ack_payload_round_trip_rejected) {
    HelloAckPayload ack;
    ack.accepted = 0;
    ack.rejectReason = HelloRejectReason::AuthenticationFailed;

    ByteBuffer buf;
    serializeHelloAckPayload(buf, ack);
    auto parsed = parseHelloAckPayload(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK_EQ(parsed->accepted, 0);
    MDR_CHECK(parsed->rejectReason == HelloRejectReason::AuthenticationFailed);
}

MDR_TEST(hello_ack_payload_rejects_wrong_size) {
    ByteBuffer tooShort(5, 0);
    MDR_CHECK(!parseHelloAckPayload(tooShort.data(), tooShort.size()).has_value());
}

MDR_TEST(hello_ack_payload_rejects_invalid_accepted_flag) {
    HelloAckPayload ack;
    ByteBuffer buf;
    serializeHelloAckPayload(buf, ack);
    buf[0] = 2; // invalid

    MDR_CHECK(!parseHelloAckPayload(buf.data(), buf.size()).has_value());
}

MDR_TEST(hello_ack_payload_rejects_invalid_reject_reason) {
    HelloAckPayload ack;
    ByteBuffer buf;
    serializeHelloAckPayload(buf, ack);
    buf[1] = 200; // out of enum range

    MDR_CHECK(!parseHelloAckPayload(buf.data(), buf.size()).has_value());
}

MDR_TEST(build_hello_packet_has_correct_type) {
    HelloPayload hello;
    hello.clientName = "test";
    ByteBuffer packet = buildHelloPacket(hello);

    auto header = parseHeader(packet.data(), packet.size());
    MDR_CHECK(header.has_value());
    MDR_CHECK(header->type == PacketType::Hello);

    auto parsed = parseHelloPayload(packet.data() + kPacketHeaderWireSize,
                                     packet.size() - kPacketHeaderWireSize);
    MDR_CHECK(parsed.has_value());
    MDR_CHECK(parsed->clientName == "test");
}

MDR_TEST(build_hello_ack_packet_has_correct_type) {
    HelloAckPayload ack;
    ack.accepted = 1;
    ByteBuffer packet = buildHelloAckPacket(ack);

    auto header = parseHeader(packet.data(), packet.size());
    MDR_CHECK(header.has_value());
    MDR_CHECK(header->type == PacketType::HelloAck);
}
