#include "./network/handlers/ProtocolRouteHandler.h"
#include "core/MemoryBudget.h"
#include "./network/handlers/HandlerUtils.h"
#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_PERIPH_EXEC
#include "core/PeriphExecManager.h"
#include "core/AsyncExecTypes.h"
#endif
#include "systems/LoggerSystem.h"
#if FASTBEE_ENABLE_MODBUS
#include "./network/handlers/ModbusRouteHandler.h"
#endif
#if FASTBEE_ENABLE_MQTT
#include "./network/handlers/MqttRouteHandler.h"
#include "./protocols/MQTTClient.h"
#endif
#include "./network/WebHandlerContext.h"
#include "./network/NetworkManager.h"
#include "./network/ProtocolSecret.h"
#include "network/OtaConfigStorage.h"
#include "protocols/FastBeeOTAProtocol.h"
#include "protocols/MotorProtocol.h"
#if FASTBEE_ENABLE_REMOTE_RESOURCES
#include "protocols/RemoteResourceManager.h"
#endif
#include "systems/ConfigStorage.h"
#include "./protocols/ProtocolManager.h"
#include "systems/SystemRebooter.h"
#include "utils/PsramJsonDocument.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <memory>

static const char* PROTOCOL_CONFIG_PATH = "/config/protocol.json";

