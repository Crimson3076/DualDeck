#pragma once

// Touch coordinate mapping between a client's rendered display rectangle
// and the DS touchscreen's native 256x192 coordinate space (spec section
// 7.4). This is pure, transport-independent math so it can be unit tested
// without any windowing or networking involved.

#include <cstdint>
#include <optional>

#include "melonds_remote/protocol.h"

namespace melonds_remote {

// Describes where the aspect-correct DS image rectangle sits within a
// larger window/display surface, in the same pixel units as the raw touch
// event (e.g. Steam Deck panel pixels).
struct RenderRect {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
};

// Computes the largest `contentAspect` (width/height) rectangle that fits
// within `surfaceWidth` x `surfaceHeight`, centered on the surface. This is
// the "aspect-correct fit" display mode and is the default per spec section
// 7.4. `contentAspect` defaults to 4:3, the DS/3DS bottom-screen ratio;
// callers streaming a differently-shaped surface (e.g. the Wii U GamePad's
// 854x480 output) must pass its real aspect ratio instead.
RenderRect computeAspectFitRect(double surfaceWidth, double surfaceHeight,
                                 double contentAspect = 4.0 / 3.0);

// Converts a raw touch/pointer position (in the same coordinate space as
// `rect`, e.g. window pixels) into normalized DS touchscreen coordinates.
//
// Returns std::nullopt if the point falls outside `rect` -- callers must
// ignore touches outside the rendered DS rectangle by default (spec
// section 7.4, step 2), rather than clamping them into range.
std::optional<std::pair<uint16_t, uint16_t>> mapPointToDSCoords(
    double pointX, double pointY, const RenderRect& rect);

} // namespace melonds_remote
