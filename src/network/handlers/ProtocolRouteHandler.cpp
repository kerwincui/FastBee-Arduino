#include "./network/handlers/ProtocolRouteHandler.h"
#include "./network/handlers/HandlerUtils.h"
#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_MODBUS
#include "./network/handlers/ModbusRouteHandler.h"
#endif
#if FASTBEE_ENABLE_MQTT
#include "./network/handlers/MqttRouteHandler.h"
#endif
#include "./network/WebHandlerContext.h"
#include "./network/NetworkManager.h"
#include "./protocols/ProtocolManager.h"
#include "utils/PsramJsonDocument.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <memory>

static const char* PROTOCOL_CONFIG_PATH = "/config/protocol.json";

namespace {
constexpr uint8_t PROTOCOL_CONFIG_VERSION = 2;
constexpr uint16_t PROTOCOL_MIN_MODBUS_POLL_INTERVAL_SEC = 2;
constexpr uint16_t PROTOCOL_MAX_MODBUS_POLL_INTERVAL_SEC = 3600;

template <typename T>
T clampProtocolValue(T value, T minVal, T maxVal) {
    if (value < minVal) return minVal;
    if (value > maxVal) return maxVal;
    return value;
}

bool hasProtocolParam(AsyncWebServerRequest* request, const char* key) {
    return request && (request->hasParam(key, true) || request->hasParam(key, false));
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
    if (!doc["modbusRtu"]["timeout"].is<int>()) {
        doc["modbusRtu"]["timeout"] = 1000;
    }
    if (!doc["mqtt"]["enabled"].is<bool>()) {
        doc["mqtt"]["enabled"] = true;
    }
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

void copyJsonMember(JsonObject dst, JsonObject src, const char* key) {
    if (!src[key].isNull()) {
        dst[key].set(src[key]);
    }
}

void copyMqttTopicList(JsonArray dst, JsonArray src, bool publishList) {
    for (JsonObject topicIn : src) {
        JsonObject topicOut = dst.add<JsonObject>();
        topicOut["topic"] = topicIn["topic"] | "";
        topicOut["qos"] = topicIn["qos"] | 0;
        topicOut["enabled"] = topicIn["enabled"] | true;
        topicOut["autoPrefix"] = topicIn["autoPrefix"] | true;
        topicOut["topicType"] = topicIn["topicType"] | 0;
        if (publishList) {
            topicOut["retain"] = topicIn["retain"] | false;
        }
    }
}

void sendCompactMqttConfigFromFile(AsyncWebServerRequest* request) {
    auto source = FastBee::makeJsonDocument(32768);
    loadProtocolConfigDocument(source);

    auto doc = FastBee::makeJsonDocument(16384);
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    JsonObject mqttOut = data["mqtt"].to<JsonObject>();
    JsonObject mqttIn = source["mqtt"].as<JsonObject>();

    mqttOut["enabled"] = mqttIn["enabled"] | true;
    mqttOut["scheme"] = mqttIn["scheme"] | "mqtt";
    mqttOut["server"] = mqttIn["server"] | "iot.fastbee.cn";
    mqttOut["port"] = mqttIn["port"] | 1883;
    mqttOut["clientId"] = mqttIn["clientId"] | "";
    mqttOut["username"] = mqttIn["username"] | "";
    mqttOut["password"] = mqttIn["password"] | "";
    mqttOut["keepAlive"] = mqttIn["keepAlive"] | 60;
    mqttOut["autoReconnect"] = mqttIn["autoReconnect"] | true;
    mqttOut["connectionTimeout"] = mqttIn["connectionTimeout"] | 30000;
    mqttOut["authType"] = mqttIn["authType"] | 0;
    mqttOut["mqttSecret"] = mqttIn["mqttSecret"] | "";
    mqttOut["authCode"] = mqttIn["authCode"] | "";
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
}

void copyPeriphExecModbusSummary(JsonObject out, JsonObject in) {
    out["enabled"] = in["enabled"] | true;
    out["transferType"] = in["transferType"] | 0;

    JsonObject masterOut = out["master"].to<JsonObject>();
    JsonObject masterIn = in["master"].as<JsonObject>();

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
    }
}
#endif // FASTBEE_ENABLE_MODBUS

void sendCompactPeriphExecConfig(AsyncWebServerRequest* request, ProtocolManager* protocolManager) {
    auto doc = FastBee::makeJsonDocument(12288);
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();

#if FASTBEE_ENABLE_MODBUS
    ModbusHandler* modbus = protocolManager ? protocolManager->getModbusHandler() : nullptr;
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

#if FASTBEE_ENABLE_TCP
    if (!deviceControlOnly) {
        JsonObject tcpOut = data["modbusTcp"].to<JsonObject>();
        tcpOut["enabled"] = false;
        tcpOut["compact"] = true;
    }
#endif

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
    ModbusHandler* modbus = protocolManager ? protocolManager->getModbusHandler() : nullptr;
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

#if FASTBEE_ENABLE_TCP
    if (!deviceControlOnly) {
        JsonObject tcpOut = data["modbusTcp"].to<JsonObject>();
        tcpOut["enabled"] = false;
        tcpOut["compact"] = true;
    }
#endif

    HandlerUtils::sendJsonStream(request, doc);
}

bool isInternalHeapTight(uint32_t minFreeHeap, uint32_t minMaxAlloc) {
    return ESP.getFreeHeap() < minFreeHeap || ESP.getMaxAllocHeap() < minMaxAlloc;
}

void sendDegradedRuntimeProtocolConfig(AsyncWebServerRequest* request) {
    AsyncWebServerResponse* response = request->beginResponse(200, "application/json",
        F("{\"success\":true,\"degraded\":true,\"message\":\"Low memory - runtime protocol config temporarily reduced\",\"data\":{\"compact\":true,\"degraded\":true,\"modbusRtu\":{\"enabled\":false,\"degraded\":true,\"transferType\":0,\"master\":{\"tasks\":[],\"devices\":[]}},\"modbusTcp\":{\"enabled\":false,\"degraded\":true}}}"));
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

    if (LittleFS.exists(PROTOCOL_CONFIG_PATH)) {
        // 使用 chunked response 避免一次性分配大缓冲区
        // 响应格式: {"success":true,"data":<文件内容>}
        struct ChunkState {
            File file;
            uint8_t phase;  // 0=prefix, 1=file, 2=suffix, 3=done
        };
        auto state = std::make_shared<ChunkState>();
        state->file = LittleFS.open(PROTOCOL_CONFIG_PATH, "r");
        if (!state->file) {
            JsonDocument doc;
            doc["success"] = true;
            doc["data"].to<JsonObject>();
            HandlerUtils::sendJsonStream(request, doc);
            return;
        }
        state->phase = 0;

        request->sendChunked("application/json",
            [state](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
                if (state->phase == 0) {
                    // 发送 prefix
                    static const char prefix[] = "{\"success\":true,\"data\":";
                    size_t pLen = sizeof(prefix) - 1;
                    if (maxLen < pLen) return RESPONSE_TRY_AGAIN;
                    memcpy(buffer, prefix, pLen);
                    state->phase = 1;
                    return pLen;
                }
                if (state->phase == 1) {
                    // 流式读文件
                    if (!state->file.available()) {
                        state->file.close();
                        state->phase = 2;
                        // 发送 suffix
                        buffer[0] = '}';
                        state->phase = 3;
                        return 1;
                    }
                    size_t toRead = std::min(maxLen, (size_t)256);
                    int n = state->file.read(buffer, toRead);
                    return (n > 0) ? (size_t)n : 0;
                }
                // done
                return 0;
            });
        return;
    }

    JsonDocument doc;
    doc["success"] = true;
    doc["data"].to<JsonObject>();
    HandlerUtils::sendJsonStream(request, doc);
}

void ProtocolRouteHandler::handleSaveProtocolConfig(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    JsonDocument doc;

    // 增量更新：先加载现有配置
    if (LittleFS.exists(PROTOCOL_CONFIG_PATH)) {
        File f = LittleFS.open(PROTOCOL_CONFIG_PATH, "r");
        if (f) {
            deserializeJson(doc, f);
            f.close();
        }
    }
    normalizeProtocolConfig(doc);
    doc["version"] = PROTOCOL_CONFIG_VERSION;

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
                      hasProtocolParam(request, "mqtt_publishTopics") ||
                      hasProtocolParam(request, "mqtt_subscribeTopics");
    bool updateModbusTcp = hasProtocolParam(request, "modbusTcp_enabled");
    bool updateHttp = hasProtocolParam(request, "http_enabled");
    bool updateCoap = hasProtocolParam(request, "coap_enabled");
    bool updateTcp = hasProtocolParam(request, "tcp_enabled");
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

    doc["modbusRtu"]["enabled"] = GP("modbusRtu_enabled", currentEnabled ? "true" : "false") == "true";
    doc["modbusRtu"]["peripheralId"] = GP("modbusRtu_peripheralId", currentPeripheralId);
    doc["modbusRtu"]["timeout"] = 1000; // 硬编码默认值，前端已移除此字段
    doc["modbusRtu"]["mode"] = GP("modbusRtu_mode", currentMode);
    doc["modbusRtu"]["dePin"] = GPI("modbusRtu_dePin", String(currentDePin));
    doc["modbusRtu"]["transferType"] = GPI("modbusRtu_transferType", String(currentTransferType));
    // workMode 已移除：由轮询任务配置自动推导，不再保存

    // Modbus RTU Master 配置（仅在配置中不存在时写入默认值，不覆盖已有配置）
    if (!doc["modbusRtu"]["master"].containsKey("responseTimeout"))
        doc["modbusRtu"]["master"]["responseTimeout"] = 1000;
    if (!doc["modbusRtu"]["master"].containsKey("maxRetries"))
        doc["modbusRtu"]["master"]["maxRetries"] = 2;
    if (!doc["modbusRtu"]["master"].containsKey("interPollDelay"))
        doc["modbusRtu"]["master"]["interPollDelay"] = 100;

    // Modbus RTU Master 轮询任务
    // 仅在前端实际发送了 tasks 数据时才清空并重建数组，避免从其他标签页保存时误删现有配置
    String masterTasksJson = GP("modbusRtu_master_tasks", "");
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

#if FASTBEE_ENABLE_TCP
    // Modbus TCP
    if (updateModbusTcp) {
    doc["modbusTcp"]["enabled"] = GP("modbusTcp_enabled", "false") == "true";
    doc["modbusTcp"]["server"] = GP("modbusTcp_server", "192.168.1.100");
    doc["modbusTcp"]["port"] = GPI("modbusTcp_port", "502");
    doc["modbusTcp"]["slaveId"] = GPI("modbusTcp_slaveId", "1");
    doc["modbusTcp"]["timeout"] = GPI("modbusTcp_timeout", "5000");
    }
#endif

    // MQTT
    if (updateMqtt) {
    doc["mqtt"]["enabled"] = GP("mqtt_enabled", "true") == "true";
    doc["mqtt"]["scheme"] = GP("mqtt_scheme", "mqtt");
    doc["mqtt"]["server"] = GP("mqtt_server", "iot.fastbee.cn");
    doc["mqtt"]["port"] = GPI("mqtt_port", "1883");

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
    doc["mqtt"]["password"] = GP("mqtt_password", "");
    doc["mqtt"]["keepAlive"] = GPI("mqtt_keepAlive", "60");
    doc["mqtt"]["autoReconnect"] = GP("mqtt_autoReconnect", "true") == "true";
    doc["mqtt"]["connectionTimeout"] = GPI("mqtt_connectionTimeout", "30000");
    // MQTT 认证配置
    doc["mqtt"]["authType"] = authType;
    doc["mqtt"]["mqttSecret"] = GP("mqtt_mqttSecret", "");
    doc["mqtt"]["authCode"] = GP("mqtt_authCode", "");
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
                JsonObject topicObj = publishTopics.add<JsonObject>();
                topicObj["topic"] = v["topic"] | "";
                topicObj["qos"] = v["qos"] | 0;
                topicObj["retain"] = v["retain"] | false;
                topicObj["enabled"] = v["enabled"] | true;
                topicObj["autoPrefix"] = v["autoPrefix"] | false;
                topicObj["topicType"] = v["topicType"] | 0;
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
                JsonObject topicObj = subscribeTopics.add<JsonObject>();
                topicObj["topic"] = v["topic"] | "";
                topicObj["qos"] = v["qos"] | 0;
                topicObj["enabled"] = v["enabled"] | true;
                topicObj["autoPrefix"] = v["autoPrefix"] | false;
                topicObj["topicType"] = v["topicType"] | 1;
            }
        }
    }
    if (doc["mqtt"]["subscribeTopics"].isNull()) {
        doc["mqtt"]["subscribeTopics"].to<JsonArray>();
    }
    } // end: updateMqtt

