#include <Arduino.h>
#undef malloc
#undef free
#include <ArduinoJson.h>
#include "utils/SharedJsonSerializer.h"
#include "utils/SharedJsonDeserializer.h"
#include "network/FixedJsonField.h"
#include <LittleFS.h>
#include <set>
#include <new>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <fstream>
#include "core/PeriphExecConfigValidation.h"
#include "core/DailyTriggerState.h"
#include "core/ThermostatConfig.h"
#include "core/SdRecorderConfig.h"
#include "core/RuleOutputIdentity.h"
using std::max;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); std::exit(86); } } while (false)
inline void convertToJson(const String& value, JsonVariant output) { output.set(std::string(value)); }
inline void convertFromJson(JsonVariantConst value, String& output) { output = value.as<std::string>(); }

static int failAfter = -1;
void* operator new(size_t size) {
    if (failAfter == 0) { failAfter = -1; throw std::bad_alloc(); }
    if (failAfter > 0) --failAfter;
    void* p = malloc(size ? size : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
namespace FastBee { namespace MemoryBudget {
static constexpr size_t GUARD8_SEVERE_DRAM_FREE = 8192;
static size_t free8 = 1000000, largest8 = 1000000;
size_t dram8Free() { return free8; }
size_t dram8Largest() { return largest8; }
}}
static bool lockAllowed = true;
struct MutexGuard { MutexGuard(int, int = 0) {} bool isLocked() const { return lockAllowed; } };
#define pdMS_TO_TICKS(x) (x)
struct Log {
    template<class... T> void warningf(const char*, T...) {}
    template<class... T> void errorf(const char*, T...) {}
    template<class... T> void infof(const char*, T...) {}
    void warning(const char*) {} void error(const char*) {} void info(const char*) {}
} LOGGER;
#define LOG_ERRORF(...) do {} while (false)
struct FileUtils {
    static File openAtomicWrite(const String& path) { return LittleFS.open(path + ".tmp", "w"); }
    static bool commitAtomicWrite(const String& tmp, const String& path);
    static bool copyFile(const String& from, const String& to) {
        if (!LittleFS.exists(from) || LittleFS.exists(to) || to == LittleFS.failOpen.c_str()) return false;
        LittleFS.files[to.c_str()] = LittleFS.files.at(from.c_str()); return true;
    }
};

// PRODUCTION_HELPERS

class PeriphExecManager {
public:
    using RuleMap = std::map<String, PeriphExecRule>;
    RuleMap rules;
    String _irKeysJson;
    std::set<String> _buttonEventCache, _dataSourceCache;
    int _rulesMutex = 1;
    uint64_t _ruleRuntimeRevision = 0;
    bool _configurationRestartPending = false, _configurationLoadFailed = false, _weatherReferenceLease = false;
    const char* configurationWriteError() const;
    bool saveConfiguration(); bool saveConfigurationLocked();
    bool writeConfigurationLocked(const RuleMap&, const String&);
    bool loadConfiguration();
    void refreshRuleRuntimeIndexesLocked(bool) {
        if (_ruleRuntimeRevision != UINT64_MAX) ++_ruleRuntimeRevision;
        for (auto& entry : rules) entry.second.runtimeRevision = _ruleRuntimeRevision;
    }
    static bool validateConfigurationFile(const char*, String&);
    bool importConfigurationFile(const char*, String&);
    bool setIrKeysJson(const String&, const String* = nullptr);
    bool copyIrKeysJson(String&) const;
    bool removeRuleLocked(const String&, String&);
    bool addRuleLocked(const PeriphExecRule&, String&, String* = nullptr);
    bool updateRuleLocked(const String&, const PeriphExecRule&, String&);
    String generateUniqueId() const { return "generated"; }
    bool validateRuleActionsLocked(const PeriphExecRule&, String&) const { return true; }
    bool setRuleEnabledLocked(const String&, bool, String&);
    bool sanitizeRuleForSafety(PeriphExecRule&) const;
    bool sanitizeTriggerForSafety(ExecTrigger&, bool, const String&) const;
    bool ruleHasPollCollectionAction(const PeriphExecRule& rule) const {
        for (const auto& a : rule.actions) if (a.actionType == 18) return true;
        return false;
    }
    bool ruleNeedsModbus(const PeriphExecRule& rule) const { return ruleHasPollCollectionAction(rule); }
};

// PRODUCTION_METHODS

static std::string rule(const std::string& id = "new", int interval = 8) {
    return "{\"id\":\"" + id + "\",\"name\":\"A retained rule\",\"triggers\":[{\"triggerType\":1,\"intervalSec\":" +
        std::to_string(interval) + "}],\"actions\":[{\"actionType\":8}]}";
}
static std::string config(const std::string& r = rule()) { return "{\"rules\":[" + r + "]}"; }
static std::string ruleControl(const std::string& id, const std::string& target, int type = 22, bool enabled = false) {
    JsonDocument doc; CHECK(!deserializeJson(doc, rule(id)));
    doc["enabled"] = enabled;
    doc["actions"][0]["actionType"] = type;
    doc["actions"][0]["targetPeriphId"] = target;
    std::string result; serializeJson(doc, result); return result;
}
static uint64_t retainedActor = 0;
static uint32_t retainedEpoch = 0;
static void reset(PeriphExecManager& m, const std::string& content) {
    failAfter = -1;
    LittleFS = {};
    FastBee::MemoryBudget::free8 = FastBee::MemoryBudget::largest8 = 1000000;
    m = PeriphExecManager();
    PeriphExecRule old;
    old.id = "old"; old.name = "Old state retained";
    old.outputActor = retainedActor = FastBee::RuleOutputIdentity::allocateActor();
    retainedEpoch = FastBee::RuleOutputIdentity::currentEpoch();
    old.triggers.emplace_back(); old.actions.emplace_back();
    m.rules.emplace(old.id, old);
    m._irKeysJson = "[\"old\"]";
    m._buttonEventCache.insert("old-button");
    m._dataSourceCache.insert("old-source");
    LittleFS.files[PERIPH_EXEC_CONFIG_FILE] = content;
}
static void retained(const PeriphExecManager& m, const std::string& content) {
    CHECK(m.rules.size() == 1 && m.rules.count("old") == 1);
    CHECK(m.rules.at("old").name == "Old state retained");
    CHECK(m.rules.at("old").outputActor == retainedActor);
    CHECK(FastBee::RuleOutputIdentity::currentEpoch() == retainedEpoch);
    CHECK(m._irKeysJson == "[\"old\"]");
    CHECK(m._buttonEventCache.count("old-button") == 1);
    CHECK(m._dataSourceCache.count("old-source") == 1);
    CHECK(LittleFS.files.at(PERIPH_EXEC_CONFIG_FILE) == content);
}
int main(int argc, char** argv) {
    PeriphExecManager m;
    String error;
    if (argc > 1) {
        std::ifstream input(argv[1], std::ios::binary);
        const std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        reset(m, content);
        CHECK(PeriphExecManager::validateConfigurationFile(PERIPH_EXEC_CONFIG_FILE, error));
        CHECK(m.loadConfiguration());
        printf("snapshot rules=%u validated and loaded\n", static_cast<unsigned>(m.rules.size()));
        return 0;
    }
    // Boot-load creates distinct output actors without trusting serialized
    // runtime fields. Reload retires prior queued output contexts, even when
    // the durable rule content did not change.
    reset(m, config(rule("first") + "," + rule("second")));
    const uint32_t beforeLoadEpoch = FastBee::RuleOutputIdentity::currentEpoch();
    CHECK(m.loadConfiguration());
    const uint64_t firstActor = m.rules.at("first").outputActor;
    const uint64_t secondActor = m.rules.at("second").outputActor;
    CHECK(firstActor && secondActor && firstActor != secondActor);
    const uint32_t loadedEpoch = FastBee::RuleOutputIdentity::currentEpoch();
    CHECK(FastBee::RuleOutputIdentity::matches(loadedEpoch));
    CHECK(!FastBee::RuleOutputIdentity::matches(beforeLoadEpoch));
    CHECK(m.saveConfiguration());
    CHECK(LittleFS.files.at(PERIPH_EXEC_CONFIG_FILE).find("outputActor") == std::string::npos);
    CHECK(m.loadConfiguration());
    CHECK(m.rules.at("first").outputActor && m.rules.at("first").outputActor != firstActor);
    CHECK(m.rules.at("second").outputActor && m.rules.at("second").outputActor != secondActor);
    CHECK(m.rules.at("first").outputActor != m.rules.at("second").outputActor);
    CHECK(!FastBee::RuleOutputIdentity::matches(loadedEpoch));
    const auto reloadActor = m.rules.at("first").outputActor;
    const auto reloadRevision = m._ruleRuntimeRevision;
    const auto reloadEpoch = FastBee::RuleOutputIdentity::currentEpoch();
    LittleFS.files[PERIPH_EXEC_CONFIG_FILE] = "{";
    CHECK(!m.loadConfiguration());
    CHECK(m.rules.size() == 2 && m.rules.at("first").outputActor == reloadActor);
    CHECK(m._ruleRuntimeRevision == reloadRevision);
    CHECK(FastBee::RuleOutputIdentity::matches(reloadEpoch));

    // Actual add/update/enable/save/load methods retain only the current
    // boot's accepted daily occurrence, never client-supplied runtime fields.
    reset(m, config(rule("daily"))); CHECK(m.loadConfiguration());
    PeriphExecRule daily = m.rules.at("daily");
    daily.triggers[0].timerMode = 1; daily.triggers[0].timePoint = "06:00";
    daily.triggers[0].lastCalendarDay = 20991231;
    CHECK(m.updateRuleLocked("daily", daily, error));
    CHECK(m.rules.at("daily").triggers[0].lastCalendarDay == 0);
    m.rules.at("daily").triggers[0].lastCalendarDay = 20261004;
    daily = m.rules.at("daily");
    ExecTrigger later = daily.triggers[0];later.timePoint="07:00";later.lastCalendarDay=20991231;
    daily.triggers.push_back(later);daily.name="Renamed daily rule";
    CHECK(m.updateRuleLocked("daily", daily, error));
    CHECK(m.rules.at("daily").triggers[0].lastCalendarDay == 20261004);
    CHECK(m.rules.at("daily").triggers[1].lastCalendarDay == 0);
    m.rules.at("daily").triggers[1].lastCalendarDay = 20261005;
    daily = m.rules.at("daily");std::swap(daily.triggers[0],daily.triggers[1]);
    CHECK(m.updateRuleLocked("daily", daily, error));
    CHECK(m.rules.at("daily").triggers[0].lastCalendarDay == 20261005);
    CHECK(m.rules.at("daily").triggers[1].lastCalendarDay == 20261004);
    CHECK(m.setRuleEnabledLocked("daily", false, error));
    CHECK(m.setRuleEnabledLocked("daily", true, error));
    CHECK(m.rules.at("daily").triggers[0].lastCalendarDay == 20261005);
    daily = m.rules.at("daily");daily.triggers[0].timePoint="08:00";
    CHECK(m.updateRuleLocked("daily", daily, error));
    CHECK(m.rules.at("daily").triggers[0].lastCalendarDay == 0);
    CHECK(m.rules.at("daily").triggers[1].lastCalendarDay == 20261004);
    daily = m.rules.at("daily");daily.name="";
    CHECK(!m.updateRuleLocked("daily", daily, error));
    CHECK(m.rules.at("daily").triggers[1].lastCalendarDay == 20261004);
    daily = m.rules.at("daily");daily.id="new_daily";
    CHECK(m.addRuleLocked(daily, error));
    CHECK(m.rules.at("new_daily").triggers[1].lastCalendarDay == 0);
    CHECK(m.saveConfiguration());
    CHECK(LittleFS.files.at(PERIPH_EXEC_CONFIG_FILE).find("lastCalendarDay") == std::string::npos);
    CHECK(m.loadConfiguration());
    CHECK(m.rules.at("daily").triggers[1].lastCalendarDay == 0);
    reset(m, "{\"version\":2,\"rules\":[" + rule() + "],\"irKeys\":[{\"name\":\"k\",\"code\":3},{\"name\":\"k2\",\"code\":4}]}");
    CHECK(PeriphExecManager::validateConfigurationFile(PERIPH_EXEC_CONFIG_FILE, error));
    CHECK(m.loadConfiguration());
    CHECK(m.rules.size() == 1 && m.rules.at("new").triggers.size() == 1);
    CHECK(!m._configurationLoadFailed);
    CHECK(m._ruleRuntimeRevision == 1 && m.rules.at("new").runtimeRevision == 1);
    CHECK(m._irKeysJson == "[{\"name\":\"k\",\"code\":3},{\"name\":\"k2\",\"code\":4}]");
    CHECK(m.saveConfiguration());
    CHECK(m.loadConfiguration());
    CHECK(m.rules.at("new").triggers[0].intervalSec == 8);
    CHECK(m._ruleRuntimeRevision == 2 && m.rules.at("new").runtimeRevision == 2);

    auto booleans = rule();
    booleans.insert(1, "\"enabled\":0,\"reportAfterExec\":0,\"allowPanelControl\":1,\"recordHistory\":1,");
    const size_t actionAt = booleans.find("\"actionType\":8");
    booleans.insert(actionAt, "\"useReceivedValue\":1,");
    reset(m, config(booleans)); CHECK(m.loadConfiguration());
    const auto& decoded = m.rules.at("new");
    CHECK(!decoded.enabled && !decoded.reportAfterExec && decoded.allowPanelControl && decoded.recordHistory);
    CHECK(decoded.actions[0].useReceivedValue);
    auto maxInterval = rule();
    maxInterval.replace(maxInterval.find("\"intervalSec\":8"), 15, "\"intervalSec\":4294967295");
    reset(m, config(maxInterval)); CHECK(m.loadConfiguration());
    CHECK(m.rules.at("new").triggers[0].intervalSec == PERIPH_EXEC_MAX_INTERVAL_SEC);

    std::vector<std::string> invalid = {"{", "[]", "{}", "{\"rules\":{}}", "{\"rules\":null}",
        "{\"rules\":[42]}", "{\"rules\":[],}", "{\"rules\":[]}x", "{\"rules\":[],\"rules\":[]}",
        config(rule() + "," + rule()), config(rule(std::string(41, 'a'))),
        "{\"rules\":[],\"irKeys\":false}", "{\"rules\":[" + rule() + ",]}", "{\"rules\\u0000extra\":[]}"};
    for (const auto& change : std::vector<std::pair<std::string,std::string>> {
        {"\"name\":\"A retained rule\"", "\"name\":7"},
        {"\"id\":\"new\"", "\"id\":\"n\\u0000ew\""},
        {"\"triggers\":[{\"triggerType\":1,\"intervalSec\":8}]", "\"triggers\":[]"},
        {"\"actions\":[{\"actionType\":8}]", "\"actions\":[{}, {}, {}, {}, {}]"},
        {"\"triggers\":[{\"triggerType\":1,\"intervalSec\":8}]", "\"triggers\":[{}, {}, {}, {}]"},
        {"\"actionType\":8", "\"actionType\":256"}, {"\"triggerType\":1", "\"triggerType\":-1"},
        {"\"actionType\":8", "\"actionType\":8,\"actionValue\":\"" + std::string(1025,'x') + "\""}}) {
        auto text = config(); size_t at = text.find(change.first); CHECK(at != std::string::npos);
        text.replace(at, change.first.size(), change.second); invalid.push_back(text);
    }
    std::string many;
    for (size_t i=0; i<FastBee::ResourceProfile::MAX_PERIPH_EXEC_RULES+1; ++i) {
        if (i) many += ',';
        many += rule("rule" + std::to_string(i));
    }
    invalid.push_back(config(many));
    invalid.push_back(std::string(FastBee::PeriphExecConfig::kMaxBytes + 1, ' '));
    for (const auto& content : invalid) {
        reset(m, content);
        CHECK(!PeriphExecManager::validateConfigurationFile(PERIPH_EXEC_CONFIG_FILE, error));
        CHECK(!m.loadConfiguration()); retained(m, content);
        CHECK(!m.saveConfiguration()); retained(m, content);
        CHECK(!m.setIrKeysJson("[]")); retained(m, content);
        CHECK(!m.removeRuleLocked("old", error));
        CHECK(error == "CONFIG_LOAD_FAILED");
    }

    // Exact rule count accepted by file and in-memory schema checks.
    many.clear();
    for (size_t i=0; i<FastBee::ResourceProfile::MAX_PERIPH_EXEC_RULES; ++i) {
        if (i) many += ',';
        many += rule("rule" + std::to_string(i));
    }
    reset(m, config(many));
    CHECK(PeriphExecManager::validateConfigurationFile(PERIPH_EXEC_CONFIG_FILE, error));
    CHECK(m.loadConfiguration()); CHECK(m.rules.size() == FastBee::ResourceProfile::MAX_PERIPH_EXEC_RULES);
    CHECK(m.saveConfiguration());
    const auto exact = LittleFS.files.at(PERIPH_EXEC_CONFIG_FILE);
    JsonDocument doc; CHECK(!deserializeJson(doc, exact));
    CHECK(!FastBee::PeriphExecConfig::validateDocument(doc.as<JsonVariantConst>()));
    // Aggregate resident estimate refuses a combination that individually fits.
    reset(m, config(many)); FastBee::MemoryBudget::free8 = 16000;
    CHECK(!PeriphExecManager::validateConfigurationFile(PERIPH_EXEC_CONFIG_FILE, error));
    CHECK(error == "CONFIG_MEMORY_PRESSURE");

    const std::string ir = "[\"" + std::string(5000, 'i') + "\"]";
    for (bool irFirst : {false, true}) {
        reset(m, irFirst ? "{\"irKeys\":" + ir + ",\"rules\":[" + many + "]}" :
            "{\"rules\":[" + many + "],\"irKeys\":" + ir + "}");
        FastBee::MemoryBudget::free8 = 20000;
        CHECK(!PeriphExecManager::validateConfigurationFile(PERIPH_EXEC_CONFIG_FILE, error));
        CHECK(error == "CONFIG_MEMORY_PRESSURE");
    }

    // Low total memory, fragmented memory, and pressure appearing mid-parse.
    for (int mode=0; mode<3; ++mode) {
        const auto content = config(); reset(m, content);
        if (mode == 0) FastBee::MemoryBudget::free8 = 8192;
        if (mode == 1) FastBee::MemoryBudget::largest8 = 8;
        if (mode == 2) LittleFS.onRead = [](size_t n) { if (n > 100) FastBee::MemoryBudget::free8 = 8192; };
        CHECK(!m.loadConfiguration()); retained(m, content);
    }
    // Throw at every successive C++ allocation until a complete load is reached.
    bool reachedSuccess = false;
    for (int nth=0; nth<150; ++nth) {
        const auto content = config(); reset(m, content);
        failAfter = nth;
        bool loaded = m.loadConfiguration();
        failAfter = -1;
        if (loaded) { reachedSuccess = true; break; }
        retained(m, content);
    }
    CHECK(reachedSuccess);

    // IR migration must not delete the old file before committing the main file.
    for (int mode=0; mode<4; ++mode) {
        const auto content = config(rule("new", 0)); reset(m, content);
        LittleFS.files["/config/ir_keys.json"] = "{\"keys\":[{\"name\":\"legacy\",\"code\":7}]}";
        if (mode == 0) LittleFS.failOpen = "/config/periph_exec.json.tmp";
        if (mode == 1) LittleFS.writeLimit = 8;
        if (mode == 2) LittleFS.failRename = "/config/periph_exec.json.tmp";
        if (mode == 3) LittleFS.files["/config/ir_keys.json"] = "{";
        CHECK(!m.loadConfiguration()); retained(m, content);
        CHECK(LittleFS.exists("/config/ir_keys.json"));
    }
    reset(m, config(rule("new", 0)));
    LittleFS.files["/config/ir_keys.json"] = "{\"keys\":[{\"name\":\"legacy\",\"code\":7}]}";
    CHECK(m.loadConfiguration()); CHECK(!LittleFS.exists("/config/ir_keys.json"));
    CHECK(m.rules.at("new").triggers[0].intervalSec == 1);
    CHECK(m.loadConfiguration()); CHECK(m._irKeysJson == "[{\"name\":\"legacy\",\"code\":7}]");

    // New imports resolve rule-control targets against the complete replacement,
    // never against the old live map. Disabled rules also need valid references.
    const auto referenceOriginal = config(rule("old"));
    for (int type : {22, 23}) for (bool enabled : {false, true}) {
        for (const auto& target : {std::string(), std::string("missing"), std::string("old"),
                std::string("NEW"), std::string(" new"), std::string(41, 'x')}) {
            reset(m, referenceOriginal);
            const auto staged = config(ruleControl("new", target, type, enabled));
            LittleFS.files["/config/staged.tmp"] = staged;
            CHECK(!m.importConfigurationFile("/config/staged.tmp", error));
            CHECK(error == "CONFIG_RULE_REFERENCE" && !m._configurationRestartPending);
            retained(m, referenceOriginal);
            CHECK(LittleFS.files.at("/config/staged.tmp") == staged);
        }
    }
    // Forward, backward, self, mutual and profile-limit-length IDs all remain legal.
    for (int shape = 0; shape < 3; ++shape) {
        JsonDocument input; CHECK(!deserializeJson(input, config(ruleControl("new", "new"))));
        auto action = input["rules"][0]["actions"][0].as<JsonObject>();
        if (shape == 0) action.remove("targetPeriphId");
        if (shape == 1) action["targetPeriphId"] = nullptr;
        if (shape == 2) action["targetPeriphId"] = std::string("new\0other", 9);
        std::string staged; serializeJson(input, staged);
        reset(m, referenceOriginal); LittleFS.files["/config/staged.tmp"] = staged;
        CHECK(!m.importConfigurationFile("/config/staged.tmp", error));
        CHECK(error == (shape == 2 ? "CONFIG_ACTION_TEXT" : "CONFIG_RULE_REFERENCE"));
        CHECK(!m._configurationRestartPending); retained(m, referenceOriginal);
    }
    const std::string maximumId(FastBee::ResourceProfile::MAX_RULE_ID_LEN, 'z');
    for (const auto& content : {config(ruleControl("new", "later") + "," + rule("later")),
            config(rule("earlier") + "," + ruleControl("new", "earlier", 23)),
            config(ruleControl("new", "new")),
            config(ruleControl("first", "second") + "," + ruleControl("second", "first", 23)),
            config(ruleControl("new", maximumId) + "," + rule(maximumId))}) {
        reset(m, referenceOriginal); LittleFS.files["/config/staged.tmp"] = content;
        CHECK(m.importConfigurationFile("/config/staged.tmp", error));
        CHECK(m._configurationRestartPending && m.rules.count("old") == 1);
        CHECK(LittleFS.files.at(PERIPH_EXEC_CONFIG_FILE) == content);
    }
    // Other target namespaces are not rule IDs (events, devices, remote sources).
    for (int type : {8, 19, 21, 34, 36}) {
        reset(m, referenceOriginal);
        LittleFS.files["/config/staged.tmp"] = config(ruleControl("new", "not-a-rule", type));
        CHECK(m.importConfigurationFile("/config/staged.tmp", error));
    }
    // Legacy load/save remain schema-compatible; only new import is stricter.
    reset(m, config(ruleControl("legacy", "missing")));
    CHECK(m.loadConfiguration()); CHECK(!m.rules.at("legacy").enabled); CHECK(m.saveConfiguration());
    // Full-profile forward references use a fixed index, including its last slot.
    many.clear();
    for (size_t i = 0; i < FastBee::ResourceProfile::MAX_PERIPH_EXEC_RULES; ++i) {
        if (i) many += ',';
        many += ruleControl("r" + std::to_string(i), "r" + std::to_string(FastBee::ResourceProfile::MAX_PERIPH_EXEC_RULES - 1));
    }
    reset(m, referenceOriginal); LittleFS.files["/config/staged.tmp"] = config(many);
    CHECK(m.importConfigurationFile("/config/staged.tmp", error));
    // The second pass still fails closed on renewed pressure; no partial commit.
    reset(m, referenceOriginal); LittleFS.files["/config/staged.tmp"] = config(ruleControl("new", "new"));
    int parseStarts = 0;
    LittleFS.onRead = [&](size_t position) { if (position == 1 && ++parseStarts == 2) FastBee::MemoryBudget::free8 = 8192; };
    CHECK(!m.importConfigurationFile("/config/staged.tmp", error));
    CHECK(error == "CONFIG_MEMORY_PRESSURE" && !m._configurationRestartPending);
    retained(m, referenceOriginal);

    // Pending restart changes the durable file only and blocks ALL old-runtime saves.
    reset(m, config(rule("old")));
    const auto original = LittleFS.files.at(PERIPH_EXEC_CONFIG_FILE);
    LittleFS.files["/config/staged.tmp"] = config();
    m._weatherReferenceLease = true;
    CHECK(!m.importConfigurationFile("/config/staged.tmp", error)); CHECK(error == "WEATHER_MIGRATION_BUSY");
    CHECK(!m.saveConfiguration() && !m.loadConfiguration() && !m.setIrKeysJson("[]"));
    CHECK(!m.removeRuleLocked("old", error) && error == "WEATHER_MIGRATION_BUSY");
    PeriphExecRule incoming = m.rules.at("old"); incoming.id = "added";
    CHECK(!m.addRuleLocked(incoming, error) && error == "WEATHER_MIGRATION_BUSY");
    CHECK(!m.updateRuleLocked("old", incoming, error) && error == "WEATHER_MIGRATION_BUSY");
    CHECK(!m.setRuleEnabledLocked("old", false, error) && error == "WEATHER_MIGRATION_BUSY");
    CHECK(m.rules.at("old").enabled && LittleFS.files.at(PERIPH_EXEC_CONFIG_FILE) == original);
    m._weatherReferenceLease = false;
    CHECK(m.importConfigurationFile("/config/staged.tmp", error));
    CHECK(m._configurationRestartPending && m.rules.count("old"));
    const auto imported = LittleFS.files.at(PERIPH_EXEC_CONFIG_FILE);
    CHECK(imported != original && !LittleFS.exists("/config/staged.tmp"));
    CHECK(!m.saveConfiguration()); CHECK(!m.setIrKeysJson("[]")); CHECK(!m.loadConfiguration());
    CHECK(!m.removeRuleLocked("old", error)); CHECK(error == "CONFIG_PENDING_RESTART");
    CHECK(!m.setRuleEnabledLocked("old", false, error)); CHECK(error == "CONFIG_PENDING_RESTART");
    CHECK(m.rules.at("old").enabled && LittleFS.files.at(PERIPH_EXEC_CONFIG_FILE) == imported);

    reset(m, original); LittleFS.files["/config/staged.tmp"] = config();
    LittleFS.failRename = "/config/staged.tmp";
    CHECK(!m.importConfigurationFile("/config/staged.tmp", error));
    CHECK(error == "CONFIG_COMMIT_FAILED" && !m._configurationRestartPending);
    retained(m, original);
    // Successful import can repair a rejected boot file without erasing its old runtime first.
    reset(m, "{"); CHECK(!m.loadConfiguration());
    LittleFS.files["/config/staged.tmp"] = config();
    CHECK(m.importConfigurationFile("/config/staged.tmp", error));
    CHECK(m.rules.count("old") && m._configurationRestartPending);

    reset(m, original); LittleFS.files.erase(PERIPH_EXEC_CONFIG_FILE);
    LittleFS.failOpen = "/config/periph_exec.json.tmp";
    CHECK(!m.loadConfiguration() && m._configurationLoadFailed);
    // Real checked IR copy/CAS must preserve external changes and durable bytes.
    reset(m, original);
    String observed;
    CHECK(m.copyIrKeysJson(observed) && observed == m._irKeysJson);
    const String replacement = "[{\"name\":\"power\",\"code\":\"0x12\",\"protocol\":\"NEC\"}]";
    CHECK(m.setIrKeysJson(replacement, &observed));
    const auto irCommittedFile = LittleFS.files.at(PERIPH_EXEC_CONFIG_FILE);
    CHECK(m._irKeysJson == replacement);
    CHECK(!m.setIrKeysJson("", &observed));
    CHECK(m._irKeysJson == replacement && LittleFS.files.at(PERIPH_EXEC_CONFIG_FILE) == irCommittedFile);
    CHECK(m.copyIrKeysJson(observed));
    LittleFS.failOpen = "/config/periph_exec.json.tmp";
    CHECK(!m.setIrKeysJson("", &observed));
    CHECK(m._irKeysJson == replacement && LittleFS.files.at(PERIPH_EXEC_CONFIG_FILE) == irCommittedFile);
    LittleFS.failOpen = "";
    lockAllowed = false; observed = "stale";
    CHECK(!m.copyIrKeysJson(observed) && observed.isEmpty());
    lockAllowed = true;
    CHECK(m.copyIrKeysJson(observed));
    LittleFS.files["/config/staged.tmp"] = config(rule("imported"));
    CHECK(m.importConfigurationFile("/config/staged.tmp", error));
    const auto importedIrFile = LittleFS.files.at(PERIPH_EXEC_CONFIG_FILE);
    // Import retains old runtime JSON but its pending flag must still reject CAS.
    CHECK(!m.setIrKeysJson("", &observed));
    CHECK(LittleFS.files.at(PERIPH_EXEC_CONFIG_FILE) == importedIrFile && m._irKeysJson == replacement);
    CHECK(!m.copyIrKeysJson(observed) && observed.isEmpty());
    // Exhaustion cannot wrap identities, publish a partial candidate, or write
    // a normalized/migrated file before rejecting. Counter resets below model
    // independent boots in this host fixture only; production never rewinds.
    for (int mode = 0; mode < 5; ++mode) {
        const auto content = config(rule("first", 0) + "," + rule("second"));
        reset(m, content);
        const auto actorCounter = FastBee::RuleOutputIdentity::actorStorage().load();
        const auto epochCounter = FastBee::RuleOutputIdentity::currentEpoch();
        LittleFS.files["/config/ir_keys.json"] = "{\"keys\":[{\"name\":\"legacy\",\"code\":7}]}";
        const auto legacyFile = LittleFS.files.at("/config/ir_keys.json");
        if (mode == 0) FastBee::RuleOutputIdentity::actorStorage().store(UINT32_MAX);
        if (mode == 1) FastBee::RuleOutputIdentity::actorStorage().store(UINT32_MAX - 1);
        if (mode == 2) FastBee::RuleOutputIdentity::epochStorage().store(0);
        if (mode == 3) FastBee::RuleOutputIdentity::epochStorage().store(UINT32_MAX);
        if (mode == 4) m._ruleRuntimeRevision = UINT64_MAX - 1;
        retainedEpoch = FastBee::RuleOutputIdentity::currentEpoch();
        const auto priorRevision = m._ruleRuntimeRevision;
        CHECK(!m.loadConfiguration() && m._configurationLoadFailed);
        retained(m, content);
        CHECK(m._ruleRuntimeRevision == priorRevision);
        CHECK(LittleFS.files.at("/config/ir_keys.json") == legacyFile);
        CHECK(!LittleFS.exists("/config/periph_exec.json.tmp"));
        if (mode < 2) CHECK(FastBee::RuleOutputIdentity::actorStorage().load() == UINT32_MAX);
        FastBee::RuleOutputIdentity::actorStorage().store(actorCounter);
        FastBee::RuleOutputIdentity::epochStorage().store(epochCounter);
    }
    puts("config validation, rollback, memory, migration, output identities and IR CAS passed");
}


