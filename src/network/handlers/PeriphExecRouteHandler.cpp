#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_PERIPH_EXEC

#include "./network/handlers/PeriphExecRouteHandler.h"
#include "./network/handlers/HandlerUtils.h"
#include "./network/WebHandlerContext.h"
#include "core/PeriphExecManager.h"
#include "core/PeripheralManager.h"
#include "core/ResourceProfile.h"
#include "core/AsyncExecTypes.h"  // MutexGuard
#if FASTBEE_ENABLE_MQTT
#include "protocols/MQTTClient.h"
#endif
#include "protocols/ProtocolManager.h"
#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <algorithm>

namespace {
String escapePeriphExecJsonString(const String& value) {
    String out;
    out.reserve(value.length() + 8);
    for (size_t i = 0; i < value.length(); ++i) {
        char c = value[i];
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<uint8_t>(c) < 0x20) {
                    char buf[7];
                    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<uint8_t>(c));
                    out += buf;
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

bool shouldDegradeControlsResponse() {
    return ESP.getFreeHeap() < 16384 || ESP.getMaxAllocHeap() < 8192;
}

void sendDegradedControlsResponse(AsyncWebServerRequest* request) {
    request->send(200, "application/json",
        F("{\"success\":true,\"degraded\":true,\"message\":\"Low memory - controls temporarily reduced\","
          "\"data\":{\"gpio\":[],\"modbus\":[],\"system\":[],\"script\":[],\"sensor\":[],\"other\":[]}}"));
}
}

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

