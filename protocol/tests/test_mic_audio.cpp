#include "melonds_remote/protocol.h"
#include "test_framework.h"

using namespace melonds_remote;

MDR_TEST(mic_audio_frame_payload_round_trip) {
    MicAudioFramePayload frame;
    frame.sequence = 42;
    frame.clientTimestampUs = 123456789;
    frame.numSamples = 4;
    frame.samples = {1, -1, 32767, -32768};

    ByteBuffer buf;
    serializeMicAudioFramePayload(buf, frame);
    auto parsed = parseMicAudioFramePayload(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK_EQ(parsed->sequence, frame.sequence);
    MDR_CHECK_EQ(parsed->clientTimestampUs, frame.clientTimestampUs);
    MDR_CHECK_EQ(parsed->numSamples, frame.numSamples);
    MDR_CHECK(parsed->samples == frame.samples);
}

MDR_TEST(mic_audio_frame_payload_empty_samples_round_trip) {
    MicAudioFramePayload frame; // numSamples = 0, samples empty
    ByteBuffer buf;
    serializeMicAudioFramePayload(buf, frame);
    auto parsed = parseMicAudioFramePayload(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK_EQ(parsed->numSamples, 0);
    MDR_CHECK(parsed->samples.empty());
}

MDR_TEST(mic_audio_frame_payload_full_packet_round_trip) {
    MicAudioFramePayload frame;
    frame.numSamples = kMicAudioSamplesPerPacket;
    frame.samples.assign(kMicAudioSamplesPerPacket, 0);
    for (uint16_t i = 0; i < kMicAudioSamplesPerPacket; ++i) {
        frame.samples[i] = static_cast<int16_t>(i);
    }

    ByteBuffer buf;
    serializeMicAudioFramePayload(buf, frame);
    auto parsed = parseMicAudioFramePayload(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK_EQ(parsed->numSamples, kMicAudioSamplesPerPacket);
    MDR_CHECK(parsed->samples == frame.samples);
}

MDR_TEST(mic_audio_frame_payload_rejects_numSamples_over_cap) {
    // Hand-craft a buffer whose declared numSamples exceeds
    // kMicAudioSamplesPerPacket, with enough trailing bytes that a naive
    // parser might otherwise accept it.
    ByteBuffer buf;
    appendU32(buf, 1);
    appendU64(buf, 0);
    appendU16(buf, static_cast<uint16_t>(kMicAudioSamplesPerPacket + 1));
    for (uint16_t i = 0; i < kMicAudioSamplesPerPacket + 1; ++i) {
        appendI16(buf, 0);
    }

    MDR_CHECK(!parseMicAudioFramePayload(buf.data(), buf.size()).has_value());
}

MDR_TEST(mic_audio_frame_payload_rejects_sample_count_mismatch) {
    MicAudioFramePayload frame;
    frame.numSamples = 4;
    frame.samples = {1, 2, 3, 4};

    ByteBuffer buf;
    serializeMicAudioFramePayload(buf, frame);
    buf.resize(buf.size() - 2); // drop the last sample's bytes without updating numSamples

    MDR_CHECK(!parseMicAudioFramePayload(buf.data(), buf.size()).has_value());
}

MDR_TEST(mic_audio_frame_payload_rejects_truncated_header) {
    ByteBuffer buf(10, 0); // shorter than the 14-byte fixed prefix
    MDR_CHECK(!parseMicAudioFramePayload(buf.data(), buf.size()).has_value());
}

MDR_TEST(build_mic_audio_frame_packet_has_correct_type) {
    MicAudioFramePayload frame;
    frame.sequence = 7;
    frame.numSamples = 2;
    frame.samples = {100, -100};

    ByteBuffer packet = buildMicAudioFramePacket(frame);
    auto header = parseHeader(packet.data(), packet.size());
    MDR_CHECK(header.has_value());
    MDR_CHECK(header->type == PacketType::MicAudioFrame);

    auto parsed = parseMicAudioFramePayload(packet.data() + kPacketHeaderWireSize,
                                             packet.size() - kPacketHeaderWireSize);
    MDR_CHECK(parsed.has_value());
    MDR_CHECK_EQ(parsed->sequence, 7);
    MDR_CHECK(parsed->samples == frame.samples);
}
