// These declarations connect the existing config fixture to actual Manager
// transaction, review and completion-dispatch methods extracted by the test.
// PRODUCTION_MUTATION_TYPES
bool addRuleAndSave(const PeriphExecRule&, String&);
bool updateRuleAndSave(const String&, const PeriphExecRule&, String&);
bool removeRuleAndSave(const String&, String&);
bool enableRuleAndSave(const String&, String&);
bool disableRuleAndSave(const String&, String&);
bool applyRuleMutationTransactional(RuleMutationType, const String&, const PeriphExecRule*, String&);
bool restoreRuleMutationLocked(const RuleMutationSnapshot&, const String&);
using ConfigurationReviewVisitor = bool (*)(const PeriphExecRule&, void*, String&);
static bool reviewConfigurationFile(const char*, ConfigurationReviewVisitor, void*, String&);
struct SchedulerFixture { void validateLoadedConfig() {} };
SchedulerFixture* _scheduler = nullptr;
struct Admitted { String id; size_t triggerIndex; uint64_t revision; String data; };
std::vector<Admitted> admitted;
bool dispatchAndCommitTrigger(const String& id, size_t triggerIndex, const String& data,
        const char* label, const FastBee::UartEventOrigin&, uint64_t revision) {
    CHECK(std::strcmp(label, "periph-event") == 0 || std::strcmp(label, "event") == 0);
    CHECK(rules.at(id).runtimeRevision == revision);
    admitted.push_back({id, triggerIndex, revision, data});
    if (std::strcmp(label, "event") == 0) {
        rules.at(id).triggers[triggerIndex].lastTriggerTime = millis();
        ++rules.at(id).triggers[triggerIndex].triggerCount;
    }
    return true;
}
void dispatchPeriphExecEvent(const String&, const String&, const FastBee::UartEventOrigin& = {});

// Action21 fixture delegates into the actual event matcher. The no-graph legacy
// matcher is frozen independently so a delay longer than cooldown is observable.
static inline PeriphExecManager* fixture = nullptr;
static PeriphExecManager& getInstance() { CHECK(fixture); return *fixture; }
bool legacyEventMatcher = false;
unsigned eventPublishes = 0;
void dispatchEventMatchedRules(const String&, const String&, const FastBee::UartEventOrigin& = {});
void dispatchEventMatchedRulesLegacy(const String&, const String&, const FastBee::UartEventOrigin& = {});
void triggerEventById(const String& id, const String& data, const FastBee::UartEventOrigin& origin = {}, bool = true) {
    if (legacyEventMatcher) dispatchEventMatchedRulesLegacy(id, data, origin);
    else dispatchEventMatchedRules(id, data, origin);
}
bool notifyMqttEventPublish(const String&, const String&, const String&, const FastBee::UartEventOrigin& = {}) {
    ++eventPublishes; return true;
}