namespace {
constexpr uint8_t PROTOCOL_CONFIG_VERSION = 2;
constexpr uint16_t PROTOCOL_MIN_MODBUS_POLL_INTERVAL_SEC = 2;
constexpr uint16_t PROTOCOL_MAX_MODBUS_POLL_INTERVAL_SEC = 3600;
constexpr int MQTT_TOPIC_TYPE_OTA_UPGRADE = 5;
constexpr int MQTT_TOPIC_TYPE_OTA_BINARY = 6;
constexpr const char* MQTT_OTA_REPLY_TOPIC = "/{deviceNum}/http/upgrade/reply";
constexpr const char* MQTT_OTA_COMMAND_TOPIC = "/{deviceNum}/http/upgrade/set";
constexpr const char* MQTT_OTA_REPLY_TOPIC_LEGACY = "/http/upgrade/reply";
constexpr const char* MQTT_OTA_COMMAND_TOPIC_LEGACY = "/http/upgrade/set";

template <typename T>
T clampProtocolValue(T value, T minVal, T maxVal) {
    if (value < minVal) return minVal;
    if (value > maxVal) return maxVal;
    return value;
}

bool hasProtocolParam(AsyncWebServerRequest* request, const char* key) {
    return request && (request->hasParam(key, true) || request->hasParam(key, false));
}

// Deleting an array entry also renumbers every subsequent execution target.
// Reject affected references instead of silently redirecting an existing rule.
bool validateModbusArrayUpdate(WebHandlerContext* ctx, AsyncWebServerRequest* request,
                               JsonArrayConst previous, const String& submitted,
                               bool tasks) {
    if (submitted.isEmpty()) return true;
    JsonDocument incoming;
    if (deserializeJson(incoming, submitted) || !incoming.is<JsonArray>()) {
        ctx->sendError(request, 400, "Modbus tasks/devices must be a JSON array");
        return false;
    }
    JsonArrayConst next = incoming.as<JsonArrayConst>();
    const size_t maxCount = tasks ? Protocols::MODBUS_MAX_POLL_TASKS : 8;
    if (next.size() > maxCount) {
        ctx->sendError(request, 400, "Too many Modbus tasks/devices");
        return false;
    }
    for (JsonVariantConst item : next) {
        if (!item.is<JsonObjectConst>()) {
            ctx->sendError(request, 400, "Each Modbus task/device must be an object");
            return false;
        }
    }
    if (!tasks) for (JsonObjectConst device : next) {
        MotorProtocol profile;
        if (const char* error = loadMotorProtocol(device, profile)) {
            ctx->sendError(request, 400, error);
            return false;
        }
    }
#if FASTBEE_ENABLE_PERIPH_EXEC
    if (next.size() >= previous.size()) return true;
    auto affected = [&](int index) {
        if (index < 0 || static_cast<size_t>(index) >= previous.size()) return false;
        if (static_cast<size_t>(index) >= next.size()) return true;
        // Only a shortened list needs this guard. Ordinary edits to addresses,
        // function codes or mappings do not rename positional references.
        const char* fields[] = {"slaveAddress", "deviceType", "name", "sensorId",
                                "functionCode", "startAddress", "quantity"};
        for (const char* field : fields) {
            if (previous[index][field].as<String>() != next[index][field].as<String>()) return true;
        }
        return false;
    };
    const String prefix = tasks ? "modbus-task:" : "modbus:";
    auto affectedId = [&](const String& id) {
        return id.startsWith(prefix) && affected(id.substring(prefix.length()).toInt());
    };
    auto& manager = PeriphExecManager::getInstance();
    MutexGuard lock(manager.getRulesMutex(), pdMS_TO_TICKS(2000));
    if (!lock.isLocked()) {
        ctx->sendError(request, 503, "Execution rules are busy; retry deletion shortly");
        return false;
    }
    for (const auto& entry : manager.getRules()) {
        const auto& rule = entry.second;
        bool conflict = false;
        for (const auto& trigger : rule.triggers) conflict |= affectedId(trigger.triggerPeriphId);
        for (const auto& action : rule.actions) {
            conflict |= affectedId(action.targetPeriphId);
            if (action.actionType != static_cast<uint8_t>(ExecActionType::ACTION_MODBUS_POLL)) continue;
            JsonDocument value;
            if (!deserializeJson(value, action.actionValue) && value.is<JsonObject>()) {
                if (tasks) {
                    for (JsonVariantConst index : value["poll"].as<JsonArrayConst>()) conflict |= affected(index.as<int>());
                } else {
                    for (JsonVariantConst control : value["ctrl"].as<JsonArrayConst>()) conflict |= affected(control["d"] | 0);
                }
            } else if (tasks && !action.actionValue.startsWith("{")) {
                int start = 0;
                while (start < static_cast<int>(action.actionValue.length())) {
                    int end = action.actionValue.indexOf(',', start);
                    if (end < 0) end = action.actionValue.length();
                    String token = action.actionValue.substring(start, end);
                    token.trim();
                    if (!token.isEmpty()) conflict |= affected(token.toInt());
                    start = end + 1;
                }
            }
        }
        if (conflict) {
            ctx->sendError(request, 409, String("删除会改变外设执行规则的 Modbus 索引，请先调整规则：") +
                           rule.name + " (" + rule.id + ")");
            return false;
        }
    }
#endif
    return true;
}

void ensureFastBeeOtaTopic(JsonArray topics,
                           const char* canonicalTopic,
                           const char* legacyTopic,
                           bool publishTopic) {
    bool hasCanonical = false;
    for (JsonObject topic : topics) {
        String configured = topic["topic"] | "";
        configured.trim();
        if (configured != canonicalTopic) continue;

        // A complete FastBee template must not receive the product/device
        // prefix again. Preserve enabled=false as the user's OTA opt-out.
        topic["topic"] = canonicalTopic;
        topic["autoPrefix"] = false;
        topic["topicType"] = MQTT_TOPIC_TYPE_OTA_UPGRADE;
        hasCanonical = true;
    }

    if (!hasCanonical) {
        for (JsonObject topic : topics) {
            if ((topic["topicType"] | -1) != MQTT_TOPIC_TYPE_OTA_UPGRADE ||
                !(topic["autoPrefix"] | true)) {
                continue;
            }

            String configured = topic["topic"] | "";
            configured.trim();
            if (configured != legacyTopic) continue;

            // Migrate the former suffix+autoPrefix representation in place so
            // existing enabled/disabled intent and QoS/retain remain intact.
            topic["topic"] = canonicalTopic;
            topic["autoPrefix"] = false;
            hasCanonical = true;
            break;
        }
    }

    if (hasCanonical) return;

    JsonObject topic = topics.add<JsonObject>();
    topic["topic"] = canonicalTopic;
    topic["qos"] = 0;
    if (publishTopic) topic["retain"] = false;
    topic["enabled"] = true;
    topic["autoPrefix"] = false;
    topic["topicType"] = MQTT_TOPIC_TYPE_OTA_UPGRADE;
}

void ensureFastBeeOtaTopics(JsonObject mqtt) {
#if FASTBEE_ENABLE_OTA
    JsonArray publishTopics = mqtt["publishTopics"].as<JsonArray>();
    if (publishTopics.isNull()) {
        publishTopics = mqtt["publishTopics"].to<JsonArray>();
    }
    ensureFastBeeOtaTopic(publishTopics, MQTT_OTA_REPLY_TOPIC,
                          MQTT_OTA_REPLY_TOPIC_LEGACY, true);

    JsonArray subscribeTopics = mqtt["subscribeTopics"].as<JsonArray>();
    if (subscribeTopics.isNull()) {
        subscribeTopics = mqtt["subscribeTopics"].to<JsonArray>();
    }
    ensureFastBeeOtaTopic(subscribeTopics, MQTT_OTA_COMMAND_TOPIC,
                          MQTT_OTA_COMMAND_TOPIC_LEGACY, false);
#else
    (void)mqtt;
#endif
}

void disableUnsupportedBinaryOtaTopics(JsonObject mqtt) {
    static const char* topicLists[] = {"publishTopics", "subscribeTopics"};
    for (const char* listName : topicLists) {
        JsonArray topics = mqtt[listName].as<JsonArray>();
        if (topics.isNull()) continue;
        for (JsonObject topic : topics) {
            if ((topic["topicType"] | -1) == MQTT_TOPIC_TYPE_OTA_BINARY) {
                topic["enabled"] = false;
            }
        }
    }
}

bool validateOtaBaseUrl(const String& value, String& error) {
    return FastBeeOtaProtocol::validateBaseUrl(value, error);
}

void normalizeProtocolConfig(JsonDocument& doc) {
    if (!doc["version"].is<int>()) {
        doc["version"] = PROTOCOL_CONFIG_VERSION;
    }
    if (!doc["modbusRtu"]["enabled"].is<bool>()) {
        doc["modbusRtu"]["enabled"] = true;
    }
    if (!doc["modbusRtu"]["mode"].is<const char*>()) {
        doc["modbusRtu"]["mode"] = "master";
    }
    const int transferType = doc["modbusRtu"]["transferType"] | 0;
    if (transferType != 0 && transferType != 1) {
        doc["modbusRtu"]["transferType"] = 0;
    }
    doc.as<JsonObject>().remove("modbusTcp");
    if (!doc["mqtt"]["enabled"].is<bool>()) {
        doc["mqtt"]["enabled"] = true;
    }
    ensureFastBeeOtaTopics(doc["mqtt"].as<JsonObject>());
    disableUnsupportedBinaryOtaTopics(doc["mqtt"].as<JsonObject>());
}

bool loadProtocolConfigDocument(JsonDocument& doc) {
    if (!LittleFS.exists(PROTOCOL_CONFIG_PATH)) {
        normalizeProtocolConfig(doc);
        return false;
    }

    File f = LittleFS.open(PROTOCOL_CONFIG_PATH, "r");
    if (!f) {
        normalizeProtocolConfig(doc);
        return false;
    }

    DeserializationError err = deserializeJson(doc, f);
    f.close();
    normalizeProtocolConfig(doc);
    return !err;
}

bool loadMqttConfigDocument(JsonDocument& doc) {
    if (!LittleFS.exists(PROTOCOL_CONFIG_PATH)) return false;

    File file = LittleFS.open(PROTOCOL_CONFIG_PATH, "r");
    if (!file) return false;

    JsonDocument filter;
    filter["mqtt"] = true;
    DeserializationError error = deserializeJson(
        doc, file, DeserializationOption::Filter(filter));
    file.close();
    return !error;
}

void copyJsonMember(JsonObject dst, JsonObject src, const char* key) {
    if (!src[key].isNull()) {
        dst[key].set(src[key]);
    }
}

void copyMqttTopicList(JsonArray dst, JsonArray src, bool publishList) {
    for (JsonObject topicIn : src) {
        const int topicType = topicIn["topicType"] | (publishList ? 0 : 1);
        JsonObject topicOut = dst.add<JsonObject>();
        topicOut["topic"] = topicIn["topic"] | "";
        topicOut["qos"] = topicIn["qos"] | 0;
        topicOut["enabled"] = topicType == MQTT_TOPIC_TYPE_OTA_BINARY
            ? false
            : (topicIn["enabled"] | true);
        topicOut["autoPrefix"] = topicIn["autoPrefix"] | true;
        topicOut["topicType"] = topicType;
        if (publishList) {
            topicOut["retain"] = topicIn["retain"] | false;
        }
    }
}

void sendCompactMqttConfigFromFile(AsyncWebServerRequest* request) {
    // Store only the MQTT section. On constrained targets this avoids parsing
    // large Modbus task/device arrays into DRAM just to render the MQTT tab.
    auto source = FastBee::makeJsonDocument();
    loadMqttConfigDocument(source);

    auto doc = FastBee::makeJsonDocument();
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    JsonObject mqttOut = data["mqtt"].to<JsonObject>();
    JsonObject mqttIn = source["mqtt"].as<JsonObject>();
    if (mqttIn.isNull()) {
        mqttIn = source["mqtt"].to<JsonObject>();
    }
    ensureFastBeeOtaTopics(mqttIn);
    disableUnsupportedBinaryOtaTopics(mqttIn);

    // 运行时检测 PSRAM：有 PSRAM 则 TLS 可用，比编译期 BOARD_HAS_PSRAM 更准确
    // esp32-F4R0 固件部署到带 PSRAM 的板子时也能正确报告 TLS 支持
    mqttOut["tlsSupported"] = (psramFound() && ESP.getPsramSize() > 0);
    mqttOut["enabled"] = mqttIn["enabled"] | true;
    mqttOut["scheme"] = mqttIn["scheme"] | "mqtt";
    mqttOut["server"] = mqttIn["server"] | "iot.fastbee.cn";
    mqttOut["otaBaseUrl"] = mqttIn["otaBaseUrl"] | "";
    mqttOut["port"] = mqttIn["port"] | 1883;
    mqttOut["clientId"] = mqttIn["clientId"] | "";
    mqttOut["username"] = mqttIn["username"] | "";
    mqttOut["password"] = mqttIn["password"] | "";
    mqttOut["keepAlive"] = mqttIn["keepAlive"] | 60;
    mqttOut["autoReconnect"] = mqttIn["autoReconnect"] | true;
    mqttOut["authType"] = mqttIn["authType"] | 0;
    mqttOut["mqttSecret"] = mqttIn["mqttSecret"] | "";
    mqttOut["authCode"] = mqttIn["authCode"] | "";
    FastBee::ProtocolSecret::redactMqttConfig(mqttOut);
    mqttOut["willTopic"] = mqttIn["willTopic"] | "";
    mqttOut["willPayload"] = mqttIn["willPayload"] | "";
    mqttOut["willQos"] = mqttIn["willQos"] | 0;
    mqttOut["willRetain"] = mqttIn["willRetain"] | false;
    mqttOut["longitude"] = mqttIn["longitude"] | 0;
    mqttOut["latitude"] = mqttIn["latitude"] | 0;
    mqttOut["iccid"] = mqttIn["iccid"] | "";
    mqttOut["cardPlatformId"] = mqttIn["cardPlatformId"] | 0;
    mqttOut["summary"] = mqttIn["summary"] | "";
    copyMqttTopicList(
        mqttOut["publishTopics"].to<JsonArray>(),
        mqttIn["publishTopics"].as<JsonArray>(),
        true);
    copyMqttTopicList(
        mqttOut["subscribeTopics"].to<JsonArray>(),
        mqttIn["subscribeTopics"].as<JsonArray>(),
        false);
    HandlerUtils::sendJsonStream(request, doc);
}

void sendRedactedProtocolConfigFromFile(AsyncWebServerRequest* request) {
    // JsonDocument grows to the actual minified input size. Parsing directly
    // into data avoids both a fixed 32 KB reserve and a second full document.
    auto doc = FastBee::makeJsonDocument();
    doc["success"] = true;
    doc["data"].to<JsonObject>();

    if (LittleFS.exists(PROTOCOL_CONFIG_PATH)) {
        File file = LittleFS.open(PROTOCOL_CONFIG_PATH, "r");
        if (file) {
            DeserializationError error = deserializeJson(doc["data"], file);
            file.close();
            if (error) {
                LOG_ERRORF("Protocol config response redaction failed: %s", error.c_str());
                doc["data"].clear();
                doc["data"].to<JsonObject>();
            }
        }
    }

    JsonObject config = doc["data"].as<JsonObject>();
    JsonObject mqtt = config["mqtt"].as<JsonObject>();
    if (mqtt.isNull()) {
        mqtt = config["mqtt"].to<JsonObject>();
    }
    ensureFastBeeOtaTopics(mqtt);
    disableUnsupportedBinaryOtaTopics(mqtt);
    FastBee::ProtocolSecret::redactMqttConfig(mqtt);
    FastBee::ProtocolSecret::redactRemoteConfig(config);
    HandlerUtils::sendJsonStream(request, doc);
}

void copyModbusTaskSummary(JsonObject out, JsonObject in) {
    out["enabled"] = in["enabled"] | true;
    out["name"] = in["name"] | "";
    out["label"] = in["label"] | "";
    out["slaveAddress"] = in["slaveAddress"] | 1;
    out["functionCode"] = in["functionCode"] | 3;
    out["startAddress"] = in["startAddress"] | 0;
    out["quantity"] = in["quantity"] | 1;

    JsonArray mappingsOut = out["mappings"].to<JsonArray>();
    JsonArray mappingsIn = in["mappings"].as<JsonArray>();
    for (JsonObject mappingIn : mappingsIn) {
        JsonObject mappingOut = mappingsOut.add<JsonObject>();
        copyJsonMember(mappingOut, mappingIn, "sensorId");
        copyJsonMember(mappingOut, mappingIn, "unit");
        copyJsonMember(mappingOut, mappingIn, "regOffset");
    }
}

void copyModbusDeviceSummary(JsonObject out, JsonObject in) {
    out["enabled"] = in["enabled"] | true;
    out["name"] = in["name"] | "";
    out["sensorId"] = in["sensorId"] | "";
    out["deviceType"] = in["deviceType"] | "relay";
    out["slaveAddress"] = in["slaveAddress"] | 1;
    out["channelCount"] = in["channelCount"] | 2;
    out["pwmResolution"] = in["pwmResolution"] | 8;
    copyJsonMember(out, in, "coilBase");
    copyJsonMember(out, in, "ncMode");
    copyJsonMember(out, in, "controlProtocol");
    copyJsonMember(out, in, "batchRegister");
    copyJsonMember(out, in, "batchRegType");
    copyJsonMember(out, in, "pwmRegBase");
    copyJsonMember(out, in, "pidDecimals");
    // 电机参数透传
    copyJsonMember(out, in, "motorDecimals");
    copyJsonMember(out, in, "motorProtocol");
    copyJsonMember(out, in, "motorMinPosition");
    copyJsonMember(out, in, "motorMaxPosition");
    copyJsonMember(out, in, "motorCurrentPosition");
    copyJsonMember(out, in, "motorMoveStep");
    copyJsonMember(out, in, "motorLastPulse");
    if (in["motorRegs"].is<JsonArray>()) {
        JsonArray mrOut = out["motorRegs"].to<JsonArray>();
        for (JsonVariant mv : in["motorRegs"].as<JsonArray>())
            mrOut.add(mv.as<int>());
    }
    if (in["pidAddrs"].is<JsonArray>()) {
        JsonArray paOut = out["pidAddrs"].to<JsonArray>();
        for (JsonVariant pv : in["pidAddrs"].as<JsonArray>())
            paOut.add(pv.as<int>());
    }
}

void copyPeriphExecModbusSummary(JsonObject out, JsonObject in) {
    out["enabled"] = in["enabled"] | true;
    out["transferType"] = in["transferType"] | 0;

    JsonObject masterOut = out["master"].to<JsonObject>();
    JsonObject masterIn = in["master"].as<JsonObject>();

    // Master 高级参数使用硬编码常量（不再从 JSON 读取）
    masterOut["responseTimeout"] = 1000;
    masterOut["maxRetries"] = 2;
    masterOut["interPollDelay"] = 100;

    JsonArray tasksOut = masterOut["tasks"].to<JsonArray>();
    JsonArray tasksIn = masterIn["tasks"].as<JsonArray>();
    for (JsonObject taskIn : tasksIn) {
        JsonObject taskOut = tasksOut.add<JsonObject>();
        copyModbusTaskSummary(taskOut, taskIn);
    }

    JsonArray devicesOut = masterOut["devices"].to<JsonArray>();
    JsonArray devicesIn = masterIn["devices"].as<JsonArray>();
    for (JsonObject deviceIn : devicesIn) {
        JsonObject deviceOut = devicesOut.add<JsonObject>();
        copyModbusDeviceSummary(deviceOut, deviceIn);
    }
}

#if FASTBEE_ENABLE_MODBUS
void copyPeriphExecModbusSummary(JsonObject out, const ModbusConfig& cfg, bool enabled) {
    out["enabled"] = enabled;
    out["transferType"] = cfg.transferType;

    JsonObject masterOut = out["master"].to<JsonObject>();

    // Master 高级参数透传
    masterOut["responseTimeout"] = cfg.master.responseTimeout;
    masterOut["maxRetries"] = cfg.master.maxRetries;
    masterOut["interPollDelay"] = cfg.master.interPollDelay;

    JsonArray tasksOut = masterOut["tasks"].to<JsonArray>();
    for (uint8_t i = 0; i < cfg.master.taskCount && i < Protocols::MODBUS_MAX_POLL_TASKS; ++i) {
        const PollTask& task = cfg.master.tasks[i];
        JsonObject taskOut = tasksOut.add<JsonObject>();
        taskOut["enabled"] = task.enabled;
        taskOut["name"] = task.name;
        taskOut["label"] = task.name;
        taskOut["slaveAddress"] = task.slaveAddress;
        taskOut["functionCode"] = task.functionCode;
        taskOut["startAddress"] = task.startAddress;
        taskOut["quantity"] = task.quantity;

        JsonArray mappingsOut = taskOut["mappings"].to<JsonArray>();
        for (uint8_t j = 0; j < task.mappingCount && j < Protocols::MODBUS_MAX_MAPPINGS_PER_TASK; ++j) {
            const RegisterMapping& mapping = task.mappings[j];
            JsonObject mappingOut = mappingsOut.add<JsonObject>();
            mappingOut["sensorId"] = mapping.sensorId;
            mappingOut["unit"] = mapping.unit;
            mappingOut["regOffset"] = mapping.regOffset;
        }
    }

    JsonArray devicesOut = masterOut["devices"].to<JsonArray>();
    for (uint8_t i = 0; i < cfg.master.deviceCount && i < Protocols::MODBUS_MAX_SUB_DEVICES; ++i) {
        const ModbusSubDevice& device = cfg.master.devices[i];
        JsonObject deviceOut = devicesOut.add<JsonObject>();
        deviceOut["enabled"] = device.enabled;
        deviceOut["name"] = device.name;
        deviceOut["sensorId"] = device.sensorId;
        deviceOut["deviceType"] = device.deviceType;
        deviceOut["slaveAddress"] = device.slaveAddress;
        deviceOut["channelCount"] = device.channelCount;
        deviceOut["pwmResolution"] = device.pwmResolution;
        deviceOut["coilBase"] = device.coilBase;
        deviceOut["ncMode"] = device.ncMode;
        deviceOut["controlProtocol"] = device.controlProtocol;
        deviceOut["batchRegister"] = device.batchRegister;
        deviceOut["batchRegType"] = device.batchRegType;
        deviceOut["pwmRegBase"] = device.pwmRegBase;
        deviceOut["pidDecimals"] = device.pidDecimals;
        // 电机参数透传
        deviceOut["motorDecimals"] = device.motorDecimals;
        saveMotorProtocol(deviceOut, device.motorProtocol);
        deviceOut["motorMinPosition"] = device.motorMinPosition;
        deviceOut["motorMaxPosition"] = device.motorMaxPosition;
        deviceOut["motorCurrentPosition"] = device.motorCurrentPosition;
        deviceOut["motorMoveStep"] = device.motorMoveStep;
        deviceOut["motorLastPulse"] = device.motorLastPulse;
        JsonArray mrOut = deviceOut["motorRegs"].to<JsonArray>();
        for (uint8_t j = 0; j < 5; j++) mrOut.add(device.motorRegs[j]);
        JsonArray paOut = deviceOut["pidAddrs"].to<JsonArray>();
        for (uint8_t j = 0; j < 6; j++) paOut.add(device.pidAddrs[j]);
    }
}
#endif // FASTBEE_ENABLE_MODBUS

void sendCompactPeriphExecConfig(AsyncWebServerRequest* request, ProtocolManager* protocolManager) {
    auto doc = FastBee::makeJsonDocument(12288);
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();

#if FASTBEE_ENABLE_MODBUS
    auto modbusLease = protocolManager
        ? protocolManager->acquireModbusHandler(pdMS_TO_TICKS(500))
        : ProtocolManager::ModbusHandlerLease();
    ModbusHandler* modbus = modbusLease.get();
    if (modbus) {
        JsonObject rtuOut = data["modbusRtu"].to<JsonObject>();
        copyPeriphExecModbusSummary(rtuOut, modbus->getConfigRef(), modbus->isRunning());
    } else
#endif
    {
        JsonObject rtuOut = data["modbusRtu"].to<JsonObject>();
        rtuOut["enabled"] = false;
        rtuOut["transferType"] = 0;
        JsonObject masterOut = rtuOut["master"].to<JsonObject>();
        masterOut["tasks"].to<JsonArray>();
        masterOut["devices"].to<JsonArray>();
    }

    HandlerUtils::sendJsonStream(request, doc);
}

void sendCompactRuntimeProtocolConfigFromFile(AsyncWebServerRequest* request, bool deviceControlOnly) {
    auto source = FastBee::makeJsonDocument(32768);
    loadProtocolConfigDocument(source);

    auto doc = FastBee::makeJsonDocument(12288);
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    data["compact"] = true;

    JsonObject rtuIn = source["modbusRtu"].as<JsonObject>();
    if (!rtuIn.isNull()) {
        JsonObject rtuOut = data["modbusRtu"].to<JsonObject>();
        copyPeriphExecModbusSummary(rtuOut, rtuIn);
    } else {
        JsonObject rtuOut = data["modbusRtu"].to<JsonObject>();
        rtuOut["enabled"] = false;
        rtuOut["transferType"] = 0;
        JsonObject masterOut = rtuOut["master"].to<JsonObject>();
        masterOut["tasks"].to<JsonArray>();
        masterOut["devices"].to<JsonArray>();
    }

    HandlerUtils::sendJsonStream(request, doc);
}

void sendCompactRuntimeProtocolConfig(AsyncWebServerRequest* request,
                                      ProtocolManager* protocolManager,
                                      bool deviceControlOnly) {
    auto doc = FastBee::makeJsonDocument(12288);
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    data["compact"] = true;

    bool hasRuntimeData = false;
#if FASTBEE_ENABLE_MODBUS
    auto modbusLease = protocolManager
        ? protocolManager->acquireModbusHandler(pdMS_TO_TICKS(500))
        : ProtocolManager::ModbusHandlerLease();
    ModbusHandler* modbus = modbusLease.get();
    if (modbus) {
        const ModbusConfig& cfg = modbus->getConfigRef();
        if (cfg.master.taskCount > 0 || cfg.master.deviceCount > 0) {
            JsonObject rtuOut = data["modbusRtu"].to<JsonObject>();
            copyPeriphExecModbusSummary(rtuOut, cfg, modbus->isRunning());
            hasRuntimeData = true;
        }
    }
#endif
    if (!hasRuntimeData) {
        // Runtime config empty/unavailable, fall back to file
        sendCompactRuntimeProtocolConfigFromFile(request, deviceControlOnly);
        return;
    }

    HandlerUtils::sendJsonStream(request, doc);
}

bool isInternalHeapTight(uint32_t minFreeHeap, uint32_t minMaxAlloc) {
    // 审计 P2-4：函数名即内部堆，必须用 DRAM 口径（总堆含 PSRAM 会误判）
    return FastBee::MemoryBudget::dramFree() < minFreeHeap ||
           FastBee::MemoryBudget::dramLargest() < minMaxAlloc;
}

void sendDegradedRuntimeProtocolConfig(AsyncWebServerRequest* request) {
    AsyncWebServerResponse* response = request->beginResponse(200, "application/json",
        F("{\"success\":true,\"degraded\":true,\"message\":\"Low memory - runtime protocol config temporarily reduced\",\"data\":{\"compact\":true,\"degraded\":true,\"modbusRtu\":{\"enabled\":false,\"degraded\":true,\"transferType\":0,\"master\":{\"tasks\":[],\"devices\":[]}}}}"));
    HandlerUtils::sendWithClose(request, response);
}

void sendDegradedPeriphExecProtocolConfig(AsyncWebServerRequest* request) {
    AsyncWebServerResponse* response = request->beginResponse(200, "application/json",
        F("{\"success\":true,\"degraded\":true,\"message\":\"Low memory - periph-exec protocol config temporarily reduced\",\"data\":{\"degraded\":true,\"modbusRtu\":{\"enabled\":false,\"degraded\":true,\"transferType\":0,\"master\":{\"tasks\":[],\"devices\":[]}}}}"));
    HandlerUtils::sendWithClose(request, response);
}
}

