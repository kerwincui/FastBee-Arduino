void writeRuleCandidate(JsonObject target, const PeriphExecRule& rule) {
    target["id"] = rule.id;
    target["name"] = rule.name;
    target["enabled"] = rule.enabled;
    target["execMode"] = rule.execMode;
    target["protocolType"] = rule.protocolType;
    target["scriptContent"] = rule.scriptContent;
    target["reportAfterExec"] = rule.reportAfterExec;
    target["allowPanelControl"] = rule.allowPanelControl;
    target["recordHistory"] = rule.recordHistory;

    JsonArray triggers = target["triggers"].to<JsonArray>();
    for (const ExecTrigger& trigger : rule.triggers) {
        JsonObject item = triggers.add<JsonObject>();
        item["triggerType"] = trigger.triggerType;
        item["triggerPeriphId"] = trigger.triggerPeriphId;
        item["operatorType"] = trigger.operatorType;
        item["compareValue"] = trigger.compareValue;
        item["timerMode"] = trigger.timerMode;
        item["intervalSec"] = trigger.intervalSec;
        item["timePoint"] = trigger.timePoint;
        item["eventId"] = trigger.eventId;
        item["pollResponseTimeout"] = trigger.pollResponseTimeout;
        item["pollMaxRetries"] = trigger.pollMaxRetries;
        item["pollInterPollDelay"] = trigger.pollInterPollDelay;
    }

    JsonArray actions = target["actions"].to<JsonArray>();
    for (const ExecAction& action : rule.actions) {
        JsonObject item = actions.add<JsonObject>();
        item["targetPeriphId"] = action.targetPeriphId;
        item["actionType"] = action.actionType;
        item["actionValue"] = action.actionValue;
        item["useReceivedValue"] = action.useReceivedValue;
        item["syncDelayMs"] = action.syncDelayMs;
        item["execMode"] = action.execMode;
    }
}

