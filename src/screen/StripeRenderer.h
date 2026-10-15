#ifndef STRIPE_RENDERER_H
#define STRIPE_RENDERER_H

#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_SCREEN_UI && defined(ARDUINO_ARCH_ESP32)

#include <Arduino.h>
#include <U8g2lib.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include "core/ScreenPanelManager.h"
#include "screen/ScreenBackend.h"
#include "screen/ScreenGeometry.h"
#include "screen/ScreenInteraction.h"
#include "screen/StripeChangeCache.h"
#include "screen/WeatherStatusText.h"
#include "screen/PanelStatusText.h"
#include "screen/ScreenThemeVocabulary.h"

namespace FastBee {
namespace Screen {

constexpr uint16_t rgb565(uint8_t red, uint8_t green, uint8_t blue) {
    return static_cast<uint16_t>(((red & 0xF8U) << 8U) |
                                 ((green & 0xFCU) << 3U) |
                                 (blue >> 3U));
}
inline uint16_t blend565(uint16_t foreground, uint16_t background, uint8_t alpha) {
    const uint16_t inverse = 255U - alpha;
    const uint8_t fr = static_cast<uint8_t>(((foreground >> 11U) & 0x1FU) << 3U);
    const uint8_t fg = static_cast<uint8_t>(((foreground >> 5U) & 0x3FU) << 2U);
    const uint8_t fb = static_cast<uint8_t>((foreground & 0x1FU) << 3U);
    const uint8_t br = static_cast<uint8_t>(((background >> 11U) & 0x1FU) << 3U);
    const uint8_t bg = static_cast<uint8_t>(((background >> 5U) & 0x3FU) << 2U);
    const uint8_t bb = static_cast<uint8_t>((background & 0x1FU) << 3U);
    return rgb565(static_cast<uint8_t>((fr * alpha + br * inverse) / 255U),
                  static_cast<uint8_t>((fg * alpha + bg * inverse) / 255U),
                  static_cast<uint8_t>((fb * alpha + bb * inverse) / 255U));
}
// Label color that stays readable on a 5/6/5 fill: light fills get a near
// black label, dark fills stay white. Used for palette-derived action
// buttons so theme switches cannot push contrast below a readable level.
inline uint16_t textOn565(uint16_t fill) {
    const unsigned r = ((fill >> 11U) & 0x1FU) * 255U / 31U;
    const unsigned g = ((fill >> 5U) & 0x3FU) * 255U / 63U;
    const unsigned b = (fill & 0x1FU) * 255U / 31U;
    const unsigned luminance = (299U * r + 587U * g + 114U * b) / 1000U;
    return luminance > 140U ? rgb565(12, 14, 16) : rgb565(255, 255, 255);
}
struct Palette {
    uint16_t background;
    uint16_t backgroundAlt;
    uint16_t card;
    uint16_t cardBorder;
    uint16_t text;
    uint16_t muted;
    uint16_t accent;
    uint16_t success;
    uint16_t warning;
    uint16_t danger;
};

Palette paletteFor(const char* theme);

using PixelRect = ScreenPixelRect;

uint16_t decodeUtf8(const char*& text);

void* allocatePreferred(size_t bytes);

class StripeCanvas;
extern StripeCanvas* activeFontCanvas;

class StripeCanvas {
public:
    StripeCanvas(uint16_t* pixels, uint16_t width, uint16_t fullHeight,
                 uint16_t stripeY, uint16_t stripeHeight)
        : _pixels(pixels), _width(width), _fullHeight(fullHeight),
          _stripeY(stripeY), _stripeHeight(stripeHeight) {
        memset(&_font, 0, sizeof(_font));
        _font.ll_hvline = fontLine;
        _font.cb = U8G2_R0;
        _font.width = width;
        _font.height = fullHeight;
        _font.pixel_buf_width = width;
        _font.pixel_buf_height = stripeHeight;
        _font.pixel_curr_row = stripeY;
        _font.user_x0 = 0;
        _font.user_x1 = width;
        _font.user_y0 = stripeY;
        _font.user_y1 = stripeY + stripeHeight;
        _font.buf_y0 = stripeY;
        _font.buf_y1 = stripeY + stripeHeight;
        // This canvas bypasses SetupBuffer: initialize its clip intersection too.
        // A zeroed intersection suppresses every glyph in U8g2_DrawHVLine.
        u8g2_SetMaxClipWindow(&_font);
        _font.draw_color = 1;
        activeFontCanvas = this;
        u8g2_SetFont(&_font, u8g2_font_wqy12_t_gb2312);
        u8g2_SetFontMode(&_font, 1);
        u8g2_SetFontDirection(&_font, 0);
        u8g2_SetFontPosTop(&_font);
    }

    ~StripeCanvas() {
        if (activeFontCanvas == this) activeFontCanvas = nullptr;
    }

    bool intersectsRows(int32_t top, int32_t bottom) const {
        return top < static_cast<int32_t>(_stripeY + _stripeHeight) &&
               bottom > static_cast<int32_t>(_stripeY);
    }

    void clear(uint16_t color) {
        std::fill(_pixels, _pixels + static_cast<size_t>(_width) * _stripeHeight,
                  color);
    }

    void gradient(uint16_t top, uint16_t bottom) {
        for (uint16_t localY = 0; localY < _stripeHeight; ++localY) {
            const uint16_t globalY = _stripeY + localY;
            const uint8_t alpha = _fullHeight <= 1 ? 0 : static_cast<uint8_t>(
                (static_cast<uint32_t>(globalY) * 255U) / (_fullHeight - 1U));
            const uint16_t color = blend565(bottom, top, alpha);
            std::fill(_pixels + static_cast<size_t>(localY) * _width,
                      _pixels + static_cast<size_t>(localY + 1U) * _width,
                      color);
        }
    }

    void pixel(int16_t x, int16_t y, uint16_t color) {
        if (x < 0 || x >= static_cast<int16_t>(_width) ||
            y < static_cast<int16_t>(_stripeY) ||
            y >= static_cast<int16_t>(_stripeY + _stripeHeight)) return;
        _pixels[static_cast<size_t>(y - _stripeY) * _width + x] = color;
    }

    void fillRect(int16_t x, int16_t y, int16_t width, int16_t height,
                  uint16_t color) {
        if (width <= 0 || height <= 0) return;
        const int16_t x0 = std::max<int16_t>(0, x);
        const int16_t x1 = std::min<int16_t>(_width, x + width);
        const int16_t y0 = std::max<int16_t>(_stripeY, y);
        const int16_t y1 = std::min<int16_t>(_stripeY + _stripeHeight, y + height);
        if (x0 >= x1 || y0 >= y1) return;
        for (int16_t row = y0; row < y1; ++row) {
            std::fill(_pixels + static_cast<size_t>(row - _stripeY) * _width + x0,
                      _pixels + static_cast<size_t>(row - _stripeY) * _width + x1,
                      color);
        }
    }

    void rect(int16_t x, int16_t y, int16_t width, int16_t height,
              uint16_t color) {
        if (width <= 0 || height <= 0) return;
        line(x, y, x + width - 1, y, color);
        line(x, y + height - 1, x + width - 1, y + height - 1, color);
        line(x, y, x, y + height - 1, color);
        line(x + width - 1, y, x + width - 1, y + height - 1, color);
    }

    void roundedRect(int16_t x, int16_t y, int16_t width, int16_t height,
                     uint8_t radius, uint16_t fill, uint16_t border) {
        if (width <= 0 || height <= 0) return;
        radius = static_cast<uint8_t>(std::min<int16_t>(
            radius, std::min<int16_t>(width / 2, height / 2)));
        // Two rounded fills produce one border. Drawing a rectangular border
        // inside the rounded fill left extra vertical lines behind every card.
        const auto spanFill = [&](int16_t left, int16_t top, int16_t w,
                                  int16_t h, int16_t r, uint16_t color) {
            const int16_t first = std::max<int16_t>(top, _stripeY);
            const int16_t last = std::min<int16_t>(top + h, _stripeY + _stripeHeight);
            for (int16_t row = first; row < last; ++row) {
                const int16_t edge = std::min<int16_t>(row - top, top + h - 1 - row);
                int16_t inset = 0;
                if (edge < r) {
                    const int16_t dy = r - edge - 1;
                    while (inset < r && (r - inset - 1) * (r - inset - 1) + dy * dy >= r * r) ++inset;
                }
                fillRect(left + inset, row, w - 2 * inset, 1, color);
            }
        };
        spanFill(x, y, width, height, radius, border);
        if (width > 2 && height > 2)
            spanFill(x + 1, y + 1, width - 2, height - 2,
                     radius > 0 ? radius - 1 : 0, fill);
    }

    void line(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
              uint16_t color) {
        // Reject only the entire primitive. Clipping its endpoints would
        // change Bresenham's error term at stripe boundaries.
        if (!intersectsRows(std::min<int32_t>(y0, y1),
                            std::max<int32_t>(y0, y1) + 1)) return;
        int16_t dx = abs(x1 - x0);
        const int16_t sx = x0 < x1 ? 1 : -1;
        int16_t dy = -abs(y1 - y0);
        const int16_t sy = y0 < y1 ? 1 : -1;
        int16_t error = dx + dy;
        for (;;) {
            pixel(x0, y0, color);
            if (x0 == x1 && y0 == y1) break;
            const int16_t twice = error * 2;
            if (twice >= dy) { error += dy; x0 += sx; }
            if (twice <= dx) { error += dx; y0 += sy; }
        }
    }

    void circle(int16_t centerX, int16_t centerY, int16_t radius,
                uint16_t color, bool filled = false) {
        if (radius <= 0) return;
        if (!intersectsRows(static_cast<int32_t>(centerY) - radius,
                            static_cast<int32_t>(centerY) + radius + 1)) return;
        int16_t x = radius;
        int16_t y = 0;
        int16_t decision = 1 - radius;
        while (x >= y) {
            if (filled) {
                line(centerX - x, centerY + y, centerX + x, centerY + y, color);
                line(centerX - x, centerY - y, centerX + x, centerY - y, color);
                line(centerX - y, centerY + x, centerX + y, centerY + x, color);
                line(centerX - y, centerY - x, centerX + y, centerY - x, color);
            } else {
                pixel(centerX + x, centerY + y, color);
                pixel(centerX + y, centerY + x, color);
                pixel(centerX - y, centerY + x, color);
                pixel(centerX - x, centerY + y, color);
                pixel(centerX - x, centerY - y, color);
                pixel(centerX - y, centerY - x, color);
                pixel(centerX + y, centerY - x, color);
                pixel(centerX + x, centerY - y, color);
            }
            ++y;
            if (decision <= 0) decision += 2 * y + 1;
            else { --x; decision += 2 * (y - x) + 1; }
        }
    }

    // Scale tiers: 1 = wqy12 native (12px), 2 = wqy12 X2 (24px). Tiers 3 and
    // 4 use the larger wqy16 face (X2 = 32px, native = 16px) when
    // FASTBEE_SCREEN_LARGE_FONT is compiled in; without it they degrade to
    // tiers 2 and 1 so smaller-Flash builds keep identical pixels and never
    // reference the large font symbol.
    static constexpr uint8_t renderedTier(uint8_t scale) {
#if FASTBEE_SCREEN_LARGE_FONT
        return scale;
#else
        return scale == 3 ? 2 : scale >= 4 ? 1 : scale;
#endif
    }