ProtocolRouteHandler::ProtocolRouteHandler(WebHandlerContext* ctx)
    : ctx(ctx)
#if FASTBEE_ENABLE_MODBUS
      , modbusHandler(std::unique_ptr<ModbusRouteHandler>(new ModbusRouteHandler(ctx)))
#endif
#if FASTBEE_ENABLE_MQTT
      , mqttHandler(std::unique_ptr<MqttRouteHandler>(new MqttRouteHandler(ctx)))
#endif
{
}

ProtocolRouteHandler::~ProtocolRouteHandler() = default;

void ProtocolRouteHandler::setupRoutes(AsyncWebServer* server) {
    // 协议配置 API
    server->on("/api/protocol/config", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleGetProtocolConfig(request);
    });

    server->on("/api/protocol/config", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleSaveProtocolConfig(request);
    });

    server->on("/api/protocol/mqtt/config", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleSaveProtocolConfig(request);
    });

    server->on("/api/protocol/modbus-rtu/config", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleSaveProtocolConfig(request);
    });

    // 委托给 ModbusRouteHandler 注册所有 /api/modbus/* 路由
#if FASTBEE_ENABLE_MODBUS
    if (modbusHandler) {
        modbusHandler->setupRoutes(server);
    }
#endif

    // 委托给 MqttRouteHandler 注册所有 /api/mqtt/* 路由
