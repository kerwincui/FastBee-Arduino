static bool backupMqttConfig(JsonVariantConst original, JsonVariantConst candidate) {
    // Never overwrite a previous recovery record, even if corrupt/legacy.
    if (LittleFS.exists(MQTT_TEST_BACKUP_PATH) || !original.is<JsonObjectConst>() ||
        !candidate.is<JsonObjectConst>()) return false;
    JsonDocument backupDoc;
    backupDoc["version"] = 2;
    backupDoc["mqtt"] = original;
    backupDoc["testMqtt"] = candidate;
    if (backupDoc.overflowed()) return false;

    File f = LittleFS.open(MQTT_TEST_BACKUP_PATH, "w");
    if (!f) return false;
    const size_t expected = measureJson(backupDoc);
    const size_t written = serializeJson(backupDoc, f);
    f.flush();
    f.close();
    // Short writes and failed readback retain evidence and forbid config save.
    if (written != expected) return false;
    File check = LittleFS.open(MQTT_TEST_BACKUP_PATH, "r");
    if (!check) return false;
    JsonDocument verified;
    const bool valid = !deserializeJson(verified, check);
    check.close();
    return valid && verified.as<JsonVariantConst>() == backupDoc.as<JsonVariantConst>();
}

static bool saveMqttTestConfig(const String& server, int port, const String& username,
                               const String& password, const String& authCode,
                               int authType, const String& mqttSecret, const String& scheme = "mqtt",
                               const String& clientId = "") {
    JsonDocument doc;
    if (!ConfigStorage::getInstance().loadProtocolConfig(doc)) {
        LOG_WARNING("[MQTT Test] Failed to load protocol.json for saving test config");
        return false;
    }

    if (!doc["mqtt"].is<JsonObject>() || (doc["mqtt"]["connectionTestOnly"] | false)) return false;
    JsonDocument candidate;
    candidate.set(doc["mqtt"]);
    JsonObject mqtt = candidate.as<JsonObject>();
    // 仅更新测试表单中用户可能修改的字段
    mqtt["scheme"]   = scheme;
    mqtt["server"]   = server;
    mqtt["port"]     = port;
    mqtt["username"] = username;
    mqtt["password"] = password;
    mqtt["authType"] = authType;
    if (!clientId.isEmpty())   mqtt["clientId"]   = clientId;
    if (!authCode.isEmpty())   mqtt["authCode"]   = authCode;
    if (!mqttSecret.isEmpty()) mqtt["mqttSecret"] = mqttSecret;

    const uint32_t sequence = FastBee::nextReportInstanceId();
    if (!sequence) return false;
    s_mqttTestId = String(esp_random(), HEX) + "-" + String(sequence);
    mqtt["connectionTestOnly"] = true;
    mqtt["connectionTestId"] = s_mqttTestId;
    mqtt["enabled"] = true; // Only this explicit temporary probe, restored afterward.
    mqtt["autoReconnect"] = true; // Explicit probe must try even if normal auto-reconnect is off.
    if (candidate.overflowed() ||
        !backupMqttConfig(doc["mqtt"].as<JsonVariantConst>(), candidate.as<JsonVariantConst>()))
        return false;
    bool ok = ConfigStorage::getInstance().compareExchangeMqttConfig(
        doc["mqtt"].as<JsonVariantConst>(), candidate.as<JsonVariantConst>());
    if (ok) {
        LOG_INFO("[MQTT Test] Test config saved to protocol.json");
    } else {
        LOG_WARNING("[MQTT Test] Failed to save test config to protocol.json");
    }
    return ok;
}
