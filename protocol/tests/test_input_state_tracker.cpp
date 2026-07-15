#include "melonds_remote/input_state_tracker.h"
#include "test_framework.h"

using namespace melonds_remote;

namespace {
ControllerState makeState(uint32_t sequence, uint16_t buttons) {
    ControllerState s;
    s.sequence = sequence;
    s.dsButtons = buttons;
    return s;
}
} // namespace

MDR_TEST(tracker_starts_released) {
    InputStateTracker tracker;
    MDR_CHECK_EQ(tracker.currentState().dsButtons, 0);
    MDR_CHECK_EQ(tracker.currentState().touchActive, 0);
    MDR_CHECK(!tracker.hasReceivedAnyPacket());
}

MDR_TEST(tracker_accepts_in_order_packets) {
    InputStateTracker tracker;
    MDR_CHECK(tracker.onPacketReceived(makeState(1, DSButton_A), 1000));
    MDR_CHECK_EQ(tracker.currentState().dsButtons, DSButton_A);

    MDR_CHECK(tracker.onPacketReceived(makeState(2, DSButton_B), 1010));
    MDR_CHECK_EQ(tracker.currentState().dsButtons, DSButton_B);
}

MDR_TEST(tracker_rejects_duplicate_sequence) {
    InputStateTracker tracker;
    MDR_CHECK(tracker.onPacketReceived(makeState(5, DSButton_A), 1000));
    MDR_CHECK(!tracker.onPacketReceived(makeState(5, DSButton_B), 1010));
    // state should remain from the accepted packet
    MDR_CHECK_EQ(tracker.currentState().dsButtons, DSButton_A);
}

MDR_TEST(tracker_rejects_out_of_order_older_packet) {
    InputStateTracker tracker;
    MDR_CHECK(tracker.onPacketReceived(makeState(10, DSButton_A), 1000));
    MDR_CHECK(!tracker.onPacketReceived(makeState(9, DSButton_B), 1010));
    MDR_CHECK_EQ(tracker.currentState().dsButtons, DSButton_A);
}

MDR_TEST(tracker_accepts_sequence_wraparound) {
    InputStateTracker tracker;
    uint32_t nearMax = 0xFFFFFFFFu;
    MDR_CHECK(tracker.onPacketReceived(makeState(nearMax, DSButton_A), 1000));
    // sequence wraps from 0xFFFFFFFF to 0 -- this should still be "newer"
    MDR_CHECK(tracker.onPacketReceived(makeState(0, DSButton_B), 1010));
    MDR_CHECK_EQ(tracker.currentState().dsButtons, DSButton_B);
}

MDR_TEST(tracker_timeout_detection) {
    InputStateTracker tracker;
    // no packets yet -> never "timed out", just not connected
    MDR_CHECK(!tracker.isTimedOut(1'000'000, 500'000));

    MDR_CHECK(tracker.onPacketReceived(makeState(1, DSButton_A), 1'000'000));
    MDR_CHECK(!tracker.isTimedOut(1'200'000, 500'000)); // 200ms later, under 500ms timeout
    MDR_CHECK(tracker.isTimedOut(1'600'000, 500'000));  // 600ms later, over timeout
}

MDR_TEST(tracker_reset_produces_released_state_and_forgets_history) {
    InputStateTracker tracker;
    MDR_CHECK(tracker.onPacketReceived(makeState(100, DSButton_A | DSButton_Start), 1000));

    ControllerState touching;
    touching.sequence = 101;
    touching.touchActive = 1;
    touching.touchX = 50;
    touching.touchY = 60;
    MDR_CHECK(tracker.onPacketReceived(touching, 1010));

    tracker.reset();

    MDR_CHECK_EQ(tracker.currentState().dsButtons, 0);
    MDR_CHECK_EQ(tracker.currentState().touchActive, 0);
    MDR_CHECK(!tracker.hasReceivedAnyPacket());

    // after reset, an old-looking sequence number must be accepted again
    // since this represents a brand new session
    MDR_CHECK(tracker.onPacketReceived(makeState(1, DSButton_B), 2000));
    MDR_CHECK_EQ(tracker.currentState().dsButtons, DSButton_B);
}

MDR_TEST(released_state_clears_every_field) {
    ControllerState released = InputStateTracker::releasedState();
    MDR_CHECK_EQ(released.dsButtons, 0);
    MDR_CHECK_EQ(released.emulatorActions, 0);
    MDR_CHECK_EQ(released.leftStickX, 0);
    MDR_CHECK_EQ(released.leftStickY, 0);
    MDR_CHECK_EQ(released.rightStickX, 0);
    MDR_CHECK_EQ(released.rightStickY, 0);
    MDR_CHECK_EQ(released.touchActive, 0);
}
