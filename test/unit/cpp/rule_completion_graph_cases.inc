static PeriphExecRule completionRule(const char* id, const char* source = "", bool enabled = true) {
    PeriphExecRule r;
    r.id = id; r.name = id; r.enabled = enabled;
    r.triggers.emplace_back();
    r.triggers[0].triggerType = source[0] ? 4 : 1;
    r.triggers[0].eventId = source;
    r.triggers[0].lastTriggerTime = 1;
    r.actions.emplace_back(); r.actions[0].actionType = 8;
    r.actions[0].syncDelayMs = 1000; // A successful cycle can exceed the cooldown.
    return r;
}
static std::string completionJson(const char* id, const char* source, int enabled = 1) {
    JsonDocument doc; CHECK(!deserializeJson(doc, rule(id)));
    if (enabled >= 0) doc["enabled"] = enabled != 0; // -1 exercises the legacy true default.
    doc["triggers"][0]["triggerType"] = 4;
    doc["triggers"][0]["eventId"] = source;
    std::string out; serializeJson(doc, out); return out;
}
static void fresh(PeriphExecManager& m) {
    reset(m, config(rule("old")));
    m.rules.clear(); m._irKeysJson = "[]";
    m._buttonEventCache.clear(); m._dataSourceCache.clear();
    CHECK(m.saveConfiguration());
}
static int reviewCount = 0;
static bool reviewRule(const PeriphExecRule&, void*, String&) { ++reviewCount; return true; }

