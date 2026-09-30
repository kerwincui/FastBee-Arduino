#pragma once

#include <ArduinoJson.h>
#include <LittleFS.h>

namespace FastBee { namespace OtaConfigStorage {
// Keep the existing mqtt.otaBaseUrl storage contract. Read only that field so
// OTA settings never expose credentials or depend on a cached MQTT connection.
inline bool load(JsonDocument& doc, String& error) {
    error = "";
    doc.clear();
    if (!LittleFS.exists("/config/protocol.json")) return true;
    File file = LittleFS.open("/config/protocol.json", "r");
    if (!file) {
        error = "Unable to open OTA configuration";
        return false;
    }
    JsonDocument filter;
    filter["mqtt"]["otaBaseUrl"] = true;
    const DeserializationError result = deserializeJson(
        doc, file, DeserializationOption::Filter(filter));
    file.close();
    if (result || !doc.is<JsonObject>() ||
        (!doc["mqtt"]["otaBaseUrl"].isNull() &&
         !doc["mqtt"]["otaBaseUrl"].is<const char*>())) {
        error = "Existing OTA configuration is unreadable";
        return false;
    }
    return true;
}
}} // namespace FastBee::OtaConfigStorage
