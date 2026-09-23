#pragma once
#include "core/FeatureFlags.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <vector>

class MQTTClient;
namespace FastBee {
// Shared protocol sources, independent of the panel and page lifetime.
class RemoteResourceManager {
public:
    static RemoteResourceManager& instance();
    bool initialize();
    void tick();                         // Existing acquisition scheduler.
    void handleMqtt(MQTTClient* client); // Existing protocol loop.
    bool consumeMqtt(const String& topic, const String& payload);
    bool execute(const String& id, bool target, String& status, String& error);
    bool execute(const String& id, const String& numericValue,
                 String& status, String& error);
    bool requestRead(const String& id, String& error);
    bool getWeatherTestStatus(const String& id, JsonObject out);
    void appendChoices(JsonArray sources, JsonArray controls);
    bool getValue(const String& ref, JsonObject value);
    void getConfig(JsonObject out);
    bool saveConfig(JsonVariantConst submitted, String& error);
    void getScreenWeather(JsonObject out);
    bool saveScreenWeather(JsonVariantConst submitted, String& error);
    bool beginProtocolConfigTransaction(uint32_t timeoutMs = 2000);
    void endProtocolConfigTransaction();
    static void redact(JsonObject config);

private:
    struct WeatherState {
        String id, sourceRevision;
        JsonDocument value;
        uint32_t updatedMs = 0, nextMs = 0;
        uint32_t lastManualRequestMs = 0;
        uint8_t failures = 0;
        String error;
        bool requested = false, known = false, manualRequestSeen = false;
        bool manualRequestPending = false;
    };
    JsonDocument config_;
    JsonDocument weatherMigration_;
    uint32_t weatherRevision_ = 1;
    std::vector<WeatherState> weather_;
    std::vector<String> retiredTopics_;
    SemaphoreHandle_t mutex_ = nullptr;
    uint32_t generation_ = 0;
    // Fences configuration replacement while a validated command publish or
    // observation write is executing without mutex_.
    size_t activeExternalCommits_ = 0;
    bool workerActive_ = false;
    void applyConfig(JsonVariantConst config);
#if FASTBEE_ENABLE_PERIPH_EXEC
    bool beginObservationCommit(bool weatherSource, const String& id,
                                const String& sourceRevision,
                                uint32_t generation);
#endif
    void endExternalCommit();
    static void weatherTask(void* arg);
    void collectWeather();
};
}
