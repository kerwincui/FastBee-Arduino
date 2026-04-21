#include "./network/handlers/PeripheralRouteHandler.h"
#include "./network/handlers/HandlerUtils.h"
#include "./network/WebHandlerContext.h"
#include "core/PeripheralManager.h"
#include <ArduinoJson.h>
#include <algorithm>
#include <vector>

PeripheralRouteHandler::PeripheralRouteHandler(WebHandlerContext* ctx)
    : ctx(ctx) {
}

void PeripheralRouteHandler::setupRoutes(AsyncWebServer* server) {
    // 注意：更具体的路由必须放在通用路由之前

    server->on("/api/peripherals/types", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetPeripheralTypes(request); });

    server->on("/api/peripherals/enable", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleEnablePeripheral(request); });

    server->on("/api/peripherals/disable", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleDisablePeripheral(request); });

    server->on("/api/peripherals/status", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetPeripheralStatus(request); });

    server->on("/api/peripherals/read", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleReadPeripheral(request); });

    server->on("/api/peripherals/write", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleWritePeripheral(request); });

    server->on("/api/peripherals/", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetPeripheral(request); });

    server->on("/api/peripherals/", HTTP_PUT,
              [this](AsyncWebServerRequest* request) { handleUpdatePeripheral(request); });

    server->on("/api/peripherals/update", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleUpdatePeripheral(request); });

    server->on("/api/peripherals/", HTTP_DELETE,
              [this](AsyncWebServerRequest* request) { handleDeletePeripheral(request); });

    server->on("/api/peripherals", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetPeripherals(request); });

    server->on("/api/peripherals", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleAddPeripheral(request); });
}

