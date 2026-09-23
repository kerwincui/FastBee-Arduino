#include <cassert>
#include <fstream>
#include <sstream>
#include <cmath>
using std::isnan;
using std::isinf;
#include "ScreenPanelNativeDependencies.h"
#define private public
#include "core/ScreenPanelManager.h"
#undef private
#define xSemaphoreCreateMutex() reinterpret_cast<SemaphoreHandle_t>(1)
#include "../../src/core/ScreenPanelManager.cpp"
#undef assert
#define assert(condition) do { if (!(condition)) { std::fprintf(stderr, "Assertion failed at %d: %s\n", __LINE__, #condition); std::exit(1); } } while (false)

static const char* legacy = R"json({"storageVersion":1,"revision":9,"config":{
 "schemaVersion":1,"displayRef":null,"settings":{"brightnessPercent":63,"idleSleepSeconds":300,"homePageId":"controls","themeId":"dark","background":{"presetId":"solid"},"allowSystemRestart":false},
 "pages":[{"id":"controls","title":"User title","layoutId":"tile-grid","layoutMode":"canvas","logicalWidth":480,"logicalHeight":480,"rotation":0,"cards":[
 {"id":"remote_switch","kind":"control","visualType":"switch","title":"Not matched by title","controlRef":"","rect":{"x":16,"y":16,"w":216,"h":208}},
 {"id":"remote_scene","kind":"control","visualType":"action","title":"Keep bound rule","controlRef":"rule.real","bindingFingerprint":"abcdef12","rect":{"x":248,"y":16,"w":216,"h":208}},
 {"id":"system_link","kind":"page-link","visualType":"navigation","title":"System","pageId":"system","rect":{"x":16,"y":240,"w":448,"h":224}}]},
 {"id":"system","title":"System","layoutId":"tile-grid","layoutMode":"canvas","logicalWidth":480,"logicalHeight":480,"rotation":0,"cards":[
 {"id":"brightness_25","kind":"system","visualType":"action","title":"25","systemActionId":"brightness","actionValue":25,"rect":{"x":16,"y":16,"w":144,"h":136}},
 {"id":"brightness_70","kind":"system","visualType":"action","title":"70","systemActionId":"brightness","actionValue":70,"rect":{"x":168,"y":16,"w":144,"h":136}},
 {"id":"brightness_100","kind":"system","visualType":"action","title":"100","systemActionId":"brightness","actionValue":100,"rect":{"x":320,"y":16,"w":144,"h":136}},
 {"id":"sleep","kind":"system","visualType":"action","title":"Sleep","systemActionId":"sleep","rect":{"x":16,"y":168,"w":216,"h":136}}]}
 ]}})json";