    uint16_t text(int16_t x, int16_t y, const char* value, uint16_t color,
                  uint16_t maxWidth, uint8_t scale = 1) {
        if (!value || !value[0] || maxWidth == 0) return 0;
        const uint8_t tier = renderedTier(scale);
        const bool largeFace = tier >= 3;
        const uint8_t drawScale = tier == 3 ? 2 : tier >= 4 ? 1 : tier;
        _fontColor = color;
        uint16_t cursor = static_cast<uint16_t>(std::max<int16_t>(0, x));
        const uint16_t limit = static_cast<uint16_t>(
            std::min<uint32_t>(_width, static_cast<uint32_t>(cursor) + maxWidth));
        const char* current = value;
        bool truncated = false;
        while (*current && cursor < limit) {            const uint16_t codepoint = decodeUtf8(current);
            const uint8_t* font = selectFont(codepoint, largeFace);
            if (!font) {
                const uint16_t replacementWidth = (largeFace ? 14U : 10U) * drawScale;
                if (cursor + replacementWidth > limit) { truncated = true; break; }
                rect(cursor, y, replacementWidth - 2,
                     (largeFace ? 15U : 11U) * drawScale, _fontColor);
                line(cursor + 2, y + 2, cursor + replacementWidth - 4,
                     y + (largeFace ? 13U : 9U) * drawScale - 2, _fontColor);
                cursor += replacementWidth;
                continue;
            }
            u8g2_SetFont(&_font, font);
            const int8_t glyphWidth = u8g2_GetGlyphWidth(&_font, codepoint);
            const uint16_t advance = static_cast<uint16_t>(
                std::max<int16_t>(4, glyphWidth)) * drawScale;
            if (cursor + advance > limit) { truncated = true; break; }
            const u8g2_uint_t drawn = drawScale == 2
                ? u8g2_DrawGlyphX2(&_font, cursor, y, codepoint)
                : u8g2_DrawGlyph(&_font, cursor, y, codepoint);
            cursor += drawn > 0 ? drawn : advance;
        }
        // Text is cut whenever content remains: either an overflow break or
        // the cursor reaching the limit with more string left.
        if (truncated || *current) {
            // Mark the cut: give the ellipsis the last character slot so a
            // clipped label is visibly distinct from a complete one. The
            // wqy12 gb2312 font ships a U+2026 glyph (verified), and the
            // fallback path stays silent when nothing was drawn yet.
            const uint16_t startX = static_cast<uint16_t>(std::max<int16_t>(0, x));
            if (cursor > startX) {
                const uint16_t back = std::min<uint16_t>(
                    cursor - startX, (largeFace ? 16U : 12U) * drawScale);
                const uint8_t* ellipsisFont = selectFont(0x2026U, largeFace);
                if (ellipsisFont) {
                    u8g2_SetFont(&_font, ellipsisFont);
                    if (drawScale == 2) u8g2_DrawGlyphX2(&_font, cursor - back, y, 0x2026U);
                    else u8g2_DrawGlyph(&_font, cursor - back, y, 0x2026U);
                }
            }
        }
        return cursor - static_cast<uint16_t>(std::max<int16_t>(0, x));
    }

    void dim(uint8_t opacity) {
        const size_t count = static_cast<size_t>(_width) * _stripeHeight;
        for (size_t i = 0; i < count; ++i) {
            _pixels[i] = blend565(rgb565(0, 0, 0), _pixels[i], opacity);
        }
    }

private:
    static void fontLine(u8g2_t* font, u8g2_uint_t x, u8g2_uint_t y,
                         u8g2_uint_t length, uint8_t direction) {
        StripeCanvas* canvas = activeFontCanvas;
        if (!canvas || font->draw_color == 0) return;
        for (u8g2_uint_t offset = 0; offset < length; ++offset) {
            const int16_t px = static_cast<int16_t>(x + (direction == 0 ? offset : 0));
            const int16_t py = static_cast<int16_t>(y + (direction == 1 ? offset : 0));
            if (px >= 0 && px < static_cast<int16_t>(canvas->_width) &&
                py >= 0 && py < static_cast<int16_t>(canvas->_stripeHeight)) {
                canvas->_pixels[static_cast<size_t>(py) * canvas->_width + px] =
                    canvas->_fontColor;
            }
        }
    }

    const uint8_t* selectFont(uint16_t codepoint, bool large = false) {
        // The small Chinese example subsets omit ordinary UI characters such
        // as those in navigation/status labels. This flash-resident GB2312
        // font covers configurable Simplified Chinese without a RAM atlas.
        const uint8_t* font = u8g2_font_wqy12_t_gb2312;
#if FASTBEE_SCREEN_LARGE_FONT
        if (large) font = u8g2_font_wqy16_t_gb2312;
#else
        (void)large;
#endif
        u8g2_SetFont(&_font, font);
        return u8g2_IsGlyph(&_font, codepoint) ? font : nullptr;
    }

    uint16_t* _pixels;
    uint16_t _width;
    uint16_t _fullHeight;
    uint16_t _stripeY;
    uint16_t _stripeHeight;
    uint16_t _fontColor = 0xFFFF;
    u8g2_t _font{};
};


enum class NumericControlHit : uint8_t { NONE = 0, DECREMENT, INCREMENT, EXECUTE };

// Fault bits painted as a small warning strip while a fault is active. Normal
// frames pass SCREEN_FAULT_NONE and cost nothing extra.
enum ScreenFaultFlag : uint8_t {
    SCREEN_FAULT_NONE = 0,
    SCREEN_FAULT_RENDER_RETRY = 0x01,
    SCREEN_FAULT_TOUCH = 0x02,
};

class StripeRenderer {
public:
    bool begin(IDisplayBackend* display, const DisplayConfigParams& config,
               String& error) {
        end();
        if (!display || !display->ready()) {
            error = "SCREEN_RENDER_DISPLAY_NOT_READY";
            return false;
        }
        _display = display;
        const DisplayCapabilities capabilities = display->capabilities();
        _width = capabilities.width;
        _height = capabilities.height;
        _lines = std::max<uint16_t>(1, std::min<uint16_t>(
            config.bufferLines, _height));
        if (_width == 0 || _height == 0) {
            error = "SCREEN_RENDER_DIMENSIONS_INVALID";
            return false;
        }
        const size_t pixels = static_cast<size_t>(_width) * _lines;
        _buffer = static_cast<uint16_t*>(allocatePreferred(pixels * sizeof(uint16_t)));
        if (!_buffer) {
            error = "SCREEN_RENDER_BUFFER_ALLOCATION_FAILED";
            return false;
        }
        _bufferBytes = pixels * sizeof(uint16_t);
        if (strcasecmp(config.backend, "rgb") == 0) {
            const size_t count = (_height + _lines - 1U) / _lines;
            const size_t bytes = count * sizeof(StripeChangeCache::Entry);
            _stripeEntries = static_cast<StripeChangeCache::Entry*>(allocatePreferred(bytes));
            if (_stripeEntries) {
                _stripeCache.reset(_stripeEntries, count);
                _bufferBytes += bytes;
            }
        }
        error = "";
        return true;
    }

    void end() {
        _stripeCache.reset();
        if (_stripeEntries) heap_caps_free(_stripeEntries);
        _stripeEntries = nullptr;
        if (_buffer) heap_caps_free(_buffer);
        _buffer = nullptr;
        _bufferBytes = 0;
        _display = nullptr;
        _width = _height = _lines = 0;
        _navigationOpen = false;
    }

    ~StripeRenderer() { end(); }

    size_t bufferBytes() const { return _bufferBytes; }
    uint16_t width() const { return _width; }
    uint16_t height() const { return _height; }
    void invalidate() { _stripeCache.invalidate(); }
    void setPressed(const char* cardId, uint16_t x = 0, uint16_t y = 0) {
        snprintf(_pressedCard, sizeof(_pressedCard), "%s", cardId ? cardId : "");
        _pressedX = x; _pressedY = y;
    }
    bool controlPressed(const ScreenPanelCardSnapshot& card, const PixelRect& bounds) const {
        return _pressedCard[0] && !strcmp(card.id, _pressedCard) &&
            screenRectContains(bounds, _pressedX, _pressedY);
    }

    bool render(const ScreenPanelSnapshot& snapshot,
                const char* confirmationCardId, String& error) {
        return render(snapshot, confirmationCardId, error, SCREEN_FAULT_NONE);
    }

    bool render(const ScreenPanelSnapshot& snapshot,
                const char* confirmationCardId, String& error,
                uint8_t faultFlags) {
        if (!_display || !_buffer || !_display->ready()) {
            error = "SCREEN_RENDER_NOT_READY";
            return false;
        }
        const uint8_t pageIndex = snapshot.pageCount == 0 ? 0 :
            std::min<uint8_t>(snapshot.activePageIndex, snapshot.pageCount - 1U);
        const ScreenPanelPageSnapshot* page = snapshot.pageCount > 0
            ? &snapshot.pages[pageIndex] : nullptr;
        const Palette palette = paletteFor(
            page && page->themeId[0] ? page->themeId : snapshot.themeId);
        for (uint16_t stripeY = 0; stripeY < _height; stripeY += _lines) {
            const uint16_t stripeHeight = std::min<uint16_t>(_lines, _height - stripeY);
            StripeCanvas canvas(_buffer, _width, _height, stripeY, stripeHeight);
            const char* background = page && page->backgroundPresetId[0]
                ? page->backgroundPresetId : snapshot.backgroundPresetId;
            if (background && strcmp(background, kScreenBackgroundSoftGradient) == 0) {
                canvas.gradient(palette.background, palette.backgroundAlt);
            } else {
                canvas.clear(palette.background);
            }
            drawBackgroundDecoration(canvas, background, palette);
            if (page) drawPage(canvas, snapshot, *page, palette);
            else drawEmpty(canvas, palette);
            if (faultFlags != SCREEN_FAULT_NONE)
                drawFaultIndicator(canvas, palette, faultFlags);
#if FASTBEE_SCREEN_WORKER_ISOLATION == 1
            // Page titles are metadata in the normal card layout. Paint the
            // temporary phase label explicitly so observers can identify it.
            if (page && (strcmp(page->title, "A: HOLD") == 0 ||
                         strcmp(page->title, "B: SNAPSHOT") == 0 ||
                         strcmp(page->title, "C: RENDER") == 0)) {
                canvas.fillRect(0, 0, _width, 18, palette.background);
                canvas.text(4, 2, page->title, palette.warning, _width - 8, 1);
            }
#endif
            if (confirmationCardId && confirmationCardId[0]) {
                drawConfirmation(canvas, snapshot, confirmationCardId, palette);
            }
            if (_navigationOpen) drawPageMenu(canvas, snapshot, palette);
            const size_t pixelCount = static_cast<size_t>(_width) * stripeHeight;
            if (!_stripeCache.submit(stripeY / _lines, _buffer, pixelCount, [&]() {
                    return _display->pushPixels(0, stripeY, _width, stripeHeight,
                                                _buffer, pixelCount);
                })) {
                error = "SCREEN_RENDER_FLUSH_FAILED";
                return false;
            }
        }
        error = "";
        return true;
    }

    int hitTest(const ScreenPanelSnapshot& snapshot, uint16_t x, uint16_t y) const {
        if (_navigationOpen) return -1;
        if (y >= contentHeight()) return -1;
        if (snapshot.pageCount == 0) return -1;
        const uint8_t pageIndex = std::min<uint8_t>(
            snapshot.activePageIndex, snapshot.pageCount - 1U);
        const ScreenPanelPageSnapshot& page = snapshot.pages[pageIndex];
        for (int index = page.cardCount - 1; index >= 0; --index) {
            const size_t cardIndex = page.firstCard + static_cast<size_t>(index);
            if (cardIndex >= snapshot.cardCount) continue;
            const ScreenPanelCardSnapshot& card = snapshot.cards[cardIndex];
            // Trend charts answer taps even though panel config never marks
            // them interactive; a still tap selects the nearest plotted sample.
            if (!card.interactive && card.visualType != ScreenVisualType::LINE) continue;
            const PixelRect rect = mapRect(card.rect, page);
            if (screenRectContains(rect, x, y) && actionHitTest(snapshot, card, x, y)) {
                return static_cast<int>(cardIndex);
            }
        }
        return -1;
    }