    server->on("/api/periph-exec/results", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetRecentResults(request); });

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
    if (!ctx->requirePermission(request, "system.view")) return;

    if (HandlerUtils::rejectHeavyRequestOnPressure(request, "Rule list", MemoryGuardLevel::CRITICAL, 5)) {
        return;
    }

    PeriphExecManager& mgr = PeriphExecManager::getInstance();
    PeripheralManager& pm = PeripheralManager::getInstance();

    // ============ 单条查询：用 getRule() 直接拿指针，零拷贝 ============
    if (request->hasParam("id")) {
        String ruleId = request->getParam("id")->value();
        // getRule() 内部不持锁，但返回指向 map 内的指针；这里短暂持锁，序列化后立即释放
        MutexGuard lock(mgr.getRulesMutex());
        if (!lock.isLocked()) {
            HandlerUtils::sendJsonError(request, 503, "Service busy, try again");
            return;
        }
        const PeriphExecRule* rule = mgr.getRule(ruleId);
        if (!rule) {
            request->send(404, "application/json", "{\"success\":false,\"message\":\"Rule not found\"}");
            return;
        }
        String response = serializeRuleFull(*rule);
        // 锁在 RAII 释放后，再发送响应（响应可能涉及异步发送）
        // 这里 lock 仍在作用域内，但 send 是同步入队，不阻塞太久
        HandlerUtils::sendJsonSuccess(request, response);
        return;
    }

    // ============ 列表查询：仅指针排序 + 当前页序列化，避免全量深拷贝 ============
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

    // 检查堆内存是否充足（chunked流式发送内存占用很小，只需保证最低运行空间）
    if (HandlerUtils::checkLowMemory(request, 3072)) return;

    // degraded 模式：内存不足时限制每页条数，与 PeripheralRouteHandler 一致
    const bool degraded = (ESP.getFreeHeap() < 16384) || (ESP.getMaxAllocHeap() < 8192);
    int effectivePageSize = pageSize;
    if (degraded && effectivePageSize > 5) {
        effectivePageSize = 5;
    }

    // 持锁短临界区：构建指针 vector + 排序 + 序列化当前页到 items
    // 超时 2000ms：避免规则执行占用锁时无限等待导致分页请求卡死
    int total = 0;
    int startIdx = 0;
    int endIdx = 0;
    std::vector<String> items;
    std::vector<String> sensorSourceIds;
    String sensorSources;
    {
        MutexGuard lock(mgr.getRulesMutex(), pdMS_TO_TICKS(2000));
        if (!lock.isLocked()) {
            HandlerUtils::sendJsonError(request, 503, "Service busy (rules locked), retry in 2s", 2);
            return;
        }
        auto& ruleMap = mgr.getRules();
        // 仅指针 vector，每项 4-8 字节，46 条规则也只有 ~370 字节
        std::vector<const PeriphExecRule*> ptrs;
        ptrs.reserve(ruleMap.size());
        for (const auto& pair : ruleMap) {
            ptrs.push_back(&pair.second);
        }

        auto addSensorSource = [&](const ExecAction& action) {
            if (action.actionType != static_cast<uint8_t>(ExecActionType::ACTION_SENSOR_READ)) return;
            if (action.actionValue.isEmpty()) return;

            JsonDocument sDoc;
            DeserializationError sErr = deserializeJson(sDoc, action.actionValue);
            if (sErr) return;

            String periphId = sDoc["periphId"].as<String>();
            if (periphId.isEmpty()) periphId = action.targetPeriphId;
            if (periphId.isEmpty()) return;

            String dataField = sDoc["dataField"].as<String>();
            if (dataField.isEmpty()) dataField = "temperature";
            String sourceId = periphId + "_" + dataField;
            for (const auto& existingId : sensorSourceIds) {
                if (existingId == sourceId) return;
            }
            sensorSourceIds.push_back(sourceId);

            String sensorLabel = sDoc["sensorLabel"].as<String>();
            if (sensorLabel.isEmpty()) {
                if (dataField == "humidity") sensorLabel = "湿度";
                else if (dataField == "voltage") sensorLabel = "电压";
                else if (dataField == "value") sensorLabel = "数值";
                else sensorLabel = "温度";
            }
            String unit = sDoc["unit"].as<String>();

            String periphName = periphId;
            const PeripheralConfig* periph = pm.getPeripheral(periphId);
            if (periph && !periph->name.isEmpty()) periphName = periph->name;

            String displayLabel = periphName + " - " + sensorLabel;
            if (!sensorSources.isEmpty()) sensorSources += ",";
            sensorSources += "{\"id\":\"" + escapePeriphExecJsonString(sourceId) + "\"";
            sensorSources += ",\"label\":\"" + escapePeriphExecJsonString(displayLabel) + "\"";
            sensorSources += ",\"periphId\":\"" + escapePeriphExecJsonString(periphId) + "\"";
            sensorSources += ",\"field\":\"" + escapePeriphExecJsonString(dataField) + "\"";
            if (!unit.isEmpty()) sensorSources += ",\"unit\":\"" + escapePeriphExecJsonString(unit) + "\"";
            sensorSources += "}";
        };

        for (const auto* rule : ptrs) {
            if (!rule) continue;
            for (const auto& action : rule->actions) {
                addSensorSource(action);
            }
        }

        // 排序：启用的排前面，然后按名称排序
        std::sort(ptrs.begin(), ptrs.end(), [](const PeriphExecRule* a, const PeriphExecRule* b) {
            if (a->enabled != b->enabled) return a->enabled > b->enabled;
            return strcmp(a->name.c_str(), b->name.c_str()) < 0;
        });

        total = static_cast<int>(ptrs.size());
        startIdx = (page - 1) * effectivePageSize;
        endIdx = (startIdx < total) ? min(startIdx + effectivePageSize, total) : startIdx;

        items.reserve(endIdx - startIdx);
        for (int i = startIdx; i < endIdx; i++) {
            const auto& rule = *ptrs[i];

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

            // 构建流程摘要: 触发 → 动作 → 上报
            String flowSummary;
            flowSummary.reserve(80);
            if (triggerCount > 0) {
                const auto& t = rule.triggers[0];
                switch (t.triggerType) {
                    case 0: flowSummary = (t.operatorType == 1) ? F("MQTT设值") : F("MQTT指令"); break;
                    case 1:
                        if (t.timerMode == 1 && !t.timePoint.isEmpty()) {
                            flowSummary = F("每日"); flowSummary += t.timePoint;
                        } else {
                            flowSummary = F("每"); flowSummary += String(t.intervalSec); flowSummary += F("秒");
                        }
                        break;
                    case 4: flowSummary = F("事件:"); flowSummary += t.eventId; break;
                    case 5: flowSummary = F("轮询采集"); break;
                    case 6: flowSummary = F("规则链"); break;
                    default: flowSummary = F("触发"); break;
                }
                if (triggerCount > 1) { flowSummary += F("…"); }
            }
            flowSummary += F(" → ");
            if (actionCount > 0) {
                const auto& a = rule.actions[0];
                switch (a.actionType) {
                    case 0: flowSummary += F("设高电平"); break;
                    case 1: flowSummary += F("设低电平"); break;
                    case 2: flowSummary += F("闪烁"); break;
                    case 3: flowSummary += F("呼吸灯"); break;
                    case 4: flowSummary += F("PWM"); break;
                    case 5: flowSummary += F("DAC"); break;
                    case 6: flowSummary += F("重启"); break;
                    case 10: flowSummary += F("调用"); break;
                    case 13: flowSummary += F("高反转"); break;
                    case 14: flowSummary += F("低反转"); break;
                    case 15: flowSummary += F("脚本"); break;
                    case 16: flowSummary += F("写线圈"); break;
                    case 17: flowSummary += F("写寄存器"); break;
                    case 18: flowSummary += F("Modbus采集"); break;
                    case 19: flowSummary += F("读传感器"); break;
                    case 21: flowSummary += F("发事件"); break;
                    case 22: flowSummary += F("启用规则"); break;
                    case 23: flowSummary += F("禁用规则"); break;
                    case 24: flowSummary += F("显数字"); break;
                    case 25: flowSummary += F("显文本"); break;
                    case 26: flowSummary += F("清数码管"); break;
                    case 27: flowSummary += F("OLED显示"); break;
                    default: flowSummary += F("动作"); break;
                }
                if (!targetPeriphName.isEmpty()) { flowSummary += F(" → "); flowSummary += targetPeriphName; }
                if (actionCount > 1) { flowSummary += F("…"); }
            }
            if (rule.reportAfterExec) { flowSummary += F(" → 上报"); }

            // 条件分支（平台触发有比较时）
            if (triggerCount > 0 && rule.triggers[0].triggerType == 0 && rule.triggers[0].operatorType != 1 && !rule.triggers[0].compareValue.isEmpty()) {
                // 在触发和动作之间插入条件
                String condPart = F("若>"); condPart += rule.triggers[0].compareValue;
                // 替换第一个 " → " 为 " → 若X → "
                int arrowPos = flowSummary.indexOf(F(" → "));
                if (arrowPos >= 0) {
                    flowSummary = flowSummary.substring(0, arrowPos + 4) + condPart + F(" → ") + flowSummary.substring(arrowPos + 4);
                }
            }

            String item;
            item.reserve(200 + rule.id.length() + rule.name.length() + targetPeriphName.length() + flowSummary.length());
            item = F("{\"id\":\"");
            item += escapePeriphExecJsonString(rule.id);
            item += F("\",\"name\":\"");
            item += escapePeriphExecJsonString(rule.name);
            item += F("\",\"enabled\":");
            item += rule.enabled ? F("true") : F("false");
            item += F(",\"execMode\":");
            item += String(rule.execMode);
            item += F(",\"reportAfterExec\":");
            item += rule.reportAfterExec ? F("true") : F("false");
            item += F(",\"triggerCount\":");
            item += String(triggerCount);
            item += F(",\"actionCount\":");
            item += String(actionCount);
            item += F(",\"triggerSummary\":");
            item += String(triggerSummary);
            item += F(",\"actionSummary\":");
            item += String(actionSummary);
            item += F(",\"hasSetMode\":");
            item += hasSetMode ? F("true") : F("false");
            item += F(",\"targetPeriphName\":\"");
            item += escapePeriphExecJsonString(targetPeriphName);
            item += F("\",\"flowSummary\":\"");
            item += escapePeriphExecJsonString(flowSummary);
            item += F("\"}");
            items.emplace_back(std::move(item));
        }
    }  // 释放 _rulesMutex

    // 处理空页边界（避免 items 为空时调用 sendJsonListChunked）
    // 依然需要返回有效的分页响应结构

    // 复用 HandlerUtils::sendJsonListChunked（与 handleGetPeripherals 同模式）
    String header;
    const int profileMax = static_cast<int>(FastBee::ResourceProfile::MAX_PERIPH_EXEC_RULES);
    const int profileRemaining = std::max(0, profileMax - total);

    header.reserve(220 + sensorSources.length());
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
    header += String(total);
    header += F(",\"remaining\":");
    header += String(profileRemaining);
    header += F("}");
    header += F(",\"sensorSources\":[");
    header += sensorSources;
    header += F("]");
    header += F(",\"data\":[");

    if (!HandlerUtils::sendJsonListChunked(request, header, std::move(items))) {
        HandlerUtils::sendJsonError(request, 503, "Failed to create streaming response");
    }
}

