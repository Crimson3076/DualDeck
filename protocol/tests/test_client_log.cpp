#include <string>

#include "melonds_remote/protocol.h"
#include "test_framework.h"

using namespace melonds_remote;

MDR_TEST(client_log_payload_round_trip) {
    ClientLogPayload payload;
    payload.line = "connection attempt already in progress; ignoring duplicate request\n";

    ByteBuffer buf;
    serializeClientLogPayload(buf, payload);
    auto parsed = parseClientLogPayload(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK(parsed->line == payload.line);
}

MDR_TEST(client_log_payload_round_trip_empty_line) {
    ClientLogPayload payload;
    payload.line = "";

    ByteBuffer buf;
    serializeClientLogPayload(buf, payload);
    auto parsed = parseClientLogPayload(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK(parsed->line.empty());
}

MDR_TEST(client_log_payload_serialize_truncates_overlong_line) {
    ClientLogPayload payload;
    payload.line = std::string(kMaxClientLogLineLength + 100, 'x');

    ByteBuffer buf;
    serializeClientLogPayload(buf, payload);
    auto parsed = parseClientLogPayload(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK(parsed->line.size() == kMaxClientLogLineLength);
}

MDR_TEST(client_log_payload_rejects_declared_length_over_max) {
    ByteBuffer buf;
    appendU16(buf, static_cast<uint16_t>(kMaxClientLogLineLength + 1));
    buf.insert(buf.end(), kMaxClientLogLineLength + 1, 'x');

    MDR_CHECK(!parseClientLogPayload(buf.data(), buf.size()).has_value());
}

MDR_TEST(client_log_payload_rejects_short_buffer) {
    ByteBuffer buf;
    buf.push_back(0); // one byte -- not even a full u16 length prefix

    MDR_CHECK(!parseClientLogPayload(buf.data(), buf.size()).has_value());
}

MDR_TEST(client_log_payload_rejects_truncated_line) {
    ClientLogPayload payload;
    payload.line = "a diagnostic message";

    ByteBuffer buf;
    serializeClientLogPayload(buf, payload);
    buf.resize(buf.size() - 1);

    MDR_CHECK(!parseClientLogPayload(buf.data(), buf.size()).has_value());
}

MDR_TEST(client_log_payload_rejects_trailing_garbage) {
    ClientLogPayload payload;
    payload.line = "a diagnostic message";

    ByteBuffer buf;
    serializeClientLogPayload(buf, payload);
    buf.push_back(0xFF);

    MDR_CHECK(!parseClientLogPayload(buf.data(), buf.size()).has_value());
}

MDR_TEST(build_client_log_packet_has_correct_type) {
    ClientLogPayload payload;
    payload.line = "hello from a client";
    ByteBuffer packet = buildClientLogPacket(payload);

    auto header = parseHeader(packet.data(), packet.size());
    MDR_CHECK(header.has_value());
    MDR_CHECK(header->type == PacketType::ClientLog);

    auto parsed = parseClientLogPayload(packet.data() + kPacketHeaderWireSize,
                                         packet.size() - kPacketHeaderWireSize);
    MDR_CHECK(parsed.has_value());
    MDR_CHECK(parsed->line == "hello from a client");
}

// Same reasoning as test_mode_changed.cpp's matching test: ClientLog is a
// brand new packet type an older peer simply never sends/reads, not a
// change to any existing negotiated struct's shape, so it doesn't need a
// kProtocolVersion bump. This guards against that being reintroduced
// under the mistaken assumption every new packet type needs one.
MDR_TEST(client_log_did_not_require_a_protocol_version_bump) {
    MDR_CHECK(kProtocolVersion >= 9);
}
