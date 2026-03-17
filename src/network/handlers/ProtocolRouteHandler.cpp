#include "./network/handlers/ProtocolRouteHandler.h"
#include "./network/WebHandlerContext.h"
#include "./protocols/ProtocolManager.h"
#include "./protocols/ModbusHandler.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <PubSubClient.h>

static const char* PROTOCOL_CONFIG_PATH = "/config/protocol.json";

ProtocolRouteHandler::ProtocolRouteHandler(WebHandlerContext* ctx)
    : ctx(ctx) {
}

void ProtocolRouteHandler::setupRoutes(AsyncWebServer* server) {
    server->on("/api/protocol/config", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleGetProtocolConfig(request);
    });

    server->on("/api/protocol/config", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleSaveProtocolConfig(request);
    });

    // Modbus Master API
    server->on("/api/modbus/status", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleGetModbusStatus(request);
    });

    server->on("/api/modbus/write", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleModbusWrite(request);
    });

    // MQTT Connection Test API
    server->on("/api/mqtt/test", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleTestMqttConnection(request);
    });

    // MQTT Status API
    server->on("/api/mqtt/status", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleGetMqttStatus(request);
    });

    // MQTT Reconnect API
    server->on("/api/mqtt/reconnect", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleMqttReconnect(request);
    });
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

    #define GP(key, def) ctx->getParamValue(request, key, def)
    #define GPI(key, def) GP(key, def).toInt()

    // Modbus RTU
    doc["modbusRtu"]["enabled"] = GP("modbusRtu_enabled", "false") == "true";
    doc["modbusRtu"]["port"] = GP("modbusRtu_port", "/dev/ttyS0");
    doc["modbusRtu"]["baudRate"] = GPI("modbusRtu_baudRate", "19200");
    doc["modbusRtu"]["dataBits"] = GPI("modbusRtu_dataBits", "8");
    doc["modbusRtu"]["stopBits"] = GPI("modbusRtu_stopBits", "1");
    doc["modbusRtu"]["parity"] = GP("modbusRtu_parity", "none");
    doc["modbusRtu"]["timeout"] = GPI("modbusRtu_timeout", "1000");
    doc["modbusRtu"]["mode"] = GP("modbusRtu_mode", "slave");
    // Modbus RTU 引脚配置
    doc["modbusRtu"]["txPin"] = GPI("modbusRtu_txPin", "17");
    doc["modbusRtu"]["rxPin"] = GPI("modbusRtu_rxPin", "16");
    doc["modbusRtu"]["dePin"] = GPI("modbusRtu_dePin", "-1");
    doc["modbusRtu"]["slaveAddress"] = GPI("modbusRtu_slaveAddress", "1");

    // Modbus RTU Master 配置
    doc["modbusRtu"]["master"]["defaultPollInterval"] = GPI("modbusRtu_master_defaultPollInterval", "30");
    doc["modbusRtu"]["master"]["responseTimeout"] = GPI("modbusRtu_master_responseTimeout", "1000");
    doc["modbusRtu"]["master"]["maxRetries"] = GPI("modbusRtu_master_maxRetries", "2");
    doc["modbusRtu"]["master"]["interPollDelay"] = GPI("modbusRtu_master_interPollDelay", "100");

    // Modbus RTU Master 轮询任务
    String masterTasksJson = GP("modbusRtu_master_tasks", "[]");
    JsonArray masterTasks = doc["modbusRtu"]["master"]["tasks"].to<JsonArray>();
    if (masterTasksJson.length() > 2) {
        JsonDocument tasksDoc;
        DeserializationError tasksErr = deserializeJson(tasksDoc, masterTasksJson);
        if (!tasksErr && tasksDoc.is<JsonArray>()) {
            JsonArray arr = tasksDoc.as<JsonArray>();
            for (JsonVariant v : arr) {
                JsonObject taskObj = masterTasks.add<JsonObject>();
                taskObj["slaveAddress"] = v["slaveAddress"] | 1;
                taskObj["functionCode"] = v["functionCode"] | 3;
                taskObj["startAddress"] = v["startAddress"] | 0;
                taskObj["quantity"] = v["quantity"] | 10;
                taskObj["pollInterval"] = v["pollInterval"] | 30;
                taskObj["enabled"] = v["enabled"] | true;
                taskObj["label"] = v["label"] | "";
            }
        }
    }

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
    doc["mqtt"]["clientId"] = GP("mqtt_clientId", "");
    doc["mqtt"]["username"] = GP("mqtt_username", "");
    doc["mqtt"]["password"] = GP("mqtt_password", "");
    doc["mqtt"]["keepAlive"] = GPI("mqtt_keepAlive", "60");
    doc["mqtt"]["directConnect"] = GP("mqtt_directConnect", "true") == "true";
    doc["mqtt"]["autoReconnect"] = GP("mqtt_autoReconnect", "true") == "true";
    doc["mqtt"]["connectionTimeout"] = GPI("mqtt_connectionTimeout", "30000");
    // MQTT 遗嘱消息
    doc["mqtt"]["willTopic"] = GP("mqtt_willTopic", "");
    doc["mqtt"]["willPayload"] = GP("mqtt_willPayload", "");
    doc["mqtt"]["willQos"] = GPI("mqtt_willQos", "0");
    doc["mqtt"]["willRetain"] = GP("mqtt_willRetain", "false") == "true";

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
                topicObj["content"] = v["content"] | "";
                topicObj["topicType"] = v["topicType"] | 0;
            }
        }
    }
    if (publishTopics.size() == 0) {
        JsonObject defaultTopic = publishTopics.add<JsonObject>();
        defaultTopic["topic"] = "";
        defaultTopic["qos"] = 0;
        defaultTopic["retain"] = false;
        defaultTopic["content"] = "";
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
                topicObj["action"] = v["action"] | "";
                topicObj["topicType"] = v["topicType"] | 1;
            }
        }
    }
    if (subscribeTopics.size() == 0) {
        JsonObject defaultTopic = subscribeTopics.add<JsonObject>();
        defaultTopic["topic"] = "";
        defaultTopic["qos"] = 0;
        defaultTopic["action"] = "";
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

    // 保存成功后，尝试重启MQTT（如果MQTT启用）
    bool mqttReconnected = false;
    int mqttError = 0;
    if (doc["mqtt"]["enabled"].as<bool>()) {
        ProtocolManager* pm = ctx->protocolManager;
        if (pm) {
            mqttReconnected = pm->restartMQTT();
            if (!mqttReconnected) {
                MQTTClient* mqtt = pm->getMQTTClient();
                mqttError = mqtt ? mqtt->getLastErrorCode() : -99;
            }
        }
    }

    JsonDocument resp;
    resp["success"] = true;
    resp["message"] = "Protocol configuration saved";
    resp["data"]["mqttReconnected"] = mqttReconnected;
    if (!mqttReconnected && doc["mqtt"]["enabled"].as<bool>()) {
        resp["data"]["mqttError"] = mqttError;
    }
    String out;
    serializeJson(resp, out);
    request->send(200, "application/json", out);
}