static void exerciseGraphAndMutations() {
    PeriphExecManager m; String error;
    fresh(m);
    const auto emptyBytes = LittleFS.files.at(PERIPH_EXEC_CONFIG_FILE);
    const auto emptyRevision = m._ruleRuntimeRevision;
    CHECK(!m.addRuleAndSave(completionRule("a", "a"), error));
    CHECK(error == "CONFIG_RULE_EVENT_CYCLE" && m.rules.empty());
    CHECK(LittleFS.files.at(PERIPH_EXEC_CONFIG_FILE) == emptyBytes && m._ruleRuntimeRevision == emptyRevision);

    CHECK(m.addRuleAndSave(completionRule("a", "b"), error));
    auto aBefore = m.rules.at("a");
    const auto firstBytes = LittleFS.files.at(PERIPH_EXEC_CONFIG_FILE);
    CHECK(!m.addRuleAndSave(completionRule("b", "a"), error));
    CHECK(error == "CONFIG_RULE_EVENT_CYCLE" && m.rules.size() == 1 && rulesMatch(aBefore, m.rules.at("a")));
    CHECK(LittleFS.files.at(PERIPH_EXEC_CONFIG_FILE) == firstBytes);

    CHECK(m.addRuleAndSave(completionRule("b", "a", false), error));
    const auto disabledBytes = LittleFS.files.at(PERIPH_EXEC_CONFIG_FILE);
    CHECK(!m.enableRuleAndSave("b", error) && error == "CONFIG_RULE_EVENT_CYCLE");
    CHECK(!m.rules.at("b").enabled && LittleFS.files.at(PERIPH_EXEC_CONFIG_FILE) == disabledBytes);
    CHECK(m.disableRuleAndSave("a", error));
    CHECK(m.enableRuleAndSave("b", error));
    CHECK(!m.enableRuleAndSave("a", error) && error == "CONFIG_RULE_EVENT_CYCLE");
    CHECK(m.removeRuleAndSave("b", error));
    CHECK(m.enableRuleAndSave("a", error));

    fresh(m);
    CHECK(m.addRuleAndSave(completionRule("a"), error));
    CHECK(m.addRuleAndSave(completionRule("b", "a"), error));
    CHECK(m.addRuleAndSave(completionRule("c", "b"), error));
    auto cyclic = completionRule("noncanonical-payload-id", "c");
    const auto dagBytes = LittleFS.files.at(PERIPH_EXEC_CONFIG_FILE);
    CHECK(!m.updateRuleAndSave("a", cyclic, error) && error == "CONFIG_RULE_EVENT_CYCLE");
    CHECK(m.rules.at("a").id == "a" && m.rules.at("a").triggers[0].triggerType == 1);
    CHECK(LittleFS.files.at(PERIPH_EXEC_CONFIG_FILE) == dagBytes);
    auto multiple = m.rules.at("b"); multiple.triggers.push_back(multiple.triggers[0]);
    CHECK(m.updateRuleAndSave("b", multiple, error)); // Same edge is counted once.
    auto multiCycle = m.rules.at("a");
    multiCycle.triggers.push_back(completionRule("unused", "c").triggers[0]);
    const auto multiBefore = LittleFS.files.at(PERIPH_EXEC_CONFIG_FILE);
    CHECK(!m.updateRuleAndSave("a", multiCycle, error) && error == "CONFIG_RULE_EVENT_CYCLE");
    CHECK(m.rules.at("a").triggers.size() == 1 && LittleFS.files.at(PERIPH_EXEC_CONFIG_FILE) == multiBefore);
    CHECK(m.addRuleAndSave(completionRule("disconnected"), error));
    auto unchanged = m.rules.at("b");
    const auto beforeFailure = LittleFS.files.at(PERIPH_EXEC_CONFIG_FILE);
    multiple.name = "failed-write";
    LittleFS.writeLimit = 0;
    CHECK(!m.updateRuleAndSave("b", multiple, error));
    CHECK(error.startsWith("PERSISTENCE_ROLLBACK_OK") && rulesMatch(m.rules.at("b"), unchanged));
    CHECK(LittleFS.files.at(PERIPH_EXEC_CONFIG_FILE) == beforeFailure);
    LittleFS.writeLimit = SIZE_MAX;

    // The graph's real helper does not allocate after rule construction.
    failAfter = 0;
    const char* graphIssue = FastBee::validateRuleCompletionGraph(m.rules);
    CHECK(!graphIssue && failAfter == 0);
    failAfter = -1;
    for (auto& entry : m.rules) {
        entry.second.triggers[0].triggerType = 4;
        entry.second.triggers[0].eventId = "wifi_connected";
        entry.second.actions[0].actionType = 21;
        entry.second.actions[0].targetPeriphId = "custom_alarm";
    }
    CHECK(!FastBee::validateRuleCompletionGraph(m.rules)); // Exact IDs do not match: this event emission graph is acyclic.
    m.rules.at("a").triggers.resize(MAX_TRIGGERS_PER_RULE + 1);
    CHECK(std::strcmp(FastBee::validateRuleCompletionGraph(m.rules), "CONFIG_RULE_EVENT_GRAPH_LIMIT") == 0);

    // All profile slots, exact long identities and non-recursive elimination.
    fresh(m);
    for (size_t i = 0; i < FastBee::RuleCompletionGraph::Capacity; ++i) {
        const std::string id = std::string(36, 'n') + std::to_string(i);
        const std::string prev = i ? std::string(36, 'n') + std::to_string(i - 1) : "";
        auto r = completionRule(id.c_str(), prev.c_str()); m.rules.emplace(r.id, r);
    }
    CHECK(!FastBee::validateRuleCompletionGraph(m.rules));
    const String first = std::string(36, 'n') + "0";
    const String last = std::string(36, 'n') + std::to_string(FastBee::RuleCompletionGraph::Capacity - 1);
    m.rules.at(first).triggers[0].triggerType = 4; m.rules.at(first).triggers[0].eventId = last;
    CHECK(std::strcmp(FastBee::validateRuleCompletionGraph(m.rules), "CONFIG_RULE_EVENT_CYCLE") == 0);
    m.rules.emplace("extra", completionRule("extra"));
    CHECK(std::strcmp(FastBee::validateRuleCompletionGraph(m.rules), "CONFIG_RULE_EVENT_GRAPH_LIMIT") == 0);
    FastBee::RuleCompletionGraph overLimit(FastBee::RuleCompletionGraph::Capacity + 1);
    CHECK(std::strcmp(overLimit.error(), "CONFIG_RULE_EVENT_GRAPH_LIMIT") == 0);
    FastBee::RuleCompletionGraph invalid(1); CHECK(!invalid.addEdge(1, 0));
    CHECK(std::strcmp(invalid.error(), "CONFIG_RULE_EVENT_GRAPH_LIMIT") == 0);
}

