#include "melonds_remote/touch_mapping.h"
#include "test_framework.h"

using namespace melonds_remote;

MDR_TEST(aspect_fit_rect_wide_surface) {
    // Steam Deck panel: 1280x800. Widescreen surface -> height-limited fit.
    RenderRect rect = computeAspectFitRect(1280, 800);
    MDR_CHECK_EQ(rect.height, 800.0);
    MDR_CHECK(rect.width < 1280.0);
    // width/height should be 4:3
    MDR_CHECK(rect.width / rect.height > 1.332 && rect.width / rect.height < 1.334);
    // centered horizontally
    MDR_CHECK_EQ(rect.y, 0.0);
    MDR_CHECK(rect.x > 0.0);
}

MDR_TEST(aspect_fit_rect_tall_surface) {
    RenderRect rect = computeAspectFitRect(800, 1280);
    MDR_CHECK_EQ(rect.width, 800.0);
    MDR_CHECK(rect.height < 1280.0);
    MDR_CHECK_EQ(rect.x, 0.0);
    MDR_CHECK(rect.y > 0.0);
}

MDR_TEST(aspect_fit_rect_degenerate_surface) {
    RenderRect rect = computeAspectFitRect(0, 800);
    MDR_CHECK_EQ(rect.width, 0.0);
    MDR_CHECK_EQ(rect.height, 0.0);
}

MDR_TEST(map_point_inside_rect_top_left_corner) {
    RenderRect rect{100, 50, 1000, 750}; // arbitrary 4:3-ish rect for the math
    auto result = mapPointToDSCoords(100, 50, rect);
    MDR_CHECK(result.has_value());
    MDR_CHECK_EQ(result->first, 0);
    MDR_CHECK_EQ(result->second, 0);
}

MDR_TEST(map_point_inside_rect_center) {
    RenderRect rect{0, 0, 1000, 750};
    auto result = mapPointToDSCoords(500, 375, rect);
    MDR_CHECK(result.has_value());
    // center should map close to (128, 96)
    MDR_CHECK(result->first >= 126 && result->first <= 130);
    MDR_CHECK(result->second >= 94 && result->second <= 98);
}

MDR_TEST(map_point_inside_rect_bottom_right_clamped) {
    RenderRect rect{0, 0, 1000, 750};
    // just inside the bottom-right edge
    auto result = mapPointToDSCoords(999.9, 749.9, rect);
    MDR_CHECK(result.has_value());
    MDR_CHECK_EQ(result->first, kTouchMaxX);
    MDR_CHECK_EQ(result->second, kTouchMaxY);
}

MDR_TEST(map_point_outside_rect_is_ignored) {
    RenderRect rect{100, 100, 500, 375};

    MDR_CHECK(!mapPointToDSCoords(0, 0, rect).has_value());
    MDR_CHECK(!mapPointToDSCoords(99, 200, rect).has_value());
    MDR_CHECK(!mapPointToDSCoords(700, 200, rect).has_value());
    MDR_CHECK(!mapPointToDSCoords(200, 476, rect).has_value());
}

MDR_TEST(map_point_degenerate_rect_never_matches) {
    RenderRect rect{0, 0, 0, 0};
    MDR_CHECK(!mapPointToDSCoords(0, 0, rect).has_value());
}

MDR_TEST(full_screen_letterbox_ignores_touches_in_bars) {
    // Reproduces spec 7.4: 256x192 rendered inside 1280x800 leaves unused
    // vertical space that must not register touches.
    RenderRect rect = computeAspectFitRect(1280, 800);
    // A touch in the top letterbox bar (y=0) must be ignored since rect.y > 0
    // only when width-limited; for 1280x800 height is full so instead check
    // the side bars.
    MDR_CHECK(!mapPointToDSCoords(1.0, 400.0, rect).has_value());
    MDR_CHECK(!mapPointToDSCoords(1279.0, 400.0, rect).has_value());
    // A touch in the middle of the rendered rect must register.
    MDR_CHECK(mapPointToDSCoords(640.0, 400.0, rect).has_value());
}