void PeripheralRouteHandler::handleGetPeripherals(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "system.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

    PeripheralManager& pm = PeripheralManager::getInstance();

    // 如果传了 id 参数，只返回单条外设（避免编辑时加载全部导致内存不足）
    if (request->hasParam("id")) {
        String periphId = request->getParam("id")->value();
        const PeripheralConfig* config = pm.getPeripheral(periphId);
        if (!config) {
            request->send(404, "application/json", "{\"success\":false,\"message\":\"Peripheral not found\"}");
            return;
        }

        JsonDocument doc;
        doc["id"] = config->id;
        doc["name"] = config->name;
        doc["type"] = static_cast<int>(config->type);
        doc["typeName"] = getPeripheralTypeName(config->type);
        doc["category"] = getCategoryName(getPeripheralCategory(config->type));
        doc["enabled"] = config->enabled;

        JsonArray pins = doc["pins"].to<JsonArray>();
        for (int i = 0; i < config->pinCount && i < 8; i++) {
            if (config->pins[i] != 255) {
                pins.add(config->pins[i]);
            }
        }

        auto runtimeState = pm.getRuntimeState(periphId);
        if (runtimeState) {
            doc["status"] = static_cast<int>(runtimeState->status);
        } else {
            doc["status"] = 0;
        }

        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", "{\"success\":true,\"data\":" + response + "}");
        return;
    }

    String typeFilter = ctx->getParamValue(request, "type", "");
    String categoryFilter = ctx->getParamValue(request, "category", "");

    std::vector<PeripheralConfig> peripherals;

    if (!typeFilter.isEmpty()) {
        PeripheralType type = parsePeripheralType(typeFilter.c_str());
        peripherals = pm.getPeripheralsByType(type);
    } else if (!categoryFilter.isEmpty()) {
        PeripheralCategory category = PeripheralCategory::CATEGORY_GPIO;
        if (categoryFilter == "communication") category = PeripheralCategory::CATEGORY_COMMUNICATION;
        else if (categoryFilter == "gpio") category = PeripheralCategory::CATEGORY_GPIO;
        else if (categoryFilter == "analog") category = PeripheralCategory::CATEGORY_ANALOG_SIGNAL;
        else if (categoryFilter == "debug") category = PeripheralCategory::CATEGORY_DEBUG;
        else if (categoryFilter == "special") category = PeripheralCategory::CATEGORY_SPECIAL;
        peripherals = pm.getPeripheralsByCategory(category);
    } else {
        peripherals = pm.getAllPeripherals();
    }

    // Modbus 子设备由 protocol.json 管理，不在外设列表中展示
    peripherals.erase(
        std::remove_if(peripherals.begin(), peripherals.end(),
            [](const PeripheralConfig& p) { return p.type == PeripheralType::MODBUS_DEVICE; }),
        peripherals.end());

    // 排序：启用的排前面，然后按名称排序
    std::sort(peripherals.begin(), peripherals.end(), [](const PeripheralConfig& a, const PeripheralConfig& b) {
        if (a.enabled != b.enabled) return a.enabled > b.enabled;
        return strcmp(a.name.c_str(), b.name.c_str()) < 0;
    });

    int total = peripherals.size();

    // 解析分页参数
    int page = 1;
    int pageSize = 10;
    if (request->hasParam("page")) {
        page = request->getParam("page")->value().toInt();
        if (page < 1) page = 1;
    }
    if (request->hasParam("pageSize")) {
        pageSize = request->getParam("pageSize")->value().toInt();
        if (pageSize < 1) pageSize = 1;
        if (pageSize > 50) pageSize = 50;
    }

    // 计算分页范围
    int startIdx = (page - 1) * pageSize;
    int endIdx = (startIdx < total) ? std::min(startIdx + pageSize, total) : startIdx;

    // 检查堆内存是否充足（预留20KB给系统其他部分）
    if (HandlerUtils::checkLowMemory(request)) return;

    // 使用流式响应避免大内存分配
    AsyncResponseStream* response = request->beginResponseStream("application/json");
    if (!response) {
        request->send(500, "application/json", "{\"success\":false,\"message\":\"无法创建响应流\"}");
        return;
    }

    // 手动构建JSON响应，避免使用JsonDocument缓存大量数据
    response->printf("{\"success\":true,\"total\":%d,\"page\":%d,\"pageSize\":%d,\"data\":[",
                     total, page, pageSize);

    bool first = true;
    for (int i = startIdx; i < endIdx; i++) {
        if (!first) {
            response->print(",");
        }
        first = false;

        const auto& config = peripherals[i];
        // 构建单个外设的JSON
        JsonDocument itemDoc;
        itemDoc["id"] = config.id;
        itemDoc["name"] = config.name;
        itemDoc["type"] = static_cast<int>(config.type);
        itemDoc["typeName"] = getPeripheralTypeName(config.type);
        itemDoc["category"] = getCategoryName(getPeripheralCategory(config.type));
        itemDoc["enabled"] = config.enabled;

        JsonArray pins = itemDoc["pins"].to<JsonArray>();
        for (int j = 0; j < config.pinCount && j < 8; j++) {
            if (config.pins[j] != 255) {
                pins.add(config.pins[j]);
            }
        }

        auto runtimeState = pm.getRuntimeState(config.id);
        if (runtimeState) {
            itemDoc["status"] = static_cast<int>(runtimeState->status);
        } else {
            itemDoc["status"] = 0;
        }

        // 序列化单个外设到响应流
        serializeJson(itemDoc, *response);
    }

    response->print("]}");
    request->send(response);
}