static void exerciseFiles() {
    PeriphExecManager m; String error;
    const auto original = config(rule("old"));
    const std::string cycles[] = {
        config(completionJson("a", "a", -1)),
        config(completionJson("a", "b") + "," + completionJson("b", "a")),
        config(completionJson("c", "b") + "," + completionJson("a", "c") + "," + completionJson("b", "a"))
    };
    for (const auto& content : cycles) {
        reset(m, original); LittleFS.files["/config/staged.tmp"] = content;
        const auto filesBefore = LittleFS.files;
        JsonDocument whole; CHECK(!deserializeJson(whole, content));
        CHECK(std::strcmp(FastBee::PeriphExecConfig::validateDocument(whole.as<JsonVariantConst>()), "CONFIG_RULE_EVENT_CYCLE") == 0);
        CHECK(!m.validateConfigurationFile("/config/staged.tmp", error) && error == "CONFIG_RULE_EVENT_CYCLE");
        reviewCount = 0;
        CHECK(!m.reviewConfigurationFile("/config/staged.tmp", reviewRule, nullptr, error));
        CHECK(error == "CONFIG_RULE_EVENT_CYCLE" && reviewCount > 0);
        CHECK(!m.importConfigurationFile("/config/staged.tmp", error) && error == "CONFIG_RULE_EVENT_CYCLE");
        CHECK(!m._configurationRestartPending && LittleFS.files == filesBefore); retained(m, original);
        reset(m, content);
        LittleFS.files["/config/ir_keys.json"] = "{\"keys\":[\"old migration\"]}";
        const auto legacyBytes = LittleFS.files;
        CHECK(!m.loadConfiguration() && m._configurationLoadFailed);
        retained(m, content); CHECK(LittleFS.files == legacyBytes);
    }
    const std::string accepted[] = {
        config(completionJson("c", "b") + "," + rule("a") + "," + completionJson("b", "a")),
        config(completionJson("a", "b", 0) + "," + completionJson("b", "a")),
        config(completionJson("a", "a", 0)),
        config(completionJson("a", "missing_completion_source")),
        config(ruleControl("a", "a", 22, true)),
        config(ruleControl("a", "b", 22, true) + "," + ruleControl("b", "a", 23, true))
    };
    for (const auto& content : accepted) {
        reset(m, original); LittleFS.files["/config/staged.tmp"] = content;
        JsonDocument whole; CHECK(!deserializeJson(whole, content));
        CHECK(!FastBee::PeriphExecConfig::validateDocument(whole.as<JsonVariantConst>()));
        CHECK(m.validateConfigurationFile("/config/staged.tmp", error));
        CHECK(m.reviewConfigurationFile("/config/staged.tmp", reviewRule, nullptr, error));
        CHECK(m.importConfigurationFile("/config/staged.tmp", error));
        CHECK(m._configurationRestartPending && LittleFS.files.at(PERIPH_EXEC_CONFIG_FILE) == content);
        CHECK(m.rules.size() == 1 && m.rules.count("old"));
        reset(m, content); CHECK(m.loadConfiguration() && !m._configurationLoadFailed);
        CHECK(LittleFS.files.at(PERIPH_EXEC_CONFIG_FILE) == content);
    }
    // Existing shape-only validators must not inherit import-only action target
    // restrictions. Completion edges never convert action22/23 into graph edges.
    reset(m, original);
    const auto missingActionTarget = config(ruleControl("a", "external_rule", 22, true));
    JsonDocument shapeOnly; CHECK(!deserializeJson(shapeOnly, missingActionTarget));
    CHECK(!FastBee::PeriphExecConfig::validateDocument(shapeOnly.as<JsonVariantConst>()));
    LittleFS.files["/config/staged.tmp"] = missingActionTarget;
    CHECK(m.validateConfigurationFile("/config/staged.tmp", error));
    CHECK(!m.importConfigurationFile("/config/staged.tmp", error));
    CHECK(error == "CONFIG_RULE_REFERENCE"); retained(m, original);
    // A corrected disabled import is the existing recovery route after boot rejection.
    reset(m, cycles[0]); CHECK(!m.loadConfiguration());
    const auto corrected = config(completionJson("a", "a", 0));
    LittleFS.files["/config/staged.tmp"] = corrected;
    CHECK(m.importConfigurationFile("/config/staged.tmp", error));
    CHECK(m._configurationRestartPending && m.rules.count("old"));

    // More than one legacy cycle requires a complete corrected import. A partial
    // persisted fix must not leave another active cycle in the durable file.
    const auto twoCycles = config(completionJson("a", "a") + "," + completionJson("b", "b"));
    reset(m, twoCycles); CHECK(!m.loadConfiguration()); retained(m, twoCycles);
    m._configurationLoadFailed = false; m.rules.clear(); // Deliberate internal bypass fixture.
    m.rules.emplace("a", completionRule("a", "a")); m.rules.emplace("b", completionRule("b", "b"));
    CHECK(!m.disableRuleAndSave("a", error) && error.startsWith("PERSISTENCE_ROLLBACK_OK"));
    CHECK(m.rules.at("a").enabled && m.rules.at("b").enabled);
    CHECK(LittleFS.files.at(PERIPH_EXEC_CONFIG_FILE) == twoCycles);
    const auto bothDisabled = config(completionJson("a", "a", 0) + "," + completionJson("b", "b", 0));
    LittleFS.files["/config/staged.tmp"] = bothDisabled;
    CHECK(m.importConfigurationFile("/config/staged.tmp", error));
    CHECK(LittleFS.files.at(PERIPH_EXEC_CONFIG_FILE) == bothDisabled && m._configurationRestartPending);

    reset(m, original); LittleFS.files["/config/staged.tmp"] = accepted[0];
    LittleFS.failRename = "/config/staged.tmp";
    CHECK(!m.importConfigurationFile("/config/staged.tmp", error) && error == "CONFIG_COMMIT_FAILED");
    retained(m, original); CHECK(!m._configurationRestartPending);
}

