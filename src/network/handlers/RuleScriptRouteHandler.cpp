#include "./network/handlers/RuleScriptRouteHandler.h"
#include "./network/handlers/HandlerUtils.h"
#include "./network/WebHandlerContext.h"
#include "core/RuleScriptManager.h"
#include <ArduinoJson.h>

RuleScriptRouteHandler::RuleScriptRouteHandler(WebHandlerContext* ctx)
    : ctx(ctx) {
}

void RuleScriptRouteHandler::setupRoutes(AsyncWebServer* server) {
    server->on("/api/rule-script/update", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleUpdateRule(request); });

    server->on("/api/rule-script/enable", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleEnableRule(request); });

    server->on("/api/rule-script/disable", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleDisableRule(request); });

    server->on("/api/rule-script/", HTTP_DELETE,
              [this](AsyncWebServerRequest* request) { handleDeleteRule(request); });

    server->on("/api/rule-script", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetRules(request); });

    server->on("/api/rule-script", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleAddRule(request); });
}

// ========== 获取所有规则 ==========

void RuleScriptRouteHandler::handleGetRules(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "system.view")) {
        ctx->sendUnauthorized(request);
        return;
    }

    RuleScriptManager& mgr = RuleScriptManager::getInstance();
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
        obj["protocolType"] = rule.protocolType;
        obj["scriptContent"] = rule.scriptContent;
        obj["lastTriggerTime"] = rule.lastTriggerTime;
        obj["triggerCount"] = rule.triggerCount;
    }

    String output;
    serializeJson(doc, output);
    request->send(200, "application/json", output);
}

// ========== 新增规则 ==========

void RuleScriptRouteHandler::handleAddRule(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    RuleScript rule;
    rule.id = ctx->getParamValue(request, "id", "");
    rule.name = ctx->getParamValue(request, "name", "");
    rule.enabled = ctx->getParamBool(request, "enabled", true);
    rule.triggerType = ctx->getParamInt(request, "triggerType", 0);
    rule.protocolType = ctx->getParamInt(request, "protocolType", 0);
    rule.scriptContent = ctx->getParamValue(request, "scriptContent", "");

    if (rule.name.isEmpty()) {
        ctx->sendBadRequest(request, "Name is required");
        return;
    }

    RuleScriptManager& mgr = RuleScriptManager::getInstance();
    if (mgr.addRule(rule)) {
        mgr.saveConfiguration();
        ctx->sendSuccess(request, "Rule added");
    } else {
        ctx->sendError(request, 500, "Failed to add rule");
    }
}

// ========== 更新规则 ==========

void RuleScriptRouteHandler::handleUpdateRule(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String id = ctx->getParamValue(request, "id", "");
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Rule ID is required");
        return;
    }

    RuleScriptManager& mgr = RuleScriptManager::getInstance();
    RuleScript* existing = mgr.getRule(id);
    if (!existing) {
        ctx->sendError(request, 404, "Rule not found");
        return;
    }

    RuleScript rule;
    rule.id = id;
    rule.name = ctx->getParamValue(request, "name", existing->name);
    rule.enabled = ctx->getParamBool(request, "enabled", existing->enabled);
    rule.triggerType = ctx->getParamInt(request, "triggerType", existing->triggerType);
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

void RuleScriptRouteHandler::handleDeleteRule(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String id = ctx->getParamValue(request, "id", "");
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Rule ID is required");
        return;
    }

    RuleScriptManager& mgr = RuleScriptManager::getInstance();
    if (mgr.removeRule(id)) {
        mgr.saveConfiguration();
        ctx->sendSuccess(request, "Rule deleted");
    } else {
        ctx->sendError(request, 404, "Rule not found");
    }
}

// ========== 启用规则 ==========

void RuleScriptRouteHandler::handleEnableRule(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String id = ctx->getParamValue(request, "id", "");
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Rule ID is required");
        return;
    }

    RuleScriptManager& mgr = RuleScriptManager::getInstance();
    if (mgr.enableRule(id)) {
        mgr.saveConfiguration();
        ctx->sendSuccess(request, "Rule enabled");
    } else {
        ctx->sendError(request, 404, "Rule not found");
    }
}

// ========== 禁用规则 ==========

void RuleScriptRouteHandler::handleDisableRule(AsyncWebServerRequest* request) {
    if (!ctx->checkPermission(request, "config.edit")) {
        ctx->sendUnauthorized(request);
        return;
    }

    String id = ctx->getParamValue(request, "id", "");
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Rule ID is required");
        return;
    }

    RuleScriptManager& mgr = RuleScriptManager::getInstance();
    if (mgr.disableRule(id)) {
        mgr.saveConfiguration();
        ctx->sendSuccess(request, "Rule disabled");
    } else {
        ctx->sendError(request, 404, "Rule not found");
    }
}
