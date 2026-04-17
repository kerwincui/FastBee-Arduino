#include "./network/handlers/ProtocolRouteHandler.h"
#include "./network/handlers/HandlerUtils.h"
#include "./network/handlers/ModbusRouteHandler.h"
#include "./network/handlers/MqttRouteHandler.h"
#include "./network/WebHandlerContext.h"
#include "./network/NetworkManager.h"
#include "./protocols/ProtocolManager.h"
#include <ArduinoJson.h>
#include <LittleFS.h>

static const char* PROTOCOL_CONFIG_PATH = "/config/protocol.json";

namespace {
constexpr uint16_t PROTOCOL_MIN_MODBUS_POLL_INTERVAL_SEC = 2;
constexpr uint16_t PROTOCOL_MAX_MODBUS_POLL_INTERVAL_SEC = 3600;

template <typename T>
T clampProtocolValue(T value, T minVal, T maxVal) {
    if (value < minVal) return minVal;
    if (value > maxVal) return maxVal;
    return value;
}
}

ProtocolRouteHandler::ProtocolRouteHandler(WebHandlerContext* ctx)
    : ctx(ctx),
      modbusHandler(std::unique_ptr<ModbusRouteHandler>(new ModbusRouteHandler(ctx))),
      mqttHandler(std::unique_ptr<MqttRouteHandler>(new MqttRouteHandler(ctx))) {
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

    // 委托给 ModbusRouteHandler 注册所有 /api/modbus/* 路由
    if (modbusHandler) {
        modbusHandler->setupRoutes(server);
    }

    // 委托给 MqttRouteHandler 注册所有 /api/mqtt/* 路由
    if (mqttHandler) {
        mqttHandler->setupRoutes(server);
    }
}

