#include <vector>

#include "melonds_remote/protocol.h"
#include "test_framework.h"

using namespace melonds_remote;

MDR_TEST(video_frame_payload_round_trip) {
    VideoFramePayload payload;
    payload.captureTimestampUs = 1234567890123ULL;
    payload.jpeg = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10}; // not real JPEG, just distinct bytes

    ByteBuffer buf;
    serializeVideoFramePayload(buf, payload);
    auto parsed = parseVideoFramePayload(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK(parsed->captureTimestampUs == payload.captureTimestampUs);
    MDR_CHECK(parsed->jpeg == payload.jpeg);
}

MDR_TEST(video_frame_payload_round_trip_empty_jpeg) {
    VideoFramePayload payload;
    payload.captureTimestampUs = 42;
    payload.jpeg = {};

    ByteBuffer buf;
    serializeVideoFramePayload(buf, payload);
    auto parsed = parseVideoFramePayload(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK(parsed->captureTimestampUs == 42);
    MDR_CHECK(parsed->jpeg.empty());
}

MDR_TEST(video_frame_payload_rejects_buffer_shorter_than_timestamp) {
    ByteBuffer buf(kVideoFrameTimestampWireSize - 1, 0);
    MDR_CHECK(!parseVideoFramePayload(buf.data(), buf.size()).has_value());
}

MDR_TEST(video_frame_payload_rejects_empty_buffer) {
    ByteBuffer buf;
    MDR_CHECK(!parseVideoFramePayload(buf.data(), buf.size()).has_value());
}

MDR_TEST(build_video_frame_packet_has_correct_type_and_wire_layout) {
    VideoFramePayload payload;
    payload.captureTimestampUs = 999;
    payload.jpeg = {0x01, 0x02, 0x03};
    ByteBuffer packet = buildVideoFramePacket(payload);

    auto header = parseHeader(packet.data(), packet.size());
    MDR_CHECK(header.has_value());
    MDR_CHECK(header->type == PacketType::VideoFrame);
    MDR_CHECK(header->payloadSize == kVideoFrameTimestampWireSize + payload.jpeg.size());

    auto parsed =
        parseVideoFramePayload(packet.data() + kPacketHeaderWireSize, packet.size() - kPacketHeaderWireSize);
    MDR_CHECK(parsed.has_value());
    MDR_CHECK(parsed->captureTimestampUs == 999);
    MDR_CHECK(parsed->jpeg == payload.jpeg);
}

// VideoFrame's payload shape genuinely changed (an 8-byte timestamp now
// precedes the JPEG bytes) -- unlike ModeChanged/ClientLog, this is not a
// brand new packet type an older peer can just ignore: an old client
// reading a new-format frame would misinterpret the timestamp as JPEG
// data, and vice versa. This guards against that requirement being
// silently dropped in a future refactor.
MDR_TEST(video_frame_timestamp_prefix_required_a_protocol_version_bump) {
    MDR_CHECK(kProtocolVersion >= 10);
}