    // Map a tap inside a trend chart to the index of the nearest plotted
    // sample, using the same axis math as the plot so the selection always
    // matches the dot under the finger. -1 when nothing is plottable.
    int historyTapIndex(const ScreenPanelCardSnapshot& card, const PixelRect& rect,
                        uint16_t tapX) const {
        const int16_t width = rect.w - 62;
        if (!card.historyCount || width < 20) return -1;
        const HistoryAxis axis = historyAxis(card);
        int best = 0;
        int32_t bestDistance = -1;
        for (uint8_t i = 0; i < card.historyCount; ++i) {
            const int16_t pointX = static_cast<int16_t>(
                rect.x + 48 + historyFraction(card, i, axis) * width);
            const int32_t distance = std::abs(static_cast<int32_t>(tapX) - pointX);
            if (bestDistance < 0 || distance < bestDistance) {
                bestDistance = distance;
                best = i;
            }
        }
        return best;
    }

    static constexpr uint16_t FOOTER_HEIGHT = 72;
    uint16_t contentHeight() const {
        return _height > FOOTER_HEIGHT + 24 ? _height - FOOTER_HEIGHT : _height;
    }
    uint8_t navigationCount(const ScreenPanelSnapshot& snapshot) const {
        if (snapshot.pageCount > 5) return 5;
        return std::min<uint8_t>(5, snapshot.pageCount);
    }
    int navigationPageIndex(const ScreenPanelSnapshot& snapshot, uint8_t tab) const {
        if (tab >= navigationCount(snapshot)) return -1;
        if (snapshot.pageCount > 5 && tab == 4) return -2;
        // Home remains a reliable first target. Apply user order to the
        // remaining shortcuts, then fill unused slots without blank targets.
        std::array<uint8_t, ScreenPanelSnapshot::MAX_PAGES> indices{};
        uint8_t count = 1;
        const auto append = [&](uint8_t index) {
            for (uint8_t i = 0; i < count; ++i) if (indices[i] == index) return;
            if (count < indices.size()) indices[count++] = index;
        };
        for (uint8_t configured = 0; configured < snapshot.bottomNavCount &&
             configured < ScreenPanelSnapshot::MAX_BOTTOM_NAV_PAGES; ++configured) {
            for (uint8_t i = 0; i < snapshot.pageCount; ++i)
                if (strcmp(snapshot.pages[i].id, snapshot.bottomNavPageIds[configured].data()) == 0) append(i);
        }
        for (uint8_t i = 0; i < snapshot.pageCount; ++i) append(i);
        return tab < count ? indices[tab] : -1;
    }
    bool navigationOpen() const { return _navigationOpen; }
    void toggleNavigation() { _navigationOpen = !_navigationOpen; }
    void closeNavigation() { _navigationOpen = false; }
    PixelRect pageMenuRect(uint8_t page) const {
        const int columns = _width >= 240 ? 2 : 1;
        const int width = (_width - 24 - (columns - 1) * 8) / columns;
        const int rows = (ScreenPanelSnapshot::MAX_PAGES + columns - 1) / columns;
        const int height = std::min<int>(64, (contentHeight() - 42) / rows - 6);
        return {static_cast<int16_t>(12 + (page % columns) * (width + 8)),
                static_cast<int16_t>(36 + (page / columns) * (height + 6)),
                static_cast<int16_t>(width), static_cast<int16_t>(height)};
    }
    int pageMenuHit(const ScreenPanelSnapshot& snapshot, uint16_t x, uint16_t y) const {
        if (!_navigationOpen) return -1;
        for (uint8_t i = 0; i < snapshot.pageCount; ++i)
            if (screenRectContains(pageMenuRect(i), x, y)) return i;
        return -1;
    }
    bool homeHitTest(const ScreenPanelSnapshot& snapshot, uint16_t x, uint16_t y) const {
        return navigationPageIndex(snapshot, pageNavigationHit(snapshot, x, y)) == 0;
    }
    int pageNavigationHit(const ScreenPanelSnapshot& snapshot, uint16_t x, uint16_t y) const {
        if (_height <= FOOTER_HEIGHT + 24 || y < contentHeight() + 4 || y >= _height - 4 || !_width) return -1;
        const uint8_t tabCount = navigationCount(snapshot);
        if (!tabCount) return -1;
        const int tab = static_cast<int>(x) * tabCount / _width;
        return tab >= 0 && tab < tabCount ? tab : -1;
    }

    bool actionHitTest(const ScreenPanelSnapshot& snapshot,
                       const ScreenPanelCardSnapshot& card,
                       uint16_t x, uint16_t y) const {
        const PixelRect bounds = cardBounds(snapshot, card);
        if (card.kind == ScreenCardKind::PAGE_LINK) return screenRectContains(navigationButton(bounds), x, y);
        if (card.visualType == ScreenVisualType::MOTOR_CONTROL) {
            const ScreenMotorLayout layout{bounds};
            if (!layout.valid()) return false;
            if (screenRectContains(layout.stop(), x, y) ||
                screenRectContains(layout.direction(0), x, y) ||
                screenRectContains(layout.direction(1), x, y)) return true;
            for (unsigned row = 0; row < 2; ++row)
                for (unsigned col = 0; col < 3; ++col)
                    if (screenRectContains(layout.adjust(row, col), x, y)) return true;
            return false;
        }
        // A trend chart's whole surface selects the nearest plotted sample.
        if (card.visualType == ScreenVisualType::LINE) return true;
        if (card.kind == ScreenCardKind::DATA ||
            card.kind == ScreenCardKind::CHART ||
            card.kind == ScreenCardKind::CLOCK ||
            card.kind == ScreenCardKind::WEATHER) return false;
        if (card.visualType == ScreenVisualType::SLIDER) {
            return x >= bounds.x + 14 && x < bounds.x + bounds.w - 14 &&
                   y >= bounds.y + bounds.h - 50 && y < bounds.y + bounds.h - 4;
        }
        if (card.visualType == ScreenVisualType::SELECT) {
            return screenRectContains(screenSelectButton(bounds, false), x, y) ||
                   screenRectContains(screenSelectButton(bounds, true), x, y);
        }
        if (card.visualType == ScreenVisualType::ACTION) {
            const int16_t buttonY = bounds.y + std::max<int16_t>(34, bounds.h / 2 - 1);
            const int16_t buttonH = std::min<int16_t>(44, bounds.h - (buttonY - bounds.y) - 6);
            return buttonH > 0 && screenRectContains({static_cast<int16_t>(bounds.x + 10), buttonY,
                                                       static_cast<int16_t>(bounds.w - 20), buttonH}, x, y);
        }
        if (card.visualType == ScreenVisualType::SWITCH) {
            // The whole card is the toggle hot zone (>=44px even on small
            // legacy cards); the pill stays as the visual anchor only.
            return true;
        }
        if (card.visualType == ScreenVisualType::NUMBER_CONTROL) {
            return numericControlHitTest(snapshot, card, x, y) != NumericControlHit::NONE;
        }
        return false;
    }
    bool feedbackHitTest(const ScreenPanelSnapshot& snapshot, uint16_t x, uint16_t y) const {
        if (_navigationOpen) return pageMenuHit(snapshot, x, y) >= 0 || pageNavigationHit(snapshot, x, y) >= 0;
        if (pageNavigationHit(snapshot, x, y) >= 0) return true;
        if (!snapshot.pageCount || y >= contentHeight()) return false;
        const int hit = hitTest(snapshot, x, y);
        return hit >= 0 && !(snapshot.cards[hit].kind == ScreenCardKind::SYSTEM &&
            !strcmp(snapshot.cards[hit].systemActionId, "sleep"));
    }

    // Capture a specific control, never its containing card. A release in a
    // different button, or a press beginning in padding, cannot issue commands.
    int tapTarget(const ScreenPanelSnapshot& snapshot, uint16_t x, uint16_t y,
                  const char* confirmationCardId) const {
        const int tab = pageNavigationHit(snapshot, x, y);
        if (tab >= 0) return 0x1000 + tab;
        if (_navigationOpen) {
            const int page = pageMenuHit(snapshot, x, y);
            return page >= 0 ? 0x1100 + page : 0;
        }
        if (confirmationCardId && confirmationCardId[0]) {
            bool motor = false;
            for (uint8_t i = 0; i < snapshot.cardCount; ++i)
                if (!strcmp(snapshot.cards[i].id, confirmationCardId))
                    motor = snapshot.cards[i].visualType == ScreenVisualType::MOTOR_CONTROL;
            if (motor && confirmationStopHit(x, y)) return 0x1200;
            bool confirmed = false;
            return confirmationChoice(x, y, confirmed, motor) ? 0x1201 + confirmed : 0;
        }
        const int index = hitTest(snapshot, x, y);
        if (index < 0) return 0;
        const auto& card = snapshot.cards[index];
        const PixelRect bounds = cardBounds(snapshot, card);
        int control = 1;
        if (card.visualType == ScreenVisualType::MOTOR_CONTROL) {
            const ScreenMotorLayout layout{bounds};
            if (screenRectContains(layout.stop(), x, y)) control = 3;
            for (unsigned col = 0; col < 2; ++col)
                if (screenRectContains(layout.direction(col), x, y)) control = 1 + col;
            for (unsigned row = 0; row < 2; ++row) for (unsigned col = 0; col < 3; ++col)
                if (screenRectContains(layout.adjust(row, col), x, y)) control = 4 + row * 3 + col;
        } else if (card.visualType == ScreenVisualType::SELECT) {
            control = screenRectContains(screenSelectButton(bounds, true), x, y) ? 2 : 1;
        } else if (card.visualType == ScreenVisualType::NUMBER_CONTROL) {
            control = static_cast<int>(numericControlHitTest(snapshot, card, x, y));
        }
        return 1 + index * 16 + control;
    }

    PixelRect cardBounds(const ScreenPanelSnapshot& snapshot, const ScreenPanelCardSnapshot& card) const {
        return mapRect(card.rect, snapshot.pages[snapshot.activePageIndex]);
    }

    NumericControlHit numericControlHitTest(
            const ScreenPanelSnapshot& snapshot,
            const ScreenPanelCardSnapshot& card,
            uint16_t x, uint16_t y) const {
        if (snapshot.pageCount == 0 ||
            card.visualType != ScreenVisualType::NUMBER_CONTROL) {
            return NumericControlHit::NONE;
        }
        const uint8_t pageIndex = std::min<uint8_t>(
            snapshot.activePageIndex, snapshot.pageCount - 1U);
        const PixelRect rect = mapRect(card.rect, snapshot.pages[pageIndex]);
        const int16_t executeTop = rect.y + rect.h - 38;
        if (x >= rect.x + 10 && x < rect.x + rect.w - 10 &&
            y >= executeTop && y < executeTop + 29) {
            return NumericControlHit::EXECUTE;
        }
        const int16_t stepTop = rect.y + 31;
        // Step buttons are drawn 44x44; keep the hit region identical to the
        // painted buttons so touches outside a button never register.
        const int16_t stepBottom = std::min<int16_t>(executeTop - 4, stepTop + 44);
        if (y < stepTop || y >= stepBottom) return NumericControlHit::NONE;
        if (x >= rect.x + 10 && x < rect.x + 54) {
            return NumericControlHit::DECREMENT;
        }
        if (x >= rect.x + rect.w - 54 && x < rect.x + rect.w - 10) {
            return NumericControlHit::INCREMENT;
        }
        return NumericControlHit::NONE;
    }

