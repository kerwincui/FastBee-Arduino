#include "./network/handlers/PeriphExecRouteHandler.h"
#include "./network/WebHandlerContext.h"
#include "core/PeriphExecManager.h"
#include "core/PeripheralManager.h"
#include "protocols/MQTTClient.h"
#include "protocols/ProtocolManager.h"
#include <ArduinoJson.h>
#include <AsyncJson.h>

PeriphExecRouteHandler::PeriphExecRouteHandler(WebHandlerContext* ctx)
    : ctx(ctx) {
}

void PeriphExecRouteHandler::setupRoutes(AsyncWebServer* server) {
    // 注意：更具体的路由必须先注册，否则会被 /api/periph-exec 拦截
    
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
    auto* addJsonHandler = new AsyncCallbackJsonWebHandler("/api/periph-exec",
        [this](AsyncWebServerRequest* request, JsonVariant& json) {
            handleAddRuleJson(request, json);
        });
    addJsonHandler->setMethod(HTTP_POST);
    server->addHandler(addJsonHandler);

    auto* updateJsonHandler = new AsyncCallbackJsonWebHandler("/api/periph-exec/update",
        [this](AsyncWebServerRequest* request, JsonVariant& json) {
            handleUpdateRuleJson(request, json);
        });
    updateJsonHandler->setMethod(HTTP_POST);
    server->addHandler(updateJsonHandler);

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
    auto rules = mgr.getAllRules();

    JsonDocument doc;
    doc["success"] = true;
    JsonArray data = doc["data"].to<JsonArray>();

    for (const auto& rule : rules) {
        JsonObject obj = data.add<JsonObject>();
        obj["id"] = rule.id;
        obj["name"] = rule.name;
        obj["enabled"] = rule.enabled;
        obj["execMode"] = rule.execMode;

        // 触发器数组
        JsonArray trigArr = obj["triggers"].to<JsonArray>();
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
        JsonArray actArr = obj["actions"].to<JsonArray>();
        for (const auto& a : rule.actions) {
            JsonObject aObj = actArr.add<JsonObject>();
            aObj["targetPeriphId"] = a.targetPeriphId;
            aObj["actionType"] = a.actionType;
            aObj["actionValue"] = a.actionValue;
            aObj["useReceivedValue"] = a.useReceivedValue;
            aObj["syncDelayMs"] = a.syncDelayMs;

            // 关联目标外设名称
            if (!a.targetPeriphId.isEmpty()) {
                const PeripheralConfig* periph = pm.getPeripheral(a.targetPeriphId);
                if (periph) {
                    aObj["targetPeriphName"] = periph->name;
                    aObj["targetPeriphType"] = static_cast<int>(periph->type);
                }
            }
        }

        // 数据转换管道
        obj["protocolType"] = rule.protocolType;
        obj["scriptContent"] = rule.scriptContent;

        // 数据上报控制
        obj["reportAfterExec"] = rule.reportAfterExec;
    }

    String output;
    serializeJson(doc, output);
    request->send(200, "application/json", output);
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
    rule.triggers.push_back(t);

    ExecAction a;
    a.targetPeriphId = ctx->getParamValue(request, "targetPeriphId", "");
    a.actionType = ctx->getParamInt(request, "actionType", 0);
    a.actionValue = ctx->getParamValue(request, "actionValue", "");
    a.useReceivedValue = ctx->getParamBool(request, "useReceivedValue", false);
    a.syncDelayMs = ctx->getParamInt(request, "syncDelayMs", 0);
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

    PeriphExecManager& mgr = PeriphExecManager::getInstance();
    
    // 执行规则动作
    bool ok = mgr.runOnce(id);

    // 触发设备状态上报（如果MQTT已连接）
    if (ctx->protocolManager) {
        MQTTClient* mqtt = ctx->protocolManager->getMQTTClient();
        if (mqtt && mqtt->getIsConnected()) {
            // 发布设备信息作为状态上报
            mqtt->publishDeviceInfo();
        }
    }

    JsonDocument doc;
    doc["success"] = ok;
    doc["message"] = ok ? "Rule executed successfully" : "Rule execution failed";
    String output;
    serializeJson(doc, output);
    request->send(200, "application/json", output);
}

