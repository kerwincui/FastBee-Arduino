#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_PERIPH_EXEC

#include "./network/handlers/PeriphExecRouteHandler.h"
#include "./network/handlers/HandlerUtils.h"
#include "./network/WebHandlerContext.h"
#include "core/PeriphExecManager.h"
#include "core/PeripheralManager.h"
#if FASTBEE_ENABLE_MQTT
#include "protocols/MQTTClient.h"
#endif
#include "protocols/ProtocolManager.h"
#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <algorithm>

PeriphExecRouteHandler::PeriphExecRouteHandler(WebHandlerContext* ctx)
    : ctx(ctx) {
}

void PeriphExecRouteHandler::setupRoutes(AsyncWebServer* server) {
    // 注意：更具体的路由必须先注册，否则会被 /api/periph-exec 拦截
    
    // 设备控制API（按动作类型分组）
    server->on("/api/periph-exec/controls", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetControls(request); });

    // 触发事件相关API
    server->on("/api/periph-exec/events/static", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetStaticEvents(request); });
    
    server->on("/api/periph-exec/events/dynamic", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetDynamicEvents(request); });
    
    server->on("/api/periph-exec/events/categories", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetEventCategories(request); });
    
    server->on("/api/periph-exec/trigger-types", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetTriggerTypes(request); });

    // 向后兼容：旧版系统事件API重定向到静态事件
    server->on("/api/periph-exec/system-events", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetStaticEvents(request); });

    // JSON body handlers for add/update（支持 triggers[]/actions[] 数组）
    // 注意: update 必须先于 add 注册，因为 AsyncCallbackJsonWebHandler 使用前缀匹配
    auto* updateJsonHandler = new AsyncCallbackJsonWebHandler("/api/periph-exec/update",
        [this](AsyncWebServerRequest* request, JsonVariant& json) {
            handleUpdateRuleJson(request, json);
        });
    updateJsonHandler->setMethod(HTTP_POST);
    server->addHandler(updateJsonHandler);

    auto* addJsonHandler = new AsyncCallbackJsonWebHandler("/api/periph-exec",
        [this](AsyncWebServerRequest* request, JsonVariant& json) {
            handleAddRuleJson(request, json);
        });
    addJsonHandler->setMethod(HTTP_POST);
    server->addHandler(addJsonHandler);

    // 旧版 form-encoded fallback（向后兼容）
    server->on("/api/periph-exec/update", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleUpdateRule(request); });

    server->on("/api/periph-exec/enable", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleEnableRule(request); });

    server->on("/api/periph-exec/disable", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleDisableRule(request); });

    server->on("/api/periph-exec/run", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleRunOnce(request); });

    server->on("/api/periph-exec/", HTTP_DELETE,
              [this](AsyncWebServerRequest* request) { handleDeleteRule(request); });

    server->on("/api/periph-exec", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetRules(request); });

    // 旧版 form-encoded add fallback
    server->on("/api/periph-exec", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleAddRule(request); });
}

// ========== 获取所有规则 ==========