    bool confirmationChoice(uint16_t x, uint16_t y, bool& confirmed, bool motor = false) const {
        confirmed = screenRectContains(confirmationButton(motor ? 2 : 1, motor ? 3 : 2), x, y);
        return confirmed || screenRectContains(confirmationButton(0, motor ? 3 : 2), x, y);
    }
    bool confirmationStopHit(uint16_t x, uint16_t y) const {
        return screenRectContains(confirmationButton(1, 3), x, y);
    }

private:
    PixelRect mapRect(const ScreenPanelRect& input,
                      const ScreenPanelPageSnapshot& page) const {
        return mapScreenRect(
            input.x, input.y, input.w, input.h,
            page.logicalWidth ? page.logicalWidth : _width,
            page.logicalHeight ? page.logicalHeight : _height,
            _width, contentHeight(), page.rotation);
    }

    PixelRect confirmationRect() const {
        const int16_t width = std::min<int16_t>(380, _width - 24);
        const int16_t height = std::min<int16_t>(212, _height - 24);
        return {static_cast<int16_t>((_width - width) / 2),
                static_cast<int16_t>((_height - height) / 2), width, height};
    }
    PixelRect confirmationButton(unsigned index, unsigned count) const {
        const auto dialog = confirmationRect();
        const int width = (dialog.w - 24 - (count - 1) * 10) / count;
        return {static_cast<int16_t>(dialog.x + 12 + index * (width + 10)),
                static_cast<int16_t>(dialog.y + dialog.h - 56), static_cast<int16_t>(width), 44};
    }

    static uint16_t qualityColor(ScreenValueQuality quality,
                                 const Palette& palette) {
        switch (quality) {
            case ScreenValueQuality::GOOD: return palette.success;
            case ScreenValueQuality::STALE: return palette.warning;
            case ScreenValueQuality::ERROR: return palette.danger;
            case ScreenValueQuality::OFFLINE: return palette.muted;
            default: return palette.muted;
        }
    }

    static const char* qualityText(ScreenValueQuality quality) {
        switch (quality) {
            case ScreenValueQuality::GOOD: return "";
            case ScreenValueQuality::STALE: return "数据已过期";
            case ScreenValueQuality::OFFLINE: return "离线";
            case ScreenValueQuality::ERROR: return "读取失败";
            default: return "等待数据";
        }
    }

    bool drawCardIcon(StripeCanvas& canvas, const char* iconId,
                      int16_t centerX, int16_t centerY,
                      const Palette& palette) const {
        if (!iconId || !iconId[0]) return false;
        String id(iconId);
        id.toLowerCase();
        const uint16_t color = palette.accent;

        if (id == "temperature") {
            canvas.circle(centerX, centerY + 4, 3, color, true);
            canvas.line(centerX, centerY - 6, centerX, centerY + 3, color);
            canvas.line(centerX + 1, centerY - 6, centerX + 1,
                        centerY + 3, color);
        } else if (id == "humidity") {
            canvas.line(centerX, centerY - 7, centerX - 5, centerY + 1, color);
            canvas.line(centerX - 5, centerY + 1, centerX, centerY + 7, color);
            canvas.line(centerX, centerY + 7, centerX + 5, centerY + 1, color);
            canvas.line(centerX + 5, centerY + 1, centerX, centerY - 7, color);
        } else if (id == "light") {
            canvas.circle(centerX, centerY - 1, 4, color, false);
            canvas.line(centerX - 3, centerY + 5, centerX + 3,
                        centerY + 5, color);
            canvas.line(centerX, centerY - 8, centerX, centerY - 6, color);
        } else if (id == "power") {
            canvas.circle(centerX, centerY, 7, color, false);
            canvas.line(centerX, centerY - 8, centerX, centerY + 1, color);
        } else if (id == "sound") {
            canvas.rect(centerX - 7, centerY - 3, 4, 7, color);
            canvas.line(centerX - 3, centerY - 3, centerX + 1, centerY - 7, color);
            canvas.line(centerX + 1, centerY - 7, centerX + 1, centerY + 7, color);
            canvas.line(centerX + 1, centerY + 7, centerX - 3, centerY + 3, color);
            canvas.line(centerX + 5, centerY - 4, centerX + 7, centerY, color);
            canvas.line(centerX + 7, centerY, centerX + 5, centerY + 4, color);
        } else if (id == "sleep") {
            canvas.circle(centerX, centerY, 7, color, false);
            canvas.circle(centerX + 4, centerY - 3, 6, palette.card, true);
        } else if (id == "switch") {
            canvas.roundedRect(centerX - 7, centerY - 4, 14, 8, 3,
                               palette.backgroundAlt, color);
            canvas.circle(centerX + 3, centerY, 3, color, true);
        } else if (id == "wifi") {
            canvas.line(centerX - 7, centerY - 2, centerX, centerY - 6, color);
            canvas.line(centerX, centerY - 6, centerX + 7, centerY - 2, color);
            canvas.line(centerX - 4, centerY + 1, centerX, centerY - 1, color);
            canvas.line(centerX, centerY - 1, centerX + 4, centerY + 1, color);
            canvas.circle(centerX, centerY + 5, 2, color, true);
        } else if (id == "home") {
            canvas.line(centerX - 7, centerY, centerX, centerY - 6, color);
            canvas.line(centerX, centerY - 6, centerX + 7, centerY, color);
            canvas.rect(centerX - 5, centerY, 10, 7, color);
        } else if (id == "chart") {
            canvas.rect(centerX - 7, centerY + 2, 3, 5, color);
            canvas.rect(centerX - 2, centerY - 2, 3, 9, color);
            canvas.rect(centerX + 3, centerY - 6, 3, 13, color);
        } else if (id == "clock") {
            canvas.circle(centerX, centerY, 7, color, false);
            canvas.line(centerX, centerY, centerX, centerY - 4, color);
            canvas.line(centerX, centerY, centerX + 4, centerY + 2, color);
        } else if (id == "warning") {
            canvas.line(centerX, centerY - 7, centerX - 7, centerY + 6, color);
            canvas.line(centerX - 7, centerY + 6, centerX + 7,
                        centerY + 6, color);
            canvas.line(centerX + 7, centerY + 6, centerX, centerY - 7, color);
            canvas.line(centerX, centerY - 2, centerX, centerY + 2, color);
            canvas.pixel(centerX, centerY + 4, color);
        } else {
            // A configured but unknown icon remains visible instead of being
            // silently discarded, which also exposes Web/firmware drift.
            canvas.line(centerX, centerY - 7, centerX + 7, centerY, color);
            canvas.line(centerX + 7, centerY, centerX, centerY + 7, color);
            canvas.line(centerX, centerY + 7, centerX - 7, centerY, color);
            canvas.line(centerX - 7, centerY, centerX, centerY - 7, color);
            canvas.text(centerX - 3, centerY - 5, "?", color, 7, 1);
        }
        return true;
    }

    void drawBackgroundDecoration(StripeCanvas& canvas, const char* preset,
                                  const Palette& palette) const {
        if (!preset || strcmp(preset, kScreenBackgroundGrid) != 0) return;
        const uint16_t color = blend565(palette.cardBorder, palette.background, 96);
        for (int16_t x = 0; x < _width; x += 28)
            canvas.line(x, 0, x, _height - 1, color);
        for (int16_t y = 0; y < _height; y += 28)
            canvas.line(0, y, _width - 1, y, color);
    }

    void drawEmpty(StripeCanvas& canvas, const Palette& palette) const {
        canvas.text(18, _height / 2 - 28, "暂无面板内容", palette.muted,
                    _width - 36, 2);
        canvas.text(18, _height / 2 + 4, "请在 Web 端「屏幕面板」添加页面与卡片",
                    palette.muted, _width - 36, 1);
    }

    void drawFaultIndicator(StripeCanvas& canvas, const Palette& palette,
                            uint8_t faultFlags) const {
        const char* labels[2];
        uint8_t count = 0;
        if (faultFlags & SCREEN_FAULT_RENDER_RETRY) labels[count++] = "渲染异常";
        if (faultFlags & SCREEN_FAULT_TOUCH) labels[count++] = "触摸异常";
        if (!count) return;
        int labelWidth = (count - 1) * 6;
        for (uint8_t i = 0; i < count; ++i)
            for (const unsigned char* p = reinterpret_cast<const unsigned char*>(labels[i]); *p; ++p)
                if ((*p & 0xc0) != 0x80) labelWidth += *p < 0x80 ? 6 : 12;
        const int16_t x = static_cast<int16_t>(_width - 8 - labelWidth);
        canvas.fillRect(x - 4, 2, labelWidth + 8, 16,
                        blend565(palette.warning, palette.background, 40));
        canvas.text(x, 4, labels[0], palette.warning, labelWidth, 1);
        if (count > 1) {
            int firstWidth = 0;
            for (const unsigned char* p = reinterpret_cast<const unsigned char*>(labels[0]); *p; ++p)
                if ((*p & 0xc0) != 0x80) firstWidth += *p < 0x80 ? 6 : 12;
            canvas.text(x + firstWidth + 6, 4, labels[1], palette.warning,
                        labelWidth - firstWidth - 6, 1);
        }
    }

    void drawPage(StripeCanvas& canvas, const ScreenPanelSnapshot& snapshot,
                  const ScreenPanelPageSnapshot& page,
                  const Palette& palette) const {
        for (uint8_t offset = 0; offset < page.cardCount; ++offset) {
            const size_t index = page.firstCard + offset;
            if (index >= snapshot.cardCount) break;
            drawCard(canvas, snapshot.cards[index], mapRect(snapshot.cards[index].rect, page),
                     palette);
        }
        if (_height > 104) {
            const int16_t top = contentHeight();
            canvas.fillRect(0, top, _width, _height - top, palette.backgroundAlt);
            canvas.line(0, top, _width - 1, top, palette.cardBorder);
            const uint8_t tabCount = navigationCount(snapshot);
            if (tabCount) {
                const int tabWidth = std::max<int>(1, _width / tabCount);
                for (uint8_t tab = 0; tab < tabCount; ++tab) {
                    const int pageIndex = navigationPageIndex(snapshot, tab);
                    if (pageIndex == -1) continue;
                    const int left = tab * tabWidth;
                    const int right = tab == tabCount - 1 ? _width : (tab + 1) * tabWidth;
                    const bool selected = pageIndex == -2 ? _navigationOpen : static_cast<uint8_t>(pageIndex) == snapshot.activePageIndex;
                    const uint16_t color = selected ? palette.accent : palette.text;
                    if (selected) canvas.roundedRect(left + 5, top + 6, right - left - 10,
                                                     std::max<int>(1, _height - top - 12), 8,
                                                     blend565(palette.accent, palette.card, 24), palette.accent);
                    const int center = (left + right) / 2;
                    if (pageIndex == 0) drawCardIcon(canvas, "home", center, top + 23, palette);
                    else if (pageIndex == -2) {
                        for (int dot = -1; dot <= 1; ++dot) canvas.circle(center + dot * 7, top + 23, 2, color, true);
                    } else {
                        canvas.roundedRect(center - 8, top + 15, 16, 16, 3, palette.backgroundAlt, color);
                        canvas.line(center - 4, top + 21, center + 4, top + 21, color);
                        canvas.line(center - 4, top + 26, center + 2, top + 26, color);
                    }
                    const char* label = pageIndex == -2 ? "更多" : snapshot.pages[pageIndex].title;
                    int labelWidth = 0;
                    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(label); *p; ++p)
                        if ((*p & 0xc0) != 0x80) labelWidth += *p < 0x80 ? 6 : 12;
                    const int labelX = left + std::max(5, (right - left - labelWidth) / 2);
                    canvas.text(labelX, top + 44, label, color, right - labelX - 5, 1);
                }
            }
        }
    }

