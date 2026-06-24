#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_RULE_SCRIPT

#include "./network/handlers/RuleScriptRouteHandler.h"
#include "./network/handlers/HandlerUtils.h"
#include "./network/WebHandlerContext.h"
#include "core/RuleScriptManager.h"
#include "core/AsyncExecTypes.h"  // MutexGuard
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
    if (!ctx->requireAuth(request)) return;

    if (HandlerUtils::rejectHeavyRequestOnPressure(request, "Script rule list", MemoryGuardLevel::SEVERE, 8)) {
        return;
    }

    RuleScriptManager& mgr = RuleScriptManager::getInstance();

    // 检查堆内存
    if (HandlerUtils::checkLowMemory(request)) return;

    // 持锁迭代引用，避免全量 vector 拷贝；在持锁临界区内序列化为 items
    std::vector<String> items;
    {
        MutexGuard lock(mgr.getMutex());
        if (!lock.isLocked()) {
            HandlerUtils::sendJsonError(request, 503, "Service busy, try again");
            return;
        }
        const auto& ruleMap = mgr.getRulesRef();
        items.reserve(ruleMap.size());
        for (const auto& pair : ruleMap) {
            const auto& rule = pair.second;
            // 转义 name 中的双引号
            String safeName = rule.name;
            safeName.replace("\"", "\\\"");
            // scriptContent 可能较长，逐项拼接（在持锁内完成，脱锁后不再依赖 map 内部字符串）
            String safeScript = rule.scriptContent;
            safeScript.replace("\\", "\\\\");
            safeScript.replace("\"", "\\\"");
            safeScript.replace("\n", "\\n");
            safeScript.replace("\r", "\\r");
            safeScript.replace("\t", "\\t");

            String item;
            item.reserve(128 + safeScript.length());
            item += F("{\"id\":\"");
            item += rule.id;
            item += F("\",\"name\":\"");
            item += safeName;
            item += F("\",\"enabled\":");
            item += rule.enabled ? F("true") : F("false");
            item += F(",\"triggerType\":");
            item += String(rule.triggerType);
            item += F(",\"protocolType\":");
            item += String(rule.protocolType);
            item += F(",\"scriptContent\":\"");
            item += safeScript;
            item += F("\",\"lastTriggerTime\":");
            item += String(rule.lastTriggerTime);
            item += F(",\"triggerCount\":");
            item += String(rule.triggerCount);
            // 主题转换字段
            String safeSrcTopic = rule.sourceTopic;
            safeSrcTopic.replace("\"", "\\\"");
            String safeTgtTopic = rule.targetTopic;
            safeTgtTopic.replace("\"", "\\\"");
            item += F(",\"sourceTopic\":\"");
            item += safeSrcTopic;
            item += F("\",\"targetTopic\":\"");
            item += safeTgtTopic;
            item += F("\"");
            item += F("}");
            items.emplace_back(std::move(item));
        }
    }  // 释放锁

    // 流式输出，避免一次性 String 拼接所有 item
    String header = F("{\"success\":true,\"data\":[");
    if (!HandlerUtils::sendJsonListChunked(request, header, std::move(items))) {
        HandlerUtils::sendJsonError(request, 503, "Failed to create streaming response");
    }
}

// ========== 新增规则 ==========

void RuleScriptRouteHandler::handleAddRule(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    RuleScript rule;
    rule.id = ctx->getParamValue(request, "id", "");
    rule.name = ctx->getParamValue(request, "name", "");
    rule.enabled = ctx->getParamBool(request, "enabled", true);
    rule.triggerType = ctx->getParamInt(request, "triggerType", 0);
    rule.protocolType = ctx->getParamInt(request, "protocolType", 0);
    rule.scriptContent = ctx->getParamValue(request, "scriptContent", "");
    rule.sourceTopic = ctx->getParamValue(request, "sourceTopic", "");
    rule.targetTopic = ctx->getParamValue(request, "targetTopic", "");

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
    if (!ctx->requireAuth(request)) return;

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
    rule.sourceTopic = ctx->getParamValue(request, "sourceTopic", existing->sourceTopic);
    rule.targetTopic = ctx->getParamValue(request, "targetTopic", existing->targetTopic);

    if (mgr.updateRule(id, rule)) {
        mgr.saveConfiguration();
        ctx->sendSuccess(request, "Rule updated");
    } else {
        ctx->sendError(request, 500, "Failed to update rule");
    }
}

// ========== 删除规则 ==========

void RuleScriptRouteHandler::handleDeleteRule(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

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
    if (!ctx->requireAuth(request)) return;

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
    if (!ctx->requireAuth(request)) return;

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
#endif // FASTBEE_ENABLE_RULE_SCRIPT
