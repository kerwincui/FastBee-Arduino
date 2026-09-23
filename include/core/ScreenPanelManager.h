#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <array>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

enum class ScreenCardKind : uint8_t {
    DATA = 0,
    CHART,
    CONTROL,
    SYSTEM,
    CLOCK,
    WEATHER,
    PAGE_LINK
};

enum class ScreenVisualType : uint8_t {
    VALUE = 0,
    STATUS,
    GAUGE,
    BAR,
    LINE,
    WEATHER,
    CLOCK,
    NAVIGATION,
    SWITCH,
    ACTION,
    NUMBER_CONTROL,
    MOTOR_CONTROL,
    SLIDER,
    SELECT
};

enum class ScreenValueQuality : uint8_t {
    UNKNOWN = 0,
    GOOD,
    STALE,
    OFFLINE,
    ERROR
};

enum class ScreenActionKind : uint8_t {
    SET_BOOLEAN = 0,
    SET_NUMBER,
    TRIGGER,
    SYSTEM_RESTART,
    SYSTEM_ACTION,
    MOTOR_COMMAND
};

struct ScreenPanelRect {
    uint16_t x = 0;
    uint16_t y = 0;
    uint16_t w = 0;
    uint16_t h = 0;
};

struct ScreenPanelHistoryPoint {
    double value = 0.0;
    uint32_t sequence = 0;
    uint32_t uptimeMs = 0;
    uint64_t epochMs = 0;
    bool restored = false;
};

struct ScreenPanelBarValue {
    char sourceRef[97]{};
    char label[33]{};
    char unit[17]{};
    char value[33]{};
    ScreenValueQuality quality = ScreenValueQuality::UNKNOWN;
    uint32_t updatedMs = 0;
};

struct ScreenPanelPageSnapshot {
    char id[41]{};
    char title[65]{};
    char layoutId[25]{};
    char themeId[17]{};
    char backgroundPresetId[17]{};
    uint16_t logicalWidth = 480;
    uint16_t logicalHeight = 480;
    uint16_t firstCard = 0;
    uint8_t cardCount = 0;
    uint16_t rotation = 0;
};

struct ScreenPanelCardSnapshot {
    char id[41]{};
    char pageId[41]{};
    char targetPageId[41]{};
    char title[65]{};
    char unit[17]{};
    char sourceRef[97]{};
    char controlRef[97]{};
    char iconId[25]{};
    char value[65]{};
    char systemActionId[17]{};
    uint16_t motorSpeed = 0, motorPulse = 0;
    uint8_t motorPrecision = 0;
    uint16_t motorSpeedInput = 50, motorPulseInput = 1600;
    uint16_t motorSpeedStep = 1, motorPulseStep = 100;
    uint8_t motorConfirmAction = 0;
    uint16_t motorConfirmValue = 0;
    uint32_t selectedHistorySequence = 0;
    bool motorKnown = false, motorSpeedDirty = false, motorPulseDirty = false;
    char error[97]{};
    char historyError[97]{};
    char weatherCondition[25]{};
    char weatherCity[49]{};
    ScreenCardKind kind = ScreenCardKind::DATA;
    ScreenVisualType visualType = ScreenVisualType::VALUE;
    ScreenValueQuality quality = ScreenValueQuality::UNKNOWN;
    ScreenPanelRect rect;
    float rangeMin = 0.0f;
    float rangeMax = 0.0f;
    float numericStep = 1.0f;
    float numericValue = 0.0f;
    float weatherTemperature = 0.0f;
    bool hasRange = false;
    bool hasWeatherTemperature = false;
    bool pending = false;
    bool historyStale = false;
    bool interactive = false;
    bool requiresConfirmation = false;
    uint32_t updatedMs = 0;
    uint32_t operationId = 0;
    uint8_t historyCount = 0;
    uint8_t seriesCount = 0;
    uint8_t precision = 1;
    std::array<ScreenPanelHistoryPoint, 10> history{};
    std::array<ScreenPanelBarValue, 4> series{};
};

struct ScreenPanelSnapshot {
    static constexpr size_t MAX_PAGES = 8;
    static constexpr size_t MAX_CARDS = 24;
    static constexpr size_t MAX_HISTORY = 10;
    static constexpr size_t MAX_BOTTOM_NAV_PAGES = 5;

    uint32_t revision = 1;
    uint32_t idleSleepSeconds = 120;
    uint32_t controlConfirmIdleSeconds = 60;
    bool touchBeepEnabled = true;
    uint8_t activePageIndex = 0;
    uint8_t pageCount = 0;
    uint8_t cardCount = 0;
    uint8_t bottomNavCount = 0;
    uint8_t brightnessPercent = 70;
    char homePageId[41]{};
    char themeId[17]{};
    char backgroundPresetId[17]{};
    std::array<ScreenPanelPageSnapshot, MAX_PAGES> pages{};
    std::array<std::array<char, 41>, MAX_BOTTOM_NAV_PAGES> bottomNavPageIds{};
    std::array<ScreenPanelCardSnapshot, MAX_CARDS> cards{};
};