#if FASTBEE_ENABLE_HTTP
    // HTTP
    if (updateHttp) {
    doc["http"]["enabled"] = GP("http_enabled", "false") == "true";
    doc["http"]["url"] = GP("http_url", "https://api.example.com");
    doc["http"]["port"] = GPI("http_port", "80");
    doc["http"]["method"] = GP("http_method", "POST");
    doc["http"]["timeout"] = GPI("http_timeout", "30");
    doc["http"]["interval"] = GPI("http_interval", "60");
    doc["http"]["retry"] = GPI("http_retry", "3");
    // HTTP 认证配置
    doc["http"]["authType"] = GP("http_authType", "none");
    doc["http"]["authUser"] = GP("http_authUser", "");
    doc["http"]["authToken"] = GP("http_authToken", "");
    doc["http"]["contentType"] = GP("http_contentType", "application/json");
    }
#endif

#if FASTBEE_ENABLE_COAP
    // CoAP
    if (updateCoap) {
    doc["coap"]["enabled"] = GP("coap_enabled", "false") == "true";
    doc["coap"]["server"] = GP("coap_server", "coap://example.com");
    doc["coap"]["port"] = GPI("coap_port", "5683");
    doc["coap"]["method"] = GP("coap_method", "POST");
    doc["coap"]["path"] = GP("coap_path", "sensors/temperature");
    doc["coap"]["msgType"] = GP("coap_msgType", "CON");
    doc["coap"]["retransmit"] = GPI("coap_retransmit", "3");
    doc["coap"]["timeout"] = GPI("coap_timeout", "5000");
    }
