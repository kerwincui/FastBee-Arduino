#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_RULE_SCRIPT

#include "./network/handlers/RuleScriptRouteHandler.h"
#include "./network/handlers/HandlerUtils.h"
#include "./network/WebHandlerContext.h"
#include "core/RuleScriptManager.h"
#include "core/RuleScriptConfigValidation.h"
#include "core/RuntimeFeatureManager.h"
#include "core/AsyncExecTypes.h"  // MutexGuard
#include <ArduinoJson.h>

namespace {
bool requireRuleEngine(WebHandlerContext* ctx, AsyncWebServerRequest* request) {
    if (FastBee::RuntimeFeatureManager::instance().shouldEnable("ruleScript")) return true;
    ctx->sendError(request, 503, "Rule engine disabled by runtime feature config");
    return false;
}

bool requireRuleMutationMemory(AsyncWebServerRequest* request) {
    return !HandlerUtils::rejectHeavyRequestOnPressure(request, "Script change", MemoryGuardLevel::SEVERE, 8) &&
        !HandlerUtils::checkLowMemory(request, 32768);
}

bool readRuleScriptFormText(AsyncWebServerRequest* request, const char* key, const String& fallback,
                            String& output, size_t limit) {
    const String* value = &fallback;
    if (request->hasParam(key, true)) value = &request->getParam(key, true)->value();
    else if (request->hasParam(key)) value = &request->getParam(key)->value();
    return FastBee::copyCheckedText(output, value->c_str(), value->length(), limit);
}

bool readRuleScriptForm(AsyncWebServerRequest* request, const RuleScript& defaults, RuleScript& output) {
    String enabled, trigger, protocol;
    return readRuleScriptFormText(request, "id", defaults.id, output.id, 40) &&
        readRuleScriptFormText(request, "name", defaults.name, output.name, 128) &&
        readRuleScriptFormText(request, "scriptContent", defaults.scriptContent, output.scriptContent, 2048) &&
        readRuleScriptFormText(request, "sourceTopic", defaults.sourceTopic, output.sourceTopic, 256) &&
        readRuleScriptFormText(request, "targetTopic", defaults.targetTopic, output.targetTopic, 256) &&
        readRuleScriptFormText(request, "enabled", defaults.enabled ? "true" : "false", enabled, 5) &&
        readRuleScriptFormText(request, "triggerType", defaults.triggerType == 1 ? "1" : "0", trigger, 1) &&
        readRuleScriptFormText(request, "protocolType", defaults.protocolType == 1 ? "1" : "0", protocol, 1) &&
        FastBee::parseRuleScriptFormChoices(enabled, trigger, protocol, output);
}
}  // namespace

RuleScriptRouteHandler::RuleScriptRouteHandler(WebHandlerContext* ctx)
    : ctx(ctx) {
}

void RuleScriptRouteHandler::setupRoutes(AsyncWebServer* server) {
#if FASTBEE_ENABLE_CONFIG_TRANSFER
    server->on("/api/rule-script/import", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleImportRules(request); });
#endif
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

void RuleScriptRouteHandler::handleImportRules(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request) || !ctx->requireDeveloperMode(request)) return;
    if (!requireRuleEngine(ctx, request)) return;
    if (HandlerUtils::rejectHeavyRequestOnPressure(request, "Script import", MemoryGuardLevel::SEVERE, 8)) return;
    if (!request->hasParam("content", true)) { ctx->sendError(request, 400, "Missing script content"); return; }
    const String& content = request->getParam("content", true)->value();
    if (content.isEmpty() || content.length() > FastBee::RuleScriptImportMaxBytes ||
        std::memchr(content.c_str(), '\0', content.length())) {
        ctx->sendError(request, 413, "Script import must be nonempty text, at most 24KB"); return;
    }
    if (HandlerUtils::checkLowMemory(request, 32768)) return;
    JsonDocument doc;
    if (deserializeJson(doc, content.c_str(), content.length()) || doc.overflowed()) { ctx->sendError(request, 422, "Invalid script configuration JSON"); return; }
    String error;
    if (!RuleScriptManager::getInstance().importConfiguration(doc, error)) {
        ctx->sendError(request, 409, error.length() ? error.c_str() : "Script import not confirmed"); return;
    }
    JsonDocument result;
    result["name"] = "rule_scripts.json";
    result["size"] = content.length();
    result["ruleCount"] = doc["rules"].size();
    result["saved"] = true;
    result["applied"] = true;
    result["restartRequired"] = false;
    ctx->sendSuccess(request, result);
}

