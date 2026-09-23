#include <cassert>
#include <cstdint>

#include "screen/ScreenGeometry.h"

using FastBee::Screen::ScreenPixelRect;
using FastBee::Screen::mapScreenRect;
using FastBee::Screen::screenRectContains;

static void expectRect(const ScreenPixelRect& rect, int x, int y, int w, int h) {
    assert(rect.x == x);
    assert(rect.y == y);
    assert(rect.w == w);
    assert(rect.h == h);
}

int main() {
    expectRect(mapScreenRect(10, 20, 30, 40, 100, 200, 100, 200, 0),
               10, 20, 30, 40);
    expectRect(mapScreenRect(10, 20, 30, 40, 100, 200, 200, 100, 90),
               140, 10, 40, 30);
    expectRect(mapScreenRect(10, 20, 30, 40, 100, 200, 100, 200, 180),
               60, 140, 30, 40);
    expectRect(mapScreenRect(10, 20, 30, 40, 100, 200, 200, 100, 270),
               20, 60, 40, 30);

    for (uint16_t rotation : {0, 90, 180, 270}) {
        const bool quarterTurn = rotation == 90 || rotation == 270;
        const uint16_t outputWidth = quarterTurn ? 200 : 100;
        const uint16_t outputHeight = quarterTurn ? 100 : 200;
        const ScreenPixelRect full = mapScreenRect(
            0, 0, 100, 200, 100, 200, outputWidth, outputHeight, rotation);
        expectRect(full, 0, 0, outputWidth, outputHeight);

        const ScreenPixelRect corner = mapScreenRect(
            90, 180, 10, 20, 100, 200, outputWidth, outputHeight, rotation);
        assert(screenRectContains(corner, corner.x, corner.y));
        assert(screenRectContains(
            corner, static_cast<uint16_t>(corner.x + corner.w - 1),
            static_cast<uint16_t>(corner.y + corner.h - 1)));
        assert(!screenRectContains(
            corner, static_cast<uint16_t>(corner.x + corner.w),
            static_cast<uint16_t>(corner.y + corner.h - 1)));
    }

    const ScreenPixelRect scaled = mapScreenRect(
        1, 1, 1, 1, 480, 480, 240, 320, 0);
    assert(scaled.w == 1 && scaled.h == 1);
    return 0;
}
