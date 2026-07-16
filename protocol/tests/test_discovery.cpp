#include "melonds_remote/protocol.h"
#include "test_framework.h"

using namespace melonds_remote;

MDR_TEST(discovery_request_packet_has_correct_type_and_empty_payload) {
    ByteBuffer packet = buildDiscoveryRequestPacket();
    MDR_CHECK_EQ(packet.size(), kPacketHeaderWireSize);

    auto header = parseHeader(packet.data(), packet.size());
    MDR_CHECK(header.has_value());
    MDR_CHECK(header->type == PacketType::DiscoveryRequest);
    MDR_CHECK_EQ(header->payloadSize, 0);
}

MDR_TEST(discovery_response_payload_round_trip) {
    DiscoveryResponsePayload response;
    response.hostName = "living-room-htpc";
    response.controlPort = 8760;
    response.inputPort = 8761;
    response.videoPort = 8762;

    ByteBuffer buf;
    serializeDiscoveryResponsePayload(buf, response);
    auto parsed = parseDiscoveryResponsePayload(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK(parsed->hostName == response.hostName);
    MDR_CHECK_EQ(parsed->controlPort, response.controlPort);
    MDR_CHECK_EQ(parsed->inputPort, response.inputPort);
    MDR_CHECK_EQ(parsed->videoPort, response.videoPort);
}

MDR_TEST(discovery_response_payload_empty_hostname_round_trip) {
    DiscoveryResponsePayload response; // default hostName is empty
    ByteBuffer buf;
    serializeDiscoveryResponsePayload(buf, response);
    auto parsed = parseDiscoveryResponsePayload(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK(parsed->hostName.empty());
}

MDR_TEST(discovery_response_payload_rejects_truncated_buffer) {
    DiscoveryResponsePayload response;
    response.hostName = "abc";
    ByteBuffer buf;
    serializeDiscoveryResponsePayload(buf, response);

    buf.resize(buf.size() - 1); // chop off the last byte
    MDR_CHECK(!parseDiscoveryResponsePayload(buf.data(), buf.size()).has_value());
}

MDR_TEST(discovery_response_payload_rejects_trailing_garbage) {
    DiscoveryResponsePayload response;
    ByteBuffer buf;
    serializeDiscoveryResponsePayload(buf, response);
    buf.push_back(0xFF);

    MDR_CHECK(!parseDiscoveryResponsePayload(buf.data(), buf.size()).has_value());
}

MDR_TEST(build_discovery_response_packet_has_correct_type) {
    DiscoveryResponsePayload response;
    response.hostName = "test-host";
    ByteBuffer packet = buildDiscoveryResponsePacket(response);

    auto header = parseHeader(packet.data(), packet.size());
    MDR_CHECK(header.has_value());
    MDR_CHECK(header->type == PacketType::DiscoveryResponse);

    auto parsed = parseDiscoveryResponsePayload(packet.data() + kPacketHeaderWireSize,
                                                 packet.size() - kPacketHeaderWireSize);
    MDR_CHECK(parsed.has_value());
    MDR_CHECK(parsed->hostName == "test-host");
}