void RuleScriptRouteHandler::handleGetRules(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!requireRuleEngine(ctx, request)) return;

    if (HandlerUtils::rejectHeavyRequestOnPressure(request, "Script rule list", MemoryGuardLevel::SEVERE, 8)) {
        return;
    }

    RuleScriptManager& mgr = RuleScriptManager::getInstance();

    // 检查堆内存
    if (HandlerUtils::checkLowMemory(request)) return;

    // 持锁迭代引用，避免全量 vector 拷贝；在持锁临界区内序列化为 items
    std::vector<String> items;
    {
        RuleScriptManager::AccessGuard access(mgr);
        MutexGuard lock(access.mutex(), pdMS_TO_TICKS(50));
        if (!lock.isLocked()) {
            HandlerUtils::sendJsonError(request, 503, "Service busy, try again");
            return;
        }
        const auto& ruleMap = mgr.getRulesRef();
        items.reserve(ruleMap.size());
        for (const auto& pair : ruleMap) {
            const auto& rule = pair.second;
            JsonDocument row;
            row["id"] = rule.id;
            row["name"] = rule.name;
            row["enabled"] = rule.enabled;
            row["triggerType"] = rule.triggerType;
            row["protocolType"] = rule.protocolType;
            row["scriptContent"] = rule.scriptContent;
            row["lastTriggerTime"] = rule.lastTriggerTime;
            row["triggerCount"] = rule.triggerCount;
            row["sourceTopic"] = rule.sourceTopic;
            row["targetTopic"] = rule.targetTopic;
            String item;
            const size_t expected = measureJson(row);
            if (row.overflowed() || !item.reserve(expected) || serializeJson(row, item) != expected || item.length() != expected) {
                HandlerUtils::sendJsonError(request, 503, "Script list snapshot could not be serialized");
                return;
            }
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
    if (!requireRuleEngine(ctx, request)) return;
    if (!ctx->requireDeveloperMode(request)) return;
    if (!requireRuleMutationMemory(request)) return;

    RuleScript rule;
    if (!readRuleScriptForm(request, RuleScript(), rule)) {
        ctx->sendBadRequest(request, "Invalid or incomplete script form"); return;
    }

    if (rule.name.isEmpty()) {
        ctx->sendBadRequest(request, "Name is required");
        return;
    }

    RuleScriptManager& mgr = RuleScriptManager::getInstance();
    if (mgr.addRule(rule)) {
        ctx->sendSuccess(request, "Rule saved and added");
    } else {
        ctx->sendError(request, 409, "Rule change not confirmed; running rules retained. Reload before retry");
    }
}

// ========== 更新规则 ==========

void RuleScriptRouteHandler::handleUpdateRule(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!requireRuleEngine(ctx, request)) return;
    if (!ctx->requireDeveloperMode(request)) return;
    if (!requireRuleMutationMemory(request)) return;

    String id;
    if (!readRuleScriptFormText(request, "id", "", id, 40)) {
        ctx->sendBadRequest(request, "Invalid script identity"); return;
    }
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Rule ID is required");
        return;
    }

    RuleScriptManager& mgr = RuleScriptManager::getInstance();
    RuleScript snapshot;
    if (!mgr.copyRule(id, snapshot)) {
        ctx->sendError(request, 409, "Rule snapshot unavailable; reload before updating");
        return;
    }
    RuleScript rule;
    if (!readRuleScriptForm(request, snapshot, rule)) {
        ctx->sendBadRequest(request, "Invalid or incomplete script form"); return;
    }

    if (mgr.updateRule(id, rule, &snapshot)) {
        ctx->sendSuccess(request, "Rule saved and updated");
    } else {
        ctx->sendError(request, 409, "Rule change not confirmed; running rules retained. Reload before retry");
    }
}

// ========== 删除规则 ==========

void RuleScriptRouteHandler::handleDeleteRule(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!requireRuleEngine(ctx, request)) return;
    if (!ctx->requireDeveloperMode(request)) return;
    if (!requireRuleMutationMemory(request)) return;

    String id;
    if (!readRuleScriptFormText(request, "id", "", id, 40)) {
        ctx->sendBadRequest(request, "Invalid script identity"); return;
    }
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Rule ID is required");
        return;
    }

    RuleScriptManager& mgr = RuleScriptManager::getInstance();
    if (mgr.removeRule(id)) {
        ctx->sendSuccess(request, "Rule deletion saved");
    } else {
        ctx->sendError(request, 409, "Rule change not confirmed; running rules retained. Reload before retry");
    }
}

// ========== 启用规则 ==========

void RuleScriptRouteHandler::handleEnableRule(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!requireRuleEngine(ctx, request)) return;
    if (!ctx->requireDeveloperMode(request)) return;
    if (!requireRuleMutationMemory(request)) return;

    String id;
    if (!readRuleScriptFormText(request, "id", "", id, 40)) {
        ctx->sendBadRequest(request, "Invalid script identity"); return;
    }
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Rule ID is required");
        return;
    }

    RuleScriptManager& mgr = RuleScriptManager::getInstance();
    if (mgr.enableRule(id)) {
        ctx->sendSuccess(request, "Rule saved and enabled");
    } else {
        ctx->sendError(request, 409, "Rule change not confirmed; running rules retained. Reload before retry");
    }
}

// ========== 禁用规则 ==========

void RuleScriptRouteHandler::handleDisableRule(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!requireRuleEngine(ctx, request)) return;
    if (!ctx->requireDeveloperMode(request)) return;
    if (!requireRuleMutationMemory(request)) return;

    String id;
    if (!readRuleScriptFormText(request, "id", "", id, 40)) {
        ctx->sendBadRequest(request, "Invalid script identity"); return;
    }
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Rule ID is required");
        return;
    }

    RuleScriptManager& mgr = RuleScriptManager::getInstance();
    if (mgr.disableRule(id)) {
        ctx->sendSuccess(request, "Rule saved and disabled");
    } else {
        ctx->sendError(request, 409, "Rule change not confirmed; running rules retained. Reload before retry");
    }
}
#endif // FASTBEE_ENABLE_RULE_SCRIPT
