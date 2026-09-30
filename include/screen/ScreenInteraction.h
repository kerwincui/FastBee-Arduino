#pragma once

#include "screen/ScreenGeometry.h"
#include <cstring>

namespace FastBee {
namespace Screen {

// One active local motor session. A different binding, configuration revision,
// sleep, or inactivity closes it. No permission is persisted across restarts.
class ScreenControlSession {
public:
    void reset() { _approved = false; _binding[0] = '\0'; }
    void observe(uint32_t revision, uint32_t now, uint32_t idleSeconds) {
        if (_revision != revision || (_approved && now - _lastUse >= idleSeconds * 1000U)) reset();
        _revision = revision;
    }
    bool allows(const char* binding) const {
        return _approved && binding && binding[0] && std::strcmp(binding, _binding) == 0;
    }
    void approve(const char* binding, uint32_t now) {
        if (!binding || !binding[0] || std::strlen(binding) >= sizeof(_binding)) return;
        std::strcpy(_binding, binding); _lastUse = now; _approved = true;
    }
    void used(const char* binding, uint32_t now) { if (allows(binding)) _lastUse = now; }
private:
    char _binding[97]{};
    uint32_t _revision = 0, _lastUse = 0;
    bool _approved = false;
};

// All motor drawing and hit testing use these exact rectangles. Gaps and
// labels never become command targets, including on smaller legacy layouts.
struct ScreenMotorLayout {
    ScreenPixelRect bounds;
    static constexpr int commandHeight = 64;
    bool valid() const { return bounds.w >= 240 && bounds.h >= 280; }
    int rowHeight() const { return (bounds.h - commandHeight - 76) / 2; }
    ScreenPixelRect parameter(unsigned row) const {
        return {static_cast<int16_t>(bounds.x + 12),
                static_cast<int16_t>(bounds.y + 56 + row * rowHeight()),
                static_cast<int16_t>(bounds.w - 24), static_cast<int16_t>(rowHeight() - 8)};
    }
    ScreenPixelRect adjust(unsigned row, unsigned column) const {
        const auto p = parameter(row);
        const int width = std::max(40, (bounds.w - 162) / 3);
        return {static_cast<int16_t>(bounds.x + bounds.w - 12 - 3 * width - 16 + column * (width + 8)),
                static_cast<int16_t>(p.y + std::max(0, (p.h - 44) / 2)),
                static_cast<int16_t>(width), static_cast<int16_t>(std::min(44, static_cast<int>(p.h)))};
    }
    ScreenPixelRect command(unsigned column) const {
        const int width = (bounds.w - 44) / 3;
        return {static_cast<int16_t>(bounds.x + 12 + column * (width + 10)),
                static_cast<int16_t>(bounds.y + bounds.h - commandHeight - 18),
                static_cast<int16_t>(width), commandHeight};
    }
    ScreenPixelRect direction(unsigned column) const { return command(column * 2); }
    ScreenPixelRect stop() const { return command(1); }
};

inline ScreenPixelRect screenSelectButton(const ScreenPixelRect& bounds, bool next) {
    const int size = std::min(44, std::max(0, static_cast<int>(bounds.h) - 38));
    return {static_cast<int16_t>(next ? bounds.x + bounds.w - size - 12 : bounds.x + 12),
            static_cast<int16_t>(bounds.y + bounds.h - size - 8),
            static_cast<int16_t>(size), static_cast<int16_t>(size)};
}

}  // namespace Screen
}  // namespace FastBee
