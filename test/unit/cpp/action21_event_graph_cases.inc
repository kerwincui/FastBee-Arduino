static PeriphExecRule emissionRule(const char* id, const char* listen, const char* emit, bool enabled = true) {
    auto r = completionRule(id, listen, enabled);
    r.actions[0].actionType = 21; r.actions[0].targetPeriphId = emit;
    r.actions[0].actionValue = "payload"; r.actions[0].syncDelayMs = 1200;
    r.reportAfterExec = false; return r;
}
static std::string emissionJson(const char* id, const char* listen, const char* emit, bool enabled = true) {
    JsonDocument doc; CHECK(!deserializeJson(doc, completionJson(id, listen, enabled)));
    doc["actions"][0]["actionType"] = 21;
    doc["actions"][0]["targetPeriphId"] = emit;
    doc["actions"][0]["actionValue"] = "payload";
    doc["actions"][0]["syncDelayMs"] = 1200;
    doc["reportAfterExec"] = false;
    std::string out; serializeJson(doc, out); return out;
}
static void appendActionResult(std::vector<ActionExecResult>& results, const String& target,
        const String& value, bool success, const char* remark) {
    ActionExecResult result; result.targetPeriphId = target; result.actualValue = value;
    result.success = success; result.remark = remark; results.push_back(result);
}
static void executeFixtureEvent(PeriphExecManager& manager, const PeriphExecRule& rule) {
    PeriphExecManager::fixture = &manager;
    const auto& action = rule.actions[0];
    CHECK(action.actionType == 21 && action.syncDelayMs == 1200);
    // Advance the host monotonic-clock seam instead of sleeping or driving I/O.
    // It models a completed legal action delay, shorter than each job deadline.
    __fastbeeMockStartTime -= std::chrono::milliseconds(action.syncDelayMs);
    const String effectiveValue = action.actionValue;
    const bool suppressReport = true;
    struct Trace { FastBee::UartEventOrigin uartOrigin; };
    Trace* traceOut = nullptr;
    bool ok = false;
    std::vector<ActionExecResult> actionResults;
    // ACTUAL_ACTION21_BODY
    CHECK(ok && actionResults.size() == 1 && actionResults[0].remark == "event_triggered");
}
static size_t runAlternatingChain(bool legacy) {
    PeriphExecManager m; fresh(m);
    m.rules.emplace("exec_a", emissionRule("exec_a", "wifi_connected", "ntp_synced"));
    m.rules.emplace("exec_b", emissionRule("exec_b", "ntp_synced", "wifi_connected"));
    m.legacyEventMatcher = legacy;
    __fastbeeMockStartTime -= std::chrono::seconds(60);
    m.triggerEventById("wifi_connected", "initial");
    size_t completed = 0;
    // Deliberately capped observation: the legacy queue still contains the next
    // rule at the end; this does not wait for or claim an actual infinite run.
    while (completed < 12 && completed < m.admitted.size()) {
        const String id = m.admitted[completed].id;
        CHECK(id == (completed % 2 ? "exec_b" : "exec_a"));
        executeFixtureEvent(m, m.rules.at(id)); ++completed;
    }
    if (legacy) CHECK(completed == 12 && m.admitted.size() == 13);
    else CHECK(completed == 0 && m.admitted.empty());
    return completed;
}
static std::string dynamicEventJson(int enabled) {
    JsonDocument doc; CHECK(!deserializeJson(doc, emissionJson("exec_dynamic", "start", "finish")));
    if (enabled < 0) doc.remove("enabled"); else doc["enabled"] = enabled != 0;
    doc["actions"][0].remove("targetPeriphId");
    doc["actions"][0]["useReceivedValue"] = true;
    std::string out; serializeJson(doc, out); return out;
}
static void exerciseDynamicEventTargets() {
    PeriphExecManager m; String error;
    const auto base = config(rule("old"));
    for (int enabled : {-1, 0, 1}) {
        const bool rejected = enabled != 0;
        const auto content = config(dynamicEventJson(enabled));
        JsonDocument doc; CHECK(!deserializeJson(doc, content));
        failAfter = 0;
        const char* issue = FastBee::PeriphExecConfig::validateDocument(doc.as<JsonVariantConst>());
        CHECK(failAfter == 0); failAfter = -1;
        CHECK((issue != nullptr) == rejected);
        if (issue) CHECK(std::strcmp(issue, "CONFIG_RULE_EVENT_DYNAMIC_TARGET") == 0);
        reset(m, base); LittleFS.files["/config/staged.tmp"] = content;
        const auto before = LittleFS.files;
        CHECK(m.validateConfigurationFile("/config/staged.tmp", error) != rejected);
        CHECK(m.reviewConfigurationFile("/config/staged.tmp", reviewRule, nullptr, error) != rejected);
        CHECK(m.importConfigurationFile("/config/staged.tmp", error) != rejected);
        if (rejected) {
            CHECK(error == "CONFIG_RULE_EVENT_DYNAMIC_TARGET" && LittleFS.files == before);
            retained(m, base);
        }
        reset(m, content); CHECK(m.loadConfiguration() != rejected);
        if (rejected) {
            CHECK(m._configurationLoadFailed); retained(m, content);
        } else {
            CHECK(!m.rules.at("exec_dynamic").enabled);
            CHECK(m.rules.at("exec_dynamic").actions[0].targetPeriphId.isEmpty());
            CHECK(m.rules.at("exec_dynamic").actions[0].useReceivedValue);
            const auto saved = LittleFS.files.at(PERIPH_EXEC_CONFIG_FILE);
            CHECK(!m.enableRuleAndSave("exec_dynamic", error));
            CHECK(error == "CONFIG_RULE_EVENT_DYNAMIC_TARGET" && !m.rules.at("exec_dynamic").enabled);
            CHECK(LittleFS.files.at(PERIPH_EXEC_CONFIG_FILE) == saved);
            auto corrected = m.rules.at("exec_dynamic");
            corrected.actions[0].targetPeriphId = "finish";
            corrected.enabled = true;
            CHECK(m.updateRuleAndSave("exec_dynamic", corrected, error));
        }
    }
    fresh(m);
    auto unsafe = emissionRule("exec_dynamic", "start", "");
    unsafe.actions[0].useReceivedValue = true;
    m.rules.emplace(unsafe.id, unsafe); // Deliberate internal bypass.
    const auto saved = LittleFS.files.at(PERIPH_EXEC_CONFIG_FILE);
    const char* graphIssue = FastBee::validateRuleCompletionGraph(m.rules);
    CHECK(graphIssue && std::strcmp(graphIssue, "CONFIG_RULE_EVENT_DYNAMIC_TARGET") == 0);
    CHECK(!m.saveConfiguration() && LittleFS.files.at(PERIPH_EXEC_CONFIG_FILE) == saved);
    m.dispatchEventMatchedRules("start", "finish");
    m.dispatchPeriphExecEvent("start", "finish");
    CHECK(m.admitted.empty());
}

