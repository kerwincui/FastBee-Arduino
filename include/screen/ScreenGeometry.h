#pragma once

#include <algorithm>
#include <cstdint>

namespace FastBee {
namespace Screen {

struct ScreenPixelRect {
    int16_t x = 0;
    int16_t y = 0;
    int16_t w = 0;
    int16_t h = 0;
};

inline uint16_t normalizePageRotation(uint16_t rotation) {
    return rotation == 90 || rotation == 180 || rotation == 270 ? rotation : 0;
}

// Page rectangles are persisted in the unrotated logical coordinate space.
// Rotate there first, then scale once into the physical display coordinate
// space so drawing and touch hit testing share identical rounded boundaries.
inline ScreenPixelRect mapScreenRect(uint16_t x, uint16_t y,
                                     uint16_t width, uint16_t height,
                                     uint16_t logicalWidth,
                                     uint16_t logicalHeight,
                                     uint16_t physicalWidth,
                                     uint16_t physicalHeight,
                                     uint16_t rotation) {
    if (!logicalWidth || !logicalHeight || !physicalWidth || !physicalHeight ||
        !width || !height) {
        return {};
    }

    rotation = normalizePageRotation(rotation);
    uint32_t rotatedX = x;
    uint32_t rotatedY = y;
    uint32_t rotatedWidth = width;
    uint32_t rotatedHeight = height;
    uint32_t rotatedCanvasWidth = logicalWidth;
    uint32_t rotatedCanvasHeight = logicalHeight;

    if (rotation == 90) {
        rotatedX = logicalHeight - y - height;
        rotatedY = x;
        rotatedWidth = height;
        rotatedHeight = width;
        rotatedCanvasWidth = logicalHeight;
        rotatedCanvasHeight = logicalWidth;
    } else if (rotation == 180) {
        rotatedX = logicalWidth - x - width;
        rotatedY = logicalHeight - y - height;
    } else if (rotation == 270) {
        rotatedX = y;
        rotatedY = logicalWidth - x - width;
        rotatedWidth = height;
        rotatedHeight = width;
        rotatedCanvasWidth = logicalHeight;
        rotatedCanvasHeight = logicalWidth;
    }

    const uint32_t left = rotatedX * physicalWidth / rotatedCanvasWidth;
    const uint32_t top = rotatedY * physicalHeight / rotatedCanvasHeight;
    const uint32_t right = (rotatedX + rotatedWidth) * physicalWidth /
                           rotatedCanvasWidth;
    const uint32_t bottom = (rotatedY + rotatedHeight) * physicalHeight /
                            rotatedCanvasHeight;
    if (left >= physicalWidth || top >= physicalHeight) return {};

    ScreenPixelRect result;
    result.x = static_cast<int16_t>(left);
    result.y = static_cast<int16_t>(top);
    result.w = static_cast<int16_t>(std::max<uint32_t>(
        1, std::min<uint32_t>(physicalWidth, right) - left));
    result.h = static_cast<int16_t>(std::max<uint32_t>(
        1, std::min<uint32_t>(physicalHeight, bottom) - top));
    return result;
}

inline bool screenRectContains(const ScreenPixelRect& rect,
                               uint16_t x, uint16_t y) {
    return rect.w > 0 && rect.h > 0 && x >= rect.x && y >= rect.y &&
           x < rect.x + rect.w && y < rect.y + rect.h;
}

}  // namespace Screen
}  // namespace FastBee