void PeripheralRouteHandler::handleGetPeripheralTypes(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "system.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();

    // 通信接口
    JsonArray commTypes = data["communication"].to<JsonArray>();
    for (int i = 1; i <= 5; i++) {
        PeripheralType type = static_cast<PeripheralType>(i);
        JsonObject typeObj = commTypes.add<JsonObject>();
        typeObj["value"] = i;
        typeObj["name"] = getPeripheralTypeName(type);
        typeObj["pinCount"] = getPeripheralPinCount(type);
    }

    // GPIO接口
    JsonArray gpioTypes = data["gpio"].to<JsonArray>();
    for (int i = 11; i <= 21; i++) {
        PeripheralType type = static_cast<PeripheralType>(i);
        JsonObject typeObj = gpioTypes.add<JsonObject>();
        typeObj["value"] = i;
        typeObj["name"] = getPeripheralTypeName(type);
        typeObj["pinCount"] = getPeripheralPinCount(type);
    }

    // 模拟信号
    JsonArray analogTypes = data["analog"].to<JsonArray>();
    for (int i = 26; i <= 27; i++) {
        PeripheralType type = static_cast<PeripheralType>(i);
        JsonObject typeObj = analogTypes.add<JsonObject>();
        typeObj["value"] = i;
        typeObj["name"] = getPeripheralTypeName(type);
        typeObj["pinCount"] = getPeripheralPinCount(type);
    }

    // 调试接口
    JsonArray debugTypes = data["debug"].to<JsonArray>();
    for (int i = 31; i <= 32; i++) {
        PeripheralType type = static_cast<PeripheralType>(i);
        JsonObject typeObj = debugTypes.add<JsonObject>();
        typeObj["value"] = i;
        typeObj["name"] = getPeripheralTypeName(type);
        typeObj["pinCount"] = getPeripheralPinCount(type);
    }

    // 专用外设
    JsonArray specialTypes = data["special"].to<JsonArray>();
    for (int i = 36; i <= 45; i++) {
        PeripheralType type = static_cast<PeripheralType>(i);
        JsonObject typeObj = specialTypes.add<JsonObject>();
        typeObj["value"] = i;
        typeObj["name"] = getPeripheralTypeName(type);
        typeObj["pinCount"] = getPeripheralPinCount(type);
    }

    HandlerUtils::sendJsonStream(request, doc);
}

void PeripheralRouteHandler::handleGetPeripheral(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "system.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String id = ctx->getParamValue(request, "id", "");
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Missing id parameter");
        return;
    }

    PeripheralManager& pm = PeripheralManager::getInstance();
    auto config = pm.getPeripheral(id);

    if (!config) {
        ctx->sendError(request, 404, "Peripheral not found");
        return;
    }

    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();

    data["id"] = config->id;
    data["name"] = config->name;
    data["type"] = static_cast<int>(config->type);
    data["typeName"] = getPeripheralTypeName(config->type);
    data["enabled"] = config->enabled;

    JsonArray pins = data["pins"].to<JsonArray>();
    for (int i = 0; i < config->pinCount && i < 8; i++) {
        if (config->pins[i] != 255) {
            pins.add(config->pins[i]);
        }
    }

    // 类型特定参数
    JsonObject params = data["params"].to<JsonObject>();
    if (config->type == PeripheralType::UART) {
        params["baudRate"] = config->params.uart.baudRate;
        params["dataBits"] = config->params.uart.dataBits;
        params["stopBits"] = config->params.uart.stopBits;
        params["parity"] = config->params.uart.parity;
    } else if (config->type == PeripheralType::I2C) {
        params["frequency"] = config->params.i2c.frequency;
        params["address"] = config->params.i2c.address;
        params["isMaster"] = config->params.i2c.isMaster;
    } else if (config->type == PeripheralType::SPI) {
        params["frequency"] = config->params.spi.frequency;
        params["mode"] = config->params.spi.mode;
        params["msbFirst"] = config->params.spi.msbFirst;
    } else if (config->isGPIOPeripheral()) {
        params["initialState"] = static_cast<int>(config->params.gpio.initialState);
        params["pwmChannel"] = config->params.gpio.pwmChannel;
        params["pwmFrequency"] = config->params.gpio.pwmFrequency;
        params["pwmResolution"] = config->params.gpio.pwmResolution;
        params["defaultDuty"] = config->params.gpio.defaultDuty;
    } else if (config->type == PeripheralType::ADC) {
        params["attenuation"] = config->params.adc.attenuation;
        params["resolution"] = config->params.adc.resolution;
    } else if (config->type == PeripheralType::DAC) {
        params["channel"] = config->params.dac.channel;
        params["defaultValue"] = config->params.dac.defaultValue;
    }

    auto runtimeState = pm.getRuntimeState(id);
    if (runtimeState) {
        JsonObject status = data["status"].to<JsonObject>();
        status["state"] = static_cast<int>(runtimeState->status);
        status["initTime"] = runtimeState->initTime;
        status["lastActivity"] = runtimeState->lastActivity;
        status["errorCount"] = runtimeState->errorCount;
    }

    HandlerUtils::sendJsonStream(request, doc);
}

