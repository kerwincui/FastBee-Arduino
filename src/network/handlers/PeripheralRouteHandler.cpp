#include "./network/handlers/PeripheralRouteHandler.h"
#include "./network/handlers/HandlerUtils.h"
#include "./network/WebHandlerContext.h"
#include "core/FeatureFlags.h"
#include "core/PeripheralManager.h"
#include "core/ResourceProfile.h"
#include <ArduinoJson.h>
#include <algorithm>
#include <vector>
#include "systems/LoggerSystem.h"

namespace {
bool isListMemoryCriticallyLow() {
    return ESP.getFreeHeap() < 4096 || ESP.getMaxAllocHeap() < 1536;
}

bool shouldForceCompactList() {
    return ESP.getFreeHeap() < 16384 || ESP.getMaxAllocHeap() < 8192;
}

String escapePeriphJsonString(const String& value) {
    String out;
    out.reserve(value.length() + 8);
    for (size_t i = 0; i < value.length(); ++i) {
        char c = value[i];
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}
}

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

    server->on("/api/peripherals/validate-pins", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleValidatePins(request); });

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
    if (!ctx->requireAuth(request)) return;

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

    if (HandlerUtils::rejectHeavyRequestOnPressure(request, "Peripheral list", MemoryGuardLevel::CRITICAL, 5)) {
        return;
    }

    String typeFilter = ctx->getParamValue(request, "type", "");
    String categoryFilter = ctx->getParamValue(request, "category", "");
    bool enabledOnly = ctx->getParamValue(request, "enabledOnly", "0") == "1";
    bool requestedCompact = ctx->getParamValue(request, "compact", "0") == "1";
    bool degraded = shouldForceCompactList();
    bool compact = requestedCompact || degraded;

    // 使用指针向量代替深拷贝，大幅减少内存占用（38个外设：指针~152B vs 拷贝~6KB）
    std::vector<const PeripheralConfig*> ptrs;
    ptrs.reserve(40);  // 预估外设数量

    if (!typeFilter.isEmpty()) {
        PeripheralType type = parsePeripheralType(typeFilter.c_str());
        pm.forEachPeripheral([&ptrs, type, enabledOnly](const PeripheralConfig& p) {
            if (p.type == type &&
                p.type != PeripheralType::MODBUS_DEVICE &&
                (!enabledOnly || p.enabled)) {
                ptrs.push_back(&p);
            }
        });
    } else if (!categoryFilter.isEmpty()) {
        PeripheralCategory category = PeripheralCategory::CATEGORY_GPIO;
        if (categoryFilter == "communication") category = PeripheralCategory::CATEGORY_COMMUNICATION;
        else if (categoryFilter == "gpio") category = PeripheralCategory::CATEGORY_GPIO;
        else if (categoryFilter == "analog") category = PeripheralCategory::CATEGORY_ANALOG_SIGNAL;
        else if (categoryFilter == "debug") category = PeripheralCategory::CATEGORY_DEBUG;
        else if (categoryFilter == "special") category = PeripheralCategory::CATEGORY_SPECIAL;
        pm.forEachPeripheral([&ptrs, category, enabledOnly](const PeripheralConfig& p) {
            if (getPeripheralCategory(p.type) == category &&
                p.type != PeripheralType::MODBUS_DEVICE &&
                (!enabledOnly || p.enabled)) {
                ptrs.push_back(&p);
            }
        });
    } else {
        pm.forEachPeripheral([&ptrs, enabledOnly](const PeripheralConfig& p) {
            if (p.type != PeripheralType::MODBUS_DEVICE &&
                (!enabledOnly || p.enabled)) {
                ptrs.push_back(&p);
            }
        });
    }

    // 排序：启用的排前面，然后按名称排序
    std::sort(ptrs.begin(), ptrs.end(), [](const PeripheralConfig* a, const PeripheralConfig* b) {
        if (a->enabled != b->enabled) return a->enabled > b->enabled;
        return strcmp(a->name.c_str(), b->name.c_str()) < 0;
    });

    int total = ptrs.size();

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
        if (pageSize > (compact ? 50 : 20)) pageSize = compact ? 50 : 20;
    }

    // 分页步长始终与前端请求保持一致，避免数据间隙（items被跳过无法显示）。
    // 内存不足时由 compact 模式减少每项序列化字段来节省空间，而非减少条数。
    // 如果连单条序列化都无法完成，由下方 isListMemoryCriticallyLow() 返回 503。
    int effectivePageSize = pageSize;

    // 计算分页范围（统一使用 effectivePageSize）
    int startIdx = (page - 1) * effectivePageSize;
    int endIdx = (startIdx < total) ? std::min(startIdx + effectivePageSize, total) : startIdx;

    // Chunked responses can operate with small contiguous blocks. Only reject
    // when even per-item scratch strings are likely to fail.
    if (isListMemoryCriticallyLow()) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Low memory: heap=%lu maxAlloc=%lu",
                 (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMaxAllocHeap());
        HandlerUtils::sendJsonError(request, 503, msg, 5);
        return;
    }
    
    // 两遍法：先序列化每个项计算精确大小，再用精确 reserve 构建响应
    int itemCount = endIdx - startIdx;
    
    // 第一遍：序列化所有项到临时 String，累计精确大小
    std::vector<String> serializedItems;
    serializedItems.reserve(itemCount);
    size_t totalItemsSize = 0;
    
    for (int i = startIdx; i < endIdx; i++) {
        const auto& config = *ptrs[i];
        JsonDocument itemDoc;
        itemDoc["id"] = config.id;
        itemDoc["name"] = config.name;
        itemDoc["type"] = static_cast<int>(config.type);
        itemDoc["typeName"] = getPeripheralTypeName(config.type);
        itemDoc["category"] = getCategoryName(getPeripheralCategory(config.type));
        itemDoc["enabled"] = config.enabled;

        if (!compact) {
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
        }

        String itemStr;
        serializeJson(itemDoc, itemStr);
        totalItemsSize += itemStr.length();
        serializedItems.push_back(std::move(itemStr));
    }
    
     // ============ Chunked 流式响应（复用 HandlerUtils::sendJsonListChunked）============
    // 旧方案：先构建一个 totalJsonSize 大小的连续 String（~10-20KB），对碎片化堆极不友好。
    // 新方案：items 已在 vector<String> 中，按 index 按需复制到 TCP 发送缓冲（~256B），
    //         堆峰 = items 合计（~6KB），不再产生额外大 String。
    size_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < 4096) {
        LOG_ERRORF("[Periph] Low heap for chunked response: free=%u", (unsigned)freeHeap);
        HandlerUtils::sendJsonError(request, 503, "Low memory for response");
        return;
    }

    // 构建 header：{"success":true,"total":N,"page":P,"pageSize":S,"data":[
    const int profileMax = static_cast<int>(FastBee::ResourceProfile::MAX_PERIPHERALS);
    const int profileUsed = static_cast<int>(pm.getPeripheralCount());
    const int profileRemaining = std::max(0, profileMax - profileUsed);

    String header;
    header.reserve(180);
    header = F("{\"success\":true,\"total\":");
    header += String(total);
    header += F(",\"page\":");
    header += String(page);
    header += F(",\"pageSize\":");
    header += String(effectivePageSize);
    if (degraded) {
        header += F(",\"degraded\":true");
    }
    header += F(",\"profile\":{\"name\":\"");
    header += FastBee::ResourceProfile::NAME;
    header += F("\",\"max\":");
    header += String(profileMax);
    header += F(",\"used\":");
    header += String(profileUsed);
    header += F(",\"remaining\":");
    header += String(profileRemaining);
    header += F("}");
    header += F(",\"data\":[");

    if (!HandlerUtils::sendJsonListChunked(request, header, std::move(serializedItems))) {
        LOG_ERRORF("[Periph] beginChunkedResponse failed, heap=%u", (unsigned)ESP.getFreeHeap());
        HandlerUtils::sendJsonError(request, 503, "Failed to create streaming response");
    }
}

