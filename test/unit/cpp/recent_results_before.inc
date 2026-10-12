void PeriphExecRouteHandler::handleGetRecentResults(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    int limit = 10;
    if (request->hasParam("limit")) {
        limit = request->getParam("limit")->value().toInt();
        if (limit < 1) limit = 1;
        if (limit > 20) limit = 20;
    }

    uint32_t operationFilter = 0;
    if (request->hasParam("operationId")) {
        operationFilter = static_cast<uint32_t>(
            strtoul(request->getParam("operationId")->value().c_str(), nullptr, 10));
    }
    const bool includeActions = operationFilter != 0 ||
        (request->hasParam("includeActions") &&
         request->getParam("includeActions")->value() == "true");

    auto results = PeriphExecManager::getInstance().getRecentResults();
    int total = static_cast<int>(results.size());
    int emitted = 0;
    String items;
    const size_t expectedItems = static_cast<size_t>(
        limit < total ? limit : total);
    items.reserve(expectedItems * (includeActions ? 768 : 320));

    for (int i = total - 1; i >= 0 && emitted < limit; --i) {
        const auto& result = results[static_cast<size_t>(i)];
        if (operationFilter != 0 && result.operationId != operationFilter) continue;
        if (emitted > 0) items += ",";

        unsigned long duration = 0;
        if (result.startTime != 0 && result.endTime != 0) {
            duration = result.endTime - result.startTime;
        }

        items += F("{\"operationId\":");
        items += String(result.operationId);
        items += F(",\"ruleId\":\"");
        items += escapePeriphExecJsonString(result.ruleId);
        items += F("\",\"ruleName\":\"");
        items += escapePeriphExecJsonString(result.ruleName);
        items += F("\",\"status\":");
        items += String(static_cast<int>(result.status));
        items += F(",\"statusName\":\"");
        items += asyncExecLegacyStatusName(result.status);
        items += F("\",\"state\":\"");
        items += asyncExecStatusName(result.status);
        items += F("\",\"terminal\":");
        items += asyncExecStatusIsTerminal(result.status) ? F("true") : F("false");
        items += F(",\"errorCode\":\"");
        items += asyncExecErrorName(result.errorCode);
        items += F("\",\"queuedAt\":");
        items += String(result.queuedAt);
        items += F(",\"peripheralConfigEpoch\":");
        items += String(result.peripheralConfigEpoch);
        items += F(",\"startedAt\":");
        items += String(result.startTime);
        items += F(",\"endedAt\":");
        items += String(result.endTime);
        items += F(",\"deadlineAt\":");
        items += String(result.deadlineAt);
        items += F(",\"queuePosition\":");
        items += String(static_cast<unsigned int>(result.queuePosition));
        items += F(",\"async\":");
        items += result.asyncExecution ? F("true") : F("false");
        {
            JsonDocument control;
            FastBee::writeOrdinaryControlJson(control.to<JsonObject>(), result.ordinaryControlResult);
            if (control.overflowed()) { ctx->sendError(request, 503, "CONTROL_RESULT_UNAVAILABLE"); return; }
            items += F(",\"control\":");
            serializeJson(control, items);
        }
        // Legacy timestamps retained for existing device tests and clients.
        items += F(",\"startTime\":");
        items += String(result.startTime);
        items += F(",\"endTime\":");
        items += String(result.endTime);
        items += F(",\"durationMs\":");
        items += String(duration);
        items += F(",\"reportCount\":");
        items += String(static_cast<unsigned int>(result.reportCount));
        items += F(",\"reportCountScope\":\"attempted_results\",\"deliveryVerified\":false");
        items += F(",\"actionCount\":");
        items += String(static_cast<unsigned int>(result.actionCount));
        items += F(",\"actionTraceTruncated\":");
        items += result.actionTraceTruncated ? F("true") : F("false");
        if (includeActions) {
            items += F(",\"actions\":[");
            for (uint8_t actionIndex = 0;
                 actionIndex < result.actionTraceCount &&
                 actionIndex < ASYNC_EXEC_ACTION_TRACE_LIMIT;
                 ++actionIndex) {
                if (actionIndex > 0) items += ',';
                const auto& action = result.actionTraces[actionIndex];
                items += F("{\"index\":");
                items += String(static_cast<unsigned int>(action.index));
                items += F(",\"actionType\":");
                items += String(static_cast<unsigned int>(action.actionType));
                items += F(",\"targetPeriphId\":\"");
                items += escapePeriphExecJsonString(String(action.targetPeriphId));
                items += F("\",\"value\":\"");
                items += escapePeriphExecJsonString(String(action.valueSummary));
                items += F("\",\"success\":");
                items += action.success ? F("true") : F("false");
                items += F(",\"durationMs\":");
                items += String(action.durationMs);
                items += F(",\"detailCount\":");
                items += String(static_cast<unsigned int>(action.detailCount));
                items += F(",\"remark\":\"");
                items += escapePeriphExecJsonString(String(action.remark));
                items += F("\"}");
            }
            items += F("]");
        }
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