void PeripheralRouteHandler::handleAddPeripheral(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    PeripheralConfig config;
    config.id = ctx->getParamValue(request, "id", "");
    config.name = ctx->getParamValue(request, "name", "");
    config.type = static_cast<PeripheralType>(ctx->getParamInt(request, "type", 0));
    config.enabled = ctx->getParamBool(request, "enabled", true);

    String pinsStr = ctx->getParamValue(request, "pins", "");
    config.pinCount = 0;
    if (!pinsStr.isEmpty()) {
        int start = 0;
        int end = pinsStr.indexOf(',');
        while (end != -1 && config.pinCount < 8) {
            config.pins[config.pinCount++] = pinsStr.substring(start, end).toInt();
            start = end + 1;
            end = pinsStr.indexOf(',', start);
        }
        if (config.pinCount < 8) {
            config.pins[config.pinCount++] = pinsStr.substring(start).toInt();
        }
    }

    if (config.type == PeripheralType::UART) {
        config.params.uart.baudRate = ctx->getParamInt(request, "baudRate", 115200);
        config.params.uart.dataBits = ctx->getParamInt(request, "dataBits", 8);
        config.params.uart.stopBits = ctx->getParamInt(request, "stopBits", 1);
        config.params.uart.parity = ctx->getParamInt(request, "parity", 0);
    } else if (config.type == PeripheralType::I2C) {
        config.params.i2c.frequency = ctx->getParamInt(request, "frequency", 100000);
        config.params.i2c.address = ctx->getParamInt(request, "address", 0);
        config.params.i2c.isMaster = ctx->getParamBool(request, "isMaster", true);
    } else if (config.type == PeripheralType::SPI) {
        config.params.spi.frequency = ctx->getParamInt(request, "frequency", 1000000);
        config.params.spi.mode = ctx->getParamInt(request, "mode", 0);
        config.params.spi.msbFirst = ctx->getParamBool(request, "msbFirst", true);
    } else if (config.isGPIOPeripheral()) {
        config.params.gpio.initialState = static_cast<GPIOState>(ctx->getParamInt(request, "initialState", 0));
        config.params.gpio.pwmChannel = ctx->getParamInt(request, "pwmChannel", 0);
        config.params.gpio.pwmFrequency = ctx->getParamInt(request, "pwmFrequency", 1000);
        config.params.gpio.pwmResolution = ctx->getParamInt(request, "pwmResolution", 8);
        config.params.gpio.defaultDuty = ctx->getParamInt(request, "defaultDuty", 0);
    } else if (config.type == PeripheralType::ADC) {
        config.params.adc.attenuation = ctx->getParamInt(request, "attenuation", 0);
        config.params.adc.resolution = ctx->getParamInt(request, "resolution", 12);
    } else if (config.type == PeripheralType::DAC) {
        config.params.dac.channel = ctx->getParamInt(request, "channel", 1);
        config.params.dac.defaultValue = ctx->getParamInt(request, "defaultValue", 0);
    }

    if (config.id.isEmpty()) {
        config.id = "periph_" + String(millis());
    }

    PeripheralManager& pm = PeripheralManager::getInstance();
    if (pm.addPeripheral(config)) {
        pm.saveConfiguration();
        ctx->sendSuccess(request, "Peripheral added");
    } else {
        ctx->sendError(request, 400, "Failed to add peripheral");
    }
}