    void drawPageMenu(StripeCanvas& canvas, const ScreenPanelSnapshot& snapshot,
                      const Palette& palette) const {
        canvas.fillRect(0, 0, _width, contentHeight(), palette.background);
        canvas.text(14, 12, "切换页面", palette.text, _width - 28, 1);
        for (uint8_t i = 0; i < snapshot.pageCount; ++i) {
            const auto rect = pageMenuRect(i);
            const bool selected = i == snapshot.activePageIndex;
            canvas.roundedRect(rect.x, rect.y, rect.w, rect.h, 8, palette.card,
                               selected ? palette.accent : palette.cardBorder);
            canvas.text(rect.x + 12, rect.y + (rect.h - 12) / 2,
                        snapshot.pages[i].title, selected ? palette.accent : palette.text, rect.w - 24, 1);
        }
    }

    void drawCard(StripeCanvas& canvas, const ScreenPanelCardSnapshot& card,
                  const PixelRect& rect, const Palette& palette) const {
        if (rect.w < 8 || rect.h < 8) return;
        // Normal cards contain every label/icon within their vertical bounds.
        // Small legacy cards can overflow those bounds, so retain their
        // existing pixel clipping instead of dropping visible overflow.
        if (rect.h >= 96 && !canvas.intersectsRows(rect.y,
                static_cast<int32_t>(rect.y) + rect.h)) return;
        canvas.roundedRect(rect.x, rect.y, rect.w, rect.h, 9, palette.card, palette.cardBorder);
        if (rect.w >= 96 && rect.h >= 80) {
            // Quiet surfaces keep the live values and command affordances prominent.
            canvas.line(rect.x + 12, rect.y + 29, rect.x + rect.w - 13, rect.y + 29,
                        blend565(palette.cardBorder, palette.card, 80));
        }
        const int16_t left = rect.x + 9;
        const int16_t top = rect.y + 8;
        const char* icon = card.iconId;
        if (!icon[0] && rect.w >= 140) {
            if (card.kind == ScreenCardKind::CLOCK) icon = "clock";
            else if (card.kind == ScreenCardKind::CHART) icon = "chart";
            else if (card.kind == ScreenCardKind::PAGE_LINK) icon = "home";
            else if (card.kind == ScreenCardKind::SYSTEM) icon = !strcmp(card.systemActionId, "brightness") ? "light" :
                !strcmp(card.systemActionId, "touch-beep") ? "sound" : !strcmp(card.systemActionId, "sleep") ? "sleep" :
                !strcmp(card.systemActionId, "sleep-time") || !strcmp(card.systemActionId, "confirm-idle") ? "clock" : "power";
            else if (card.kind == ScreenCardKind::CONTROL) icon = "switch";
        }
        const bool hasIcon = drawCardIcon(canvas, icon,
                                          rect.x + 16, rect.y + 14, palette);
        const int16_t titleX = left + (hasIcon ? 18 : 0);
        const int16_t titleRight = rect.x + rect.w - 18;
        const uint16_t titleWidth = static_cast<uint16_t>(
            std::max<int16_t>(1, titleRight - titleX));
        canvas.text(titleX, top, card.title[0] ? card.title : "未命名",
                    palette.text, titleWidth, rect.h >= 120 ? 4 : 1);
        if (card.kind == ScreenCardKind::DATA || card.kind == ScreenCardKind::CHART || card.kind == ScreenCardKind::WEATHER)
            canvas.circle(rect.x + rect.w - 10, rect.y + 13, 3,
                          qualityColor(card.quality, palette), true);

        switch (card.visualType) {
            case ScreenVisualType::WEATHER:
                drawWeather(canvas, card, rect, palette);
                break;
            case ScreenVisualType::CLOCK:
                drawClock(canvas, rect, palette);
                break;
            case ScreenVisualType::GAUGE:
                drawGauge(canvas, card, rect, palette);
                break;
            case ScreenVisualType::BAR:
                drawBar(canvas, card, rect, palette);
                break;
            case ScreenVisualType::LINE:
                drawLineChart(canvas, card, rect, palette);
                break;
            case ScreenVisualType::SWITCH:
                drawSwitch(canvas, card, rect, palette);
                break;
            case ScreenVisualType::ACTION:
                drawAction(canvas, card, rect, palette);
                break;
            case ScreenVisualType::MOTOR_CONTROL:
                drawMotor(canvas, card, rect, palette);
                break;
            case ScreenVisualType::SLIDER:
                drawSlider(canvas, card, rect, palette);
                break;
            case ScreenVisualType::SELECT:
                drawSleepSelect(canvas, card, rect, palette);
                break;
            case ScreenVisualType::NUMBER_CONTROL:
                drawNumberControl(canvas, card, rect, palette);
                break;
            case ScreenVisualType::NAVIGATION:
                drawNavigation(canvas, card, rect, palette);
                break;
            case ScreenVisualType::STATUS:
                drawStatus(canvas, card, rect, palette);
                break;
            default:
                drawValue(canvas, card, rect, palette);
                break;
        }
        if (card.pending) {
            canvas.circle(rect.x + rect.w - 22, rect.y + 10, 4,
                          palette.accent, false);
        }
    }

    void drawValue(StripeCanvas& canvas, const ScreenPanelCardSnapshot& card,
                   const PixelRect& rect, const Palette& palette) const {
        const int16_t y = rect.y + std::max<int16_t>(27, rect.h / 2 - 8);
        const char* value = card.value[0] ? card.value : "--";
        if (card.value[0] && strcmp(card.sourceRef, "system.network") == 0)
            value = panelNetworkStatusText(card.value);
        // Stale/offline/error values must not read like live data: keep the
        // big number in its quality color so it cannot be mistaken for a
        // current reading, while "waiting for data" (UNKNOWN) stays neutral.
        const bool pendingData = card.quality == ScreenValueQuality::UNKNOWN;
        const uint16_t valueColor = pendingData || card.quality == ScreenValueQuality::GOOD
            ? palette.text : qualityColor(card.quality, palette);
        const uint8_t tier = rect.h >= 120 ? 3 : rect.h >= 74 ? 2 : 1;
        const uint16_t used = canvas.text(rect.x + 9, y, value, valueColor,
                                          rect.w - 18, tier);
        if (card.unit[0] && used + 28 < static_cast<uint16_t>(rect.w)) {
            const uint8_t rendered = StripeCanvas::renderedTier(tier);
            canvas.text(rect.x + 12 + used,
                        y + (rendered == 3 ? 16 : rendered == 2 ? 10 : 0),
                        card.unit, palette.muted,
                        rect.w - used - 18, 1);
        }
        const char* status = card.error[0] ? panelErrorText(card.error) : qualityText(card.quality);
        if (status[0] && rect.h >= 62) {
            canvas.text(rect.x + 9, rect.y + rect.h - 18, status,
                        qualityColor(card.quality, palette), rect.w - 18, 1);
        }
    }

    void drawStatus(StripeCanvas& canvas, const ScreenPanelCardSnapshot& card,
                    const PixelRect& rect, const Palette& palette) const {
        const uint16_t color = qualityColor(card.quality, palette);
        const bool pendingData = card.quality == ScreenValueQuality::UNKNOWN;
        const uint16_t textColor = pendingData || card.quality == ScreenValueQuality::GOOD
            ? palette.text : color;
        const char* value = card.value[0] ? card.value : qualityText(card.quality);
        if (card.value[0] && strcmp(card.sourceRef, "system.network") == 0)
            value = panelNetworkStatusText(card.value);
        canvas.circle(rect.x + 19, rect.y + rect.h / 2 + 6, 7, color, true);
        canvas.text(rect.x + 34, rect.y + rect.h / 2,
                    value, textColor, rect.w - 44, 1);
    }

    static bool parseNumber(const char* value, float& output) {
        if (!value || !value[0]) return false;
        char* end = nullptr;
        output = strtof(value, &end);
        return end != value && *end == '\0' && isfinite(output);
    }

    static float normalizedValue(const ScreenPanelCardSnapshot& card) {
        float value = 0.0f;
        if (!parseNumber(card.value, value)) return 0.0f;
        const float minimum = card.hasRange ? card.rangeMin : 0.0f;
        const float maximum = card.hasRange && card.rangeMax > minimum
            ? card.rangeMax : 100.0f;
        return std::max(0.0f, std::min(1.0f, (value - minimum) / (maximum - minimum)));
    }

    void drawBar(StripeCanvas& canvas, const ScreenPanelCardSnapshot& card,
                 const PixelRect& rect, const Palette& palette) const {
        if (card.seriesCount) {
            const uint8_t count = std::min<size_t>(card.seriesCount, card.series.size());
            const int16_t rowHeight = (rect.h - 36) / count;
            if (rowHeight < 40 || rect.w < 100) {
                canvas.text(rect.x + 9, rect.y + 29, "卡片过小",
                            palette.warning, rect.w - 18, 1);
                return;
            }
            const float minimum = card.hasRange ? card.rangeMin : 0.0f;
            const float maximum = card.hasRange && card.rangeMax > minimum
                ? card.rangeMax : 100.0f;
            for (uint8_t i = 0; i < count; ++i) {
                const auto& source = card.series[i];
                const int16_t top = rect.y + 28 + i * rowHeight;
                const int16_t width = rect.w - 20;
                const bool matchingUnit = strcmp(source.unit, card.series[0].unit) == 0;
                float numeric = 0.0f;
                const bool valid = matchingUnit &&
                    source.quality != ScreenValueQuality::UNKNOWN &&
                    parseNumber(source.value, numeric);
                char value[64];
                if (valid) snprintf(value, sizeof(value), "%.*f %s",
                                    std::min<unsigned>(card.precision, 6), numeric, source.unit);
                else snprintf(value, sizeof(value), "--");
                canvas.text(rect.x + 10, top, source.label[0] ? source.label : source.sourceRef,
                            palette.text, width / 2 - 4, 1);
                canvas.text(rect.x + 10 + width / 2, top, value,
                            palette.text, width / 2, 1);
                const uint16_t color = matchingUnit
                    ? qualityColor(source.quality, palette) : palette.danger;
                canvas.fillRect(rect.x + 10, top + 17, width, 5, palette.backgroundAlt);
                if (valid) {
                    const float ratio = std::max(0.0f, std::min(1.0f,
                        (numeric - minimum) / (maximum - minimum)));
                    canvas.fillRect(rect.x + 10, top + 17,
                                    static_cast<int16_t>(width * ratio), 5, color);
                }
                char status[72];
                if (!matchingUnit) snprintf(status, sizeof(status), "单位不一致");
                else if (source.updatedMs) snprintf(status, sizeof(status), "%s T+%lus",
                    qualityText(source.quality), static_cast<unsigned long>(source.updatedMs / 1000U));
                else snprintf(status, sizeof(status), "%s", qualityText(source.quality));
                canvas.text(rect.x + 10, top + 24, status, color, width, 1);
            }
            return;
        }
        const int16_t barX = rect.x + 10;
        const int16_t barY = rect.y + rect.h - 25;
        const int16_t barWidth = rect.w - 20;
        canvas.roundedRect(barX, barY, barWidth, 10, 4,
                           palette.backgroundAlt, palette.cardBorder);
        const int16_t filled = static_cast<int16_t>((barWidth - 2) * normalizedValue(card));
        if (filled > 0) canvas.fillRect(barX + 1, barY + 1, filled, 8, palette.accent);
        drawValue(canvas, card, {rect.x, rect.y, rect.w,
                                 static_cast<int16_t>(rect.h - 18)}, palette);
    }

