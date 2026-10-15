#include "StripeRenderer.h"

#if FASTBEE_ENABLE_SCREEN_UI && defined(ARDUINO_ARCH_ESP32)

#include <esp_heap_caps.h>
#include "screen/ScreenThemeVocabulary.h"

namespace FastBee {
namespace Screen {

Palette paletteFor(const char* theme) {
    if (theme && strcmp(theme, kScreenThemeHighContrast) == 0) {
        return {rgb565(0, 0, 0), rgb565(17, 17, 17),
                rgb565(8, 8, 8), rgb565(160, 160, 160),
                rgb565(255, 255, 255), rgb565(209, 213, 219),
                rgb565(0, 168, 255), rgb565(80, 220, 120),
                rgb565(255, 210, 60), rgb565(255, 100, 100)};
    }
    if (theme && strcmp(theme, kScreenThemeLight) == 0) {
        return {rgb565(244, 247, 249), rgb565(226, 234, 240),
                rgb565(255, 255, 255), rgb565(198, 209, 216),
                rgb565(28, 34, 38), rgb565(92, 105, 112),
                rgb565(0, 122, 116), rgb565(29, 137, 79),
                rgb565(190, 122, 18), rgb565(190, 52, 54)};
    }
    // Unknown or missing themes (including drifted legacy values) fall back
    // to the dark palette; the configuration layer only admits the ids from
    // ScreenThemeVocabulary.h.
    return {rgb565(15, 22, 34), rgb565(23, 35, 49),
            rgb565(29, 42, 57), rgb565(53, 72, 89),
            rgb565(238, 246, 252), rgb565(157, 180, 198),
            rgb565(69, 201, 218), rgb565(90, 207, 155),
            rgb565(246, 189, 91), rgb565(240, 112, 117)};
}

uint16_t decodeUtf8(const char*& text) {
    const uint8_t first = static_cast<uint8_t>(*text++);
    if (first < 0x80U) return first;
    if ((first & 0xE0U) == 0xC0U) {
        if (*text == '\0') return '?';
        const uint8_t second = static_cast<uint8_t>(*text);
        if ((second & 0xC0U) != 0x80U) return '?';
        ++text;
        return static_cast<uint16_t>(((first & 0x1FU) << 6U) |
                                     (second & 0x3FU));
    }
    if ((first & 0xF0U) == 0xE0U) {
        if (text[0] == '\0' || text[1] == '\0') return '?';
        const uint8_t second = static_cast<uint8_t>(text[0]);
        const uint8_t third = static_cast<uint8_t>(text[1]);
        if ((second & 0xC0U) != 0x80U || (third & 0xC0U) != 0x80U) return '?';
        text += 2;
        return static_cast<uint16_t>(((first & 0x0FU) << 12U) |
                                     ((second & 0x3FU) << 6U) |
                                     (third & 0x3FU));
    }
    while (*text != '\0' &&
           (static_cast<uint8_t>(*text) & 0xC0U) == 0x80U) ++text;
    return '?';
}

// U8g2 omits its user-pointer field on Arduino. Rendering is deliberately
// confined to one screen task, so a single short-lived callback context is
// sufficient and avoids changing U8g2's public struct ABI with a compile flag.
StripeCanvas* activeFontCanvas = nullptr;

void* allocatePreferred(size_t bytes) {
    if (bytes == 0) return nullptr;
    void* memory = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!memory) {
        memory = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return memory;
}

} // namespace Screen
} // namespace FastBee

#endif // FASTBEE_ENABLE_SCREEN_UI && defined(ARDUINO_ARCH_ESP32)
