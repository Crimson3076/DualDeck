#include "melonds_remote/touch_mapping.h"

#include <algorithm>

namespace melonds_remote {

RenderRect computeAspectFitRect(double surfaceWidth, double surfaceHeight,
                                 double contentAspect) {
    if (surfaceWidth <= 0 || surfaceHeight <= 0 || contentAspect <= 0) {
        return RenderRect{0, 0, 0, 0};
    }

    double width = surfaceWidth;
    double height = width / contentAspect;

    if (height > surfaceHeight) {
        height = surfaceHeight;
        width = height * contentAspect;
    }

    RenderRect rect;
    rect.width = width;
    rect.height = height;
    rect.x = (surfaceWidth - width) / 2.0;
    rect.y = (surfaceHeight - height) / 2.0;
    return rect;
}

std::optional<std::pair<uint16_t, uint16_t>> mapPointToDSCoords(
    double pointX, double pointY, const RenderRect& rect) {
    if (rect.width <= 0 || rect.height <= 0) {
        return std::nullopt;
    }

    if (pointX < rect.x || pointX >= rect.x + rect.width ||
        pointY < rect.y || pointY >= rect.y + rect.height) {
        return std::nullopt;
    }

    double normX = (pointX - rect.x) / rect.width;
    double normY = (pointY - rect.y) / rect.height;

    double dsXf = normX * (static_cast<double>(kTouchMaxX) + 1.0);
    double dsYf = normY * (static_cast<double>(kTouchMaxY) + 1.0);

    int dsX = static_cast<int>(dsXf);
    int dsY = static_cast<int>(dsYf);

    dsX = std::clamp(dsX, 0, static_cast<int>(kTouchMaxX));
    dsY = std::clamp(dsY, 0, static_cast<int>(kTouchMaxY));

    return std::make_pair(static_cast<uint16_t>(dsX), static_cast<uint16_t>(dsY));
}

} // namespace melonds_remote
