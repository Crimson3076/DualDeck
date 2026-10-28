#include "button_latch.h"
#include "test_framework.h"

#include <cstdint>

using dualdeck::client::ButtonReleaseLatch;

MDR_TEST(button_latch_passes_input_through_when_nothing_blocked) {
    ButtonReleaseLatch<uint16_t> latch;
    MDR_CHECK_EQ(latch.filter(0x0005), 0x0005);
}

MDR_TEST(button_latch_hides_menu_button_until_released) {
    ButtonReleaseLatch<uint16_t> latch;
    // B was still held when it closed the menu.
    latch.block(0x0002);
    MDR_CHECK_EQ(latch.filter(0x0002), 0);
    MDR_CHECK_EQ(latch.filter(0x0002), 0);
    // Released, then pressed again for the game: goes through.
    MDR_CHECK_EQ(latch.filter(0x0000), 0);
    MDR_CHECK_EQ(latch.filter(0x0002), 0x0002);
}

MDR_TEST(button_latch_only_blocks_buttons_held_at_close) {
    ButtonReleaseLatch<uint16_t> latch;
    latch.block(0x0002);
    // A different button pressed while B is still held goes through.
    MDR_CHECK_EQ(latch.filter(0x0003), 0x0001);
    // B released, A still held: A unaffected.
    MDR_CHECK_EQ(latch.filter(0x0001), 0x0001);
}

MDR_TEST(button_latch_releases_each_button_independently) {
    ButtonReleaseLatch<uint8_t> latch;
    // The L3+R3 chord: both held when the menu closes.
    latch.block(0x03);
    MDR_CHECK_EQ(latch.filter(0x02), 0);
    // Left was released, so pressing it again counts.
    MDR_CHECK_EQ(latch.filter(0x03), 0x01);
}