    void drawGauge(StripeCanvas& canvas, const ScreenPanelCardSnapshot& card,
                   const PixelRect& rect, const Palette& palette) const {
        const int16_t radius = std::max<int16_t>(12,
            std::min<int16_t>(rect.w, rect.h) / 4);
        const int16_t cx = rect.x + rect.w / 2;
        const int16_t cy = rect.y + rect.h / 2 + 7;
        canvas.circle(cx, cy, radius, palette.cardBorder, false);
        const float angle = -2.35f + normalizedValue(card) * 4.70f;
        canvas.line(cx, cy,
                    cx + static_cast<int16_t>(cosf(angle) * (radius - 4)),
                    cy + static_cast<int16_t>(sinf(angle) * (radius - 4)),
                    palette.accent);
        canvas.circle(cx, cy, 3, palette.accent, true);
        canvas.text(rect.x + 8, rect.y + rect.h - 19,
                    card.value[0] ? card.value : "--", palette.text,
                    rect.w - 16, 1);
    }

    static void historyTime(const ScreenPanelHistoryPoint& point, char* text, size_t size, bool withDate = false) {
        if (point.epochMs >= 1609459200000ULL) {
            const time_t epoch = point.epochMs / 1000;
            struct tm info{};
            if (localtime_r(&epoch, &info)) { strftime(text, size, withDate ? "%m/%d %H:%M" : "%H:%M", &info); return; }
        }
        if (point.restored) snprintf(text, size, "重启前 #%lu", static_cast<unsigned long>(point.sequence));
        else snprintf(text, size, "+%lu:%02lu", static_cast<unsigned long>(point.uptimeMs / 60000), static_cast<unsigned long>(point.uptimeMs / 1000 % 60));
    }

    static bool sameHistoryBoot(const ScreenPanelHistoryPoint& a,
                                const ScreenPanelHistoryPoint& b) {
        if (a.bootId[0] || b.bootId[0])
            return a.bootId[0] && b.bootId[0] && !strcmp(a.bootId, b.bootId);
        return !a.restored && !b.restored;
    }

    static uint64_t historyElapsed(const ScreenPanelHistoryPoint& a,
                                   const ScreenPanelHistoryPoint& b) {
        // Persisted boot identity makes uptime comparable after restoration.
        // Prefer monotonic uptime so wall-clock corrections cannot remove lines.
        if (sameHistoryBoot(a, b) && b.uptimeMs > a.uptimeMs)
            return b.uptimeMs - a.uptimeMs;
        if (a.bootId[0] && b.bootId[0] && strcmp(a.bootId, b.bootId)) return 0;
        return a.epochMs && b.epochMs > a.epochMs ? b.epochMs - a.epochMs : 0;
    }

    struct HistoryAxis {
        bool epoch = false;
        bool uptime = false;
    };

    // The plot and tap selection share one axis decision so a tap lands on
    // the sample the user actually sees under the finger.
    static HistoryAxis historyAxis(const ScreenPanelCardSnapshot& card) {
        const auto& first = card.history[0];
        HistoryAxis axis{first.epochMs != 0, first.bootId[0] || !first.restored};
        for (uint8_t i = 1; i < card.historyCount; ++i) {
            axis.epoch = axis.epoch && card.history[i].epochMs >= card.history[i - 1].epochMs;
            axis.uptime = axis.uptime && sameHistoryBoot(card.history[i - 1], card.history[i]) &&
                          card.history[i].uptimeMs >= card.history[i - 1].uptimeMs;
        }
        return axis;
    }

    static double historyFraction(const ScreenPanelCardSnapshot& card, uint8_t index,
                                  HistoryAxis axis) {
        const auto& first = card.history[0];
        const auto& last = card.history[card.historyCount - 1];
        double fraction = card.historyCount > 1
            ? static_cast<double>(index) / (card.historyCount - 1) : 0.5;
        if (axis.uptime && last.uptimeMs > first.uptimeMs)
            fraction = static_cast<double>(card.history[index].uptimeMs - first.uptimeMs) /
                       (last.uptimeMs - first.uptimeMs);
        else if (axis.epoch && last.epochMs > first.epochMs)
            fraction = static_cast<double>(card.history[index].epochMs - first.epochMs) /
                       (last.epochMs - first.epochMs);
        return fraction;
    }

    void drawLineChart(StripeCanvas& canvas, const ScreenPanelCardSnapshot& card,
                       const PixelRect& rect, const Palette& palette) const {
        const int16_t x0 = rect.x + 48, y0 = rect.y + 35;
        const int16_t width = rect.w - 62, height = rect.h - 91;
        if (width < 20 || height < 15 || !card.historyCount) {
            canvas.text(rect.x + 10, rect.y + 35, "等待真实采样", palette.muted, rect.w - 20, 1); return;
        }
        double minimum = card.history[0].value, maximum = minimum;
        for (uint8_t i = 1; i < card.historyCount; ++i) {
            minimum = std::min(minimum, card.history[i].value); maximum = std::max(maximum, card.history[i].value);
        }
        if (maximum <= minimum) { maximum += 1; minimum -= 1; }
        for (int tick = 0; tick < 3; ++tick) {
            const int16_t y = y0 + tick * height / 2;
            // Tick precision follows the card; the top tick also carries the
            // unit so the axis reads without scrolling to the detail row.
            char label[40];
            if (tick == 0 && card.unit[0])
                snprintf(label, sizeof(label), "%.*f%s", std::min<int>(card.precision, 6),
                         maximum - tick * (maximum - minimum) / 2, card.unit);
            else
                snprintf(label, sizeof(label), "%.*f", std::min<int>(card.precision, 6),
                         maximum - tick * (maximum - minimum) / 2);
            canvas.text(rect.x + 5, y - 5, label, palette.muted, 44, 1);
            canvas.line(x0, y, x0 + width, y, palette.cardBorder);
        }
        const auto& first = card.history[0]; const auto& last = card.history[card.historyCount - 1];
        const HistoryAxis axis = historyAxis(card);
        // Sequences are global across sources, not consecutive within a chart.
        // Use elapsed sample time; incompatible clock domains remain disconnected.
        std::array<uint64_t, 10> intervals{};
        size_t intervalCount = 0;
        for (uint8_t i = 1; i < card.historyCount; ++i) {
            const auto& a = card.history[i - 1]; const auto& b = card.history[i];
            const uint64_t elapsed = historyElapsed(a, b);
            if (elapsed) intervals[intervalCount++] = elapsed;
        }
        std::sort(intervals.begin(), intervals.begin() + intervalCount);
        const uint64_t cadence = intervalCount ? intervals[(intervalCount - 1) / 2] : 0;
        int selected = card.historyCount - 1;
        for (uint8_t i = 0; i < card.historyCount; ++i) if (card.history[i].sequence == card.selectedHistorySequence) selected = i;
        int16_t previousX = 0, previousY = 0;
        for (uint8_t i = 0; i < card.historyCount; ++i) {
            const double fraction = historyFraction(card, i, axis);
            const int16_t x = x0 + fraction * width;
            const int16_t y = y0 + height - (card.history[i].value - minimum) * height / (maximum - minimum);
            if (i) {
                const auto& a = card.history[i - 1]; const auto& b = card.history[i];
                const uint64_t elapsed = historyElapsed(a, b);
                if (elapsed && cadence && elapsed <= cadence * 3U)
                    canvas.line(previousX, previousY, x, y, palette.accent);
            }
            canvas.circle(x, y, i == selected ? 4 : 2, palette.accent, true);
            previousX = x; previousY = y;
        }
        char start[40], end[40], selectedTime[40], detail[96];
        char firstDate[40], lastDate[40];
        historyTime(first, firstDate, sizeof(firstDate), true); historyTime(last, lastDate, sizeof(lastDate), true);
        const bool acrossDays = axis.epoch && strncmp(firstDate, lastDate, 5) != 0;
        historyTime(first, start, sizeof(start), acrossDays); historyTime(last, end, sizeof(end), acrossDays);
        canvas.text(x0, y0 + height + 6, start, palette.muted, width / 2, 1);
        canvas.text(x0 + width / 2, y0 + height + 6, end, palette.muted, width / 2, 1);
        historyTime(card.history[selected], selectedTime, sizeof(selectedTime), acrossDays);
        snprintf(detail, sizeof(detail), "%s  %.*f%s%s", selectedTime, card.precision, card.history[selected].value, card.unit, card.historyStale ? " · 历史记录" : "");
        canvas.text(rect.x + 10, rect.y + rect.h - 22, detail, palette.text, rect.w - 20, 1);
    }

    void drawMotor(StripeCanvas& canvas, const ScreenPanelCardSnapshot& card, const PixelRect& rect, const Palette& palette) const {
        const ScreenMotorLayout layout{rect};
        if (!layout.valid()) {
            canvas.text(rect.x + 10, rect.y + 34, "请在网页放大电机卡片", palette.muted, rect.w - 20, 1);
            return;
        }
        char commandStatus[80];
        const char* actionName = !strcmp(card.value, "forward") ? "正转" : !strcmp(card.value, "reverse") ? "反转" :
            !strcmp(card.value, "stop") ? "停止" : !strcmp(card.value, "setSpeed") ? "设速" : "设脉冲";
        if (card.pending) snprintf(commandStatus, sizeof(commandStatus), "正在发送指令");
        else if (card.motorCommandKnown) snprintf(commandStatus, sizeof(commandStatus), "%s%s", actionName,
            card.motorCommandSuccess ? "指令已应答" : "指令失败");
        else snprintf(commandStatus, sizeof(commandStatus), "就绪");
        canvas.text(rect.x + 12, rect.y + 36, commandStatus, palette.muted, rect.w - 24, 1);
        const uint8_t precision = std::min<uint8_t>(card.motorPrecision, 6);
        const double speedScale = std::pow(10.0, precision);
        for (int row = 0; row < 2; ++row) {
            const auto parameter = layout.parameter(row);
            const auto first = layout.adjust(row, 0);
            const int valueWidth = std::max(12, first.x - parameter.x - 10);
            const bool dirty = row ? card.motorPulseDirty : card.motorSpeedDirty;
            canvas.text(parameter.x, parameter.y + 3, row ? "单次脉冲" : "设定速度", palette.muted, valueWidth, 1);
            char value[32];
            if (row) snprintf(value, sizeof(value), "%u", card.motorPulseInput);
            else snprintf(value, sizeof(value), "%.*f", precision, card.motorSpeedInput / speedScale);
            const uint8_t scale = parameter.h >= 54 && static_cast<int>(strlen(value)) * 12 <= valueWidth ? 2 : 1;
            canvas.text(parameter.x, parameter.y + 23, value, dirty ? palette.warning : palette.text, valueWidth, scale);
            if (parameter.h >= 78) {
                char feedback[32];
                if (dirty) snprintf(feedback, sizeof(feedback), "待应用");
                else if (!card.motorKnown) snprintf(feedback, sizeof(feedback), "尚无读回");
                else if (row) snprintf(feedback, sizeof(feedback), "读回 %u", card.motorPulse);
                else snprintf(feedback, sizeof(feedback), "读回 %.*f", precision, card.motorSpeed / speedScale);
                canvas.text(parameter.x, parameter.y + 54, feedback, palette.muted, valueWidth, 1);
            }
            const char* labels[] = {"-", "+", "应用"};
            for (int col = 0; col < 3; ++col) {
                const auto button = layout.adjust(row, col);
                const uint16_t color = card.pending ? palette.muted : col == 2 ? palette.accent : palette.text;
                canvas.roundedRect(button.x, button.y, button.w, button.h, 6,
                    controlPressed(card, button) ? blend565(palette.accent, palette.card, 50) : palette.backgroundAlt,
                    col == 2 ? color : palette.cardBorder);
                canvas.text(button.x + (button.w - (col == 2 ? 24 : 12)) / 2, button.y + (button.h - 12) / 2,
                            labels[col], color, button.w - 8, 1);
            }
        }
        for (int col = 0; col < 2; ++col) {
            const auto button = layout.direction(col);
            const uint16_t color = col ? palette.accent : palette.success;
            canvas.roundedRect(button.x, button.y, button.w, button.h, 7,
                controlPressed(card, button) ? blend565(color, palette.card, 185) : color, color);
            canvas.text(button.x + (button.w - 48) / 2, button.y + (button.h - 24) / 2, col ? "反转" : "正转",
                        textOn565(color), button.w - 12, 2);
        }
        const auto stop = layout.stop();
        const uint16_t stopColor = palette.danger;
        canvas.roundedRect(stop.x, stop.y, stop.w, stop.h, 7,
            controlPressed(card, stop) ? blend565(stopColor, palette.card, 185) : stopColor, stopColor);
        canvas.text(stop.x + (stop.w - 48) / 2, stop.y + (stop.h - 24) / 2, "停止", textOn565(stopColor), stop.w - 12, 2);
        const char* safety = !strcmp(card.error, "TRAVEL_RANGE_EXCEEDED") ? "已到软件限位，请反向或核对位置" :
            card.motorUiFeedback[0] ? card.motorUiFeedback : card.error[0] ? panelErrorText(card.error) :
            card.motorReadError[0] ? "电机通信失败，请检查控制器电源与接线" :
            !card.motorSoftLimitEnabled ? "点动模式：点击正转或反转即可运行" :
            !card.motorReferenceKnown ? "软限位已配置，参考位置未确认" :
            !card.motorKnown ? "启动前自动读取参数；停止仍可请求" :
            !card.motorPulse ? "启动时自动设置默认点动参数" :
            !card.motorStationaryConfirmed ? "换向时自动先停止，再启动" :
            !card.motorForwardReady && !card.motorReverseReady ? "行程或脉冲读回不满足启动条件" :
            "软限位已启用；无实际位置反馈";
            if (safety[0]) canvas.text(rect.x + 12, rect.y + rect.h - 15, safety, palette.warning, rect.w - 24, 1);
    }

