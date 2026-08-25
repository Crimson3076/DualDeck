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
    hello.appVersion = "v0.1.24";
    hello.supportedVideoCodecs = kVideoCodecBit_Jpeg | kVideoCodecBit_H264;

    ByteBuffer buf;
    serializeHelloPayload(buf, hello);

    auto parsed = parseHelloPayload(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK(parsed->clientName == hello.clientName);
    MDR_CHECK(parsed->clientPlatform == hello.clientPlatform);
    MDR_CHECK_EQ(parsed->displayWidth, hello.displayWidth);
    MDR_CHECK_EQ(parsed->displayHeight, hello.displayHeight);
    MDR_CHECK(parsed->authToken == hello.authToken);
    MDR_CHECK(parsed->appVersion == hello.appVersion);
    MDR_CHECK_EQ(parsed->supportedVideoCodecs, hello.supportedVideoCodecs);
}

MDR_TEST(hello_payload_supported_video_codecs_defaults_to_jpeg_only) {
    HelloPayload hello; // all default/empty
    ByteBuffer buf;
    serializeHelloPayload(buf, hello);

    auto parsed = parseHelloPayload(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK_EQ(parsed->supportedVideoCodecs, kVideoCodecBit_Jpeg);
}

MDR_TEST(hello_payload_empty_fields_round_trip) {
    HelloPayload hello; // all default/empty
    ByteBuffer buf;
    serializeHelloPayload(buf, hello);

    auto parsed = parseHelloPayload(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK(parsed->clientName.empty());
    MDR_CHECK(parsed->authToken.empty());
    MDR_CHECK(parsed->appVersion.empty());
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
    ack.appVersion = "v0.1.24";
    ack.micSupported = 1;
    ack.system = {"nds", "Nintendo DS"};
    ack.adapter = {"melonds", "melonDS", "1.0"};
    ack.mode = HostMode::HostControl;
    ack.selectedVideoCodec = VideoCodec::H264;

    ByteBuffer buf;
    serializeHelloAckPayload(buf, ack);
    auto parsed = parseHelloAckPayload(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK_EQ(parsed->accepted, 1);
    MDR_CHECK(parsed->rejectReason == HelloRejectReason::None);
    MDR_CHECK_EQ(parsed->sessionId, ack.sessionId);
    MDR_CHECK_EQ(parsed->nativeWidth, 256);
    MDR_CHECK_EQ(parsed->nativeHeight, 192);
    MDR_CHECK(parsed->appVersion == ack.appVersion);
    MDR_CHECK_EQ(parsed->micSupported, 1);
    MDR_CHECK(parsed->system.systemId == "nds");
    MDR_CHECK(parsed->system.systemName == "Nintendo DS");
    MDR_CHECK(parsed->adapter.adapterId == "melonds");
    MDR_CHECK(parsed->adapter.adapterName == "melonDS");
    MDR_CHECK(parsed->adapter.adapterVersion == "1.0");
    MDR_CHECK(parsed->mode == HostMode::HostControl);
    MDR_CHECK(parsed->selectedVideoCodec == VideoCodec::H264);
}

MDR_TEST(hello_ack_payload_mode_defaults_to_emulation) {
    HelloAckPayload ack;
    ack.accepted = 1;

    ByteBuffer buf;
    serializeHelloAckPayload(buf, ack);
    auto parsed = parseHelloAckPayload(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK(parsed->mode == HostMode::Emulation);
}

MDR_TEST(hello_ack_payload_rejects_invalid_mode) {
    HelloAckPayload ack;
    ack.accepted = 1;
    ByteBuffer buf;
    serializeHelloAckPayload(buf, ack);
    // mode is the second-to-last byte now that selectedVideoCodec (v13)
    // follows it.
    buf[buf.size() - 2] = 200;

    MDR_CHECK(!parseHelloAckPayload(buf.data(), buf.size()).has_value());
}

MDR_TEST(hello_ack_payload_selected_video_codec_defaults_to_jpeg) {
    HelloAckPayload ack;
    ack.accepted = 1;

    ByteBuffer buf;
    serializeHelloAckPayload(buf, ack);
    auto parsed = parseHelloAckPayload(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK(parsed->selectedVideoCodec == VideoCodec::Jpeg);
}

MDR_TEST(hello_ack_payload_rejects_invalid_selected_video_codec) {
    HelloAckPayload ack;
    ack.accepted = 1;
    ByteBuffer buf;
    serializeHelloAckPayload(buf, ack);
    buf.back() = 200; // selectedVideoCodec is always the last byte

    MDR_CHECK(!parseHelloAckPayload(buf.data(), buf.size()).has_value());
}

MDR_TEST(hello_ack_payload_identity_defaults_to_empty) {
    HelloAckPayload ack;
    ack.accepted = 1;

    ByteBuffer buf;
    serializeHelloAckPayload(buf, ack);
    auto parsed = parseHelloAckPayload(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK(parsed->system.systemId.empty());
    MDR_CHECK(parsed->system.systemName.empty());
    MDR_CHECK(parsed->adapter.adapterId.empty());
    MDR_CHECK(parsed->adapter.adapterName.empty());
    MDR_CHECK(parsed->adapter.adapterVersion.empty());
}

MDR_TEST(hello_ack_payload_rejects_truncated_identity) {
    HelloAckPayload ack;
    ack.accepted = 1;
    ack.system = {"nds", "Nintendo DS"};
    ack.adapter = {"melonds", "melonDS", "1.0"};

    ByteBuffer buf;
    serializeHelloAckPayload(buf, ack);
    buf.resize(buf.size() - 1); // chop off the trailing selectedVideoCodec byte
    MDR_CHECK(!parseHelloAckPayload(buf.data(), buf.size()).has_value());
}

MDR_TEST(hello_ack_payload_mic_unsupported_by_default) {
    HelloAckPayload ack;
    ack.accepted = 1;

    ByteBuffer buf;
    serializeHelloAckPayload(buf, ack);
    auto parsed = parseHelloAckPayload(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK_EQ(parsed->micSupported, 0);
}

MDR_TEST(hello_ack_payload_rejects_invalid_mic_supported_flag) {
    HelloAckPayload ack;
    ack.accepted = 1;
    ByteBuffer buf;
    serializeHelloAckPayload(buf, ack);
    // micSupported sits right after the fixed prefix (10 bytes) and the
    // (here empty, 2-byte-length-prefix-only) appVersion string -- no
    // longer the buffer's last byte now that system/adapter identity
    // follows it.
    constexpr size_t kMicSupportedOffset = 10 + 2;
    buf[kMicSupportedOffset] = 2; // invalid

    MDR_CHECK(!parseHelloAckPayload(buf.data(), buf.size()).has_value());
}

MDR_TEST(hello_ack_payload_round_trip_app_version_mismatch) {
    HelloAckPayload ack;
    ack.accepted = 0;
    ack.rejectReason = HelloRejectReason::AppVersionMismatch;
    ack.appVersion = "v0.1.25";

    ByteBuffer buf;
    serializeHelloAckPayload(buf, ack);
    auto parsed = parseHelloAckPayload(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK_EQ(parsed->accepted, 0);
    MDR_CHECK(parsed->rejectReason == HelloRejectReason::AppVersionMismatch);
    MDR_CHECK(parsed->appVersion == "v0.1.25");
}

MDR_TEST(hello_ack_payload_round_trip_approval_required) {
    HelloAckPayload ack;
    ack.accepted = 0;
    ack.rejectReason = HelloRejectReason::ApprovalRequired;

    ByteBuffer buf;
    serializeHelloAckPayload(buf, ack);
    auto parsed = parseHelloAckPayload(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK_EQ(parsed->accepted, 0);
    MDR_CHECK(parsed->rejectReason == HelloRejectReason::ApprovalRequired);
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