void PeripheralRouteHandler::handleUpdatePeripheral(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String id = ctx->getParamValue(request, "id", "");
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Missing id parameter");
        return;
    }

    PeripheralManager& pm = PeripheralManager::getInstance();
    auto existing = pm.getPeripheral(id);
    if (!existing) {
        ctx->sendError(request, 404, "Peripheral not found");
        return;
    }

    PeripheralConfig config = *existing;

    String name = ctx->getParamValue(request, "name", "");
    if (!name.isEmpty()) config.name = name;

    // 更新外设类型（类型变更时重置参数为默认值）
    if (request->hasParam("type", true)) {
        PeripheralType newType = static_cast<PeripheralType>(ctx->getParamInt(request, "type", static_cast<int>(config.type)));
        if (newType != config.type) {
            config.type = newType;
            memset(&config.params, 0, sizeof(config.params));
        }
    }

    config.enabled = ctx->getParamBool(request, "enabled", config.enabled);

    String pinsStr = ctx->getParamValue(request, "pins", "");
    if (!pinsStr.isEmpty()) {
        config.pinCount = 0;
        int start = 0;
        int end = pinsStr.indexOf(',');
        while (end != -1 && config.pinCount < 8) {
            config.pins[config.pinCount++] = pinsStr.substring(start, end).toInt();
            start = end + 1;
            end = pinsStr.indexOf(',', start);
        }
        if (config.pinCount < 8) {
            config.pins[config.pinCount++] = pinsStr.substring(start).toInt();
        }
    }

    if (config.type == PeripheralType::UART) {
        config.params.uart.baudRate = ctx->getParamInt(request, "baudRate", config.params.uart.baudRate);
        config.params.uart.dataBits = ctx->getParamInt(request, "dataBits", config.params.uart.dataBits);
        config.params.uart.stopBits = ctx->getParamInt(request, "stopBits", config.params.uart.stopBits);
        config.params.uart.parity = ctx->getParamInt(request, "parity", config.params.uart.parity);
    } else if (config.type == PeripheralType::I2C) {
        config.params.i2c.frequency = ctx->getParamInt(request, "frequency", config.params.i2c.frequency);
        config.params.i2c.address = ctx->getParamInt(request, "address", config.params.i2c.address);
        config.params.i2c.isMaster = ctx->getParamBool(request, "isMaster", config.params.i2c.isMaster);
    } else if (config.type == PeripheralType::SPI) {
        config.params.spi.frequency = ctx->getParamInt(request, "frequency", config.params.spi.frequency);
        config.params.spi.mode = ctx->getParamInt(request, "mode", config.params.spi.mode);
        config.params.spi.msbFirst = ctx->getParamBool(request, "msbFirst", config.params.spi.msbFirst);
    } else if (config.isGPIOPeripheral()) {
        config.params.gpio.initialState = static_cast<GPIOState>(ctx->getParamInt(request, "initialState", static_cast<int>(config.params.gpio.initialState)));
        config.params.gpio.pwmChannel = ctx->getParamInt(request, "pwmChannel", config.params.gpio.pwmChannel);
        config.params.gpio.pwmFrequency = ctx->getParamInt(request, "pwmFrequency", config.params.gpio.pwmFrequency);
        config.params.gpio.pwmResolution = ctx->getParamInt(request, "pwmResolution", config.params.gpio.pwmResolution);
        config.params.gpio.defaultDuty = ctx->getParamInt(request, "defaultDuty", config.params.gpio.defaultDuty);
    } else if (config.type == PeripheralType::ADC) {
        config.params.adc.attenuation = ctx->getParamInt(request, "attenuation", config.params.adc.attenuation);
        config.params.adc.resolution = ctx->getParamInt(request, "resolution", config.params.adc.resolution);
    } else if (config.type == PeripheralType::DAC) {
        config.params.dac.channel = ctx->getParamInt(request, "channel", config.params.dac.channel);
        config.params.dac.defaultValue = ctx->getParamInt(request, "defaultValue", config.params.dac.defaultValue);
    }

    if (pm.updatePeripheral(id, config)) {
        pm.saveConfiguration();
        ctx->sendSuccess(request, "Peripheral updated");
    } else {
        ctx->sendError(request, 400, "Failed to update peripheral");
    }
}

void PeripheralRouteHandler::handleDeletePeripheral(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String id = ctx->getParamValue(request, "id", "");
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Missing id parameter");
        return;
    }

    PeripheralManager& pm = PeripheralManager::getInstance();
    if (pm.removePeripheral(id)) {
        pm.saveConfiguration();
        ctx->sendSuccess(request, "Peripheral deleted");
    } else {
        ctx->sendError(request, 404, "Peripheral not found");
    }
}

void PeripheralRouteHandler::handleEnablePeripheral(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String id = ctx->getParamValue(request, "id", "");
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Missing id parameter");
        return;
    }

    PeripheralManager& pm = PeripheralManager::getInstance();
    if (pm.enablePeripheral(id)) {
        pm.saveConfiguration();
        ctx->sendSuccess(request, "Peripheral enabled");
    } else {
        ctx->sendError(request, 400, "Failed to enable peripheral");
    }
}

