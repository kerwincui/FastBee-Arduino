#include "./network/handlers/PeriphExecRouteHandler.h"
#include "./network/WebHandlerContext.h"
#include "core/PeriphExecManager.h"
#include "core/PeripheralManager.h"
#include <ArduinoJson.h>

PeriphExecRouteHandler::PeriphExecRouteHandler(WebHandlerContext* ctx)
    : ctx(ctx) {
}

void PeriphExecRouteHandler::setupRoutes(AsyncWebServer* server) {
    server->on("/api/periph-exec/update", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleUpdateRule(request); });

    server->on("/api/periph-exec/enable", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleEnableRule(request); });

    server->on("/api/periph-exec/disable", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleDisableRule(request); });

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
        obj["sourcePeriphId"] = rule.sourcePeriphId;

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
        if (!rule.sourcePeriphId.isEmpty()) {
            const PeripheralConfig* srcPeriph = pm.getPeripheral(rule.sourcePeriphId);
            if (srcPeriph) {
                obj["sourcePeriphName"] = srcPeriph->name;
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
    rule.sourcePeriphId = ctx->getParamValue(request, "sourcePeriphId", "");

    rule.timerMode = ctx->getParamInt(request, "timerMode", 0);
    rule.intervalSec = ctx->getParamInt(request, "intervalSec", 60);
    rule.timePoint = ctx->getParamValue(request, "timePoint", "");

    rule.targetPeriphId = ctx->getParamValue(request, "targetPeriphId", "");
    rule.actionType = ctx->getParamInt(request, "actionType", 0);
    rule.actionValue = ctx->getParamValue(request, "actionValue", "");
    rule.inverted = false;

    rule.protocolType = ctx->getParamInt(request, "protocolType", 0);
    rule.scriptContent = ctx->getParamValue(request, "scriptContent", "");

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
    rule.sourcePeriphId = ctx->getParamValue(request, "sourcePeriphId", existing->sourcePeriphId);

    rule.timerMode = ctx->getParamInt(request, "timerMode", existing->timerMode);
    rule.intervalSec = ctx->getParamInt(request, "intervalSec", existing->intervalSec);
    rule.timePoint = ctx->getParamValue(request, "timePoint", existing->timePoint);

    rule.targetPeriphId = ctx->getParamValue(request, "targetPeriphId", existing->targetPeriphId);
    rule.actionType = ctx->getParamInt(request, "actionType", existing->actionType);
    rule.actionValue = ctx->getParamValue(request, "actionValue", existing->actionValue);
    rule.inverted = false;  // 不再使用独立字段，由 actionType 决定

    rule.protocolType = ctx->getParamInt(request, "protocolType", existing->protocolType);
    rule.scriptContent = ctx->getParamValue(request, "scriptContent", existing->scriptContent);

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