    void drawSlider(StripeCanvas& canvas, const ScreenPanelCardSnapshot& card, const PixelRect& rect, const Palette& palette) const {
        if (card.error[0]) { canvas.text(rect.x + 10, rect.y + 35, panelErrorText(card.error), palette.muted, rect.w - 20, 1); return; }
        char value[16]; snprintf(value, sizeof(value), "%u%%", static_cast<unsigned>(card.numericValue));
        canvas.text(rect.x + rect.w - 60, rect.y + 29, value, palette.text, 50, 1);
        if (card.numericValue == 0) canvas.text(rect.x + 12, rect.y + 31, "最低亮度", palette.muted, rect.w - 88, 1);
        const int y = rect.y + rect.h - 26, w = rect.w - 40;
        canvas.roundedRect(rect.x + 20, y - 3, w, 6, 3, palette.cardBorder, palette.cardBorder);
        const int filled = std::max(0, std::min(w, static_cast<int>(card.numericValue * w / 100)));
        if (filled > 0) canvas.roundedRect(rect.x + 20, y - 3, filled, 6, 3, palette.accent, palette.accent);
        for (int i = 0; i < 5; ++i) canvas.circle(rect.x + 20 + i * w / 4, y + 12, 1, palette.muted, true);
        canvas.circle(rect.x + 20 + filled, y, 11, palette.accent, true);
        canvas.circle(rect.x + 20 + filled, y, 5, palette.card, true);
    }

    void drawSleepSelect(StripeCanvas& canvas, const ScreenPanelCardSnapshot& card, const PixelRect& rect, const Palette& palette) const {
        if (card.error[0]) { canvas.text(rect.x + 12, rect.y + 40, panelErrorText(card.error), palette.muted, rect.w - 24, 1); return; }
        char label[32];
        if (card.numericValue > 0 && static_cast<unsigned>(card.numericValue) % 60 == 0)
            snprintf(label, sizeof(label), "%u 分钟", static_cast<unsigned>(card.numericValue) / 60);
        else if (card.numericValue > 0) snprintf(label, sizeof(label), "%u 秒", static_cast<unsigned>(card.numericValue));
        else snprintf(label, sizeof(label), "永不");
        const auto left = screenSelectButton(rect, false), right = screenSelectButton(rect, true);
        for (bool next : {false, true}) {
            const auto b = screenSelectButton(rect, next);
            canvas.roundedRect(b.x, b.y, b.w, b.h, 6,
                controlPressed(card, b) ? blend565(palette.accent, palette.card, 50) : palette.backgroundAlt, palette.cardBorder);
            canvas.text(b.x + (b.w - 12) / 2, b.y + (b.h - 12) / 2, next ? "+" : "-", palette.text, b.w - 4, 1);
        }
        canvas.text(left.x + left.w + 12, left.y + (left.h - 12) / 2, label,
                    palette.text, std::max(1, right.x - left.x - left.w - 24), 1);
    }

    static bool booleanValue(const char* value) {
        return value && (strcasecmp(value, "true") == 0 ||
                         strcasecmp(value, "on") == 0 ||
                         strcmp(value, "1") == 0 ||
                         strcasecmp(value, "open") == 0);
    }

    void drawSwitch(StripeCanvas& canvas, const ScreenPanelCardSnapshot& card,
                    const PixelRect& rect, const Palette& palette) const {
        if (card.error[0]) { canvas.text(rect.x + 12, rect.y + 40, panelErrorText(card.error), palette.muted, rect.w - 24, 1); return; }
        const bool unconfigured = card.kind == ScreenCardKind::CONTROL && !card.controlRef[0];
        const bool on = booleanValue(card.value);
        const int16_t width = std::min<int16_t>(54, rect.w - 18);
        const int16_t height = 26;
        const int16_t x = rect.x + rect.w - width - 10;
        const int16_t y = rect.y + rect.h / 2 - 4;
        const uint16_t track = !unconfigured && card.quality == ScreenValueQuality::GOOD
            ? (on ? palette.success : palette.muted) : palette.cardBorder;
        canvas.roundedRect(x, y, width, height, 7, track,
            controlPressed(card, {x, y, width, height}) ? palette.accent : track);
        canvas.circle(on ? x + width - 13 : x + 13, y + 13, 9,
                      unconfigured ? palette.muted : palette.text, true);
        canvas.text(rect.x + 9, rect.y + rect.h / 2,
                    unconfigured ? "待配置" : card.pending ? "执行中" :
                    card.quality == ScreenValueQuality::GOOD ?
                        (on ? "已开启" : "已关闭") : "状态未知",
                    palette.text, std::max<int16_t>(1, x - rect.x - 14), 1);
    }

    void drawAction(StripeCanvas& canvas, const ScreenPanelCardSnapshot& card,
                    const PixelRect& rect, const Palette& palette) const {
        if (card.error[0] && !card.interactive) { canvas.text(rect.x + 12, rect.y + 40, panelErrorText(card.error), palette.muted, rect.w - 24, 1); return; }
        const bool unconfigured = card.kind == ScreenCardKind::CONTROL && !card.controlRef[0];
        const int16_t buttonX = rect.x + 10;
        const int16_t buttonY = rect.y + std::max<int16_t>(34, rect.h / 2 - 1);
        const int16_t buttonW = rect.w - 20;
        const int16_t buttonH = std::min<int16_t>(44, rect.h - (buttonY - rect.y) - 6);
        canvas.roundedRect(buttonX, buttonY, buttonW, buttonH, 6,
                           unconfigured || card.pending ? palette.cardBorder :
                               controlPressed(card, {buttonX, buttonY, buttonW, buttonH}) ? blend565(palette.accent, palette.card, 50) : palette.backgroundAlt,
                           unconfigured ? palette.cardBorder : palette.accent);
        canvas.text(buttonX + 8, buttonY + (buttonH - 12) / 2,
                    unconfigured ? "待配置" : card.pending ? "执行中" : card.kind == ScreenCardKind::SYSTEM ? (!strcmp(card.systemActionId, "sleep") ? "轻触息屏 / 触摸唤醒" : card.title) : "执行一次",
                    unconfigured ? palette.muted : palette.text, buttonW - 16, 1);
    }

    void drawNumberControl(StripeCanvas& canvas,
                           const ScreenPanelCardSnapshot& card,
                           const PixelRect& rect,
                           const Palette& palette) const {
        const bool unconfigured = !card.controlRef[0];
        const int16_t stepTop = rect.y + 31;
        const int16_t buttonSize = 44;
        const int16_t minusX = rect.x + 10;
        const int16_t plusX = rect.x + rect.w - 10 - buttonSize;
        const uint16_t buttonColor = unconfigured ? palette.cardBorder : palette.backgroundAlt;
        canvas.roundedRect(minusX, stepTop, buttonSize, buttonSize, 5,
                           controlPressed(card, {minusX, stepTop, buttonSize, buttonSize}) ? palette.accent : buttonColor, palette.cardBorder);
        canvas.roundedRect(plusX, stepTop, buttonSize, buttonSize, 5,
                           controlPressed(card, {plusX, stepTop, buttonSize, buttonSize}) ? palette.accent : buttonColor, palette.cardBorder);
        canvas.text(minusX + (buttonSize - 12) / 2, stepTop + (buttonSize - 12) / 2,
                    "-", palette.text, buttonSize - 12, 1);
        canvas.text(plusX + (buttonSize - 12) / 2, stepTop + (buttonSize - 12) / 2,
                    "+", palette.text, buttonSize - 12, 1);
        char numeric[40];
        snprintf(numeric, sizeof(numeric), "%.*f", card.precision,
                 static_cast<double>(card.numericValue));
        String valueText(numeric);
        if (card.unit[0]) valueText += String(" ") + card.unit;
        canvas.text(minusX + buttonSize + 6, stepTop + 11,
                    valueText.c_str(), palette.text,
                    std::max<int16_t>(1, plusX - minusX - buttonSize - 12), 1);
        const int16_t executeY = rect.y + rect.h - 38;
        const int16_t executeW = rect.w - 20;
        canvas.roundedRect(rect.x + 10, executeY, executeW, 29, 5,
                           unconfigured || card.pending ? palette.cardBorder :
                               controlPressed(card, {static_cast<int16_t>(rect.x + 10), executeY, executeW, 29}) ? blend565(palette.accent, palette.text, 65) : palette.accent,
                           unconfigured ? palette.cardBorder : palette.accent);
        canvas.text(rect.x + 18, executeY + 8,
                    unconfigured ? "待配置" : card.pending ? "执行中" : "执行",
                    unconfigured ? palette.muted : palette.text,
                    executeW - 16, 1);
    }

    static PixelRect navigationButton(const PixelRect& bounds) {
        const int top = std::max(34, static_cast<int>(bounds.h) - 50);
        return {static_cast<int16_t>(bounds.x + 10), static_cast<int16_t>(bounds.y + top),
                static_cast<int16_t>(bounds.w - 20), static_cast<int16_t>(std::max(0, std::min(40, bounds.h - top - 6)))};
    }