void ProtocolRouteHandler::handleGetProtocolConfig(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

    JsonDocument doc;

    if (LittleFS.exists(PROTOCOL_CONFIG_PATH)) {
        File f = LittleFS.open(PROTOCOL_CONFIG_PATH, "r");
        if (f) {
            JsonDocument fileCfg;
            DeserializationError err = deserializeJson(fileCfg, f);
            f.close();

            if (!err) {
                doc["success"] = true;
                doc["data"] = fileCfg;
                String out;
                serializeJson(doc, out);
                request->send(200, "application/json", out);
                return;
            }
        }
    }

    doc["success"] = true;
    doc["data"].to<JsonObject>();
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void ProtocolRouteHandler::handleSaveProtocolConfig(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    JsonDocument doc;

    // 增量更新：先加载现有配置
    if (LittleFS.exists(PROTOCOL_CONFIG_PATH)) {
        File f = LittleFS.open(PROTOCOL_CONFIG_PATH, "r");
        if (f) {
            deserializeJson(doc, f);
            f.close();
        }
    }

    #define GP(key, def) ctx->getParamValue(request, key, def)
    #define GPI(key, def) GP(key, def).toInt()

    // Modbus RTU
    doc["modbusRtu"]["enabled"] = GP("modbusRtu_enabled", "false") == "true";
    doc["modbusRtu"]["peripheralId"] = GP("modbusRtu_peripheralId", "");
    doc["modbusRtu"]["timeout"] = 1000; // 硬编码默认值，前端已移除此字段
    doc["modbusRtu"]["mode"] = GP("modbusRtu_mode", "master");
    doc["modbusRtu"]["dePin"] = GPI("modbusRtu_dePin", "-1");
    doc["modbusRtu"]["transferType"] = GPI("modbusRtu_transferType", "0");
    doc["modbusRtu"]["workMode"] = GPI("modbusRtu_workMode", "1");

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
            }
        }
    }
    } // end: masterDevicesJson.length() > 0

    // Modbus TCP
    doc["modbusTcp"]["enabled"] = GP("modbusTcp_enabled", "false") == "true";
    doc["modbusTcp"]["server"] = GP("modbusTcp_server", "192.168.1.100");
    doc["modbusTcp"]["port"] = GPI("modbusTcp_port", "502");
    doc["modbusTcp"]["slaveId"] = GPI("modbusTcp_slaveId", "1");
    doc["modbusTcp"]["timeout"] = GPI("modbusTcp_timeout", "5000");

    // MQTT
    doc["mqtt"]["enabled"] = GP("mqtt_enabled", "true") == "true";
    doc["mqtt"]["server"] = GP("mqtt_server", "iot.fastbee.cn");
    doc["mqtt"]["port"] = GPI("mqtt_port", "1883");

    // 读取 clientId 和 authType，如果 clientId 为空则自动生成
    String clientId = GP("mqtt_clientId", "");
    clientId.trim();
    int authType = GPI("mqtt_authType", "0");
    bool clientIdGenerated = false;  // 标记是否为自动生成

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

        // 如果 productId 仍为空，使用默认值 "0"
        if (productId.isEmpty()) productId = "0";

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

    // 发布主题配置
    String publishTopicsJson = GP("mqtt_publishTopics", "[]");
    JsonArray publishTopics = doc["mqtt"]["publishTopics"].to<JsonArray>();
    if (publishTopicsJson.length() > 2) {
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
    if (publishTopics.size() == 0) {
        JsonObject defaultTopic = publishTopics.add<JsonObject>();
        defaultTopic["topic"] = "";
        defaultTopic["qos"] = 0;
        defaultTopic["retain"] = false;
        defaultTopic["enabled"] = true;
        defaultTopic["autoPrefix"] = false;
        defaultTopic["topicType"] = 0;
    }

    // 订阅主题配置
    String subscribeTopicsJson = GP("mqtt_subscribeTopics", "[]");
    JsonArray subscribeTopics = doc["mqtt"]["subscribeTopics"].to<JsonArray>();
    if (subscribeTopicsJson.length() > 2) {
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
    if (subscribeTopics.size() == 0) {
        JsonObject defaultTopic = subscribeTopics.add<JsonObject>();
        defaultTopic["topic"] = "";
        defaultTopic["qos"] = 0;
        defaultTopic["enabled"] = true;
        defaultTopic["autoPrefix"] = false;
        defaultTopic["topicType"] = 1;
    }

    // HTTP
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

    // CoAP
    doc["coap"]["enabled"] = GP("coap_enabled", "false") == "true";
    doc["coap"]["server"] = GP("coap_server", "coap://example.com");
    doc["coap"]["port"] = GPI("coap_port", "5683");
    doc["coap"]["method"] = GP("coap_method", "POST");
    doc["coap"]["path"] = GP("coap_path", "sensors/temperature");
    doc["coap"]["msgType"] = GP("coap_msgType", "CON");
    doc["coap"]["retransmit"] = GPI("coap_retransmit", "3");
    doc["coap"]["timeout"] = GPI("coap_timeout", "5000");

    // TCP
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

    #undef GP
    #undef GPI

    File f = LittleFS.open(PROTOCOL_CONFIG_PATH, "w");
    if (!f) {
        ctx->sendError(request, 500, "Failed to save protocol config");
        return;
    }

    serializeJsonPretty(doc, f);
    f.close();

    // 保存成功后，根据MQTT启用状态处理连接
    bool mqttReconnected = false;
    bool mqttDisconnected = false;
    int mqttError = 0;
    if (doc["mqtt"]["enabled"].as<bool>()) {
        ProtocolManager* pm = ctx->protocolManager;
        if (pm) {
            // 使用非阻塞重启：仅重载配置，由 loop 自动重连
            // 避免 PubSubClient::connect() 阻塞 Web 服务器
            mqttReconnected = pm->restartMQTTDeferred();
            if (!mqttReconnected) {
                mqttError = -1;  // begin() 失败
            }
        }
    } else {
        // MQTT未启用，断开现有连接
        ProtocolManager* pm = ctx->protocolManager;
        if (pm) {
            pm->stopMQTT();
            mqttDisconnected = true;
        }
    }

    // 保存成功后，根据Modbus启用状态处理
    bool modbusRestarted = false;
    if (doc["modbusRtu"]["enabled"].as<bool>()) {
        ProtocolManager* pm = ctx->protocolManager;
        if (pm) {
            modbusRestarted = pm->restartModbus();
        }
    } else {
        ProtocolManager* pm = ctx->protocolManager;
        if (pm) {
            pm->stopModbus();
        }
    }

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
    if (!mqttReconnected && doc["mqtt"]["enabled"].as<bool>()) {
        resp["data"]["mqttError"] = mqttError;
    }
    String out;
    serializeJson(resp, out);
    request->send(200, "application/json", out);
}