#endif

#if FASTBEE_ENABLE_TCP
    // TCP
    if (updateTcp) {
    doc["tcp"]["enabled"] = GP("tcp_enabled", "false") == "true";
    doc["tcp"]["server"] = GP("tcp_server", "192.168.1.200");
    doc["tcp"]["port"] = GPI("tcp_port", "5000");
    doc["tcp"]["timeout"] = GPI("tcp_timeout", "5000");
    doc["tcp"]["keepAlive"] = GPI("tcp_keepAlive", "60");
    doc["tcp"]["maxRetry"] = GPI("tcp_maxRetry", "5");
    doc["tcp"]["reconnectInterval"] = GPI("tcp_reconnectInterval", "10");
    doc["tcp"]["mode"] = GP("tcp_mode", "client");
    doc["tcp"]["heartbeatMsg"] = GP("tcp_heartbeatMsg", "\\n");
    doc["tcp"]["maxClients"] = GPI("tcp_maxClients", "5");
    doc["tcp"]["idleTimeout"] = GPI("tcp_idleTimeout", "120");
    doc["tcp"]["localPort"] = GPI("tcp_localPort", "8080");
    }
#endif

    #undef GP
    #undef GPI

    // 让出 CPU 时间，避免文件写入阻塞 async_tcp 任务太久
    yield();

    File f = LittleFS.open(PROTOCOL_CONFIG_PATH, "w");
    if (!f) {
        ctx->sendError(request, 500, "Failed to save protocol config");
        return;
    }

    serializeJsonPretty(doc, f);
    f.close();

    // 让出 CPU 时间，避免文件 I/O + 后续协议重启累计阻塞太久
    yield();

    // 保存成功后，根据MQTT启用状态处理连接
    bool mqttReconnected = false;
    bool mqttDisconnected = false;
    int mqttError = 0;