void ProtocolRouteHandler::handleGetModbusStatus(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

    ProtocolManager* pm = ctx->protocolManager;
    if (!pm) {
        ctx->sendError(request, 500, "Protocol manager not available");
        return;
    }

    ModbusHandler* modbus = pm->getModbusHandler();
    if (!modbus) {
        ctx->sendError(request, 404, "Modbus handler not initialized");
        return;
    }

    JsonDocument doc;
    doc["success"] = true;

    JsonObject data = doc["data"].to<JsonObject>();
    data["mode"] = (modbus->getMode() == MODBUS_MASTER) ? "master" : "slave";
    data["status"] = modbus->getStatus();

    if (modbus->getMode() == MODBUS_MASTER) {
        MasterStats stats = modbus->getMasterStats();
        data["totalPolls"] = stats.totalPolls;
        data["successPolls"] = stats.successPolls;
        data["failedPolls"] = stats.failedPolls;
        data["timeoutPolls"] = stats.timeoutPolls;
        data["taskCount"] = modbus->getPollTaskCount();

        JsonArray tasks = data["tasks"].to<JsonArray>();
        for (uint8_t i = 0; i < modbus->getPollTaskCount(); i++) {
            PollTask task = modbus->getPollTask(i);
            JsonObject t = tasks.add<JsonObject>();
            t["slaveAddress"] = task.slaveAddress;
            t["functionCode"] = task.functionCode;
            t["startAddress"] = task.startAddress;
            t["quantity"] = task.quantity;
            t["pollInterval"] = task.pollInterval;
            t["enabled"] = task.enabled;
            t["label"] = task.label;
        }
    }

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void ProtocolRouteHandler::handleModbusWrite(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    ProtocolManager* pm = ctx->protocolManager;
    if (!pm) {
        ctx->sendError(request, 500, "Protocol manager not available");
        return;
    }

    ModbusHandler* modbus = pm->getModbusHandler();
    if (!modbus) {
        ctx->sendError(request, 404, "Modbus handler not initialized");
        return;
    }

    if (modbus->getMode() != MODBUS_MASTER) {
        ctx->sendError(request, 400, "Modbus is not in Master mode");
        return;
    }

    uint8_t slaveAddr = (uint8_t)ctx->getParamInt(request, "slaveAddress", 1);
    uint16_t regAddr = (uint16_t)ctx->getParamInt(request, "regAddress", 0);
    uint16_t value = (uint16_t)ctx->getParamInt(request, "value", 0);

    if (slaveAddr < 1 || slaveAddr > 247) {
        ctx->sendBadRequest(request, "Invalid slave address (1-247)");
        return;
    }

    if (modbus->masterWriteSingleRegister(slaveAddr, regAddr, value)) {
        ctx->sendSuccess(request, "Write request queued");
    } else {
        ctx->sendError(request, 503, "Write queue full");
    }
}

void ProtocolRouteHandler::handleTestMqttConnection(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String server = ctx->getParamValue(request, "server", "");
    int port = ctx->getParamInt(request, "port", 1883);
    String clientId = ctx->getParamValue(request, "clientId", "");
    String username = ctx->getParamValue(request, "username", "");
    String password = ctx->getParamValue(request, "password", "");

    if (server.isEmpty()) {
        ctx->sendBadRequest(request, "Server address is required");
        return;
    }

    if (clientId.isEmpty()) {
        char id[20];
        snprintf(id, sizeof(id), "TEST-%04X", (unsigned)esp_random() & 0xFFFF);
        clientId = id;
    }

    WiFiClient testWifi;
    PubSubClient testClient(testWifi);
    testClient.setServer(server.c_str(), port);

    bool connected = testClient.connect(
        clientId.c_str(),
        username.isEmpty() ? nullptr : username.c_str(),
        password.isEmpty() ? nullptr : password.c_str());

    JsonDocument doc;
    doc["success"] = true;
    doc["data"]["connected"] = connected;
    if (!connected) {
        doc["data"]["error"] = testClient.state();
    }

    if (testClient.connected()) {
        testClient.disconnect();
    }

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void ProtocolRouteHandler::handleGetMqttStatus(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

    ProtocolManager* pm = ctx->protocolManager;
    if (!pm) {
        ctx->sendError(request, 500, "Protocol manager not available");
        return;
    }

    MQTTClient* mqtt = pm->getMQTTClient();
    
    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();

    if (mqtt) {
        const MQTTConfig& cfg = mqtt->getConfig();
        data["initialized"] = true;
        data["connected"] = mqtt->getIsConnected();
        data["server"] = cfg.server;
        data["port"] = cfg.port;
        data["clientId"] = cfg.clientId;
        data["lastError"] = mqtt->getLastErrorCode();
        data["reconnectCount"] = mqtt->getReconnectCount();
        data["autoReconnect"] = cfg.autoReconnect;
        unsigned long connTime = mqtt->getLastConnectedTime();
        data["lastConnectedMs"] = connTime > 0 ? (millis() - connTime) / 1000 : 0;
    } else {
        data["initialized"] = false;
        data["connected"] = false;
    }

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void ProtocolRouteHandler::handleMqttReconnect(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    ProtocolManager* pm = ctx->protocolManager;
    if (!pm) {
        ctx->sendError(request, 500, "Protocol manager not available");
        return;
    }

    bool ok = pm->restartMQTT();

    JsonDocument doc;
    doc["success"] = true;
    doc["data"]["connected"] = ok;
    if (!ok) {
        MQTTClient* mqtt = pm->getMQTTClient();
        doc["data"]["error"] = mqtt ? mqtt->getLastErrorCode() : -99;
    }

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}