void PeriphExecRouteHandler::handleGetRules(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "system.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

    PeriphExecManager& mgr = PeriphExecManager::getInstance();
    PeripheralManager& pm = PeripheralManager::getInstance();

    // 如果传了 id 参数，只返回单条规则（避免编辑时加载全部规则导致内存不足）
    if (request->hasParam("id")) {
        String ruleId = request->getParam("id")->value();
        auto rules = mgr.getAllRules();
        for (const auto& rule : rules) {
            if (rule.id == ruleId) {
                // 找到规则，使用辅助方法完整序列化
                String response = serializeRuleFull(rule);
                HandlerUtils::sendJsonSuccess(request, response);
                return;
            }
        }
        request->send(404, "application/json", "{\"success\":false,\"message\":\"Rule not found\"}");
        return;
    }

    auto rules = mgr.getAllRules();

    // 排序：启用的排前面，然后按名称排序
    std::sort(rules.begin(), rules.end(), [](const PeriphExecRule& a, const PeriphExecRule& b) {
        if (a.enabled != b.enabled) return a.enabled > b.enabled;
        return strcmp(a.name.c_str(), b.name.c_str()) < 0;
    });

    int total = rules.size();

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
    int endIdx = (startIdx < total) ? min(startIdx + pageSize, total) : startIdx;

    // 检查堆内存是否充足（预留 20KB 给系统其他部分）
    if (HandlerUtils::checkLowMemory(request)) return;
    
    // 构建 JSON String 响应，避免 AsyncResponseStream cbuf 扩容 OOM
    String json;
    json.reserve(256 + (endIdx - startIdx) * 300);
    char itemBuf[512];
    json += F("{\"success\":true,\"total\":");
    json += String(total);
    json += F(",\"page\":");
    json += String(page);
    json += F(",\"pageSize\":");
    json += String(pageSize);
    json += F(",\"data\":[");
    
    bool firstRule = true;
    for (int i = startIdx; i < endIdx; i++) {
        if (!firstRule) json += ',';
        firstRule = false;
    
        const auto& rule = rules[i];
    
        // 列表视图只返回摘要字段
        int triggerCount = rule.triggers.size();
        int actionCount = rule.actions.size();
        int triggerSummary = triggerCount > 0 ? rule.triggers[0].triggerType : -1;
        int actionSummary = actionCount > 0 ? rule.actions[0].actionType : -1;
    
        bool hasSetMode = false;
        for (const auto& trigger : rule.triggers) {
            if (trigger.triggerType == 0 && trigger.operatorType == 1) {
                hasSetMode = true;
                break;
            }
        }
    
        String targetPeriphName;
        if (actionCount > 0 && !rule.actions[0].targetPeriphId.isEmpty()) {
            const PeripheralConfig* periph = pm.getPeripheral(rule.actions[0].targetPeriphId);
            if (periph) targetPeriphName = periph->name;
        }
    
        snprintf(itemBuf, sizeof(itemBuf),
            "{\"id\":\"%s\",\"name\":\"%s\",\"enabled\":%s,"
            "\"execMode\":%d,\"reportAfterExec\":%s,"
            "\"triggerCount\":%d,\"actionCount\":%d,"
            "\"triggerSummary\":%d,\"actionSummary\":%d,"
            "\"hasSetMode\":%s,"
            "\"targetPeriphName\":\"%s\"}",
            rule.id.c_str(),
            rule.name.c_str(),
            rule.enabled ? "true" : "false",
            rule.execMode,
            rule.reportAfterExec ? "true" : "false",
            triggerCount, actionCount,
            triggerSummary, actionSummary,
            hasSetMode ? "true" : "false",
            targetPeriphName.c_str()
        );
        json += itemBuf;
    }
    
    json += F("]}");
    request->send(200, "application/json", json);
}

// ========== 新增规则（form-encoded 向后兼容） ==========

void PeriphExecRouteHandler::handleAddRule(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    PeriphExecRule rule;
    rule.id = ctx->getParamValue(request, "id", "");
    rule.name = ctx->getParamValue(request, "name", "");
    rule.enabled = ctx->getParamBool(request, "enabled", true);
    rule.execMode = ctx->getParamInt(request, "execMode", 0);
    rule.protocolType = ctx->getParamInt(request, "protocolType", 0);
    rule.scriptContent = ctx->getParamValue(request, "scriptContent", "");
    rule.reportAfterExec = ctx->getParamBool(request, "reportAfterExec", true);

    // 旧版 form-encoded: 将平铺字段封装为单元素 triggers[]/actions[]
    ExecTrigger t;
    t.triggerType = ctx->getParamInt(request, "triggerType", 0);
    t.triggerPeriphId = ctx->getParamValue(request, "triggerPeriphId", "");
    t.operatorType = ctx->getParamInt(request, "operatorType", 0);
    t.compareValue = ctx->getParamValue(request, "compareValue", "");
    t.timerMode = ctx->getParamInt(request, "timerMode", 0);
    t.intervalSec = ctx->getParamInt(request, "intervalSec", 60);
    t.timePoint = ctx->getParamValue(request, "timePoint", "");
    t.eventId = ctx->getParamValue(request, "eventId", "");
    t.pollResponseTimeout = ctx->getParamInt(request, "pollResponseTimeout", 1000);
    t.pollMaxRetries = ctx->getParamInt(request, "pollMaxRetries", 2);
    t.pollInterPollDelay = ctx->getParamInt(request, "pollInterPollDelay", 100);
    rule.triggers.push_back(t);

    ExecAction a;
    a.targetPeriphId = ctx->getParamValue(request, "targetPeriphId", "");
    a.actionType = ctx->getParamInt(request, "actionType", 0);
    a.actionValue = ctx->getParamValue(request, "actionValue", "");
    a.useReceivedValue = ctx->getParamBool(request, "useReceivedValue", false);
    a.syncDelayMs = ctx->getParamInt(request, "syncDelayMs", 0);
    a.execMode = rule.execMode;  // form-encoded 模式：从规则级别继承
    rule.actions.push_back(a);

    if (rule.name.isEmpty()) {
        return;
    }

    PeriphExecManager& mgr = PeriphExecManager::getInstance();
    if (mgr.addRule(rule)) {
        mgr.saveConfiguration();
        ctx->sendSuccess(request, "Rule added");
    } else {
        ctx->sendError(request, 500, "Failed to add rule");
    }
}