#if FASTBEE_ENABLE_MQTT
    if (mqttHandler) {
        mqttHandler->setupRoutes(server);
    }
#endif
}

void ProtocolRouteHandler::handleGetProtocolConfig(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    String section = ctx->getParamValue(request, "section", "");
    bool compact = ctx->getParamBool(request, "compact", false);
    if (compact && section == "ota") {
        if (HandlerUtils::checkLowMemory(request, 4096)) return;
        auto source = FastBee::makeJsonDocument();
        String error;
        if (!FastBee::OtaConfigStorage::load(source, error)) {
            ctx->sendError(request, 500, error);
            return;
        }
        JsonDocument data;
        data["otaBaseUrl"] = source["mqtt"]["otaBaseUrl"] | "";
        ctx->sendSuccess(request, data);
        return;
    }
    if (compact && section == "mqtt") {
        if (HandlerUtils::checkLowMemory(request, 4096)) return;
        sendCompactMqttConfigFromFile(request);
        return;
    }
    if (compact && section == "periph-exec") {
        if (isInternalHeapTight(8192, 4096)) {
            sendDegradedPeriphExecProtocolConfig(request);
            return;
        }
        if (HandlerUtils::checkLowMemory(request, 4096)) return;
        sendCompactPeriphExecConfig(request, ctx->protocolManager);
        return;
    }
    if (compact && (section == "device-control" || section == "runtime")) {
        if (section == "runtime" && isInternalHeapTight(12288, 6144)) {
            // Low internal heap but PSRAM may be available; try file-based response
            sendCompactRuntimeProtocolConfigFromFile(request, false);
            return;
        }
        if (HandlerUtils::checkLowMemory(request, 4096)) return;
        sendCompactRuntimeProtocolConfig(request, ctx->protocolManager, section == "device-control");
        return;
    }

    if (HandlerUtils::rejectHeavyRequestOnPressure(request, "Protocol config", MemoryGuardLevel::SEVERE, 8)) {
        return;
    }
    if (HandlerUtils::checkLowMemory(request, 12288)) return;

    // Never stream protocol.json verbatim: it contains MQTT credentials.
    // Parse into the response's nested data object so secrets can be masked
    // without holding a second full configuration document in memory.
    sendRedactedProtocolConfigFromFile(request);
}