#if FASTBEE_ENABLE_MQTT
    if (updateMqtt && doc["mqtt"]["enabled"].as<bool>()) {
        ProtocolManager* pm = ctx->protocolManager;
        if (pm) {
            // 使用非阻塞重启：仅重载配置，由 loop 自动重连
            // 避免 PubSubClient::connect() 阻塞 Web 服务器
            mqttReconnected = pm->restartMQTTDeferred();
            if (!mqttReconnected) {
                mqttError = -1;  // begin() 失败
            }
        }
    } else if (updateMqtt) {
        // MQTT未启用，断开现有连接
        ProtocolManager* pm = ctx->protocolManager;
        if (pm) {
            pm->stopMQTT();
            mqttDisconnected = true;
        }
    }
#endif

    // 保存成功后，根据Modbus启用状态处理
    bool modbusRestarted = false;
#if FASTBEE_ENABLE_MODBUS
    if (updateModbusRtu && doc["modbusRtu"]["enabled"].as<bool>()) {
        ProtocolManager* pm = ctx->protocolManager;
        if (pm) {
            // 延迟重启：避免在 AsyncTCP 小栈任务中执行 restartModbus()
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
    resp["data"]["mqttReconnected"] = mqttReconnected;
    resp["data"]["mqttDeferred"] = mqttReconnected;  // 标识为延迟连接模式
    resp["data"]["mqttDisconnected"] = mqttDisconnected;
    resp["data"]["modbusRestarted"] = modbusRestarted;
    // 如果 clientId 是自动生成的，返回给前端
    if (clientIdGenerated) {
        resp["data"]["mqttClientId"] = clientId;
    }
    if (updateMqtt && !mqttReconnected && doc["mqtt"]["enabled"].as<bool>()) {
        resp["data"]["mqttError"] = mqttError;
    }
    HandlerUtils::sendJsonStream(request, resp);
}