// ========== 更新规则 ==========

void PeriphExecRouteHandler::handleUpdateRule(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String id = ctx->getParamValue(request, "id", "");
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Rule ID is required");
        return;
    }

    PeriphExecManager& mgr = PeriphExecManager::getInstance();
    PeriphExecRule* existing = mgr.getRule(id);
    if (!existing) {
        ctx->sendError(request, 404, "Rule not found");
        return;
    }

    PeriphExecRule rule;
    rule.id = id;
    rule.name = ctx->getParamValue(request, "name", existing->name);
    rule.enabled = ctx->getParamBool(request, "enabled", existing->enabled);
    rule.execMode = ctx->getParamInt(request, "execMode", existing->execMode);
    rule.protocolType = ctx->getParamInt(request, "protocolType", existing->protocolType);
    rule.scriptContent = ctx->getParamValue(request, "scriptContent", existing->scriptContent);
    rule.reportAfterExec = ctx->getParamBool(request, "reportAfterExec", existing->reportAfterExec);

    // 旧版 form-encoded: 使用已有 trigger/action 作为默认值
    ExecTrigger t;
    if (!existing->triggers.empty()) {
        t = existing->triggers[0]; // 复制现有 trigger 作为默认
    }
    t.triggerType = ctx->getParamInt(request, "triggerType", t.triggerType);
    t.triggerPeriphId = ctx->getParamValue(request, "triggerPeriphId", t.triggerPeriphId);
    t.operatorType = ctx->getParamInt(request, "operatorType", t.operatorType);
    t.compareValue = ctx->getParamValue(request, "compareValue", t.compareValue);
    t.timerMode = ctx->getParamInt(request, "timerMode", t.timerMode);
    t.intervalSec = ctx->getParamInt(request, "intervalSec", t.intervalSec);
    t.timePoint = ctx->getParamValue(request, "timePoint", t.timePoint);
    t.eventId = ctx->getParamValue(request, "eventId", t.eventId);
    t.pollResponseTimeout = ctx->getParamInt(request, "pollResponseTimeout", t.pollResponseTimeout);
    t.pollMaxRetries = ctx->getParamInt(request, "pollMaxRetries", t.pollMaxRetries);
    t.pollInterPollDelay = ctx->getParamInt(request, "pollInterPollDelay", t.pollInterPollDelay);
    rule.triggers.push_back(t);

    ExecAction a;
    if (!existing->actions.empty()) {
        a = existing->actions[0]; // 复制现有 action 作为默认
    }
    a.targetPeriphId = ctx->getParamValue(request, "targetPeriphId", a.targetPeriphId);
    a.actionType = ctx->getParamInt(request, "actionType", a.actionType);
    a.actionValue = ctx->getParamValue(request, "actionValue", a.actionValue);
    a.useReceivedValue = ctx->getParamBool(request, "useReceivedValue", a.useReceivedValue);
    a.syncDelayMs = ctx->getParamInt(request, "syncDelayMs", a.syncDelayMs);
    a.execMode = rule.execMode;  // form-encoded 模式：从规则级别继承
    rule.actions.push_back(a);

    if (mgr.updateRule(id, rule)) {
        mgr.saveConfiguration();
        ctx->sendSuccess(request, "Rule updated");
    } else {
        ctx->sendError(request, 500, "Failed to update rule");
    }
}