void PeriphExecRouteHandler::handlePreflightRuleJson(AsyncWebServerRequest* request,
                                                     JsonVariant& json) {
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

    JsonObject payload = json.as<JsonObject>();
    if (payload.isNull()) {
        ctx->sendBadRequest(request, "Rule preflight body must be a JSON object");
        return;
    }

    String operation = payload["operation"] | "add";
    operation.toLowerCase();
    if (operation != "add" && operation != "update") {
        ctx->sendBadRequest(request, "Invalid operation; expected add or update");
        return;
    }

    JsonObject ruleObject = payload["rule"].is<JsonObject>()
        ? payload["rule"].as<JsonObject>() : payload;
    PeriphExecManager& manager = PeriphExecManager::getInstance();
    PeriphExecRule current;
    PeriphExecRule candidate;
    String id = ruleObject["id"] | "";

    if (operation == "update") {
        if (id.isEmpty()) {
            ctx->sendBadRequest(request, "Rule ID is required for update preflight");
            return;
        }
        if (!manager.getRuleSnapshot(id, current)) {
            ctx->sendError(request, 404, "Rule not found");
            return;
        }
        candidate = current;
    } else if (!id.isEmpty() && manager.getRuleSnapshot(id, current)) {
        ctx->sendError(request, 409, "Rule ID already exists");
        return;
    }

    parseRuleFromJson(ruleObject, candidate);
    if (!id.isEmpty()) candidate.id = id;

    JsonDocument response;
    response["success"] = true;
    JsonObject data = response["data"].to<JsonObject>();
    data["operation"] = operation;
    JsonArray issues = data["issues"].to<JsonArray>();
    size_t errorCount = 0;
    size_t warningCount = 0;
    uint8_t risk = 0;  // 0=low, 1=medium, 2=high
    auto addIssue = [&](const char* code, const char* severity,
                        const String& path, const String& message,
                        uint8_t issueRisk = 0) {
        JsonObject issue = issues.add<JsonObject>();
        issue["code"] = code;
        issue["severity"] = severity;
        issue["path"] = path;
        issue["message"] = message;
        if (strcmp(severity, "error") == 0) ++errorCount;
        else ++warningCount;
        if (issueRisk > risk) risk = issueRisk;
    };

    if (operation == "add" &&
        manager.getRuleCount() >= FastBee::ResourceProfile::MAX_PERIPH_EXEC_RULES) {
        addIssue("RULE_LIMIT_REACHED", "error", "rule",
                 String("Rule limit reached for ") + FastBee::ResourceProfile::NAME);
    }
    if (candidate.id.length() > FastBee::ResourceProfile::MAX_RULE_ID_LEN) {
        addIssue("RULE_ID_TOO_LONG", "error", "rule.id", "Rule ID exceeds profile limit");
    }
    if (candidate.name.isEmpty()) {
        addIssue("RULE_NAME_REQUIRED", "error", "rule.name", "Rule name is required");
    } else if (candidate.name.length() > FastBee::ResourceProfile::MAX_RULE_NAME_LEN) {
        addIssue("RULE_NAME_TOO_LONG", "error", "rule.name", "Rule name exceeds profile limit");
    }
    if (candidate.execMode > 1) {
        addIssue("INVALID_EXEC_MODE", "error", "rule.execMode", "execMode must be 0 or 1");
    }
    if (candidate.protocolType != 0 && candidate.protocolType != 1 &&
        candidate.protocolType != 3 && candidate.protocolType != 4 &&
        candidate.protocolType != 5) {
        addIssue("INVALID_PROTOCOL_TYPE", "error", "rule.protocolType",
                 "Unsupported protocolType");
    }
    if (candidate.scriptContent.length() > FastBee::ResourceProfile::MAX_SCRIPT_CONTENT_LEN) {
        addIssue("SCRIPT_TOO_LONG", "error", "rule.scriptContent",
                 "Rule scriptContent exceeds profile limit");
    }

    if (candidate.triggers.empty()) {
        addIssue("TRIGGER_REQUIRED", "error", "rule.triggers",
                 "At least one trigger is required");
    } else if (candidate.triggers.size() > MAX_TRIGGERS_PER_RULE) {
        addIssue("TOO_MANY_TRIGGERS", "error", "rule.triggers",
                 String("Maximum triggers per rule: ") + MAX_TRIGGERS_PER_RULE);
    }

    PeripheralManager& peripherals = PeripheralManager::getInstance();
    for (size_t index = 0; index < candidate.triggers.size(); ++index) {
        const ExecTrigger& trigger = candidate.triggers[index];
        const String path = String("rule.triggers[") + index + "]";
        const bool supportedType = trigger.triggerType == 0 || trigger.triggerType == 1 ||
                                   trigger.triggerType == 4 || trigger.triggerType == 5;
        if (!supportedType) {
            addIssue("INVALID_TRIGGER_TYPE", "error", path + ".triggerType",
                     trigger.triggerType == 6
                         ? "Rule-chain trigger type 6 is not implemented by the runtime"
                         : "Supported trigger types are 0, 1, 4 and 5");
            continue;
        }
        if ((trigger.triggerType == 0 || trigger.triggerType == 5) &&
            trigger.operatorType > static_cast<uint8_t>(ExecOperator::NOT_CONTAIN)) {
            addIssue("INVALID_OPERATOR", "error", path + ".operatorType",
                     "operatorType must be between 0 and 9");
        }
        if (trigger.triggerType == 0 && trigger.triggerPeriphId.isEmpty()) {
            addIssue("TRIGGER_SOURCE_REQUIRED", "error", path + ".triggerPeriphId",
                     "Platform trigger requires a source identifier");
        }
        if (trigger.triggerType == 1) {
            if (trigger.timerMode > 1) {
                addIssue("INVALID_TIMER_MODE", "error", path + ".timerMode",
                         "timerMode must be interval(0) or daily(1)");
            } else if (trigger.timerMode == 0 &&
                       (trigger.intervalSec < PERIPH_EXEC_TIMER_INTERVAL_MIN_SEC ||
                        trigger.intervalSec > PERIPH_EXEC_INTERVAL_MAX_SEC)) {
                addIssue("INVALID_TIMER_INTERVAL", "error", path + ".intervalSec",
                         "Timer interval must be between 1 and 86400 seconds");
            } else if (trigger.timerMode == 1 && !parseClockTime(trigger.timePoint)) {
                addIssue("INVALID_DAILY_TIME", "error", path + ".timePoint",
                         "Daily time must use HH:MM (00:00-23:59)");
            }
        }
        if (trigger.triggerType == 4) {
            if (trigger.eventId.isEmpty()) {
                addIssue("EVENT_ID_REQUIRED", "error", path + ".eventId",
                         "Event trigger requires eventId");
            } else if (!findStaticEvent(trigger.eventId.c_str())) {
                PeripheralConfig eventSource;
                if (!peripherals.getPeripheralSnapshot(trigger.eventId, eventSource) ||
                    eventSource.type != PeripheralType::DEVICE_EVENT) {
                    addIssue("UNVERIFIED_EVENT_ID", "warning", path + ".eventId",
                             "Event is not a built-in or configured DEVICE_EVENT source", 1);
                }
            }
        }
        if (trigger.triggerType == 5) {
            if (trigger.intervalSec < PERIPH_EXEC_POLL_INTERVAL_MIN_SEC ||
                trigger.intervalSec > PERIPH_EXEC_INTERVAL_MAX_SEC) {
                addIssue("INVALID_POLL_INTERVAL", "error", path + ".intervalSec",
                         "Poll interval must be between 5 and 86400 seconds");
            }
            if (trigger.pollResponseTimeout < 100 || trigger.pollResponseTimeout > 5000) {
                addIssue("INVALID_POLL_TIMEOUT", "error", path + ".pollResponseTimeout",
                         "Poll timeout must be between 100 and 5000 ms");
            }
            if (trigger.pollMaxRetries > 3) {
                addIssue("INVALID_POLL_RETRIES", "error", path + ".pollMaxRetries",
                         "Poll retries must be between 0 and 3");
            }
            if (trigger.pollInterPollDelay < 20 || trigger.pollInterPollDelay > 1000) {
                addIssue("INVALID_POLL_DELAY", "error", path + ".pollInterPollDelay",
                         "Inter-poll delay must be between 20 and 1000 ms");
            }
        }
    }

    if (candidate.actions.empty()) {
        addIssue("ACTION_REQUIRED", "error", "rule.actions",
                 "At least one action is required");
    } else if (candidate.actions.size() > MAX_ACTIONS_PER_RULE) {
        addIssue("TOO_MANY_ACTIONS", "error", "rule.actions",
                 String("Maximum actions per rule: ") + MAX_ACTIONS_PER_RULE);
    }

    for (size_t index = 0; index < candidate.actions.size(); ++index) {
        const ExecAction& action = candidate.actions[index];
        const String path = String("rule.actions[") + index + "]";
        if (action.actionType > 39 || action.actionType == 20) {
            addIssue("INVALID_ACTION_TYPE", "error", path + ".actionType",
                     "actionType must be 0-39 and cannot use reserved slot 20");
            continue;
        }
        if (action.execMode > 1) {
            addIssue("INVALID_ACTION_EXEC_MODE", "error", path + ".execMode",
                     "Action execMode must be 0 or 1");
        }
        if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_SD_STATUS)) {
            if (const char* error = FastBee::validateSdStatusAction(action))
                addIssue("INVALID_SD_STATUS_ACTION", "error", path, error);
            if (candidate.allowPanelControl)
                addIssue("PANEL_ACTION_NOT_ALLOWED", "error", path, "SD status is not a panel-control action");
        }
        if (FastBee::isRtcAction(action.actionType)) {
            if (const char* error = FastBee::validateRtcAction(action)) {
                addIssue("INVALID_RTC_ACTION", "error", path, error);
            }
            if (candidate.allowPanelControl) {
                addIssue("PANEL_ACTION_NOT_ALLOWED", "error", path,
                         "RTC actions are not panel-control actions");
            }
        }
        if (action.syncDelayMs > 10000) {
            addIssue("ACTION_DELAY_TOO_LONG", "error", path + ".syncDelayMs",
                     "Action delay cannot exceed 10000 ms");
        }
        if (action.actionValue.length() > FastBee::ResourceProfile::MAX_ACTION_VALUE_LEN) {
            addIssue("ACTION_VALUE_TOO_LONG", "error", path + ".actionValue",
                     "Action value exceeds profile limit");
        }

        if (action.actionType == 6 || action.actionType == 7 || action.actionType == 9) {
            addIssue("DANGEROUS_SYSTEM_ACTION", "warning", path,
                     action.actionType == 7 ? "Factory reset requires explicit confirmation"
                                            : "Restart or OTA requires explicit confirmation",
                     2);
        }