// ========== 获取静态事件列表 ==========

void PeriphExecRouteHandler::handleGetStaticEvents(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "system.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String eventsJson = PeriphExecManager::getStaticEventsJson();
    
    // 直接构建完整响应，避免嵌套反序列化问题
    String output = "{\"success\":true,\"data\":" + eventsJson + "}";
    request->send(200, "application/json", output);
}

// ========== 获取动态事件列表（包含外设执行规则事件） ==========

void PeriphExecRouteHandler::handleGetDynamicEvents(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "system.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

    PeriphExecManager& mgr = PeriphExecManager::getInstance();
    String eventsJson = mgr.getDynamicEventsJson();
    
    // 直接构建完整响应，避免嵌套反序列化问题
    String output = "{\"success\":true,\"data\":" + eventsJson + "}";
    request->send(200, "application/json", output);
}

// ========== 获取事件分类列表 ==========

void PeriphExecRouteHandler::handleGetEventCategories(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "system.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String categoriesJson = PeriphExecManager::getEventCategoriesJson();
    
    // 直接构建完整响应，避免嵌套反序列化问题
    String output = "{\"success\":true,\"data\":" + categoriesJson + "}";
    request->send(200, "application/json", output);
}

// ========== 获取触发类型列表 ==========

void PeriphExecRouteHandler::handleGetTriggerTypes(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "system.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String triggerTypesJson = PeriphExecManager::getValidTriggerTypes();
    
    // 直接构建完整响应，避免嵌套反序列化问题
    String output = "{\"success\":true,\"data\":" + triggerTypesJson + "}";
    request->send(200, "application/json", output);
}

// ========== JSON → PeriphExecRule 解析辅助 ==========

void PeriphExecRouteHandler::parseRuleFromJson(JsonObject& obj, PeriphExecRule& rule) {
    if (obj["name"].is<String>()) rule.name = obj["name"].as<String>();
    if (obj["enabled"].is<bool>()) rule.enabled = obj["enabled"].as<bool>();
    if (obj["execMode"].is<int>()) rule.execMode = obj["execMode"].as<int>();

    // 数据转换管道
    if (obj["protocolType"].is<int>()) rule.protocolType = obj["protocolType"].as<int>();
    if (obj["scriptContent"].is<String>()) rule.scriptContent = obj["scriptContent"].as<String>();
    if (obj["reportAfterExec"].is<bool>()) rule.reportAfterExec = obj["reportAfterExec"].as<bool>();

    // 解析 triggers 数组
    if (obj["triggers"].is<JsonArray>()) {
        rule.triggers.clear();
        JsonArray trigArr = obj["triggers"].as<JsonArray>();
        for (JsonObject tObj : trigArr) {
            ExecTrigger t;
            t.triggerType = tObj["triggerType"] | 0;
            t.triggerPeriphId = tObj["triggerPeriphId"].as<String>();
            t.operatorType = tObj["operatorType"] | 0;
            t.compareValue = tObj["compareValue"].as<String>();
            t.timerMode = tObj["timerMode"] | 0;
            t.intervalSec = tObj["intervalSec"] | 60;
            t.timePoint = tObj["timePoint"].as<String>();
            t.eventId = tObj["eventId"].as<String>();
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
            a.targetPeriphId = aObj["targetPeriphId"].as<String>();
            a.actionType = aObj["actionType"] | 0;
            a.actionValue = aObj["actionValue"].as<String>();
            a.useReceivedValue = aObj["useReceivedValue"] | false;
            a.syncDelayMs = aObj["syncDelayMs"] | 0;
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
    rule.id = obj["id"].as<String>();
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

// ========== JSON body: 更新规则 ==========

void PeriphExecRouteHandler::handleUpdateRuleJson(AsyncWebServerRequest* request, JsonVariant& json) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    JsonObject obj = json.as<JsonObject>();
    String id = obj["id"].as<String>();
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
