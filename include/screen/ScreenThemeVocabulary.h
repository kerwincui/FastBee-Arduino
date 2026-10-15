#pragma once
#include <cstring>

namespace FastBee { namespace Screen {

// Single source of truth for the theme ids and background presets the
// configuration layer accepts. The renderer falls back to the dark theme /
// solid background for anything else, so drifted values stay renderable.
inline constexpr const char* kScreenThemeDark = "dark";
inline constexpr const char* kScreenThemeLight = "light";
inline constexpr const char* kScreenThemeHighContrast = "high-contrast";
inline constexpr const char* kScreenThemeIds[] = {
    kScreenThemeDark, kScreenThemeLight, kScreenThemeHighContrast,
};

inline constexpr const char* kScreenBackgroundSolid = "solid";
inline constexpr const char* kScreenBackgroundGrid = "grid";
inline constexpr const char* kScreenBackgroundSoftGradient = "soft-gradient";
inline constexpr const char* kScreenBackgroundPresetIds[] = {
    kScreenBackgroundSolid, kScreenBackgroundGrid, kScreenBackgroundSoftGradient,
};

inline bool isKnownScreenThemeId(const char* theme) {
    if (!theme) return false;
    for (const char* id : kScreenThemeIds)
        if (!std::strcmp(theme, id)) return true;
    return false;
}

inline bool isKnownScreenBackgroundPresetId(const char* preset) {
    if (!preset) return false;
    for (const char* id : kScreenBackgroundPresetIds)
        if (!std::strcmp(preset, id)) return true;
    return false;
}
}}
