#include "melonds_remote/protocol.h"
#include "test_framework.h"

using namespace melonds_remote;

MDR_TEST(controller_state_round_trip) {
    ControllerState state;
    state.sequence = 1234;
    state.clientTimestampUs = 9876543210ULL;
    state.dsButtons = DSButton_A | DSButton_Up | DSButton_L;
    state.emulatorActions = EmulatorAction_FastForward;
    state.leftStickX = -1000;
    state.leftStickY = 32000;
    state.rightStickX = 0;
    state.rightStickY = -32768;
    state.touchActive = 1;
    state.touchX = 128;
    state.touchY = 96;
    state.mouseDeltaX = -321;
    state.mouseDeltaY = 654;
    state.mouseButtons = MouseButton_Left | MouseButton_Right;
    state.leftTrigger = 200;
    state.rightTrigger = 55;
    state.extraButtons = ExtraButton_ThumbLeft | ExtraButton_ThumbRight;

    ByteBuffer buf;
    serializeControllerState(buf, state);
    MDR_CHECK_EQ(buf.size(), kControllerStateWireSize);

    auto parsed = parseControllerState(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK_EQ(parsed->sequence, state.sequence);
    MDR_CHECK_EQ(parsed->clientTimestampUs, state.clientTimestampUs);
    MDR_CHECK_EQ(parsed->dsButtons, state.dsButtons);
    MDR_CHECK_EQ(parsed->emulatorActions, state.emulatorActions);
    MDR_CHECK_EQ(parsed->leftStickX, state.leftStickX);
    MDR_CHECK_EQ(parsed->leftStickY, state.leftStickY);
    MDR_CHECK_EQ(parsed->rightStickX, state.rightStickX);
    MDR_CHECK_EQ(parsed->rightStickY, state.rightStickY);
    MDR_CHECK_EQ(parsed->touchActive, state.touchActive);
    MDR_CHECK_EQ(parsed->touchX, state.touchX);
    MDR_CHECK_EQ(parsed->touchY, state.touchY);
    MDR_CHECK_EQ(parsed->mouseDeltaX, state.mouseDeltaX);
    MDR_CHECK_EQ(parsed->mouseDeltaY, state.mouseDeltaY);
    MDR_CHECK_EQ(parsed->mouseButtons, state.mouseButtons);
    MDR_CHECK_EQ(parsed->leftTrigger, state.leftTrigger);
    MDR_CHECK_EQ(parsed->rightTrigger, state.rightTrigger);
    MDR_CHECK_EQ(parsed->extraButtons, state.extraButtons);
}

MDR_TEST(controller_state_mouse_fields_default_to_zero) {
    // A client with no mouse-capable input source (no gamepad touchpad
    // bound as a mouse, no desktop mouse) should be able to send a
    // ControllerState that just never touches these fields -- confirms
    // the default-constructed values round-trip as zero/released rather
    // than needing every caller to explicitly zero them.
    ControllerState state;
    state.dsButtons = DSButton_A;

    ByteBuffer buf;
    serializeControllerState(buf, state);
    auto parsed = parseControllerState(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK_EQ(parsed->mouseDeltaX, static_cast<int16_t>(0));
    MDR_CHECK_EQ(parsed->mouseDeltaY, static_cast<int16_t>(0));
    MDR_CHECK_EQ(parsed->mouseButtons, static_cast<uint8_t>(0));
    MDR_CHECK_EQ(parsed->leftTrigger, static_cast<uint8_t>(0));
    MDR_CHECK_EQ(parsed->rightTrigger, static_cast<uint8_t>(0));
    MDR_CHECK_EQ(parsed->extraButtons, static_cast<uint8_t>(0));
}

MDR_TEST(controller_state_mouse_deltas_negative_round_trip) {
    // Negative deltas (motion up/left) exercise appendI16/readI16's sign
    // handling specifically, not just appendU16's.
    ControllerState state;
    state.mouseDeltaX = -32768;
    state.mouseDeltaY = -1;

    ByteBuffer buf;
    serializeControllerState(buf, state);
    auto parsed = parseControllerState(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK_EQ(parsed->mouseDeltaX, static_cast<int16_t>(-32768));
    MDR_CHECK_EQ(parsed->mouseDeltaY, static_cast<int16_t>(-1));
}

MDR_TEST(controller_state_no_touch) {
    ControllerState state;
    state.touchActive = 0;
    state.touchX = 0;
    state.touchY = 0;

    ByteBuffer buf;
    serializeControllerState(buf, state);

    auto parsed = parseControllerState(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
    MDR_CHECK_EQ(parsed->touchActive, 0);
}

MDR_TEST(controller_state_rejects_wrong_size) {
    ByteBuffer tooShort(kControllerStateWireSize - 1, 0);
    MDR_CHECK(!parseControllerState(tooShort.data(), tooShort.size()).has_value());

    ByteBuffer tooLong(kControllerStateWireSize + 1, 0);
    MDR_CHECK(!parseControllerState(tooLong.data(), tooLong.size()).has_value());
}

MDR_TEST(controller_state_rejects_out_of_range_touch_when_active) {
    ControllerState state;
    state.touchActive = 1;
    state.touchX = kTouchMaxX + 1; // out of range
    state.touchY = 50;

    ByteBuffer buf;
    serializeControllerState(buf, state);

    auto parsed = parseControllerState(buf.data(), buf.size());
    MDR_CHECK(!parsed.has_value());
}

MDR_TEST(controller_state_allows_out_of_range_touch_when_inactive) {
    // Stale/garbage touch coordinates should not matter if touch isn't active.
    ControllerState state;
    state.touchActive = 0;
    state.touchX = 9999;
    state.touchY = 9999;

    ByteBuffer buf;
    serializeControllerState(buf, state);

    auto parsed = parseControllerState(buf.data(), buf.size());
    MDR_CHECK(parsed.has_value());
}

MDR_TEST(controller_state_rejects_invalid_touch_active_flag) {
    ByteBuffer buf(kControllerStateWireSize, 0);
    // touchActive is the 17th byte (offset 16): 4 (seq) + 8 (ts) + 2 + 2 + 2*4 = 24
    // recompute offset explicitly to avoid a magic number bug in the test itself
    size_t offset = 4 + 8 + 2 + 2 + 2 + 2 + 2 + 2;
    buf[offset] = 2; // invalid: only 0 or 1 allowed

    auto parsed = parseControllerState(buf.data(), buf.size());
    MDR_CHECK(!parsed.has_value());
}

MDR_TEST(build_controller_state_packet_has_correct_type_and_size) {
    ControllerState state;
    ByteBuffer packet = buildControllerStatePacket(state);
    MDR_CHECK_EQ(packet.size(), kPacketHeaderWireSize + kControllerStateWireSize);

    auto header = parseHeader(packet.data(), packet.size());
    MDR_CHECK(header.has_value());
    MDR_CHECK(header->type == PacketType::ControllerState);
    MDR_CHECK_EQ(header->payloadSize, kControllerStateWireSize);

    auto state2 = parseControllerState(packet.data() + kPacketHeaderWireSize,
                                        packet.size() - kPacketHeaderWireSize);
    MDR_CHECK(state2.has_value());
}