// ========== 删除规则 ==========

void PeriphExecRouteHandler::handleDeleteRule(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String id = ctx->getParamValue(request, "id", "");
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Rule ID is required");
        return;
    }

    PeriphExecManager& mgr = PeriphExecManager::getInstance();
    if (mgr.removeRule(id)) {
        mgr.saveConfiguration();
        ctx->sendSuccess(request, "Rule deleted");
    } else {
        ctx->sendError(request, 404, "Rule not found");
    }
}

// ========== 启用规则 ==========

void PeriphExecRouteHandler::handleEnableRule(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String id = ctx->getParamValue(request, "id", "");
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Rule ID is required");
        return;
    }

    PeriphExecManager& mgr = PeriphExecManager::getInstance();
    if (mgr.enableRule(id)) {
        mgr.saveConfiguration();
        ctx->sendSuccess(request, "Rule enabled");
    } else {
        ctx->sendError(request, 404, "Rule not found");
    }
}

// ========== 禁用规则 ==========

void PeriphExecRouteHandler::handleDisableRule(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String id = ctx->getParamValue(request, "id", "");
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Rule ID is required");
        return;
    }

    PeriphExecManager& mgr = PeriphExecManager::getInstance();
    if (mgr.disableRule(id)) {
        mgr.saveConfiguration();
        ctx->sendSuccess(request, "Rule disabled");
    } else {
        ctx->sendError(request, 404, "Rule not found");
    }
}

// ========== 执行一次规则 ==========

void PeriphExecRouteHandler::handleRunOnce(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String id = ctx->getParamValue(request, "id", "");
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Rule ID is required");
        return;
    }

    // 可选的用户输入值（设置模式下从前端弹窗传入）
    String value = ctx->getParamValue(request, "value", "");

    PeriphExecManager& mgr = PeriphExecManager::getInstance();
    
    // 异步提交执行（不阻塞 Web 请求线程）
    bool submitted = mgr.runOnce(id, value);

    JsonDocument doc;
    if (submitted) {
        doc["success"] = true;
        doc["message"] = "Rule execution submitted";
    } else {
        doc["success"] = false;
        doc["message"] = "Rule not found";
    }
    HandlerUtils::sendJsonStream(request, doc);
}

// ========== 获取静态事件列表 ==========

void PeriphExecRouteHandler::handleGetStaticEvents(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "system.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String eventsJson = PeriphExecManager::getStaticEventsJson();
    HandlerUtils::sendJsonSuccess(request, eventsJson);
}

// ========== 获取动态事件列表（包含外设执行规则事件） ==========

void PeriphExecRouteHandler::handleGetDynamicEvents(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "system.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

    PeriphExecManager& mgr = PeriphExecManager::getInstance();
    String eventsJson = mgr.getDynamicEventsJson();
    HandlerUtils::sendJsonSuccess(request, eventsJson);
}

// ========== 获取事件分类列表 ==========

void PeriphExecRouteHandler::handleGetEventCategories(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "system.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String categoriesJson = PeriphExecManager::getEventCategoriesJson();
    HandlerUtils::sendJsonSuccess(request, categoriesJson);
}

// ========== 获取触发类型列表 ==========

void PeriphExecRouteHandler::handleGetTriggerTypes(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "system.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String triggerTypesJson = PeriphExecManager::getValidTriggerTypes();
    HandlerUtils::sendJsonSuccess(request, triggerTypesJson);
}

// ========== JSON → PeriphExecRule 解析辅助 ==========

// 从 JsonVariant 读取整数，兼容 JSON 数值和字符串两种格式
static int jsonInt(JsonVariant v, int def) {
    if (v.is<int>()) return v.as<int>();
    if (v.is<const char*>()) return String(v.as<const char*>()).toInt();
    return def;
}

