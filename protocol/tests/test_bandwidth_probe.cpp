#include "dualdeck/protocol.h"
#include "test_framework.h"

using namespace dualdeck;

MDR_TEST(bandwidth_probe_packet_round_trip) {
    BandwidthProbePayload probe;
    probe.last = 1;
    probe.fillerBytes = 1000;
    ByteBuffer packet = buildBandwidthProbePacket(probe);
    auto header = parseHeader(packet.data(), packet.size());
    MDR_CHECK(header.has_value());
    MDR_CHECK(header->type == PacketType::BandwidthProbe);
    MDR_CHECK_EQ(header->payloadSize, 1001u);
    MDR_CHECK_EQ(packet.size(), kPacketHeaderWireSize + 1001);

    auto parsed = parseBandwidthProbePayload(packet.data() + kPacketHeaderWireSize, header->payloadSize);
    MDR_CHECK(parsed.has_value());
    MDR_CHECK_EQ(parsed->last, 1);
    MDR_CHECK_EQ(parsed->fillerBytes, 1000u);
}

MDR_TEST(bandwidth_probe_payload_rejects_bad_flag_and_oversize_chunk) {
    const uint8_t badFlag[] = {2, 0, 0};
    MDR_CHECK(!parseBandwidthProbePayload(badFlag, sizeof(badFlag)).has_value());
    MDR_CHECK(!parseBandwidthProbePayload(badFlag, 0).has_value());
    ByteBuffer tooBig(kBandwidthProbeChunkBytes + 2, 0);
    MDR_CHECK(!parseBandwidthProbePayload(tooBig.data(), tooBig.size()).has_value());
}

MDR_TEST(bandwidth_report_packet_round_trip) {
    BandwidthReportPayload report;
    report.measuredKbps = 123'456;
    ByteBuffer packet = buildBandwidthReportPacket(report);
    auto header = parseHeader(packet.data(), packet.size());
    MDR_CHECK(header.has_value());
    MDR_CHECK(header->type == PacketType::BandwidthReport);
    auto parsed = parseBandwidthReportPayload(packet.data() + kPacketHeaderWireSize, header->payloadSize);
    MDR_CHECK(parsed.has_value());
    MDR_CHECK_EQ(parsed->measuredKbps, 123'456u);
    MDR_CHECK(!parseBandwidthReportPayload(packet.data() + kPacketHeaderWireSize, 3).has_value());
}