struct ScreenPanelSnapshotCache;

class ScreenPanelManager {
public:
    static constexpr const char* CONFIG_FILE = "/config/screen_panel.json";
    static constexpr size_t MAX_CONFIG_BYTES = 20 * 1024;

    enum class SaveResult : uint8_t {
        SAVED = 0,
        REVISION_CONFLICT,
        INVALID_CONFIG,
        STORAGE_ERROR,
        BUSY
    };

    static ScreenPanelManager& instance();

    ScreenPanelManager(const ScreenPanelManager&) = delete;
    ScreenPanelManager& operator=(const ScreenPanelManager&) = delete;

    bool initialize();
    bool copyConfig(JsonDocument& out, uint32_t& revision,
                    bool& persisted) const;
    SaveResult saveConfig(JsonVariantConst submitted, uint32_t expectedRevision,
                          String& error);
    static bool validateConfig(JsonVariantConst submitted, JsonDocument& normalized,
                               String& error);

    void appendCapabilities(JsonObject out) const;
    void appendChoices(JsonArray out) const;
    void appendApplication(JsonObject out) const;
    void appendState(JsonObject out) const;
    void reportApplication(bool applied, bool restartRequired,
                           const String& error = "");

    bool getSnapshot(ScreenPanelSnapshot& out) const;
    // One cache and snapshot per screen worker. Web callers continue using the
    // full getSnapshot() above; these instances must not be shared by tasks.
    bool getActiveSnapshot(ScreenPanelSnapshot& out,
                           ScreenPanelSnapshotCache& cache) const;
    bool dispatchAction(const char* cardId, ScreenActionKind kind, bool target,
                        uint32_t& operationId, String& error);
    bool dispatchAction(const char* cardId, ScreenActionKind kind,
                        const String& value, uint32_t& operationId,
                        String& error, const char* expectedControlRef = nullptr);
    bool queueSetting(const String& key, uint32_t value, String& error);
    void notifyRestartSaveFailure();
    bool consumeRestartWithoutHistorySave();
    bool setActivePage(const String& pageId);

    uint32_t revision() const { return _revision; }
    bool hasPersistedConfig() const { return _persisted; }

private:
    ScreenPanelManager() = default;

    bool loadConfigFile(const String& path, String& configJson,
                        uint32_t& revision, bool* needsMigration = nullptr) const;
    bool validateAndBindReferences(JsonDocument& normalized, String& error) const;
    bool writeEnvelope(JsonVariantConst config, uint32_t revision) const;
    static void makeDefaultConfig(JsonDocument& out);
    static String ruleFingerprint(const struct PeriphExecRule& rule);
    bool getSnapshotImpl(ScreenPanelSnapshot& out, ScreenPanelSnapshotCache& cache,
                         bool activeOnly) const;

    mutable SemaphoreHandle_t _mutex = nullptr;
    bool _initialized = false;
    bool _persisted = false;
    bool _applied = false;
    bool _restartRequired = false;
    uint32_t _revision = 1;
    String _configJson;
    String _displayRef;
    String _applicationError;
    String _activePageId;
    bool _restartSaveFailed = false;
    bool _restartWithoutHistorySave = false;
    std::array<uint32_t, ScreenPanelSnapshot::MAX_CARDS> _operationIds{};
    std::array<String, ScreenPanelSnapshot::MAX_CARDS> _operationCardIds{};
};

struct ScreenPanelSnapshotCache {
    explicit ScreenPanelSnapshotCache(bool persistent = true);
    ScreenPanelSnapshotCache(const ScreenPanelSnapshotCache&) = delete;
    ScreenPanelSnapshotCache& operator=(const ScreenPanelSnapshotCache&) = delete;

    // Call this if the caller resets/reuses the bound snapshot in place.
    void invalidate() { _snapshot = nullptr; _revision = 0; }
    uint32_t rebuildCount() const { return _rebuildCount; }
    uint8_t refreshedCardCount() const { return _refreshedCardCount; }

private:
    friend class ScreenPanelManager;
    struct TerminalOperation {
        uint32_t id = 0;
        uint8_t error = 0;
        bool failed = false;
    };
    // On ESP32 PSRAM builds this document has a strict PSRAM allocator; a
    // failed allocation never silently retains the layout in internal DRAM.
    JsonDocument _layout;
    const ScreenPanelSnapshot* _snapshot = nullptr;
    uint32_t _revision = 0;
    uint32_t _rebuildCount = 0;
    uint8_t _refreshedCardCount = 0;
    std::array<TerminalOperation, ScreenPanelSnapshot::MAX_CARDS> _terminalOperations{};
};

bool getScreenPanelSnapshot(ScreenPanelSnapshot& out);
bool dispatchScreenPanelAction(const char* cardId, ScreenActionKind kind,
                               bool target, uint32_t& operationId,
                               String& error);
bool dispatchScreenPanelAction(const char* cardId, ScreenActionKind kind,
                               const String& value, uint32_t& operationId,
                               String& error);