static String encode(const JsonDocument& value) { String result; serializeJson(value, result); return result; }
int main(int argc, char** argv) {
    assert(argc >= 2);
    const std::string scenario = argv[1];
    JsonDocument original; assert(!deserializeJson(original, legacy));
    JsonDocument validated; String validationError;
    if (!ScreenPanelManager::validateConfig(original["config"], validated, validationError)) {
        std::fprintf(stderr, "%s\n", validationError.c_str()); return 1;
    }
    if (scenario == "settings") {
        assert(validated["settings"]["touchBeepEnabled"] == true);
        assert(validated["settings"]["controlConfirmIdleSeconds"] == 60);
        JsonDocument config; config.set(original["config"]);
        config["settings"]["touchBeepEnabled"] = false;
        config["settings"]["controlConfirmIdleSeconds"] = 600;
        assert(ScreenPanelManager::validateConfig(config.as<JsonVariantConst>(), validated, validationError));
        assert(validated["settings"]["touchBeepEnabled"] == false);
        assert(validated["settings"]["controlConfirmIdleSeconds"] == 600);
        for (int invalid : {-1, 0, 14, 601}) {
            config["settings"]["controlConfirmIdleSeconds"] = invalid;
            assert(!ScreenPanelManager::validateConfig(config.as<JsonVariantConst>(), validated, validationError));
        }
        config["settings"]["controlConfirmIdleSeconds"] = 15;
        for (const char* invalid : {"true", "false", "1"}) {
            config["settings"]["touchBeepEnabled"] = invalid;
            assert(!ScreenPanelManager::validateConfig(config.as<JsonVariantConst>(), validated, validationError));
        }
        config["settings"]["touchBeepEnabled"] = 1;
        assert(!ScreenPanelManager::validateConfig(config.as<JsonVariantConst>(), validated, validationError));
        config["settings"]["touchBeepEnabled"] = true;
        JsonObject card = config["pages"][1]["cards"][0];
        config["settings"]["brightnessPercent"] = 0; card["actionValue"] = 0;
        assert(ScreenPanelManager::validateConfig(config.as<JsonVariantConst>(), validated, validationError));
        assert(validated["settings"]["brightnessPercent"] == 0);
        assert(validated["pages"][1]["cards"][0]["actionValue"] == 0);
        card["actionValue"] = -1;
        assert(!ScreenPanelManager::validateConfig(config.as<JsonVariantConst>(), validated, validationError));
        card.remove("actionValue"); card["systemActionId"] = "touch-beep"; card["visualType"] = "switch";
        assert(ScreenPanelManager::validateConfig(config.as<JsonVariantConst>(), validated, validationError));
        card["visualType"] = "action";
        assert(!ScreenPanelManager::validateConfig(config.as<JsonVariantConst>(), validated, validationError));
        card["systemActionId"] = "confirm-idle"; card["visualType"] = "select";
        assert(ScreenPanelManager::validateConfig(config.as<JsonVariantConst>(), validated, validationError));
        card["visualType"] = "switch";
        assert(!ScreenPanelManager::validateConfig(config.as<JsonVariantConst>(), validated, validationError));
        JsonObject motor = config["pages"][1]["cards"].to<JsonArray>().add<JsonObject>();
        motor["id"] = "motor"; motor["title"] = "Motor"; motor["kind"] = "control"; motor["visualType"] = "motor";
        motor["controlRef"] = "motor.test"; motor["requireConfirmation"] = false;
        motor["rect"]["x"] = 16; motor["rect"]["y"] = 16; motor["rect"]["w"] = 448; motor["rect"]["h"] = 400;
        assert(ScreenPanelManager::validateConfig(config.as<JsonVariantConst>(), validated, validationError));
        assert(validated["pages"][1]["cards"][0]["requireConfirmation"] == true);
        auto& manager = ScreenPanelManager::instance();
        assert(!manager.queueSetting("brightnessPercent", 0, validationError) && validationError == "Screen settings worker unavailable");
        assert(!manager.queueSetting("brightnessPercent", 101, validationError) && validationError == "Invalid screen setting");
        assert(!manager.queueSetting("touchBeepEnabled", 2, validationError) && validationError == "Invalid screen setting");
        assert(!manager.queueSetting("controlConfirmIdleSeconds", 14, validationError) && validationError == "Invalid screen setting");
        assert(!manager.queueSetting("controlConfirmIdleSeconds", 601, validationError) && validationError == "Invalid screen setting");
        return 0;
    }
    if (scenario == "system-template" || scenario == "system-custom") {
        JsonDocument config; config.set(validated);
        JsonObject page = config["pages"][1]; page["layoutId"] = "mixed-grid"; page["layoutMode"] = "template";
        JsonArray cards = page["cards"].to<JsonArray>();
        const char* ids[] = {"system-brightness", "system-timeout", "system-sleep"};
        const char* actions[] = {"brightness", "sleep-time", "sleep"};
        const char* visuals[] = {"slider", "select", "action"};
        for (int i=0; i<3; ++i) {
            JsonObject card = cards.add<JsonObject>(); card["id"] = ids[i]; card["kind"] = "system";
            card["systemActionId"] = actions[i]; card["visualType"] = visuals[i]; card["title"] = ids[i];
            card["rect"]["x"] = 16; card["rect"]["y"] = 16 + i * 144; card["rect"]["w"] = 448; card["rect"]["h"] = 128;
        }
        if (scenario == "system-custom") page["layoutMode"] = "canvas";
        const String before = encode(config);
        const bool changed = ScreenPanelMigration::upgrade(config.as<JsonObject>());
        // The unrelated legacy unbound control may migrate in both scenarios.
        assert(changed);
        assert(cards.size() == (scenario == "system-template" ? 5 : 3));
        assert(ScreenPanelManager::validateConfig(config.as<JsonVariantConst>(), validated, validationError));
        if (scenario == "system-template") {
            assert(cards[3]["systemActionId"] == "touch-beep" && cards[3]["visualType"] == "switch");
            assert(cards[4]["systemActionId"] == "confirm-idle" && cards[4]["visualType"] == "select");
            assert(cards[2]["rect"]["y"] == 384);
            assert(!config["settings"]["allowSystemRestart"].as<bool>());
        } else assert(cards[2]["rect"]["y"] == 304);
        assert(!ScreenPanelMigration::upgrade(config.as<JsonObject>()));
        return 0;
    }
    const String path = ScreenPanelManager::CONFIG_FILE;
    const String backup = path + ".touch-v1.bak";
    if (scenario == "sample") {
        assert(argc == 3); std::ifstream file(argv[2]); std::stringstream buffer; buffer << file.rdbuf();
        JsonDocument sample, normalized; String error;
        assert(!deserializeJson(sample, buffer.str()));
        if (!ScreenPanelManager::validateConfig(sample["config"], normalized, error)) {
            std::fprintf(stderr, "Sample validation error: %s\n", error.c_str());
            return 1;
        }
        assert(!ScreenPanelMigration::upgrade(normalized.as<JsonObject>()));
        return 0;
    }
    if (scenario == "custom") {
        original["config"]["pages"][0]["cards"][0]["id"] = "my_custom_switch";
        original["config"]["pages"][1]["cards"][0]["id"] = "custom_brightness";
    }
    if (scenario == "already") original["touchUiMigration"] = 2;
    String originalText = encode(original);
    g_mockFiles[scenario == "recover" ? path + ".bak" : path] = originalText;
    if (scenario == "bad-backup") g_mockFiles[backup] = "broken";
    if (scenario == "interrupted") g_mockFiles[backup + ".tmp"] = "partial";
    auto& manager = ScreenPanelManager::instance();
    assert(manager.initialize());
    // Native String is a stream with a copied read cursor. Real ESP32 String
    // uses ArduinoJson's string adapter, so inspect the same stored bytes.
    JsonDocument config; uint32_t revision = manager.revision();
    assert(!deserializeJson(config, manager._configJson.c_str()) && manager.hasPersistedConfig());
    if (scenario == "custom" || scenario == "already" || scenario == "bad-backup") {
        assert(revision == 9);
        assert(config["pages"][0]["cards"].size() == 3);
        assert(config["pages"][1]["cards"].size() == 4);
        assert(g_mockFiles[path] == originalText);
        if (scenario != "bad-backup") assert(!LittleFS.exists(backup));
    } else {
        assert(revision == 10 && config["pages"].size() == 2);
        assert(config["settings"]["brightnessPercent"] == 63 && config["settings"]["idleSleepSeconds"] == 300);
        assert(config["settings"]["homePageId"] == "controls");
        assert(config["pages"][0]["cards"].size() == 2);
        assert(config["pages"][0]["cards"][0]["controlRef"] == "rule.real");
        assert(config["pages"][0]["cards"][0]["bindingFingerprint"] == "abcdef12");
        assert(config["pages"][0]["cards"][1]["pageId"] == "system");
        assert(config["pages"][1]["cards"].size() == 2);
        assert(config["pages"][1]["cards"][0]["visualType"] == "slider");
        assert(config["pages"][1]["cards"][0]["actionValue"] == 63);
        assert(config["pages"][1]["cards"][0]["rect"]["w"] == 448);
        assert(config["pages"][1]["cards"][1]["rect"]["y"] == 168);
        assert(g_mockFiles[backup] == originalText);
        assert(!ScreenPanelMigration::upgrade(config.as<JsonObject>()));
        const String migrated = g_mockFiles[path];
        assert(manager.initialize() && g_mockFiles[path] == migrated && g_mockFiles[backup] == originalText);
        assert(backupPanelBeforeTouchMigration(path) && g_mockFiles[backup] == originalText);
        JsonDocument envelope; assert(!deserializeJson(envelope, migrated)); assert(envelope["touchUiMigration"] == 2);
    }
    std::puts("Actual screen manager migration passed.");
}