void ProtocolRouteHandler::handleSaveProtocolConfig(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

#if FASTBEE_ENABLE_REMOTE_RESOURCES
    struct RemoteConfigTransaction {
        bool locked = FastBee::RemoteResourceManager::instance()
                          .beginProtocolConfigTransaction();
        ~RemoteConfigTransaction() {
            if (locked) FastBee::RemoteResourceManager::instance()
                            .endProtocolConfigTransaction();
        }
        void release() {
            if (locked) FastBee::RemoteResourceManager::instance()
                            .endProtocolConfigTransaction();
            locked = false;
        }
    } remoteConfigTransaction;
    if (!remoteConfigTransaction.locked) {
        ctx->sendError(request, 503, "Protocol configuration is busy; retry shortly");
        return;
    }
#endif

    // Task 3.3: 完整 protocol.json 加载使用 PSRAM 分配器（有 PSRAM 时），降低 DRAM 峰值
    JsonDocument doc = FastBee::makeJsonDocument(8192);

    // 增量更新：先加载现有配置
    // fail-safe：文件存在但解析失败时拒绝保存。若继续，会把增量参数写进
    // 空 doc 后整体回写，导致其余协议节（mqtt/modbusRtu 等）丢失
    if (LittleFS.exists(PROTOCOL_CONFIG_PATH)) {
        File f = LittleFS.open(PROTOCOL_CONFIG_PATH, "r");
        if (!f) {
            ctx->sendError(request, 500, "Failed to open protocol config for update");
            return;
        }
        DeserializationError loadErr = deserializeJson(doc, f);
        f.close();
        if (loadErr || !doc.is<JsonObject>()) {
            LOG_ERRORF("Protocol config save aborted: existing config unreadable (%s)", loadErr.c_str());
            ctx->sendError(request, 500, "Existing protocol config unreadable, save aborted");
            return;
        }
    }
    #define GP(key, def) ctx->getParamValue(request, key, def)
    #define GPI(key, def) GP(key, def).toInt()

    bool updateModbusRtu = hasProtocolParam(request, "modbusRtu_enabled") ||
                           hasProtocolParam(request, "modbusRtu_peripheralId") ||
                           hasProtocolParam(request, "modbusRtu_mode") ||
                           hasProtocolParam(request, "modbusRtu_dePin") ||
                           hasProtocolParam(request, "modbusRtu_transferType") ||
                           hasProtocolParam(request, "modbusRtu_master_tasks") ||
                           hasProtocolParam(request, "modbusRtu_master_devices");
    bool updateMqtt = hasProtocolParam(request, "mqtt_enabled") ||
                      hasProtocolParam(request, "mqtt_scheme") ||
                      hasProtocolParam(request, "mqtt_server") ||
                      hasProtocolParam(request, "mqtt_port") ||
                      hasProtocolParam(request, "mqtt_clientId") ||
                      hasProtocolParam(request, "mqtt_username") ||
                      hasProtocolParam(request, "mqtt_password") ||
                      hasProtocolParam(request, "mqtt_keepAlive") ||
                      hasProtocolParam(request, "mqtt_autoReconnect") ||
                      hasProtocolParam(request, "mqtt_authType") ||
                      hasProtocolParam(request, "mqtt_mqttSecret") ||
                      hasProtocolParam(request, "mqtt_authCode") ||
                      hasProtocolParam(request, "mqtt_willTopic") ||
                      hasProtocolParam(request, "mqtt_willPayload") ||
                      hasProtocolParam(request, "mqtt_willQos") ||
                      hasProtocolParam(request, "mqtt_willRetain") ||
                      hasProtocolParam(request, "mqtt_longitude") ||
                      hasProtocolParam(request, "mqtt_latitude") ||
                      hasProtocolParam(request, "mqtt_iccid") ||
                      hasProtocolParam(request, "mqtt_cardPlatformId") ||
                      hasProtocolParam(request, "mqtt_summary") ||
                      hasProtocolParam(request, "mqtt_publishTopics") ||
                      hasProtocolParam(request, "mqtt_subscribeTopics");
    const bool updateOtaBase = hasProtocolParam(request, "mqtt_otaBaseUrl");
    // OTA-only saves must preserve every other protocol member, including
    // legacy topics and disabled transports. They do not restart MQTT/Modbus.
    if (updateMqtt || updateModbusRtu || !updateOtaBase) {
        normalizeProtocolConfig(doc);
        doc["version"] = PROTOCOL_CONFIG_VERSION;
    }
    if (updateOtaBase) {
        String otaBaseUrl = GP("mqtt_otaBaseUrl", "");
        otaBaseUrl.trim();
        String otaBaseUrlError;
        if (!validateOtaBaseUrl(otaBaseUrl, otaBaseUrlError)) {
            ctx->sendError(request, 400, otaBaseUrlError);
            return;
        }
        doc["mqtt"]["otaBaseUrl"] = otaBaseUrl;
    }
    bool clientIdGenerated = false;
    String clientId = doc["mqtt"]["clientId"] | "";

    if (updateModbusRtu && !ctx->requireDeveloperMode(request)) {
        return;
    }

    // Modbus RTU
    if (updateModbusRtu) {
    bool currentEnabled = doc["modbusRtu"]["enabled"] | true;
    String currentPeripheralId = doc["modbusRtu"]["peripheralId"] | "";
    String currentMode = doc["modbusRtu"]["mode"] | "master";
    int currentDePin = doc["modbusRtu"]["dePin"] | -1;
    int currentTransferType = doc["modbusRtu"]["transferType"] | 0;

    if (hasProtocolParam(request, "modbusRtu_transferType")) {
        String requestedTransferType = GP("modbusRtu_transferType", "");
        requestedTransferType.trim();
        if (requestedTransferType != "0" && requestedTransferType != "1") {
            ctx->sendError(request, 400,
                           "modbusRtu_transferType must be 0 (JSON) or 1 (RAW RTU)");
            return;
        }
        currentTransferType = requestedTransferType == "1" ? 1 : 0;
    }

    doc["modbusRtu"]["enabled"] = GP("modbusRtu_enabled", currentEnabled ? "true" : "false") == "true";
    doc["modbusRtu"]["peripheralId"] = GP("modbusRtu_peripheralId", currentPeripheralId);
    doc["modbusRtu"]["mode"] = GP("modbusRtu_mode", currentMode);
    doc["modbusRtu"]["dePin"] = GPI("modbusRtu_dePin", String(currentDePin));
    doc["modbusRtu"]["transferType"] = currentTransferType;
    // workMode 已移除：由轮询任务配置自动推导，不再保存

    // Modbus RTU Master 通信参数（responseTimeout/maxRetries/interPollDelay）已从构造函数硬编码，
    // 实际执行时由外设执行-轮询触发器覆盖，不写入 protocol.json。

    // Modbus RTU Master 轮询任务
    // 仅在前端实际发送了 tasks 数据时才清空并重建数组，避免从其他标签页保存时误删现有配置
    String masterTasksJson = GP("modbusRtu_master_tasks", "");
    if (!validateModbusArrayUpdate(ctx, request, doc["modbusRtu"]["master"]["tasks"].as<JsonArrayConst>(), masterTasksJson, true)) return;
    if (masterTasksJson.length() > 0) {
    JsonArray masterTasks = doc["modbusRtu"]["master"]["tasks"].to<JsonArray>();
    if (masterTasksJson.length() > 2) {
        JsonDocument tasksDoc;
        DeserializationError tasksErr = deserializeJson(tasksDoc, masterTasksJson);
        if (!tasksErr && tasksDoc.is<JsonArray>()) {
            JsonArray arr = tasksDoc.as<JsonArray>();
            uint8_t taskCount = 0;
            for (JsonVariant v : arr) {
                if (taskCount >= Protocols::MODBUS_MAX_POLL_TASKS) break;
                int functionCode = v["functionCode"] | 3;
                if (functionCode != 1 && functionCode != 2 && functionCode != 3 && functionCode != 4) {
                    functionCode = 3;
                }
                JsonObject taskObj = masterTasks.add<JsonObject>();
                taskObj["slaveAddress"] = clampProtocolValue<int>(v["slaveAddress"] | 1, 1, 247);
                taskObj["functionCode"] = functionCode;
                taskObj["startAddress"] = clampProtocolValue<int>(v["startAddress"] | 0, 0, 65535);
                taskObj["quantity"] = clampProtocolValue<int>(v["quantity"] | 10, 1, Protocols::MODBUS_MAX_REGISTERS_PER_READ);
                taskObj["pollInterval"] = clampProtocolValue<int>(v["pollInterval"] | 30,
                                                                  PROTOCOL_MIN_MODBUS_POLL_INTERVAL_SEC,
                                                                  PROTOCOL_MAX_MODBUS_POLL_INTERVAL_SEC);
                taskObj["enabled"] = v["enabled"] | true;
                taskObj["name"] = v["name"] | "";
                
                // 寄存器映射
                if (v.containsKey("mappings") && v["mappings"].is<JsonArray>()) {
                    JsonArray mappingsArr = taskObj["mappings"].to<JsonArray>();
                    for (JsonVariant mv : v["mappings"].as<JsonArray>()) {
                        JsonObject mo = mappingsArr.add<JsonObject>();
                        mo["regOffset"] = mv["regOffset"] | 0;
                        mo["dataType"] = mv["dataType"] | 0;
                        mo["scaleFactor"] = mv["scaleFactor"] | 1.0;
                        mo["decimalPlaces"] = mv["decimalPlaces"] | 1;
                        mo["sensorId"] = mv["sensorId"] | "";
                        mo["unit"] = mv["unit"] | "";
                    }
                }
                taskCount++;
            }
        }
    }
    } // end: masterTasksJson.length() > 0

    // Modbus RTU Master 子设备
    // 仅在前端实际发送了 devices 数据时才清空并重建数组
    String masterDevicesJson = GP("modbusRtu_master_devices", "");
    if (!validateModbusArrayUpdate(ctx, request, doc["modbusRtu"]["master"]["devices"].as<JsonArrayConst>(), masterDevicesJson, false)) return;
    if (masterDevicesJson.length() > 0) {
    JsonArray masterDevices = doc["modbusRtu"]["master"]["devices"].to<JsonArray>();
    if (masterDevicesJson.length() > 2) {
        JsonDocument devicesDoc;
        DeserializationError devErr = deserializeJson(devicesDoc, masterDevicesJson);
        if (!devErr && devicesDoc.is<JsonArray>()) {
            JsonArray arr = devicesDoc.as<JsonArray>();
            for (JsonVariant dv : arr) {
                JsonObject devObj = masterDevices.add<JsonObject>();
                devObj["name"]            = dv["name"] | "Device";
                devObj["sensorId"]        = dv["sensorId"] | "";
                devObj["deviceType"]      = dv["deviceType"] | "relay";
                devObj["slaveAddress"]    = dv["slaveAddress"] | 1;
                devObj["channelCount"]    = dv["channelCount"] | 2;
                devObj["coilBase"]        = dv["coilBase"] | 0;
                devObj["ncMode"]          = dv["ncMode"] | false;
                devObj["controlProtocol"] = dv["controlProtocol"] | 0;
                devObj["batchRegister"]   = dv["batchRegister"] | 0;
                devObj["pwmRegBase"]      = dv["pwmRegBase"] | 0;
                devObj["pwmResolution"]   = dv["pwmResolution"] | 8;
                devObj["pidDecimals"]     = dv["pidDecimals"] | 1;
                devObj["enabled"]         = dv["enabled"] | true;
                if (dv.containsKey("pidAddrs") && dv["pidAddrs"].is<JsonArray>()) {
                    JsonArray pa = devObj["pidAddrs"].to<JsonArray>();
                    for (JsonVariant pv : dv["pidAddrs"].as<JsonArray>())
                        pa.add(pv.as<int>());
                }
                // 电机参数
                devObj["motorDecimals"] = dv["motorDecimals"] | 0;
                if (dv.containsKey("motorProtocol")) devObj["motorProtocol"].set(dv["motorProtocol"]);
                devObj["motorMinPosition"] = dv["motorMinPosition"] | 0;
                devObj["motorMaxPosition"] = dv["motorMaxPosition"] | 0;
                devObj["motorCurrentPosition"] = dv["motorCurrentPosition"] | 0;
                devObj["motorMoveStep"] = dv["motorMoveStep"] | 0;
                devObj["motorLastPulse"] = dv["motorLastPulse"] | 0;
                if (dv.containsKey("motorRegs") && dv["motorRegs"].is<JsonArray>()) {
                    JsonArray mr = devObj["motorRegs"].to<JsonArray>();
                    for (JsonVariant mv : dv["motorRegs"].as<JsonArray>())
                        mr.add(mv.as<int>());
                }
            }
        }
    }
    } // end: masterDevicesJson.length() > 0
    } // end: updateModbusRtu

    // MQTT
    if (updateMqtt) {
    doc["mqtt"]["enabled"] = GP("mqtt_enabled", "true") == "true";
    String requestedScheme = GP("mqtt_scheme", "mqtt");
    doc["mqtt"]["scheme"] = requestedScheme;
    doc["mqtt"]["server"] = GP("mqtt_server", "iot.fastbee.cn");
    doc["mqtt"]["port"] = GPI("mqtt_port", "1883");

    // Any successful Web save is an explicit user decision.  Do not leave
    // memory-recovery markers behind, otherwise a later reboot could restore
    // an old MQTTS scheme or report MQTT as memory-disabled after the user has
    // re-enabled/edited the protocol.
    doc["mqtt"].remove("disabled_by_memory");
    doc["mqtt"].remove("disable_mqtt_by_memory");
    doc["mqtt"].remove("downgraded_by_memory");
    doc["mqtt"].remove("downgraded_original_scheme");
    doc["mqtt"].remove("downgraded_original_port");

    // 读取 clientId 和 authType，如果 clientId 为空则自动生成
    clientId = GP("mqtt_clientId", "");
    clientId.trim();
    int authType = GPI("mqtt_authType", "0");

    if (clientId.isEmpty()) {
        clientIdGenerated = true;  // 标记为自动生成
        // 1. 获取 deviceId、productNumber、userId：优先从 device.json 读取
        String deviceId = "";
        String productId = "";
        String userId = "1";

        // 尝试从 device.json 读取
        if (LittleFS.exists("/config/device.json")) {
            File devFile = LittleFS.open("/config/device.json", "r");
            if (devFile) {
                JsonDocument devDoc;
                if (!deserializeJson(devDoc, devFile)) {
                    deviceId = devDoc["deviceId"] | "";
                    int pn = devDoc["productNumber"] | 0;
                    if (pn > 0) productId = String(pn);
                    userId = devDoc["userId"] | "1";
                }
                devFile.close();
            }
        }

        // 如果 deviceId 仍为空，生成 FBE+MAC
        if (deviceId.isEmpty()) {
            String mac = WiFi.macAddress();
            mac.replace(":", "");
            deviceId = "FBE" + mac;
        }

        // 如果 productId 仍为空，使用默认值 "1"
        if (productId.isEmpty()) productId = "1";

        // 2. 根据认证类型构建 clientId
        String prefix = (authType == 1) ? "E" : "S";
        clientId = prefix + "&" + deviceId + "&" + productId + "&" + userId;
    }

    doc["mqtt"]["clientId"] = clientId;
    doc["mqtt"]["username"] = GP("mqtt_username", "");
    String submittedPassword = GP("mqtt_password", "");
    if (FastBee::ProtocolSecret::shouldPersist(
            submittedPassword, hasProtocolParam(request, "mqtt_password"))) {
        doc["mqtt"]["password"] = submittedPassword;
    }
    doc["mqtt"]["keepAlive"] = GPI("mqtt_keepAlive", "60");
    doc["mqtt"]["autoReconnect"] = GP("mqtt_autoReconnect", "true") == "true";
    // MQTT 认证配置
    doc["mqtt"]["authType"] = authType;
    String submittedMqttSecret = GP("mqtt_mqttSecret", "");
    if (FastBee::ProtocolSecret::shouldPersist(
            submittedMqttSecret, hasProtocolParam(request, "mqtt_mqttSecret"))) {
        doc["mqtt"]["mqttSecret"] = submittedMqttSecret;
    }
    String submittedAuthCode = GP("mqtt_authCode", "");
    if (FastBee::ProtocolSecret::shouldPersist(
            submittedAuthCode, hasProtocolParam(request, "mqtt_authCode"))) {
        doc["mqtt"]["authCode"] = submittedAuthCode;
    }
    // MQTT 遗嘱消息
    doc["mqtt"]["willTopic"] = GP("mqtt_willTopic", "");
    doc["mqtt"]["willPayload"] = GP("mqtt_willPayload", "");
    doc["mqtt"]["willQos"] = GPI("mqtt_willQos", "0");
    doc["mqtt"]["willRetain"] = GP("mqtt_willRetain", "false") == "true";

    // Card 高级配置
    doc["mqtt"]["longitude"] = atof(GP("mqtt_longitude", "0").c_str());
    doc["mqtt"]["latitude"]  = atof(GP("mqtt_latitude", "0").c_str());
    doc["mqtt"]["iccid"]     = GP("mqtt_iccid", "");
    doc["mqtt"]["cardPlatformId"] = GPI("mqtt_cardPlatformId", "0");
    doc["mqtt"]["summary"]   = GP("mqtt_summary", "");

    // 发布主题配置（仅在前端实际发送了主题数据时才清空重建，避免覆盖已有配置）
    String publishTopicsJson = GP("mqtt_publishTopics", "[]");
    if (hasProtocolParam(request, "mqtt_publishTopics") && publishTopicsJson.length() > 2) {
        JsonArray publishTopics = doc["mqtt"]["publishTopics"].to<JsonArray>();
        JsonDocument topicsDoc;
        DeserializationError err = deserializeJson(topicsDoc, publishTopicsJson);
        if (!err && topicsDoc.is<JsonArray>()) {
            JsonArray arr = topicsDoc.as<JsonArray>();
            for (JsonVariant v : arr) {
                const int topicType = v["topicType"] | 0;
                JsonObject topicObj = publishTopics.add<JsonObject>();
                topicObj["topic"] = v["topic"] | "";
                topicObj["qos"] = v["qos"] | 0;
                topicObj["retain"] = v["retain"] | false;
                topicObj["enabled"] = topicType == MQTT_TOPIC_TYPE_OTA_BINARY
                    ? false
                    : (v["enabled"] | true);
                topicObj["autoPrefix"] = v["autoPrefix"] | true;
                topicObj["topicType"] = topicType;
            }
        }
    }
    // 如果文件中完全没有 publishTopics 字段，初始化一个空数组
    if (doc["mqtt"]["publishTopics"].isNull()) {
        doc["mqtt"]["publishTopics"].to<JsonArray>();
    }

    // 订阅主题配置（同上：仅在前端发送了数据时才清空重建）
    String subscribeTopicsJson = GP("mqtt_subscribeTopics", "[]");
    if (hasProtocolParam(request, "mqtt_subscribeTopics") && subscribeTopicsJson.length() > 2) {
        JsonArray subscribeTopics = doc["mqtt"]["subscribeTopics"].to<JsonArray>();
        JsonDocument subTopicsDoc;
        DeserializationError err = deserializeJson(subTopicsDoc, subscribeTopicsJson);
        if (!err && subTopicsDoc.is<JsonArray>()) {
            JsonArray arr = subTopicsDoc.as<JsonArray>();
            for (JsonVariant v : arr) {
                const int topicType = v["topicType"] | 1;
                JsonObject topicObj = subscribeTopics.add<JsonObject>();
                topicObj["topic"] = v["topic"] | "";
                topicObj["qos"] = v["qos"] | 0;
                topicObj["enabled"] = topicType == MQTT_TOPIC_TYPE_OTA_BINARY
                    ? false
                    : (v["enabled"] | true);
                topicObj["autoPrefix"] = v["autoPrefix"] | true;
                topicObj["topicType"] = topicType;
            }
        }
    }
    if (doc["mqtt"]["subscribeTopics"].isNull()) {
        doc["mqtt"]["subscribeTopics"].to<JsonArray>();
    }
    // Normalize the arrays after rebuilding them from the submitted form.
    // Otherwise an empty/unrelated type=5 row can suppress the FastBee pair
    // until the next GET/restart, while the just-saved runtime config is wrong.
    ensureFastBeeOtaTopics(doc["mqtt"].as<JsonObject>());
    disableUnsupportedBinaryOtaTopics(doc["mqtt"].as<JsonObject>());
    } // end: updateMqtt

    #undef GP
    #undef GPI

    // 让出 CPU 时间，避免文件写入阻塞 async_tcp 任务太久
    yield();

    if (!ConfigStorage::getInstance().saveProtocolConfig(doc)) {
        ctx->sendError(request, 500, "Failed to save protocol config");
        return;
    }
#if FASTBEE_ENABLE_REMOTE_RESOURCES
    // MQTT callbacks take the source mutex while holding the transport mutex.
    // Release the configuration transaction before any protocol lifecycle call.
    remoteConfigTransaction.release();
#endif

    // 让出 CPU 时间，避免文件 I/O + 后续协议重启累计阻塞太久
    yield();

    // 用户显式保存配置后，删除测试连接的 MQTT 备份文件
    // （测试连接的自动恢复机制不应覆盖用户显式保存的配置）
    if (updateMqtt && LittleFS.exists("/config/mqtt-backup.json")) {
        LittleFS.remove("/config/mqtt-backup.json");
    }

    // 保存成功后，根据MQTT启用状态处理
    // 策略：MQTT 配置变更优先热重建（loopTask 中异步执行），避免不必要的设备重启；
    // restartMQTTDeferred 内部按 scheme/PSRAM 实测内存门槛（含释放旧客户端后的
    // 最大连续块检查），门槛不过时由 ProtocolManager::handle() 回退调度设备重启，
    // 确保新配置必定生效且不会在碎片化堆上强行 TLS 握手导致长期不可用
    bool mqttHotRestart = false;
    bool mqttStoppedNow = false;
#if FASTBEE_ENABLE_MQTT
    if (updateMqtt) {
        bool mqttEnabledNow = doc["mqtt"]["enabled"].as<bool>();
        FastBeeFramework* fw = FastBeeFramework::getInstance();
        if (fw && fw->getHealthMonitor()) {
            fw->getHealthMonitor()->clearMqttMemoryOverrideState();
        }
        ProtocolManager* pm = ctx->protocolManager;
        if (!mqttEnabledNow) {
            // MQTT 已禁用：立即停止客户端即可，无需重启也无需重建
            if (pm) {
                pm->stopMQTT();
                mqttStoppedNow = true;
            }
        } else if (pm) {
            // MQTT 启用：置标志由 loopTask 热重建（避免在 async_tcp 小栈里重建 TLS）
            pm->requestMqttRestartAsync();
            mqttHotRestart = true;
        } else {
            // 无 ProtocolManager（不应发生）：保底走设备重启确保配置生效
            SystemRebooter::scheduleConfigReboot("MQTT config changed");
        }
    }
#endif

    // Modbus 保持运行时重启（不涉及 TLS/网络栈，无内存碎片风险）
    bool modbusRestarted = false;
#if FASTBEE_ENABLE_MODBUS
    if (updateModbusRtu && doc["modbusRtu"]["enabled"].as<bool>()) {
        ProtocolManager* pm = ctx->protocolManager;
        if (pm) {
            modbusRestarted = pm->restartModbusDeferred();
        }
    } else if (updateModbusRtu) {
        ProtocolManager* pm = ctx->protocolManager;
        if (pm) {
            pm->stopModbus();
        }
    }
#endif

    JsonDocument resp;
    resp["success"] = true;
    resp["message"] = "Protocol configuration saved";
    resp["data"]["restartRequired"] = false;
    resp["data"]["modbusRestarted"] = modbusRestarted;
    // MQTT 热重建/断开状态：前端依赖这些字段显示正确的通知
    if (mqttHotRestart) {
        resp["data"]["mqttHotRestart"] = true;
        // requestMqttRestartAsync() only queues work for loopTask.  Return a
        // stable pending/reason envelope so the UI can distinguish an
        // accepted asynchronous restart from an immediate connection failure.
        resp["data"]["mqttStatusPending"] = true;
        resp["data"]["mqttStatusReason"] = "MQTT 配置已应用，正在后台连接";
#if FASTBEE_ENABLE_MQTT
        if (ctx->protocolManager) {
            MQTTClient* mqtt = ctx->protocolManager->getMQTTClient();
            if (mqtt && mqtt->getLastErrorCode() != 0) {
                String mqttErrorMessage;
                resp["data"]["mqttLastErrorCode"] = mqtt->getLastErrorCode();
                mqttErrorMessage = mqtt->getLastErrorCode() == -4
                    ? "连接 MQTT Broker 超时，请检查地址和端口"
                    : (mqtt->getLastErrorCode() == -2
                        ? "无法连接 MQTT Broker，请检查 DNS、网络和端口"
                        : "MQTT 连接失败，请查看状态详情");
                resp["data"]["mqttLastErrorMessage"] = mqttErrorMessage;
                resp["data"]["mqttStatusReason"] = mqttErrorMessage;
            }
        }
#endif
    }
    if (mqttStoppedNow) {
        resp["data"]["mqttDisconnected"] = true;
    }
    // 如果 clientId 是自动生成的，返回给前端
    if (clientIdGenerated) {
        resp["data"]["mqttClientId"] = clientId;
    }
    HandlerUtils::sendJsonStream(request, resp);
}
