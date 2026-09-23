#pragma once
#include <ArduinoJson.h>
#include <cstring>

namespace ScreenPanelMigration {
inline bool text(JsonVariantConst field, const char* value) {
    return field.is<const char*>() && !std::strcmp(field.as<const char*>(), value);
}
inline bool rect(JsonObjectConst card, int x, int y, int w, int h) {
    return card["rect"]["x"] == x && card["rect"]["y"] == y &&
           card["rect"]["w"] == w && card["rect"]["h"] == h;
}
// Deliberately recognize template identity/shape, never translated titles.
// Bound controls, page identities and navigation are preserved verbatim.
inline bool upgrade(JsonObject config) {
    bool changed = false;
    for (JsonObject page : config["pages"].as<JsonArray>()) {
        JsonArray cards = page["cards"].as<JsonArray>();
        // Only the three-card generated system template is safe to repack.
        // A canvas, extra card, binding or altered widget is user-owned.
        if (text(page["id"], "system") && text(page["layoutId"], "mixed-grid") &&
            text(page["layoutMode"], "template") && page["logicalWidth"] == 480 &&
            page["logicalHeight"] == 480 && cards.size() == 3) {
            const char* expectedIds[] = {"system-brightness", "system-timeout", "system-sleep"};
            const char* expectedActions[] = {"brightness", "sleep-time", "sleep"};
            const char* expectedVisuals[] = {"slider", "select", "action"};
            bool standard = true;
            for (size_t i = 0; i < 3; ++i) {
                JsonObjectConst card = cards[i];
                standard = standard && text(card["id"], expectedIds[i]) &&
                    text(card["kind"], "system") && text(card["systemActionId"], expectedActions[i]) &&
                    text(card["visualType"], expectedVisuals[i]) &&
                    !std::strlen(card["sourceRef"] | "") && !std::strlen(card["controlRef"] | "") &&
                    !std::strlen(card["bindingFingerprint"] | "");
            }
            if (standard) {
                const char* ids[] = {"system-touch-beep", "system-confirm-idle"};
                const char* actions[] = {"touch-beep", "confirm-idle"};
                const char* visuals[] = {"switch", "select"};
                const char* titles[] = {"触摸提示音", "操作确认间隔"};
                for (size_t i = 0; i < 2; ++i) {
                    JsonObject card = cards.add<JsonObject>();
                    card["id"] = ids[i]; card["kind"] = "system";
                    card["systemActionId"] = actions[i]; card["visualType"] = visuals[i];
                    card["title"] = titles[i]; card["requireConfirmation"] = false;
                }
                // Keep the existing cards/IDs, placing immediate sleep last visually.
                for (size_t i = 0; i < cards.size(); ++i) {
                    const int row = i == 2 ? 4 : i >= 3 ? static_cast<int>(i) - 1 : static_cast<int>(i);
                    JsonObject rect = cards[i]["rect"].to<JsonObject>();
                    rect["x"] = 12; rect["y"] = 8 + row * 94;
                    rect["w"] = 456; rect["h"] = 88;
                }
                changed = true;
            }
        }
        for (size_t i = cards.size(); i > 0; --i) {
            JsonObject card = cards[i - 1];
            if (!text(card["kind"], "control") || std::strlen(card["controlRef"] | "")) continue;
            const bool sample = text(page["id"], "controls") && text(page["layoutId"], "tile-grid") &&
                page["logicalWidth"] == 480 && page["logicalHeight"] == 480 &&
                ((text(card["id"], "remote_switch") && text(card["visualType"], "switch") && rect(card, 16, 16, 216, 208)) ||
                 (text(card["id"], "remote_scene") && text(card["visualType"], "action") && rect(card, 248, 16, 216, 208)));
            const bool generated = text(page["id"], "control") && text(page["layoutId"], "tile-grid") &&
                text(page["layoutMode"], "template") &&
                ((text(card["id"], "control-1") && text(card["visualType"], "switch")) ||
                 (text(card["id"], "control-2") && text(card["visualType"], "action")));
            if (sample || generated) { cards.remove(i - 1); changed = true; }
        }
        // Merge only the exact shipped preset row. Custom controls elsewhere
        // keep their position and dimensions, including very narrow buttons.
        int presets[3] = {-1, -1, -1};
        const char* ids[] = {"brightness_25", "brightness_70", "brightness_100"};
        const int levels[] = {25, 70, 100};
        for (size_t i = 0; i < cards.size(); ++i) {
            JsonObjectConst card = cards[i];
            for (int n = 0; n < 3; ++n) {
                if (text(card["id"], ids[n]) && text(card["kind"], "system") &&
                    text(card["systemActionId"], "brightness") && text(card["visualType"], "action") &&
                    card["actionValue"] == levels[n] && rect(card, 16 + n * 152, 16, 144, 136)) presets[n] = static_cast<int>(i);
            }
        }
        if (presets[0] >= 0 && presets[1] >= 0 && presets[2] >= 0 &&
            page["logicalWidth"] == 480 && page["logicalHeight"] == 480) {
            bool clear = true;
            for (size_t i = 0; i < cards.size(); ++i) {
                if (static_cast<int>(i) == presets[0] || static_cast<int>(i) == presets[1] || static_cast<int>(i) == presets[2]) continue;
                JsonObjectConst r = cards[i]["rect"];
                if ((r["x"] | 0) < 464 && (r["x"] | 0) + (r["w"] | 0) > 16 &&
                    (r["y"] | 0) < 152 && (r["y"] | 0) + (r["h"] | 0) > 16) clear = false;
            }
            if (clear) {
                JsonObject first = cards[presets[0]];
                first["rect"]["w"] = 448;
                first["visualType"] = "slider"; first["title"] = "亮度";
                first["actionValue"] = config["settings"]["brightnessPercent"] | 70;
                // Descending removal keeps the retained card's identity intact.
                for (size_t i = cards.size(); i > 0; --i)
                    if (static_cast<int>(i - 1) == presets[1] || static_cast<int>(i - 1) == presets[2]) cards.remove(i - 1);
                changed = true;
            }
        }
        for (JsonObject card : cards) {
            if (text(card["kind"], "system") && text(card["systemActionId"], "brightness") &&
                text(card["visualType"], "action") && (card["rect"]["w"] | 0) >= 180 && (card["rect"]["h"] | 0) >= 80) {
                card["visualType"] = "slider"; changed = true;
            }
        }
    }
    return changed;
}
}