void PeripheralRouteHandler::handleGetPeripheralTypes(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    JsonDocument doc;
    doc["success"] = true;
    JsonObject profile = doc["profile"].to<JsonObject>();
    const int profileMax = static_cast<int>(FastBee::ResourceProfile::MAX_PERIPHERALS);
    const int profileUsed = static_cast<int>(PeripheralManager::getInstance().getPeripheralCount());
    profile["name"] = FastBee::ResourceProfile::NAME;
    profile["max"] = profileMax;
    profile["used"] = profileUsed;
    profile["remaining"] = std::max(0, profileMax - profileUsed);
    JsonObject data = doc["data"].to<JsonObject>();

    // 通信接口
    JsonArray commTypes = data["communication"].to<JsonArray>();
    for (int i = 1; i <= 5; i++) {
        PeripheralType type = static_cast<PeripheralType>(i);
        JsonObject typeObj = commTypes.add<JsonObject>();
        typeObj["value"] = i;
        typeObj["name"] = getPeripheralTypeName(type);
        typeObj["pinCount"] = getPeripheralPinCount(type);
        typeObj["implemented"] = (i != 4 && i != 5);  // CAN, USB 未实现
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

    // 调试接口（已隐藏，JTAG/SWD未实现驱动）
    // JsonArray debugTypes = data["debug"].to<JsonArray>();
    // for (int i = 31; i <= 32; i++) {
    //     PeripheralType type = static_cast<PeripheralType>(i);
    //     JsonObject typeObj = debugTypes.add<JsonObject>();
    //     typeObj["value"] = i;
    //     typeObj["name"] = getPeripheralTypeName(type);
    //     typeObj["pinCount"] = getPeripheralPinCount(type);
    // }

    // 专用外设
    JsonArray specialTypes = data["special"].to<JsonArray>();
    for (int i = 36; i <= 45; i++) {
        PeripheralType type = static_cast<PeripheralType>(i);
        JsonObject typeObj = specialTypes.add<JsonObject>();
        typeObj["value"] = i;
        typeObj["name"] = getPeripheralTypeName(type);
        typeObj["pinCount"] = getPeripheralPinCount(type);
        // CAN=4, USB=5, SDIO=37, CAMERA=39, ETHERNET=40, ENCODER=43 未实现硬件驱动
        typeObj["implemented"] = (i != 37 && i != 39 && i != 40 && i != 43);
    }
    const int extraSpecialTypes[] = {47, 48, 49, 51, 60};
    for (int value : extraSpecialTypes) {
        PeripheralType type = static_cast<PeripheralType>(value);
        JsonObject typeObj = specialTypes.add<JsonObject>();
        typeObj["value"] = value;
        typeObj["name"] = getPeripheralTypeName(type);
        typeObj["pinCount"] = getPeripheralPinCount(type);
        typeObj["implemented"] = true;
    }

    HandlerUtils::sendJsonStream(request, doc);
}

void PeripheralRouteHandler::handleGetPeripheral(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

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
    } else if (config->type == PeripheralType::STEPPER_MOTOR) {
        params["stepsPerRevolution"] = config->params.stepper.stepsPerRevolution;
        params["speed"] = config->params.stepper.speed;
    } else if (config->type == PeripheralType::LCD) {
        params["width"]     = config->params.lcd.width;
        params["height"]    = config->params.lcd.height;
        params["interface"] = config->params.lcd.interface;
    } else if (config->type == PeripheralType::NEO_PIXEL) {
        params["count"] = config->params.neopixel.count;
        params["brightness"] = config->params.neopixel.brightness;
    } else if (config->type == PeripheralType::RF_MODULE) {
        params["mode"] = config->params.rf.mode;
        params["pulseWidth"] = config->params.rf.pulseWidth;
        params["repeat"] = config->params.rf.repeat;
        params["bitLength"] = config->params.rf.bitLength;
        params["activeHigh"] = config->params.rf.activeHigh;
    } else if (config->type == PeripheralType::RADAR_SENSOR) {
        params["mode"] = config->params.radar.mode;
        params["activeHigh"] = config->params.radar.activeHigh;
        params["debounceMs"] = config->params.radar.debounceMs;
        params["holdMs"] = config->params.radar.holdMs;
    } else if (config->type == PeripheralType::SEVEN_SEGMENT_TM1637) {
        params["brightness"] = config->params.segment.brightness;
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
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

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
    } else if (config.type == PeripheralType::STEPPER_MOTOR) {
        config.params.stepper.stepsPerRevolution = ctx->getParamInt(request, "stepsPerRevolution", 2048);
        config.params.stepper.speed = ctx->getParamInt(request, "speed", 8);
    } else if (config.type == PeripheralType::LCD) {
        int w = ctx->getParamInt(request, "width", 128);
        int h = ctx->getParamInt(request, "height", 64);
        int iface = ctx->getParamInt(request, "interface", 2);
        config.params.lcd.width     = (uint8_t)(w > 255 ? 255 : (w < 0 ? 0 : w));
        config.params.lcd.height    = (uint8_t)(h > 255 ? 255 : (h < 0 ? 0 : h));
        config.params.lcd.interface = (uint8_t)(iface < 0 ? 2 : iface);
    } else if (config.type == PeripheralType::NEO_PIXEL) {
        int count = ctx->getParamInt(request, "count", 1);
        int brightness = ctx->getParamInt(request, "brightness", 64);
        if (count < 1) count = 1;
        if (count > 64) count = 64;
        if (brightness < 0) brightness = 0;
        if (brightness > 255) brightness = 255;
        config.params.neopixel.count = (uint16_t)count;
        config.params.neopixel.brightness = (uint8_t)brightness;
    } else if (config.type == PeripheralType::RF_MODULE) {
        int mode = ctx->getParamInt(request, "mode", 0);
        int pulseWidth = ctx->getParamInt(request, "pulseWidth", 350);
        int repeat = ctx->getParamInt(request, "repeat", 8);
        int bitLength = ctx->getParamInt(request, "bitLength", 24);
        if (mode != 1) mode = 0;
        if (pulseWidth < 100) pulseWidth = 350;
        if (pulseWidth > 2000) pulseWidth = 2000;
        if (repeat < 1) repeat = 8;
        if (repeat > 20) repeat = 20;
        if (bitLength < 1) bitLength = 24;
        if (bitLength > 32) bitLength = 32;
        config.params.rf.mode = (uint8_t)mode;
        config.params.rf.pulseWidth = (uint16_t)pulseWidth;
        config.params.rf.repeat = (uint8_t)repeat;
        config.params.rf.bitLength = (uint8_t)bitLength;
        config.params.rf.activeHigh = ctx->getParamBool(request, "activeHigh", true);
    } else if (config.type == PeripheralType::RADAR_SENSOR) {
        int debounceMs = ctx->getParamInt(request, "debounceMs", 50);
        int holdMs = ctx->getParamInt(request, "holdMs", 2000);
        if (debounceMs < 0) debounceMs = 0;
        if (debounceMs > 5000) debounceMs = 5000;
        if (holdMs < 0) holdMs = 0;
        if (holdMs > 60000) holdMs = 60000;
        config.params.radar.mode = 0;
        config.params.radar.activeHigh = ctx->getParamBool(request, "activeHigh", true);
        config.params.radar.debounceMs = (uint16_t)debounceMs;
        config.params.radar.holdMs = (uint16_t)holdMs;
    }
    else if (config.type == PeripheralType::SEVEN_SEGMENT_TM1637) {
        int b = ctx->getParamInt(request, "brightness", 2);
        if (b < 0) b = 0;
        if (b > 7) b = 7;
        config.params.segment.brightness = (uint8_t)b;
    }

    if (config.id.isEmpty()) {
        config.id = "periph_" + String(millis());
    }

    PeripheralManager& pm = PeripheralManager::getInstance();
    String errorMsg;
    if (pm.addPeripheral(config, errorMsg)) {
        pm.saveConfiguration();
        ctx->sendSuccess(request, "Peripheral added");
    } else {
        String msg = errorMsg.isEmpty() ? String("Failed to add peripheral")
                                        : (String("Failed to add peripheral: ") + errorMsg);
        ctx->sendError(request, 400, msg);
    }
}