static size_t emissionAllocationCalls = 0;
static size_t emissionHeapAllocations = 0;
static char* guardedEmissionProbe(size_t bytes) {
    if (++emissionAllocationCalls == 2)
        FastBee::MemoryBudget::free8 = FastBee::MemoryBudget::GUARD8_SEVERE_DRAM_FREE;
    return allocateConfigEventId(bytes);
}
static void exerciseAction21Graph() {
    exerciseDynamicEventTargets();
    CHECK(runAlternatingChain(true) == 12);
    CHECK(runAlternatingChain(false) == 0);
    PeriphExecManager m; String error; fresh(m);
    CHECK(m.addRuleAndSave(emissionRule("exec_a", "wifi_connected", "ntp_synced"), error));
    const auto first = LittleFS.files.at(PERIPH_EXEC_CONFIG_FILE);
    CHECK(!m.addRuleAndSave(emissionRule("exec_b", "ntp_synced", "wifi_connected"), error));
    CHECK(error == "CONFIG_RULE_EVENT_CYCLE" && LittleFS.files.at(PERIPH_EXEC_CONFIG_FILE) == first);
    CHECK(m.addRuleAndSave(emissionRule("exec_b", "ntp_synced", "wifi_connected", false), error));
    CHECK(!m.enableRuleAndSave("exec_b", error) && error == "CONFIG_RULE_EVENT_CYCLE");
    auto noCycle = emissionRule("exec_b", "ntp_synced", "unknown_mqtt_only", true);
    CHECK(m.updateRuleAndSave("exec_b", noCycle, error));
    auto closes = emissionRule("exec_a", "unknown_mqtt_only", "ntp_synced");
    CHECK(!m.updateRuleAndSave("exec_a", closes, error) && error == "CONFIG_RULE_EVENT_CYCLE");
    // Future DEVICE_EVENT creation must not activate a previously accepted loop.
    fresh(m);
    CHECK(m.addRuleAndSave(emissionRule("exec_a", "future_left", "future_right"), error));
    CHECK(!m.addRuleAndSave(emissionRule("exec_b", "future_right", "future_left"), error));
    CHECK(error == "CONFIG_RULE_EVENT_CYCLE");
    // No wildcard syntax: '*' is only the literal event ID, not any event.
    fresh(m);
    CHECK(m.addRuleAndSave(emissionRule("exec_a", "*", "other"), error));
    CHECK(m.addRuleAndSave(emissionRule("exec_b", "unrelated", "*"), error));
    CHECK(!FastBee::validateRuleCompletionGraph(m.rules));
    // Mixed completion + explicit-event edges form the same graph.
    fresh(m);
    CHECK(m.addRuleAndSave(emissionRule("exec_a", "exec_b", "device_alarm"), error));
    CHECK(!m.addRuleAndSave(completionRule("exec_b", "device_alarm"), error));
    CHECK(error == "CONFIG_RULE_EVENT_CYCLE");
    fresh(m);
    auto repeated = emissionRule("exec_a", "start", "finish");
    repeated.actions.resize(MAX_ACTIONS_PER_RULE, repeated.actions[0]);
    CHECK(m.addRuleAndSave(repeated, error));
    failAfter = 0;
    CHECK(!FastBee::validateRuleCompletionGraph(m.rules) && failAfter == 0);
    failAfter = -1;
    const auto base = config(rule("old"));
    const auto cycle = config(emissionJson("exec_a", "wifi_connected", "ntp_synced") + "," +
                              emissionJson("exec_b", "ntp_synced", "wifi_connected"));
    const auto draft = config(emissionJson("exec_a", "wifi_connected", "ntp_synced") + "," +
                              emissionJson("exec_b", "ntp_synced", "wifi_connected", false));
    for (const auto& content : {cycle, draft}) {
        const bool rejected = content == cycle;
        reset(m, base); LittleFS.files["/config/staged.tmp"] = content;
        const auto originalFiles = LittleFS.files;
        JsonDocument doc; CHECK(!deserializeJson(doc, content));
        const char* issue = FastBee::PeriphExecConfig::validateDocument(doc.as<JsonVariantConst>());
        CHECK((issue != nullptr) == rejected);
        if (issue) CHECK(std::strcmp(issue, "CONFIG_RULE_EVENT_CYCLE") == 0);
        CHECK(m.validateConfigurationFile("/config/staged.tmp", error) != rejected);
        CHECK(m.reviewConfigurationFile("/config/staged.tmp", reviewRule, nullptr, error) != rejected);
        CHECK(m.importConfigurationFile("/config/staged.tmp", error) != rejected);
        if (rejected) { CHECK(error == "CONFIG_RULE_EVENT_CYCLE" && LittleFS.files == originalFiles); retained(m, base); }
        reset(m, content); CHECK(m.loadConfiguration() != rejected);
        if (rejected) { CHECK(m._configurationLoadFailed); retained(m, content); }
    }
    // Exact long event strings and allocation failures: no truncated or hashed
    // identity substitutes, and previously persisted bytes survive OOM.
    const std::string longEvent(2048, 'e');
    const auto longCycle = config(emissionJson("exec_a", "start", longEvent.c_str()) + "," +
                                  emissionJson("exec_b", longEvent.c_str(), "start"));
    reset(m, base); LittleFS.files["/config/staged.tmp"] = longCycle;
    CHECK(!m.validateConfigurationFile("/config/staged.tmp", error) && error == "CONFIG_RULE_EVENT_CYCLE");
    {
        // A borrowed full-document graph needs no copied-target scratch even
        // when the document exceeds the separate on-disk 96KiB size limit.
        const std::string bigA(40000, 'a'), bigB(40000, 'b'), bigC(40000, 'c');
        JsonDocument doc;
        CHECK(!deserializeJson(doc, config(emissionJson("exec_a", "start", bigA.c_str()) + "," +
            emissionJson("exec_b", "start", bigB.c_str()) + "," +
            emissionJson("exec_c", "start", bigC.c_str()))));
        failAfter = 0;
        const char* issue = FastBee::PeriphExecConfig::validateDocument(doc.as<JsonVariantConst>());
        CHECK(!issue && failAfter == 0); failAfter = -1;
        // Streaming still has the independent cumulative-copy ceiling.
        FastBee::PeriphExecConfig::RuleReferenceIndex index;
        for (JsonObjectConst raw : doc["rules"].as<JsonArrayConst>()) {
            CHECK(index.add(raw["id"]));
            issue = index.captureRuleEmissions(raw, allocateConfigEventId);
            if (index.count < 3) CHECK(!issue);
        }
        CHECK(issue && std::strcmp(issue, "CONFIG_RULE_EVENT_GRAPH_LIMIT") == 0);
    }
    {
        JsonDocument doc; CHECK(!deserializeJson(doc, emissionJson("exec_a", "start", "finish")));
        FastBee::PeriphExecConfig::RuleReferenceIndex index;
        CHECK(index.add(doc["id"])); failAfter = 0;
        const char* issue = index.captureRuleEmissions(doc.as<JsonObjectConst>(), allocateConfigEventId);
        CHECK(issue && std::strcmp(issue, "CONFIG_MEMORY_PRESSURE") == 0);
        CHECK(failAfter == -1); // The injected nothrow-array allocation failure was consumed.
    }
    {
        JsonDocument doc; CHECK(!deserializeJson(doc, emissionJson("exec_a", "start", "finish")));
        auto second = doc["actions"].as<JsonArray>().add<JsonObject>();
        second["actionType"] = 21; second["targetPeriphId"] = "finish-again";
        FastBee::PeriphExecConfig::RuleReferenceIndex index;
        CHECK(index.add(doc["id"])); emissionAllocationCalls = 0;
        emissionHeapAllocations = 0;
        graphAllocationHook = [] { ++emissionHeapAllocations; };
        const char* issue = index.captureRuleEmissions(doc.as<JsonObjectConst>(), guardedEmissionProbe);
        graphAllocationHook = nullptr;
        // The second request must be rejected BEFORE allocating. A post-floor
        // check alone reports the same error but incurs a second allocation.
        CHECK(emissionHeapAllocations == 1);
        CHECK(issue && std::strcmp(issue, "CONFIG_MEMORY_PRESSURE") == 0 && emissionAllocationCalls == 2);
        CHECK(index.emitted[0][0] && !index.emitted[0][1]);
        FastBee::MemoryBudget::free8 = 1000000;
    }
    {
        // Simulate allocator metadata/concurrent pressure becoming visible as
        // allocation returns. The actual allocator's post-floor check must
        // release its new block and report failure.
        graphAllocationHook = [] {
            FastBee::MemoryBudget::free8 = FastBee::MemoryBudget::GUARD8_SEVERE_DRAM_FREE - 1;
        };
        char* copy = allocateConfigEventId(16);
        graphAllocationHook = nullptr;
        FastBee::MemoryBudget::free8 = 1000000;
        CHECK(!copy);
    }
    CHECK(FastBee::fixedRuleActionEvent(21, "explicit", "$value", true, "exec_a") == std::string("explicit"));
    CHECK(FastBee::fixedRuleActionEvent(21, "", "legacy_event", false, "exec_a") == std::string("legacy_event"));
    CHECK(FastBee::fixedRuleActionEvent(21, "", "", false, "exec_a") == std::string("exec_a"));
    CHECK(!FastBee::fixedRuleActionEvent(21, "", "$value", true, "exec_a"));
}