void PeripheralRouteHandler::handleDisablePeripheral(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String id = ctx->getParamValue(request, "id", "");
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Missing id parameter");
        return;
    }

    PeripheralManager& pm = PeripheralManager::getInstance();
    if (pm.disablePeripheral(id)) {
        pm.saveConfiguration();
        ctx->sendSuccess(request, "Peripheral disabled");
    } else {
        ctx->sendError(request, 400, "Failed to disable peripheral");
    }
}

void PeripheralRouteHandler::handleGetPeripheralStatus(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "system.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String id = ctx->getParamValue(request, "id", "");
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Missing id parameter");
        return;
    }

    PeripheralManager& pm = PeripheralManager::getInstance();
    auto runtimeState = pm.getRuntimeState(id);
    auto config = pm.getPeripheral(id);

    if (!config) {
        ctx->sendError(request, 404, "Peripheral not found");
        return;
    }

    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();

    data["id"] = id;
    data["enabled"] = config->enabled;
    data["status"] = runtimeState ? static_cast<int>(runtimeState->status) : 0;

    if (runtimeState) {
        data["initTime"] = runtimeState->initTime;
        data["lastActivity"] = runtimeState->lastActivity;
        data["errorCount"] = runtimeState->errorCount;
        if (!runtimeState->lastError.isEmpty()) {
            data["lastError"] = runtimeState->lastError;
        }
    }

    HandlerUtils::sendJsonStream(request, doc);
}

void PeripheralRouteHandler::handleReadPeripheral(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "system.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String id = ctx->getParamValue(request, "id", "");
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Missing id parameter");
        return;
    }

    PeripheralManager& pm = PeripheralManager::getInstance();
    auto config = pm.getPeripheral(id);

    if (!config) {
        ctx->sendError(request, 404, "Peripheral not found");
        return;
    }

    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();

    data["id"] = id;
    data["type"] = static_cast<int>(config->type);
    data["typeName"] = getPeripheralTypeName(config->type);

    if (config->isGPIOPeripheral()) {
        GPIOState state = pm.readPin(id);
        data["state"] = static_cast<int>(state);
        data["stateName"] = (state == GPIOState::STATE_HIGH) ? "HIGH" : (state == GPIOState::STATE_LOW) ? "LOW" : "UNDEFINED";

        if (config->type == PeripheralType::GPIO_ANALOG_INPUT || config->type == PeripheralType::ADC) {
            data["value"] = pm.readAnalog(id);
        }
    } else if (config->isCommunicationPeripheral()) {
        uint8_t buffer[128];
        size_t len = sizeof(buffer);

        if (pm.readData(id, buffer, len) && len > 0) {
            bool isText = true;
            for (size_t i = 0; i < len; i++) {
                if (buffer[i] < 32 && buffer[i] != '\n' && buffer[i] != '\r' && buffer[i] != '\t') {
                    isText = false;
                    break;
                }
            }

            if (isText) {
                data["text"] = String((char*)buffer, len);
            }

            String hex;
            for (size_t i = 0; i < len; i++) {
                char hexBuf[4];
                snprintf(hexBuf, sizeof(hexBuf), "%02X ", buffer[i]);
                hex += hexBuf;
            }
            data["hex"] = hex;
            data["length"] = len;
        } else {
            data["text"] = "";
            data["hex"] = "";
            data["length"] = 0;
        }
    } else if (config->type == PeripheralType::DAC) {
        data["channel"] = config->params.dac.channel;
        data["pin"] = config->getPrimaryPin();
    } else {
        uint8_t buffer[64];
        size_t len = sizeof(buffer);

        if (pm.readData(id, buffer, len) && len > 0) {
            data["length"] = len;
            if (len <= 4) {
                uint32_t value = 0;
                for (size_t i = 0; i < len; i++) {
                    value |= ((uint32_t)buffer[i]) << (i * 8);
                }
                data["value"] = value;
            }
        } else {
            data["value"] = nullptr;
        }
    }

    HandlerUtils::sendJsonStream(request, doc);
}