void PeripheralRouteHandler::handleUpdatePeripheral(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

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
    } else if (config.type == PeripheralType::STEPPER_MOTOR) {
        config.params.stepper.stepsPerRevolution = ctx->getParamInt(request, "stepsPerRevolution", config.params.stepper.stepsPerRevolution ? config.params.stepper.stepsPerRevolution : 2048);
        config.params.stepper.speed = ctx->getParamInt(request, "speed", config.params.stepper.speed ? config.params.stepper.speed : 8);
    } else if (config.type == PeripheralType::LCD) {
        int w = ctx->getParamInt(request, "width", config.params.lcd.width ? config.params.lcd.width : 128);
        int h = ctx->getParamInt(request, "height", config.params.lcd.height ? config.params.lcd.height : 64);
        int iface = ctx->getParamInt(request, "interface", config.params.lcd.interface);
        config.params.lcd.width     = (uint8_t)(w > 255 ? 255 : (w < 0 ? 0 : w));
        config.params.lcd.height    = (uint8_t)(h > 255 ? 255 : (h < 0 ? 0 : h));
        config.params.lcd.interface = (uint8_t)(iface < 0 ? 2 : iface);
    } else if (config.type == PeripheralType::NEO_PIXEL) {
        int count = ctx->getParamInt(request, "count", config.params.neopixel.count ? config.params.neopixel.count : 1);
        int brightness = ctx->getParamInt(request, "brightness", config.params.neopixel.brightness);
        if (count < 1) count = 1;
        if (count > 64) count = 64;
        if (brightness < 0) brightness = 0;
        if (brightness > 255) brightness = 255;
        config.params.neopixel.count = (uint16_t)count;
        config.params.neopixel.brightness = (uint8_t)brightness;
    } else if (config.type == PeripheralType::RF_MODULE) {
        int mode = ctx->getParamInt(request, "mode", config.params.rf.mode);
        int pulseWidth = ctx->getParamInt(request, "pulseWidth", config.params.rf.pulseWidth ? config.params.rf.pulseWidth : 350);
        int repeat = ctx->getParamInt(request, "repeat", config.params.rf.repeat ? config.params.rf.repeat : 8);
        int bitLength = ctx->getParamInt(request, "bitLength", config.params.rf.bitLength ? config.params.rf.bitLength : 24);
        if (mode != 1) mode = 0;
        if (pulseWidth < 100) pulseWidth = 350;
        if (pulseWidth > 2000) pulseWidth = 2000;
        if (repeat < 1) repeat = 8;
        if (repeat > 20) repeat = 20;
        if (bitLength < 1) bitLength = 24;
        if (bitLength > 32) bitLength = 32;
        config.params.rf.mode = (uint8_t)mode;
        config.params.rf.pulseWidth = (uint16_t)pulseWidth;
        config.params.rf.repeat = (uint8_t)repeat;
        config.params.rf.bitLength = (uint8_t)bitLength;
        config.params.rf.activeHigh = ctx->getParamBool(request, "activeHigh", config.params.rf.activeHigh);
    } else if (config.type == PeripheralType::RADAR_SENSOR) {
        int debounceMs = ctx->getParamInt(request, "debounceMs", config.params.radar.debounceMs ? config.params.radar.debounceMs : 50);
        int holdMs = ctx->getParamInt(request, "holdMs", config.params.radar.holdMs ? config.params.radar.holdMs : 2000);
        if (debounceMs < 0) debounceMs = 0;
        if (debounceMs > 5000) debounceMs = 5000;
        if (holdMs < 0) holdMs = 0;
        if (holdMs > 60000) holdMs = 60000;
        config.params.radar.mode = 0;
        config.params.radar.activeHigh = ctx->getParamBool(request, "activeHigh", config.params.radar.activeHigh);
        config.params.radar.debounceMs = (uint16_t)debounceMs;
        config.params.radar.holdMs = (uint16_t)holdMs;
    }
    else if (config.type == PeripheralType::SEVEN_SEGMENT_TM1637) {
        int b = ctx->getParamInt(request, "brightness", config.params.segment.brightness);
        if (b < 0) b = 0;
        if (b > 7) b = 7;
        config.params.segment.brightness = (uint8_t)b;
    }

    if (pm.updatePeripheral(id, config)) {
        pm.saveConfiguration();
        ctx->sendSuccess(request, "Peripheral updated");
    } else {
        ctx->sendError(request, 400, "Failed to update peripheral");
    }
}

