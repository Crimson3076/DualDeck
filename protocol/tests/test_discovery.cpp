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
    response.audioPort = 8765;
    response.system = {"nds", "Nintendo DS"};
    response.adapter = {"melonds", "melonDS", "1.0"};

    ByteBuffer buf;
    serializeDiscoveryResponsePayload(buf, response);
    auto parsed = parseDiscoveryResponsePayload(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK(parsed->hostName == response.hostName);
    MDR_CHECK_EQ(parsed->controlPort, response.controlPort);
    MDR_CHECK_EQ(parsed->inputPort, response.inputPort);
    MDR_CHECK_EQ(parsed->videoPort, response.videoPort);
    MDR_CHECK_EQ(parsed->audioPort, response.audioPort);
    MDR_CHECK(parsed->system.systemId == "nds");
    MDR_CHECK(parsed->system.systemName == "Nintendo DS");
    MDR_CHECK(parsed->adapter.adapterId == "melonds");
    MDR_CHECK(parsed->adapter.adapterName == "melonDS");
    MDR_CHECK(parsed->adapter.adapterVersion == "1.0");
}

MDR_TEST(discovery_response_payload_synthetic_identity_round_trip) {
    // Mirrors the standalone host's default identity (see
    // host/remote-server/include/host/net_server.h's
    // NetServerConfig::systemIdentity/adapterIdentity) -- a UI-support
    // check that arbitrary, non-melonDS identity strings survive the
    // wire round trip untouched, not just the one real adapter that
    // exists today (GitHub issue #28: keep the identity model reusable
    // for future adapters).
    DiscoveryResponsePayload response;
    response.hostName = "test-rig";
    response.system = {"synthetic", "Synthetic Test System"};
    response.adapter = {"synthetic-test", "Synthetic Test Adapter", "dev-build"};

    ByteBuffer buf;
    serializeDiscoveryResponsePayload(buf, response);
    auto parsed = parseDiscoveryResponsePayload(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK(parsed->system.systemId == "synthetic");
    MDR_CHECK(parsed->system.systemName == "Synthetic Test System");
    MDR_CHECK(parsed->adapter.adapterId == "synthetic-test");
    MDR_CHECK(parsed->adapter.adapterName == "Synthetic Test Adapter");
    MDR_CHECK(parsed->adapter.adapterVersion == "dev-build");
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