void PeriphExecRouteHandler::parseRuleFromJson(JsonObject& obj, PeriphExecRule& rule) {
    // 注意：as<String>() 对缺失字段返回 "null" 字符串（serializeJson 序列化结果），
    // 必须用 | "" 或 is<>() 保护所有字符串提取
    if (obj["name"].is<const char*>()) rule.name = obj["name"] | "";
    if (obj["enabled"].is<bool>()) rule.enabled = obj["enabled"].as<bool>();
    else if (obj["enabled"].is<const char*>()) rule.enabled = String(obj["enabled"].as<const char*>()) == "1";
    rule.execMode = jsonInt(obj["execMode"], rule.execMode);

    // 数据转换管道
    rule.protocolType = jsonInt(obj["protocolType"], rule.protocolType);
    if (obj["scriptContent"].is<const char*>()) rule.scriptContent = obj["scriptContent"] | "";
    if (obj["reportAfterExec"].is<bool>()) rule.reportAfterExec = obj["reportAfterExec"].as<bool>();

    // 解析 triggers 数组
    if (obj["triggers"].is<JsonArray>()) {
        rule.triggers.clear();
        JsonArray trigArr = obj["triggers"].as<JsonArray>();
        for (JsonObject tObj : trigArr) {
            ExecTrigger t;
            t.triggerType = jsonInt(tObj["triggerType"], 0);
            t.triggerPeriphId = tObj["triggerPeriphId"] | "";
            t.operatorType = jsonInt(tObj["operatorType"], 0);
            t.compareValue = tObj["compareValue"] | "";
            t.timerMode = jsonInt(tObj["timerMode"], 0);
            t.intervalSec = jsonInt(tObj["intervalSec"], 60);
            t.timePoint = tObj["timePoint"] | "";
            t.eventId = tObj["eventId"] | "";
            // 轮询触发通信参数
            t.pollResponseTimeout = jsonInt(tObj["pollResponseTimeout"], 1000);
            t.pollMaxRetries = jsonInt(tObj["pollMaxRetries"], 2);
            t.pollInterPollDelay = jsonInt(tObj["pollInterPollDelay"], 100);
            t.lastTriggerTime = 0;
            t.triggerCount = 0;
            rule.triggers.push_back(t);
        }
    }

    // 解析 actions 数组
    if (obj["actions"].is<JsonArray>()) {
        rule.actions.clear();
        JsonArray actArr = obj["actions"].as<JsonArray>();
        for (JsonObject aObj : actArr) {
            ExecAction a;
            a.targetPeriphId = aObj["targetPeriphId"] | "";
            a.actionType = jsonInt(aObj["actionType"], 0);
            a.actionValue = aObj["actionValue"] | "";
            a.useReceivedValue = aObj["useReceivedValue"] | false;
            a.syncDelayMs = jsonInt(aObj["syncDelayMs"], 0);
            a.execMode = jsonInt(aObj["execMode"], 0);
            rule.actions.push_back(a);
        }
    }
}

// ========== JSON body: 新增规则 ==========

void PeriphExecRouteHandler::handleAddRuleJson(AsyncWebServerRequest* request, JsonVariant& json) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    JsonObject obj = json.as<JsonObject>();
    PeriphExecRule rule;
    // as<String>() 对缺失字段返回 "null" 字符串，用 | 运算符安全提取
    const char* idStr = obj["id"] | (const char*)nullptr;
    if (idStr && idStr[0] != '\0') rule.id = idStr;
    parseRuleFromJson(obj, rule);

    if (rule.name.isEmpty()) {
        ctx->sendError(request, 400, "Rule name is required");
        return;
    }

    PeriphExecManager& mgr = PeriphExecManager::getInstance();
    if (mgr.addRule(rule)) {
        mgr.saveConfiguration();
        ctx->sendSuccess(request, "Rule added");
    } else {
        ctx->sendError(request, 500, "Failed to add rule");
    }
}

// ========== 获取设备控制列表（按动作类型分组） ==========