#if !FASTBEE_ENABLE_OTA
        if (action.actionType == 9) {
            addIssue("OTA_NOT_COMPILED", "error", path + ".actionType",
                     "OTA action is not compiled into this firmware");
        }
#endif
        if (action.actionType == 15 &&
            !FastBee::RuntimeFeatureManager::instance().shouldEnable("commandScript")) {
            addIssue("SCRIPT_FEATURE_DISABLED", "error", path + ".actionType",
                     "Command script feature is disabled");
        }
        if ((action.actionType == 16 || action.actionType == 17 || action.actionType == 18) &&
            !FastBee::RuntimeFeatureManager::instance().shouldEnable("modbus")) {
            addIssue("MODBUS_FEATURE_DISABLED", "error", path + ".actionType",
                     "Modbus feature is disabled");
        }
#if !FASTBEE_ENABLE_REMOTE_RESOURCES
        if (action.actionType == 34) {
            addIssue("REMOTE_RESOURCES_NOT_COMPILED", "error", path + ".actionType",
                     "Remote resources are not compiled into this firmware");
        }
#endif
        if (action.actionType == 33) {
            String remoteErrorCode;
            String remoteErrorMessage;
            if (!validateRemoteChannelRuleAction(action, remoteErrorCode,
                                                 remoteErrorMessage)) {
                const bool targetIssue = remoteErrorCode.startsWith("REMOTE_TARGET") ||
                                         remoteErrorCode == "REMOTE_CHANNEL_TYPE_UNSUPPORTED";
                addIssue(remoteErrorCode.c_str(), "error",
                         path + (targetIssue ? ".targetPeriphId" : ".actionValue"),
                         remoteErrorMessage);
            }
        }
        if (action.actionType == 34 &&
            (action.targetPeriphId.isEmpty() || !action.actionValue.isEmpty() ||
             action.useReceivedValue)) {
            addIssue("INVALID_REMOTE_READ", "error", path,
                     "Remote read requires a source ID and no action value");
        }

        if (actionNeedsPeripheral(action.actionType)) {
            if (action.targetPeriphId.isEmpty()) {
                addIssue("ACTION_TARGET_REQUIRED", "error", path + ".targetPeriphId",
                         "This action requires a target peripheral");
            } else {
                PeripheralConfig target;
                if (!peripherals.getPeripheralSnapshot(action.targetPeriphId, target)) {
                    addIssue("ACTION_TARGET_NOT_FOUND", "error", path + ".targetPeriphId",
                             "Target peripheral does not exist");
                } else {
                    if (!actionTargetMatches(action.actionType, target)) {
                        addIssue("ACTION_TARGET_TYPE_MISMATCH", "error", path + ".targetPeriphId",
                                 String("Action is incompatible with ") + getPeripheralTypeName(target.type));
                    }
                    if (!target.enabled) {
                        addIssue("ACTION_TARGET_DISABLED", "warning", path + ".targetPeriphId",
                                 "Target peripheral is currently disabled", 1);
                    }
                    if (target.type == PeripheralType::UART && (action.actionType == 10 || action.actionType == 29)) {
                        if (const char* error = FastBee::validateUartAction(action))
                            addIssue(error, "error", path + ".actionValue", "Invalid UART payload; maximum 256 expanded bytes");
                        addIssue("UART_LOCAL_ONLY", "warning", path,
                                 "One local write only; peer receipt is unverified. Partial writes must not be blindly retried", 1);
                    }
                }
            }
        }

        if ((action.actionType == 22 || action.actionType == 23)) {
            PeriphExecRule targetRule;
            if (action.targetPeriphId.isEmpty() ||
                !manager.getRuleSnapshot(action.targetPeriphId, targetRule)) {
                addIssue("TARGET_RULE_NOT_FOUND", "error", path + ".targetPeriphId",
                         "Rule-control action requires an existing target rule");
            }
        }
        if (action.actionType == 21 && action.targetPeriphId.isEmpty()) {
            addIssue("EVENT_TARGET_REQUIRED", "error", path + ".targetPeriphId",
                     "Trigger-event action requires an event ID");
        }
        if (action.actionType == 31 || action.actionType == 32) {
            if (action.actionValue != "webServer" && action.actionValue != "mdns") {
                addIssue("FEATURE_NOT_ALLOWED", "error", path + ".actionValue",
                         "Feature actions may only target webServer or mdns");
            }
            if (risk < 1) risk = 1;
        }

        long numeric = 0;
        if (!action.useReceivedValue && (action.actionType == 2 || action.actionType == 3)) {
            if (!parseStrictLong(action.actionValue, numeric) || numeric < 50 || numeric > 60000) {
                addIssue("INVALID_EFFECT_INTERVAL", "error", path + ".actionValue",
                         "Blink/breathe interval must be 50-60000 ms");
            }
        }
        if (!action.useReceivedValue && action.actionType == 4) {
            if (!parseStrictLong(action.actionValue, numeric) || numeric < 0 || numeric > 65535) {
                addIssue("INVALID_PWM_VALUE", "error", path + ".actionValue",
                         "PWM value must be between 0 and 65535");
            }
        }
        if (!action.useReceivedValue && action.actionType == 5) {
            uint8_t dacValue = 0;
            if (!FastBee::parseDacActionValue(action.actionValue.c_str(), action.actionValue.length(), dacValue)) {
                addIssue("INVALID_DAC_VALUE", "error", path + ".actionValue",
                         "DAC value must be between 0 and 255");
            }
        }
        if (action.actionType == 15 && action.actionValue.isEmpty()) {
            addIssue("SCRIPT_ACTION_EMPTY", "error", path + ".actionValue",
                     "Script action requires command content");
        }
        if (action.actionType == 18) {
            JsonDocument poll;
            if (action.actionValue.isEmpty() || deserializeJson(poll, action.actionValue.c_str(), action.actionValue.length()) ||
                (!poll["poll"].is<JsonArray>() && !poll["ctrl"].is<JsonArray>())) {
                addIssue("INVALID_MODBUS_POLL", "error", path + ".actionValue",
                         "Modbus poll value must be JSON with poll or ctrl array");
            }
        }
        if (action.actionType == 19) {
            JsonDocument sensor;
            if (action.actionValue.isEmpty() || deserializeJson(sensor, action.actionValue.c_str(), action.actionValue.length())) {
                addIssue("INVALID_SENSOR_CONFIG", "error", path + ".actionValue",
                         "Sensor read actionValue must be valid JSON");
            }
        }
    }

    const bool allowed = errorCount == 0;
    data["allowed"] = allowed;
    data["requiresConfirmation"] = allowed && (warningCount > 0 || risk > 0);
    data["riskLevel"] = risk == 2 ? "high" : (risk == 1 ? "medium" : "low");
    data["errorCount"] = errorCount;
    data["warningCount"] = warningCount;
    JsonObject limits = data["limits"].to<JsonObject>();
    limits["maxRules"] = FastBee::ResourceProfile::MAX_PERIPH_EXEC_RULES;
    limits["maxTriggersPerRule"] = MAX_TRIGGERS_PER_RULE;
    limits["maxActionsPerRule"] = MAX_ACTIONS_PER_RULE;
    if (operation == "update") writeRuleCandidate(data["current"].to<JsonObject>(), current);
    writeRuleCandidate(data["candidate"].to<JsonObject>(), candidate);
    FastBee::appendRuleActionContracts(data, candidate);
    if (response.overflowed()) {
        ctx->sendError(request, 503, "Rule preflight response allocation failed");
        return;
    }
    response["allowed"] = allowed;
    HandlerUtils::sendJsonStream(request, response);
}

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
            PeripheralConfig periph;
            if (pm.getPeripheralSnapshot(t.triggerPeriphId, periph)) {
                tObj["triggerPeriphName"] = periph.name;
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
            PeripheralConfig periph;
            if (pm.getPeripheralSnapshot(a.targetPeriphId, periph)) {
                aObj["targetPeriphName"] = periph.name;
                aObj["targetPeriphType"] = static_cast<int>(periph.type);
            }
        }
    }

    // 数据转换管道
    ruleDoc["protocolType"] = rule.protocolType;
    ruleDoc["scriptContent"] = rule.scriptContent;

    // 数据上报控制
    ruleDoc["reportAfterExec"] = rule.reportAfterExec;
    ruleDoc["allowPanelControl"] = rule.allowPanelControl;
    ruleDoc["recordHistory"] = rule.recordHistory;

    String output;
    serializeJson(ruleDoc, output);
    return output;
}