    void drawNavigation(StripeCanvas& canvas, const ScreenPanelCardSnapshot& card,
                        const PixelRect& rect, const Palette& palette) const {
        const auto button = navigationButton(rect);
        canvas.roundedRect(button.x, button.y, button.w, button.h, 6,
            controlPressed(card, button) ? blend565(palette.accent, palette.card, 50) : palette.backgroundAlt, palette.accent);
        const int16_t cx = button.x + button.w - 18;
        const int16_t cy = button.y + button.h / 2;
        canvas.line(cx - 5, cy - 7, cx + 3, cy, palette.accent);
        canvas.line(cx + 3, cy, cx - 5, cy + 7, palette.accent);
        canvas.text(button.x + 9, button.y + (button.h - 12) / 2,
                    card.value[0] ? card.value : "打开页面",
                    palette.text, button.w - 42, 1);
    }

    void drawClock(StripeCanvas& canvas, const PixelRect& rect,
                   const Palette& palette) const {
        if (rect.w >= 240 && rect.h >= 110) {
            const int16_t cx = rect.x + rect.w - 43, cy = rect.y + rect.h / 2 + 3;
            const uint16_t color = blend565(palette.accent, palette.card, 48);
            canvas.circle(cx, cy, 25, color, false);
            canvas.circle(cx, cy, 19, color, false);
            canvas.line(cx, cy, cx, cy - 11, palette.accent);
            canvas.line(cx, cy, cx + 9, cy + 5, palette.accent);
        }
        time_t now = time(nullptr);
        struct tm timeInfo {};
        if (now < 1609459200 || !localtime_r(&now, &timeInfo)) {
            canvas.text(rect.x + 9, rect.y + rect.h / 2,
                        "时间未同步", palette.warning, rect.w - 18, 1);
            return;
        }
        char clockText[8];
        char dateText[16];
        strftime(clockText, sizeof(clockText), "%H:%M", &timeInfo);
        strftime(dateText, sizeof(dateText), "%Y-%m-%d", &timeInfo);
        canvas.text(rect.x + 9, rect.y + 28, clockText,
                    palette.text, rect.w - 18, 2);
        canvas.text(rect.x + 9, rect.y + rect.h - 19, dateText,
                    palette.muted, rect.w - 18, 1);
    }

    void drawWeatherIcon(StripeCanvas& canvas, const char* condition,
                         int16_t x, int16_t y, uint16_t color,
                         const Palette& palette) const {
        String normalized(condition ? condition : "");
        normalized.toLowerCase();
        const bool rain = normalized.indexOf("rain") >= 0 ||
                          normalized.indexOf("雨") >= 0;
        const bool snow = normalized.indexOf("snow") >= 0 ||
                          normalized.indexOf("雪") >= 0;
        const bool cloud = normalized.indexOf("cloud") >= 0 ||
                           normalized.indexOf("overcast") >= 0 ||
                           normalized.indexOf("云") >= 0 ||
                           normalized.indexOf("阴") >= 0;
        const bool sun = normalized.indexOf("sun") >= 0 ||
                          normalized.indexOf("clear") >= 0 ||
                          normalized.indexOf("晴") >= 0;
        const bool night = normalized == "clear-night" ||
                           normalized.indexOf("night") >= 0;
        const bool thunder = normalized.indexOf("thunder") >= 0;
        const bool fog = normalized.indexOf("fog") >= 0 ||
                         normalized.indexOf("雾") >= 0;
        const bool wind = normalized.indexOf("wind") >= 0;
        const bool cold = normalized.indexOf("cold") >= 0;
        const bool hot = normalized.indexOf("hot") >= 0;
        if (night) {
            canvas.circle(x, y, 12, palette.muted, true);
            canvas.circle(x + 6, y - 4, 10, palette.card, true);
            return;
        }
        if (fog || wind) {
            for (int16_t offset = -8; offset <= 8; offset += 8) {
                canvas.line(x - 17 + (wind ? abs(offset) / 2 : 0), y + offset,
                            x + 17, y + offset, color);
            }
            return;
        }
        if (cold) {
            canvas.line(x - 11, y, x + 11, y, palette.text);
            canvas.line(x, y - 11, x, y + 11, palette.text);
            canvas.line(x - 8, y - 8, x + 8, y + 8, palette.text);
            canvas.line(x - 8, y + 8, x + 8, y - 8, palette.text);
            return;
        }
        if ((sun || hot) && !cloud && !rain && !snow) {
            canvas.circle(x, y, 10, palette.warning, true);
            for (int i = 0; i < 8; ++i) {
                const float angle = static_cast<float>(i) * 0.785398f;
                canvas.line(x + static_cast<int16_t>(cosf(angle) * 14),
                            y + static_cast<int16_t>(sinf(angle) * 14),
                            x + static_cast<int16_t>(cosf(angle) * 19),
                            y + static_cast<int16_t>(sinf(angle) * 19),
                            palette.warning);
            }
            return;
        }
        if (cloud || rain || snow || thunder) {
            canvas.circle(x - 8, y, 9, color, true);
            canvas.circle(x + 2, y - 5, 12, color, true);
            canvas.circle(x + 12, y + 1, 8, color, true);
            canvas.fillRect(x - 16, y, 36, 10, color);
            if (thunder) {
                canvas.line(x + 2, y + 12, x - 4, y + 20, palette.warning);
                canvas.line(x - 4, y + 20, x + 2, y + 19, palette.warning);
                canvas.line(x + 2, y + 19, x - 3, y + 27, palette.warning);
            } else if (rain) {
                for (int16_t dx = -10; dx <= 10; dx += 10) {
                    canvas.line(x + dx, y + 14, x + dx - 3, y + 21,
                                palette.accent);
                }
            } else if (snow) {
                for (int16_t dx = -10; dx <= 10; dx += 10) {
                    canvas.line(x + dx - 3, y + 18, x + dx + 3, y + 18,
                                palette.text);
                    canvas.line(x + dx, y + 15, x + dx, y + 21,
                                palette.text);
                }
            }
            return;
        }
        canvas.circle(x, y, 15, color, false);
        canvas.text(x - 4, y - 5, "?", color, 10, 1);
    }

    void drawWeather(StripeCanvas& canvas, const ScreenPanelCardSnapshot& card,
                     const PixelRect& rect, const Palette& palette) const {
        const bool unconfigured = !card.sourceRef[0];
        const int16_t iconX = rect.x + std::min<int16_t>(42, rect.w / 4);
        const int16_t iconY = rect.y + rect.h / 2 + 7;
        drawWeatherIcon(canvas, unconfigured ? "cloudy" : card.weatherCondition, iconX, iconY,
                        palette.muted, palette);
        const int16_t textX = iconX + 28;
        if (unconfigured) {
            canvas.text(textX, rect.y + 28, "待配置", palette.muted,
                        rect.x + rect.w - textX - 8, 2);
            canvas.text(rect.x + 9, rect.y + rect.h - 18, "请在设备屏幕绑定天气",
                        palette.muted, rect.w - 18, 1);
            return;
        }
        char temperature[20] = "--";
        if (card.hasWeatherTemperature) {
            snprintf(temperature, sizeof(temperature), "%.1f C",
                     static_cast<double>(card.weatherTemperature));
        } else if (card.value[0]) {
            snprintf(temperature, sizeof(temperature), "%s %s",
                     card.value, card.unit);
        }
        canvas.text(textX, rect.y + 28, temperature,
                    palette.text, rect.x + rect.w - textX - 8, 2);
        const int16_t statusY = rect.y + rect.h - 18;
        const char* detail = card.weatherCity[0] ? card.weatherCity : qualityText(card.quality);
        canvas.text(textX, rect.y + 57, detail, palette.muted, rect.x + rect.w - textX - 8, 1);
        const char* reason = card.weatherReason[0] ? card.weatherReason : card.error[0] ? "unknown" : "none";
        canvas.text(rect.x + 9, statusY, weatherStatusText(reason, card.hasWeatherTemperature,
                    card.quality == ScreenValueQuality::STALE), palette.muted, rect.w - 18, 1);
    }

    void drawConfirmation(StripeCanvas& canvas,
                          const ScreenPanelSnapshot& snapshot,
                          const char* cardId,
                          const Palette& palette) const {
        const ScreenPanelCardSnapshot* card = nullptr;
        for (uint8_t index = 0; index < snapshot.cardCount; ++index) {
            if (strcmp(snapshot.cards[index].id, cardId) == 0) {
                card = &snapshot.cards[index];
                break;
            }
        }
        const bool restart = card && card->kind == ScreenCardKind::SYSTEM && !strcmp(card->systemActionId, "restart");
        const bool motor = card && card->visualType == ScreenVisualType::MOTOR_CONTROL;
        const bool historySaveFailed = restart &&
            strcmp(card->error, "history_save_failed") == 0;
        canvas.dim(155);
        const PixelRect dialog = confirmationRect();
        canvas.roundedRect(dialog.x, dialog.y, dialog.w, dialog.h, 7,
                           palette.card, palette.accent);
        const char* title = historySaveFailed ? "历史保存失败" : restart ? "确认重启设备?" :
                            motor ? (card->motorConfirmAction == 2 ? "确认开始反转?" : "确认开始正转?") : "确认执行此操作?";
        const char* detail = historySaveFailed ? "仍不保存并重启?" :
                             restart ? "重启前将保存配置和历史" :
                             motor ? "本次操作后可直接连续控制" : "触摸确认后才会发送命令";
        const char* confirmLabel = historySaveFailed ? "仍重启" :
                                   restart ? "重启" : motor ? "开始" : "确认";
        canvas.text(dialog.x + 14, dialog.y + 16, title,
                    palette.text, dialog.w - 28, 1);
        canvas.text(dialog.x + 14, dialog.y + 44, detail, palette.muted,
                    dialog.w - 28, 1);
        if (motor) {
            char idle[64];
            snprintf(idle, sizeof(idle), "闲置 %u 秒后再次确认", static_cast<unsigned>(snapshot.controlConfirmIdleSeconds));
            canvas.text(dialog.x + 14, dialog.y + 68, idle, palette.muted, dialog.w - 28, 1);
            char values[64]; snprintf(values, sizeof(values), "速度 %.*f   脉冲 %u", card->motorPrecision,
                card->motorSpeedInput / std::pow(10.0, std::min<uint8_t>(card->motorPrecision, 6)), card->motorPulseInput);
            canvas.text(dialog.x + 14, dialog.y + 98, values, palette.text, dialog.w - 28, 1);
        }
        const unsigned count = motor ? 3 : 2;
        for (unsigned i = 0; i < count; ++i) {
            const auto button = confirmationButton(i, count);
            const bool stop = motor && i == 1;
            const char* label = !i ? "取消" : stop ? "停止" : confirmLabel;
            const uint16_t color = !i ? palette.muted : stop ? palette.warning : palette.accent;
            canvas.roundedRect(button.x, button.y, button.w, button.h, 6, palette.backgroundAlt, color);
            canvas.text(button.x + (button.w - 24) / 2, button.y + 16, label, color, button.w - 12, 1);
        }
    }

    IDisplayBackend* _display = nullptr;
    char _pressedCard[41]{};
    uint16_t _pressedX = 0, _pressedY = 0;
    bool _navigationOpen = false;
    uint16_t* _buffer = nullptr;
    StripeChangeCache _stripeCache;
    StripeChangeCache::Entry* _stripeEntries = nullptr;
    uint16_t _width = 0;
    uint16_t _height = 0;
    uint16_t _lines = 0;
    size_t _bufferBytes = 0;
};

} // namespace Screen
} // namespace FastBee

#endif // FASTBEE_ENABLE_SCREEN_UI && defined(ARDUINO_ARCH_ESP32)
#endif // STRIPE_RENDERER_H