void PeripheralRouteHandler::handleWritePeripheral(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String id = ctx->getParamValue(request, "id", "");
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Missing id parameter");
        return;
    }

    PeripheralManager& pm = PeripheralManager::getInstance();
    auto config = pm.getPeripheral(id);

    if (!config) {
        ctx->sendError(request, 404, "Peripheral not found");
        return;
    }

    bool success = false;
    String message;

    if (config->isGPIOPeripheral()) {
        if (request->hasParam("state", true)) {
            int stateValue = ctx->getParamInt(request, "state", 0);
            GPIOState state = (stateValue == 1) ? GPIOState::STATE_HIGH : GPIOState::STATE_LOW;
            success = pm.writePin(id, state);
            message = success ? "GPIO state set" : "GPIO write failed";
        } else if (request->hasParam("toggle", true)) {
            success = pm.togglePin(id);
            message = success ? "GPIO toggled" : "GPIO toggle failed";
        } else if (request->hasParam("pwm", true) &&
                 (config->type == PeripheralType::GPIO_PWM_OUTPUT ||
                  config->type == PeripheralType::GPIO_ANALOG_OUTPUT ||
                  config->type == PeripheralType::PWM_SERVO)) {
            uint32_t dutyCycle = ctx->getParamInt(request, "pwm", 0);
            success = pm.writePWM(id, dutyCycle);
            message = success ? "PWM duty set" : "PWM write failed";
        } else {
            ctx->sendError(request, 400, "Missing parameter (state/toggle/pwm)");
            return;
        }
    } else if (config->type == PeripheralType::DAC) {
        if (request->hasParam("value", true)) {
            int value = ctx->getParamInt(request, "value", 0);
            if (value < 0) value = 0;
            if (value > 255) value = 255;
            uint8_t dacValue = (uint8_t)value;
            success = pm.writeData(id, &dacValue, 1);
            message = success ? "DAC value set" : "DAC write failed";
        } else {
            ctx->sendError(request, 400, "Missing value parameter");
            return;
        }
    } else if (config->isCommunicationPeripheral()) {
        if (request->hasParam("data", true)) {
            String dataStr = ctx->getParamValue(request, "data", "");
            if (!dataStr.isEmpty()) {
                if (request->hasParam("hex", true) && ctx->getParamInt(request, "hex", 0) == 1) {
                    std::vector<uint8_t> bytes;
                    String hexStr = dataStr;
                    hexStr.replace(" ", "");

                    for (size_t i = 0; i + 1 < hexStr.length(); i += 2) {
                        String byteStr = hexStr.substring(i, i + 2);
                        bytes.push_back((uint8_t)strtol(byteStr.c_str(), nullptr, 16));
                    }

                    if (!bytes.empty()) {
                        success = pm.writeData(id, bytes.data(), bytes.size());
                        message = success ? String("Sent ") + String(bytes.size()) + " bytes (HEX)" : "Send failed";
                    } else {
                        ctx->sendError(request, 400, "Invalid hex data");
                        return;
                    }
                } else {
                    success = pm.writeData(id, (const uint8_t*)dataStr.c_str(), dataStr.length());
                    message = success ? String("Sent ") + String(dataStr.length()) + " bytes" : "Send failed";
                }
            } else {
                ctx->sendError(request, 400, "Data is empty");
                return;
            }
        } else {
            ctx->sendError(request, 400, "Missing data parameter");
            return;
        }
    } else {
        if (request->hasParam("value", true)) {
            int value = ctx->getParamInt(request, "value", 0);
            uint8_t bytes[4];
            bytes[0] = value & 0xFF;
            bytes[1] = (value >> 8) & 0xFF;
            bytes[2] = (value >> 16) & 0xFF;
            bytes[3] = (value >> 24) & 0xFF;

            size_t len = 1;
            if (value > 0xFF) len = 2;
            if (value > 0xFFFF) len = 4;

            success = pm.writeData(id, bytes, len);
            message = success ? "Data written" : "Data write failed";
        } else {
            ctx->sendError(request, 400, "Missing value parameter");
            return;
        }
    }

    if (success) {
        ctx->sendSuccess(request, message);
    } else {
        ctx->sendError(request, 400, message);
    }
}