static void exerciseDispatch() {
    PeriphExecManager m;
    fresh(m);
    // Bypass normal mutation APIs deliberately to exercise the real runtime defense.
    m.rules.emplace("a", completionRule("a", "b"));
    m.rules.emplace("b", completionRule("b", "a"));
    __fastbeeMockStartTime -= std::chrono::seconds(60);
    CHECK(millis() > 500);
    for (unsigned i = 0; i < 3; ++i) {
        m.dispatchPeriphExecEvent("a", "success"); m.dispatchPeriphExecEvent("b", "success");
        __fastbeeMockStartTime -= std::chrono::seconds(1);
    }
    CHECK(m.admitted.empty());
    CHECK(m.rules.at("a").triggers[0].triggerCount == 0 && m.rules.at("b").triggers[0].triggerCount == 0);
    m.rules.at("a").triggers[0].eventId = "button_click";
    m.rules.at("b").triggers.push_back(m.rules.at("b").triggers[0]);
    m.dispatchPeriphExecEvent("a", "success");
    CHECK(m.admitted.size() == 1 && m.admitted[0].id == "b" && m.admitted[0].triggerIndex == 0);
    CHECK(m.admitted[0].data == "success");
    m.admitted.clear();
    m.rules.at("b").triggers[0].lastTriggerTime = millis();
    m.rules.at("b").triggers[1].lastTriggerTime = millis();
    m.dispatchPeriphExecEvent("a", "success"); CHECK(m.admitted.empty()); // Existing cooldown retained.
    __fastbeeMockStartTime -= std::chrono::seconds(1);
    PeripheralManager::current = false;
    m.dispatchPeriphExecEvent("a", "success", FastBee::UartEventOrigin::captured(1, 1));
    CHECK(m.admitted.empty()); PeripheralManager::current = true;
    lockAllowed = false; m.dispatchPeriphExecEvent("a", "success"); CHECK(m.admitted.empty()); lockAllowed = true;
    m.rules.at("b").enabled = false;
    m.dispatchPeriphExecEvent("a", "success"); CHECK(m.admitted.empty());
    // Manually running a disabled source can seed its enabled successor once;
    // the disabled destination is never automatically re-entered.
    m.rules.at("a").enabled = false; m.rules.at("b").enabled = true;
    m.rules.at("a").triggers[0].eventId = "b";
    m.dispatchPeriphExecEvent("a", "success"); CHECK(m.admitted.size() == 1);
    m.admitted.clear(); m.dispatchPeriphExecEvent("b", "success"); CHECK(m.admitted.empty());
    m.rules.at("a").enabled = true; m.rules.at("a").triggers[0].eventId = "button_click";
    m.rules.at("b").enabled = true; m.rules.at("b").runtimeRevision = 42;
    m.dispatchPeriphExecEvent("a", "failed");
    CHECK(m.admitted.size() == 1 && m.admitted[0].revision == 42 && m.admitted[0].data == "failed");
}

int main() {
    exerciseGraphAndMutations(); exerciseFiles(); exerciseDispatch();
    std::printf("completion graph actual CRUD/transactions/import/review/load/dispatch passed; capacity=%zu graphBytes=%zu indexBytes=%zu\n",
        FastBee::RuleCompletionGraph::Capacity, sizeof(FastBee::RuleCompletionGraph),
        sizeof(FastBee::PeriphExecConfig::RuleReferenceIndex));
}