void PeriphExecRouteHandler::handleGetControls(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "system.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

    PeriphExecManager& mgr = PeriphExecManager::getInstance();
    PeripheralManager& pm = PeripheralManager::getInstance();
    auto rules = mgr.getAllRules();

    // 构建 JSON String 响应，避免 AsyncResponseStream cbuf 扩容 OOM

    // 分组缓冲：记录每个分组是否已写入第一条
    bool firstGpio = true, firstModbus = true, firstSystem = true;
    bool firstScript = true, firstSensor = true, firstOther = true;

    // 临时缓冲各分组的内容
    String gpioItems, modbusItems, systemItems, scriptItems, sensorItems, otherItems;

    for (const auto& rule : rules) {
        if (!rule.enabled) continue;
        if (rule.actions.empty()) continue;

        const ExecAction& firstAction = rule.actions[0];
        int at = firstAction.actionType;

        // 确定分组
        String* targetBuf = nullptr;
        bool* firstFlag = nullptr;
        if ((at >= 0 && at <= 5) || at == 13 || at == 14) {
            targetBuf = &gpioItems; firstFlag = &firstGpio;
        } else if (at >= 16 && at <= 18) {
            targetBuf = &modbusItems; firstFlag = &firstModbus;
        } else if (at >= 6 && at <= 12) {
            targetBuf = &systemItems; firstFlag = &firstSystem;
        } else if (at == 15) {
            targetBuf = &scriptItems; firstFlag = &firstScript;
        } else if (at == 19) {
            targetBuf = &sensorItems; firstFlag = &firstSensor;
        } else {
            targetBuf = &otherItems; firstFlag = &firstOther;
        }

        if (!*firstFlag) {
            *targetBuf += ",";
        }
        *firstFlag = false;

        // 获取目标外设名称
        String periphName;
        if (!firstAction.targetPeriphId.isEmpty()) {
            const PeripheralConfig* periph = pm.getPeripheral(firstAction.targetPeriphId);
            if (periph) periphName = periph->name;
        }

        // 构建单条记录
        // 转义 name 和 periphName 中的双引号
        String safeName = rule.name;
        safeName.replace("\"", "\\\"");
        String safePeriphName = periphName;
        safePeriphName.replace("\"", "\\\"");
        bool hasSetMode = false;
        for (const auto& trigger : rule.triggers) {
            if (trigger.triggerType == 0 && trigger.operatorType == 1) {
                hasSetMode = true;
                break;
            }
        }

        String item = "{\"id\":\"" + rule.id + "\",\"name\":\"" + safeName + "\",\"actionType\":" + String(at);
        item += ",\"hasSetMode\":";
        item += hasSetMode ? "true" : "false";

        // 附加目标外设信息（gpio/modbus/sensor 有目标外设）
        if (!firstAction.targetPeriphId.isEmpty()) {
            item += ",\"targetPeriphId\":\"" + firstAction.targetPeriphId + "\"";
            if (periphName.length() > 0) {
                item += ",\"targetPeriphName\":\"" + safePeriphName + "\"";
            }
        }

        // Modbus 轮询：尝试解析 pollTaskIndex
        if (at == 18 && firstAction.actionValue.length() > 0) {
            // 解析 actionValue JSON 中的 "poll" 字段
            JsonDocument pollDoc;
            DeserializationError err = deserializeJson(pollDoc, firstAction.actionValue);
            if (!err && pollDoc["poll"].is<JsonArray>()) {
                JsonArray pollArr = pollDoc["poll"].as<JsonArray>();
                item += ",\"pollTaskIndex\":[";
                bool firstPoll = true;
                for (JsonVariant v : pollArr) {
                    if (!firstPoll) item += ",";
                    firstPoll = false;
                    item += String(v.as<int>());
                }
                item += "]";
            }
        }

        item += "}";
        *targetBuf += item;
    }

    // 输出完整 JSON（直接拼接 String，避免 AsyncResponseStream）
    String json;
    json.reserve(128 + gpioItems.length() + modbusItems.length() + systemItems.length()
                + scriptItems.length() + sensorItems.length() + otherItems.length());
    json += F("{\"success\":true,\"data\":{");
    json += F("\"gpio\":["); json += gpioItems; json += F("],");
    json += F("\"modbus\":["); json += modbusItems; json += F("],");
    json += F("\"system\":["); json += systemItems; json += F("],");
    json += F("\"script\":["); json += scriptItems; json += F("],");
    json += F("\"sensor\":["); json += sensorItems; json += F("],");
    json += F("\"other\":["); json += otherItems; json += ']';
    json += F("}}");
    request->send(200, "application/json", json);
}