// ========== 新增规则（form-encoded 向后兼容） ==========

void PeriphExecRouteHandler::handleAddRule(AsyncWebServerRequest* request) {
    if (!ctx->requirePermission(request, "config.edit")) return;
    if (!ctx->requireDeveloperMode(request)) return;

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
    String errorMsg;
    if (mgr.addRule(rule, errorMsg)) {
        mgr.saveConfiguration();
        ctx->sendSuccess(request, "Rule added");
    } else {
        ctx->sendError(request, 400, errorMsg.isEmpty() ? "Failed to add rule" : errorMsg);
    }
}

// ========== 更新规则 ==========

void PeriphExecRouteHandler::handleUpdateRule(AsyncWebServerRequest* request) {
    if (!ctx->requirePermission(request, "config.edit")) return;
    if (!ctx->requireDeveloperMode(request)) return;

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

    String errorMsg;
    if (mgr.updateRule(id, rule, errorMsg)) {
        mgr.saveConfiguration();
        ctx->sendSuccess(request, "Rule updated");
    } else {
        ctx->sendError(request, 400, errorMsg.isEmpty() ? "Failed to update rule" : errorMsg);
    }
}

// ========== 删除规则 ==========

void PeriphExecRouteHandler::handleDeleteRule(AsyncWebServerRequest* request) {
    if (!ctx->requirePermission(request, "config.edit")) return;
    if (!ctx->requireDeveloperMode(request)) return;

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
    if (!ctx->requirePermission(request, "config.edit")) return;
    if (!ctx->requireDeveloperMode(request)) return;

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
    if (!ctx->requirePermission(request, "config.edit")) return;
    if (!ctx->requireDeveloperMode(request)) return;

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
    if (!ctx->requirePermission(request, "config.edit")) return;

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

void PeriphExecRouteHandler::handleGetRecentResults(AsyncWebServerRequest* request) {
    if (!ctx->requirePermission(request, "system.view")) return;

    int limit = 10;
    if (request->hasParam("limit")) {
        limit = request->getParam("limit")->value().toInt();
        if (limit < 1) limit = 1;
        if (limit > 20) limit = 20;
    }

    auto results = PeriphExecManager::getInstance().getRecentResults();
    int total = static_cast<int>(results.size());
    int emitted = 0;
    String items;
    items.reserve(static_cast<size_t>(limit) * 140);

    for (int i = total - 1; i >= 0 && emitted < limit; --i) {
        const auto& result = results[static_cast<size_t>(i)];
        if (emitted > 0) items += ",";

        const char* statusName = "unknown";
        switch (result.status) {
            case AsyncExecStatus::PENDING: statusName = "pending"; break;
            case AsyncExecStatus::RUNNING: statusName = "running"; break;
            case AsyncExecStatus::COMPLETED: statusName = "completed"; break;
            case AsyncExecStatus::FAILED: statusName = "failed"; break;
        }

        unsigned long duration = 0;
        if (result.endTime >= result.startTime) {
            duration = result.endTime - result.startTime;
        }

        items += F("{\"ruleId\":\"");
        items += escapePeriphExecJsonString(result.ruleId);
        items += F("\",\"ruleName\":\"");
        items += escapePeriphExecJsonString(result.ruleName);
        items += F("\",\"status\":");
        items += String(static_cast<int>(result.status));
        items += F(",\"statusName\":\"");
        items += statusName;
        items += F("\",\"startTime\":");
        items += String(result.startTime);
        items += F(",\"endTime\":");
        items += String(result.endTime);
        items += F(",\"durationMs\":");
        items += String(duration);
        items += F("}");
        emitted++;
    }

    String response;
    response.reserve(80 + items.length());
    response += F("{\"success\":true,\"total\":");
    response += String(total);
    response += F(",\"data\":[");
    response += items;
    response += F("]}");
    request->send(200, "application/json", response);
}

// ========== 获取静态事件列表 ==========

void PeriphExecRouteHandler::handleGetStaticEvents(AsyncWebServerRequest* request) {
    if (!ctx->requirePermission(request, "system.view")) return;

    String eventsJson = PeriphExecManager::getStaticEventsJson();
    HandlerUtils::sendJsonSuccess(request, eventsJson);
}

// ========== 获取动态事件列表（包含外设执行规则事件） ==========

void PeriphExecRouteHandler::handleGetDynamicEvents(AsyncWebServerRequest* request) {
    if (!ctx->requirePermission(request, "system.view")) return;

    if (HandlerUtils::rejectHeavyRequestOnPressure(request, "Dynamic event list", MemoryGuardLevel::CRITICAL, 5)) {
        return;
    }
    if (HandlerUtils::checkLowMemory(request, 6144)) return;

    PeriphExecManager& mgr = PeriphExecManager::getInstance();
    String eventsJson = mgr.getDynamicEventsJson();
    if (eventsJson.isEmpty()) {
        AsyncWebServerResponse* response = request->beginResponse(200, "application/json",
            F("{\"success\":true,\"degraded\":true,\"message\":\"Low memory - dynamic events temporarily reduced\",\"data\":[]}"));
        HandlerUtils::sendWithClose(request, response);
        return;
    }
    HandlerUtils::sendJsonSuccess(request, eventsJson);
}

// ========== 获取事件分类列表 ==========

void PeriphExecRouteHandler::handleGetEventCategories(AsyncWebServerRequest* request) {
    if (!ctx->requirePermission(request, "system.view")) return;

    String categoriesJson = PeriphExecManager::getEventCategoriesJson();
    HandlerUtils::sendJsonSuccess(request, categoriesJson);
}

// ========== 获取触发类型列表 ==========

void PeriphExecRouteHandler::handleGetTriggerTypes(AsyncWebServerRequest* request) {
    if (!ctx->requirePermission(request, "system.view")) return;

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
    if (!ctx->requirePermission(request, "config.edit")) return;
    if (!ctx->requireDeveloperMode(request)) return;

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
    String errorMsg;
    if (mgr.addRule(rule, errorMsg)) {
        mgr.saveConfiguration();
        ctx->sendSuccess(request, "Rule added");
    } else {
        ctx->sendError(request, 400, errorMsg.isEmpty() ? "Failed to add rule" : errorMsg);
    }
}

// ========== 获取设备控制列表（按动作类型分组） ==========

void PeriphExecRouteHandler::handleGetControls(AsyncWebServerRequest* request) {
    if (!ctx->requirePermission(request, "system.view")) return;

    if (shouldDegradeControlsResponse()) {
        sendDegradedControlsResponse(request);
        return;
    }

    PeriphExecManager& mgr = PeriphExecManager::getInstance();
    PeripheralManager& pm = PeripheralManager::getInstance();

    // 持锁上下文：避免全量拷贝 vector，直接迭代 RuleMap 引用
    MutexGuard lock(mgr.getRulesMutex(), pdMS_TO_TICKS(50));
    if (!lock.isLocked()) {
        sendDegradedControlsResponse(request);
        return;
    }
    auto& ruleMap = mgr.getRules();

    // 分组缓冲：记录每个分组是否已写入第一条
    bool firstGpio = true, firstModbus = true, firstSystem = true;
    bool firstScript = true, firstSensor = true, firstOther = true;

    // 临时缓冲各分组的内容
    String gpioItems, modbusItems, systemItems, scriptItems, sensorItems, otherItems;

    for (const auto& pair : ruleMap) {
        const auto& rule = pair.second;
        if (!rule.enabled) continue;
        if (rule.actions.empty()) continue;

        const ExecAction& firstAction = rule.actions[0];
        int at = firstAction.actionType;
#if !FASTBEE_ENABLE_OTA
        if (at == static_cast<int>(ExecActionType::ACTION_SYS_OTA)) continue;
#endif
#if !FASTBEE_ENABLE_COMMAND_SCRIPT
        if (at == static_cast<int>(ExecActionType::ACTION_SCRIPT)) continue;
#endif

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

        // 传感器读取：附加最近读取值、标签、单位
        if (at == 19) {
            const auto sensorCache = mgr.getSensorReadCacheCopy();
            // 遍历该规则所有 action，收集传感器缓存数据
            item += ",\"sensors\":[";
            bool firstSensorItem = true;
            for (const auto& action : rule.actions) {
                if (action.actionType != 19) continue;
                if (action.actionValue.isEmpty()) continue;
                JsonDocument sDoc;
                DeserializationError sErr = deserializeJson(sDoc, action.actionValue);
                if (sErr) continue;
                const char* sPeriphId = sDoc["periphId"] | "";
                const char* sDataField = sDoc["dataField"] | "temperature";
                const char* sLabel = sDoc["sensorLabel"] | "";
                const char* sUnit = sDoc["unit"] | "";
                String cacheKey = String(sPeriphId) + "_" + String(sDataField);
                if (!firstSensorItem) item += ",";
                firstSensorItem = false;
                item += "{\"key\":\"" + cacheKey + "\"";
                item += ",\"label\":\"" + String(sLabel) + "\"";
                item += ",\"unit\":\"" + String(sUnit) + "\"";
                auto cacheIt = sensorCache.find(cacheKey);
                if (cacheIt != sensorCache.end()) {
                    item += ",\"value\":\"" + cacheIt->second.value + "\"";
                }
                item += "}";
            }
            item += "]";
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
    if (!ctx->requirePermission(request, "config.edit")) return;
    if (!ctx->requireDeveloperMode(request)) return;

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

    String errorMsg;
    if (mgr.updateRule(id, rule, errorMsg)) {
        mgr.saveConfiguration();
        ctx->sendSuccess(request, "Rule updated");
    } else {
        ctx->sendError(request, 400, errorMsg.isEmpty() ? "Failed to update rule" : errorMsg);
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
