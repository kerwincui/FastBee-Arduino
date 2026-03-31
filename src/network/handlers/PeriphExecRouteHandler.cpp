#include "./network/handlers/PeriphExecRouteHandler.h"
#include "./network/WebHandlerContext.h"
#include "core/PeriphExecManager.h"
#include "core/PeripheralManager.h"
#include "protocols/MQTTClient.h"
#include "protocols/ProtocolManager.h"
#include <ArduinoJson.h>

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
        obj["triggerType"] = rule.triggerType;
        obj["execMode"] = rule.execMode;

        // 设备触发
        obj["operatorType"] = rule.operatorType;
        obj["compareValue"] = rule.compareValue;

        // 触发事件
        obj["eventId"] = rule.eventId;

        // 定时触发
        obj["timerMode"] = rule.timerMode;
        obj["intervalSec"] = rule.intervalSec;
        obj["timePoint"] = rule.timePoint;

        // 数据转换管道
        obj["protocolType"] = rule.protocolType;
        obj["scriptContent"] = rule.scriptContent;

        // 动作
        obj["targetPeriphId"] = rule.targetPeriphId;
        obj["actionType"] = rule.actionType;
        obj["actionValue"] = rule.actionValue;

        // 数据上报控制
        obj["reportAfterExec"] = rule.reportAfterExec;

        // 运行时状态
        obj["lastTriggerTime"] = rule.lastTriggerTime;
        obj["triggerCount"] = rule.triggerCount;

        // 关联外设名称
        if (!rule.targetPeriphId.isEmpty()) {
            const PeripheralConfig* periph = pm.getPeripheral(rule.targetPeriphId);
            if (periph) {
                obj["targetPeriphName"] = periph->name;
                obj["targetPeriphType"] = static_cast<int>(periph->type);
            } else {
                obj["targetPeriphName"] = "";
                obj["targetPeriphType"] = 0;
            }
        }
    }

    String output;
    serializeJson(doc, output);
    request->send(200, "application/json", output);
}

// ========== 新增规则 ==========

void PeriphExecRouteHandler::handleAddRule(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    PeriphExecRule rule;
    rule.id = ctx->getParamValue(request, "id", "");
    rule.name = ctx->getParamValue(request, "name", "");
    rule.enabled = ctx->getParamBool(request, "enabled", true);
    rule.triggerType = ctx->getParamInt(request, "triggerType", 0);
    rule.execMode = ctx->getParamInt(request, "execMode", 0);

    rule.operatorType = ctx->getParamInt(request, "operatorType", 0);
    rule.compareValue = ctx->getParamValue(request, "compareValue", "");

    // 触发事件
    rule.eventId = ctx->getParamValue(request, "eventId", "");
    // 根据 eventId 解析 eventType
    if (!rule.eventId.isEmpty()) {
        const EventDef* def = findStaticEvent(rule.eventId.c_str());
        if (def) {
            rule.eventType = static_cast<uint8_t>(def->type);
        }
    }

    rule.timerMode = ctx->getParamInt(request, "timerMode", 0);
    rule.intervalSec = ctx->getParamInt(request, "intervalSec", 60);
    rule.timePoint = ctx->getParamValue(request, "timePoint", "");

    rule.targetPeriphId = ctx->getParamValue(request, "targetPeriphId", "");
    rule.actionType = ctx->getParamInt(request, "actionType", 0);
    rule.actionValue = ctx->getParamValue(request, "actionValue", "");

    rule.protocolType = ctx->getParamInt(request, "protocolType", 0);
    rule.scriptContent = ctx->getParamValue(request, "scriptContent", "");

    // 数据上报控制
    rule.reportAfterExec = ctx->getParamBool(request, "reportAfterExec", true);

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
    rule.triggerType = ctx->getParamInt(request, "triggerType", existing->triggerType);
    rule.execMode = ctx->getParamInt(request, "execMode", existing->execMode);

    rule.operatorType = ctx->getParamInt(request, "operatorType", existing->operatorType);
    rule.compareValue = ctx->getParamValue(request, "compareValue", existing->compareValue);

    // 触发事件
    rule.eventId = ctx->getParamValue(request, "eventId", existing->eventId);
    // 根据 eventId 解析 eventType
    if (!rule.eventId.isEmpty()) {
        const EventDef* def = findStaticEvent(rule.eventId.c_str());
        if (def) {
            rule.eventType = static_cast<uint8_t>(def->type);
        }
    }

    rule.timerMode = ctx->getParamInt(request, "timerMode", existing->timerMode);
    rule.intervalSec = ctx->getParamInt(request, "intervalSec", existing->intervalSec);
    rule.timePoint = ctx->getParamValue(request, "timePoint", existing->timePoint);

    rule.targetPeriphId = ctx->getParamValue(request, "targetPeriphId", existing->targetPeriphId);
    rule.actionType = ctx->getParamInt(request, "actionType", existing->actionType);
    rule.actionValue = ctx->getParamValue(request, "actionValue", existing->actionValue);

    rule.protocolType = ctx->getParamInt(request, "protocolType", existing->protocolType);
    rule.scriptContent = ctx->getParamValue(request, "scriptContent", existing->scriptContent);

    // 数据上报控制
    rule.reportAfterExec = ctx->getParamBool(request, "reportAfterExec", existing->reportAfterExec);

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