void PeripheralRouteHandler::handleDeletePeripheral(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

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
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

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
        String errMsg = "Failed to enable peripheral";
        if (pm.lastEnableError.length() > 0) {
            errMsg += ": " + pm.lastEnableError;
        }
        ctx->sendError(request, 400, errMsg.c_str());
    }
}

void PeripheralRouteHandler::handleDisablePeripheral(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

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
    if (!ctx->requireAuth(request)) return;

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
    if (!ctx->requireAuth(request)) return;

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
    } else if (config->type == PeripheralType::RADAR_SENSOR) {
        bool detected = false;
        if (pm.readRadarState(id, detected)) {
            data["detected"] = detected;
            data["value"] = detected ? 1 : 0;
            data["stateName"] = detected ? "DETECTED" : "CLEAR";
        } else {
            data["detected"] = nullptr;
            data["value"] = nullptr;
        }
    } else if (config->type == PeripheralType::RF_MODULE) {
        bool level = false;
        if (pm.readRfLevel(id, level)) {
            data["level"] = level;
            data["value"] = level ? 1 : 0;
            data["stateName"] = level ? "ACTIVE" : "IDLE";
        } else {
            data["level"] = nullptr;
            data["value"] = nullptr;
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

void PeripheralRouteHandler::handleValidatePins(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    String typeStr = ctx->getParamValue(request, "type", "");
    String pinsStr = ctx->getParamValue(request, "pins", "");
    String excludeId = ctx->getParamValue(request, "excludeId", "");

    if (typeStr.isEmpty() || pinsStr.isEmpty()) {
        request->send(400, "application/json", "{\"success\":false,\"message\":\"Missing type or pins\"}");
        return;
    }

    PeripheralType type = static_cast<PeripheralType>(typeStr.toInt());
    PeripheralManager& pm = PeripheralManager::getInstance();

    // 解析引脚列表（逗号分隔）
    String items = "";
    int pinIdx = 0;
    int start = 0;
    bool allValid = true;
    while (start < (int)pinsStr.length()) {
        int comma = pinsStr.indexOf(',', start);
        String part = (comma < 0) ? pinsStr.substring(start) : pinsStr.substring(start, comma);
        part.trim();
        if (!part.isEmpty()) {
            uint8_t pin = static_cast<uint8_t>(part.toInt());
            String pinError;
            bool pinOk = true;
            String warnings;

            // 检查引脚有效性
            if (!pm.validatePinForType(pin, type, pinError)) {
                pinOk = false;
                allValid = false;
            }

            // 检查引脚冲突（与已启用外设）
            String conflictInfo = pm.getPinConflictInfo(pin, excludeId);
            if (!conflictInfo.isEmpty()) {
                warnings = conflictInfo;
                // 冲突不算失败，但给出警告
            }

            if (pinIdx > 0) items += ",";
            items += F("{\"pin\":");
            items += String(pin);
            items += F(",\"valid\":");
            items += pinOk ? F("true") : F("false");
            if (!pinError.isEmpty()) {
                items += F(",\"error\":\"");
                items += escapePeriphJsonString(pinError);
                items += F("\"");
            }
            if (!warnings.isEmpty()) {
                items += F(",\"warning\":\"");
                items += escapePeriphJsonString(warnings);
                items += F("\"");
            }
            items += F("}");
            pinIdx++;
        }
        if (comma < 0) break;
        start = comma + 1;
    }

    String response;
    response.reserve(64 + items.length());
    response += F("{\"success\":true,\"allValid\":");
    response += allValid ? F("true") : F("false");
    response += F(",\"pins\":[");
    response += items;
    response += F("]}");
    request->send(200, "application/json", response);
}

void PeripheralRouteHandler::handleWritePeripheral(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

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
    } else if (config->type == PeripheralType::RF_MODULE) {
        String code = ctx->getParamValue(request, "code", "");
        if (code.isEmpty()) code = ctx->getParamValue(request, "data", "");
        if (code.isEmpty()) code = ctx->getParamValue(request, "value", "");
        if (code.isEmpty()) {
            ctx->sendError(request, 400, "Missing code parameter");
            return;
        }
        int bits = ctx->getParamInt(request, "bits", 0);
        int pulseWidth = ctx->getParamInt(request, "pulseWidth", 0);
        int repeat = ctx->getParamInt(request, "repeat", 0);
        if (bits < 0) bits = 0;
        if (bits > 32) bits = 32;
        if (pulseWidth < 0) pulseWidth = 0;
        if (pulseWidth > 2000) pulseWidth = 2000;
        if (repeat < 0) repeat = 0;
        if (repeat > 20) repeat = 20;
        success = pm.sendRfCode(id, code, (uint8_t)bits, (uint16_t)pulseWidth, (uint8_t)repeat);
        message = success ? "RF code sent" : "RF send failed";
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