// ========== JSON body: 更新规则 ==========

void PeriphExecRouteHandler::handleUpdateRuleJson(AsyncWebServerRequest* request, JsonVariant& json) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    JsonObject obj = json.as<JsonObject>();
    // as<String>() 对缺失字段返回 "null" 字符串，用 | 运算符安全提取
    const char* idStr = obj["id"] | "";
    String id = idStr;
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Rule ID is required");
        return;
    }

    PeriphExecManager& mgr = PeriphExecManager::getInstance();
    PeriphExecRule* existing = mgr.getRule(id);
    if (!existing) {
        ctx->sendError(request, 404, "Rule not found");
        return;
    }

    // 基于现有规则更新
    PeriphExecRule rule = *existing;
    parseRuleFromJson(obj, rule);
    rule.id = id;

    if (mgr.updateRule(id, rule)) {
        mgr.saveConfiguration();
        ctx->sendSuccess(request, "Rule updated");
    } else {
        ctx->sendError(request, 500, "Failed to update rule");
    }
}

// ========== 完整序列化单条规则 ==========

String PeriphExecRouteHandler::serializeRuleFull(const PeriphExecRule& rule) {
    PeripheralManager& pm = PeripheralManager::getInstance();

    JsonDocument ruleDoc;
    ruleDoc["id"] = rule.id;
    ruleDoc["name"] = rule.name;
    ruleDoc["enabled"] = rule.enabled;
    ruleDoc["execMode"] = rule.execMode;

    // 触发器数组
    JsonArray trigArr = ruleDoc["triggers"].to<JsonArray>();
    for (const auto& t : rule.triggers) {
        JsonObject tObj = trigArr.add<JsonObject>();
        tObj["triggerType"] = t.triggerType;
        tObj["triggerPeriphId"] = t.triggerPeriphId;
        tObj["operatorType"] = t.operatorType;
        tObj["compareValue"] = t.compareValue;
        tObj["timerMode"] = t.timerMode;
        tObj["intervalSec"] = t.intervalSec;
        tObj["timePoint"] = t.timePoint;
        tObj["eventId"] = t.eventId;
        // 轮询触发通信参数
        tObj["pollResponseTimeout"] = t.pollResponseTimeout;
        tObj["pollMaxRetries"] = t.pollMaxRetries;
        tObj["pollInterPollDelay"] = t.pollInterPollDelay;
        // 运行时状态
        tObj["lastTriggerTime"] = t.lastTriggerTime;
        tObj["triggerCount"] = t.triggerCount;

        // 关联触发外设名称
        if (!t.triggerPeriphId.isEmpty()) {
            const PeripheralConfig* periph = pm.getPeripheral(t.triggerPeriphId);
            if (periph) {
                tObj["triggerPeriphName"] = periph->name;
            }
        }
    }

    // 动作数组
    JsonArray actArr = ruleDoc["actions"].to<JsonArray>();
    for (const auto& a : rule.actions) {
        JsonObject aObj = actArr.add<JsonObject>();
        aObj["targetPeriphId"] = a.targetPeriphId;
        aObj["actionType"] = a.actionType;
        aObj["actionValue"] = a.actionValue;
        aObj["useReceivedValue"] = a.useReceivedValue;
        aObj["syncDelayMs"] = a.syncDelayMs;
        aObj["execMode"] = a.execMode;
        if (!a.targetPeriphId.isEmpty()) {
            const PeripheralConfig* periph = pm.getPeripheral(a.targetPeriphId);
            if (periph) {
                aObj["targetPeriphName"] = periph->name;
                aObj["targetPeriphType"] = static_cast<int>(periph->type);
            }
        }
    }

    // 数据转换管道
    ruleDoc["protocolType"] = rule.protocolType;
    ruleDoc["scriptContent"] = rule.scriptContent;

    // 数据上报控制
    ruleDoc["reportAfterExec"] = rule.reportAfterExec;

    String output;
    serializeJson(ruleDoc, output);
    return output;
}

#endif // FASTBEE_ENABLE_PERIPH_EXEC
