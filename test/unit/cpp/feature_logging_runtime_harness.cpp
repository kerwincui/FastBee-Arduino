#include <ArduinoJson.h>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

using String = std::string;
static std::vector<std::string> events;

namespace FastBee {
struct RuntimeFeatureManager {
    enum class ConfigUpdateResult { SAVED, NO_VALID_ENTRIES, SAVE_FAILED };
    struct UserConfigSnapshot { bool logging, mdns, screen; };
    bool logging = false, mdns = true, screen = true;
    bool compiled = true, budget = true, runtimeDisabled = false;
    bool rollbackSucceeds = true;
    ConfigUpdateResult nextResult = ConfigUpdateResult::SAVED;
    static RuntimeFeatureManager& instance() { static RuntimeFeatureManager m; return m; }
    bool isUserEnabled(const char* key) const {
        return std::string(key) == "fileLogging" ? logging :
               std::string(key) == "mdns" ? mdns : screen;
    }
    bool shouldEnable(const char* key) const {
        assert(std::string(key) == "fileLogging");
        return logging && compiled && budget && !runtimeDisabled;
    }
    ConfigUpdateResult updateAndSave(const JsonDocument& doc, UserConfigSnapshot* previous) {
        *previous = {logging, mdns, screen};
        if (nextResult != ConfigUpdateResult::SAVED) return nextResult;
        events.emplace_back("save");
        auto feature = doc["features"].as<JsonObjectConst>();
        if (feature["fileLogging"]["enabled"].is<bool>())
            logging = feature["fileLogging"]["enabled"].as<bool>();
        if (feature["mdns"]["enabled"].is<bool>())
            mdns = feature["mdns"]["enabled"].as<bool>();
        if (feature["screenPanel"]["enabled"].is<bool>())
            screen = feature["screenPanel"]["enabled"].as<bool>();
        return nextResult;
    }
    bool restoreUserConfig(const UserConfigSnapshot& saved) {
        events.emplace_back("rollback");
        logging = saved.logging; mdns = saved.mdns; screen = saved.screen;
        return rollbackSucceeds;
    }
    bool saveCurrentConfig() { return rollbackSucceeds; }
    void loadConfig() { events.emplace_back("reload"); }
    void clearRuntimeDisabled(const char* key) {
        assert(std::string(key) == "fileLogging");
        events.emplace_back("clear"); runtimeDisabled = false;
    }
};
namespace Screen {
struct ScreenRuntime {
    static ScreenRuntime& instance() { static ScreenRuntime r; return r; }
    bool start(String&) { events.emplace_back("screen:start"); return true; }
    bool stop(String*) { events.emplace_back("screen:stop"); return true; }
};
}
}

struct LoggerSystem {
    bool enabled = false, rejectEnable = false;
    int calls = 0, level = 3;
    static LoggerSystem& getInstance() { static LoggerSystem l; return l; }
    void enableFileLogging(bool requested) {
        ++calls;
        events.emplace_back(requested ? "logger:on" : "logger:off");
        if (!requested || !rejectEnable) enabled = requested;
    }
    bool isFileLoggingEnabled() const { return enabled; }
    void setLogLevel(int value) { level = value; }
    void error(const char*) {}
};
#define LOGGER LoggerSystem::getInstance()

struct ScreenPanelManager {
    static ScreenPanelManager& instance() { static ScreenPanelManager m; return m; }
    void reportApplication(bool, bool, const String&) {}
};
struct WiFiConfig { bool enableMDNS = true; };
struct NetworkManager {
    WiFiConfig config;
    bool succeeds = true;
    const WiFiConfig& getConfig() const { return config; }
    bool updateConfig(const WiFiConfig& next, bool persist) {
        assert(persist); events.emplace_back("mirror");
        if (succeeds) config = next;
        return succeeds;
    }
};
struct AsyncWebServerRequest {
    bool authenticated = true, bodyValid = true;
    int status = 0, parseCalls = 0;
    std::string error;
    JsonDocument body, response;
};
struct WebHandlerContext {
    NetworkManager* networkManager;
    bool requireAuth(AsyncWebServerRequest* request) {
        if (!request->authenticated) request->status = 401;
        return request->authenticated;
    }
    void sendBadRequest(AsyncWebServerRequest* request, const char* error) {
        request->status = 400; request->error = error;
    }
    void sendError(AsyncWebServerRequest* request, int status, const char* error) {
        request->status = status; request->error = error;
    }
};
namespace HandlerUtils {
void sendJsonStream(AsyncWebServerRequest* request, const JsonDocument& document) {
    request->response.set(document.as<JsonVariantConst>());
    request->status = 200; events.emplace_back("response");
}
}
enum class RestartReason { USER_COMMAND };
struct SystemRebooter {
    inline static int calls = 0, delay = 0;
    inline static std::string message;
    static void scheduleReboot(const char* reason, int afterMs, RestartReason kind) {
        assert(kind == RestartReason::USER_COMMAND);
        ++calls; delay = afterMs; message = reason; events.emplace_back("reboot");
    }
};
struct FeatureRouteHandler {
    enum class PersistResult { SAVED, NO_VALID_ENTRIES, FEATURE_SAVE_FAILED, MDNS_MIRROR_FAILED };
    WebHandlerContext* ctx;
    PersistResult persistFeatureUpdate(const JsonDocument& doc);
    void handleConfig(AsyncWebServerRequest* request);
    void handleApply(AsyncWebServerRequest* request);
    void handleUpdate(AsyncWebServerRequest* request, bool reboot);
    bool parseBody(AsyncWebServerRequest* request, JsonDocument& doc) {
        ++request->parseCalls;
        if (!request->bodyValid) { request->status = 400; return false; }
        doc.set(request->body.as<JsonVariantConst>()); return true;
    }
};
enum class MemoryGuardLevel { NORMAL, WARN, SEVERE, CRITICAL };
struct HealthMonitor {
    bool _degradationActive = true, _savedFileLogging = true;
    int _savedLogLevel = 1, _criticalStartTime = 30, restoreCalls = 0;
    void restoreMemoryRecovery() { ++restoreCalls; }
    void recover(MemoryGuardLevel level);
};
struct TestSerial { void println(const char*) {} } Serial;

#include "feature_logging_methods.inc"

using Manager = FastBee::RuntimeFeatureManager;
using Result = FeatureRouteHandler::PersistResult;
static void reset() {
    Manager::instance() = Manager{};
    LOGGER = LoggerSystem{};
    SystemRebooter::calls = SystemRebooter::delay = 0;
    SystemRebooter::message.clear();
    events.clear();
}
static JsonDocument update(bool enabled) {
    JsonDocument doc;
    doc["features"]["fileLogging"]["enabled"] = enabled;
    return doc;
}
static void state(bool requested, bool enabled, bool applied, bool warning) {
    JsonDocument doc;
    appendFileLoggingApplyState(doc.to<JsonObject>());
    const auto value = doc["fileLogging"];
    assert(value["requested"].as<bool>() == requested);
    assert(value["enabled"].as<bool>() == enabled);
    assert(value["applied"].as<bool>() == applied);
    assert(value["warning"].is<const char*>() == warning);
}

int main() {
    NetworkManager network;
    WebHandlerContext context{&network};
    FeatureRouteHandler handler{&context};
    auto& manager = Manager::instance();

    // Unchanged screenPanel must not skip the real logger on either direction.
    reset(); manager.logging = LOGGER.enabled = true;
    assert(handler.persistFeatureUpdate(update(false)) == Result::SAVED);
    assert(!LOGGER.enabled && LOGGER.calls == 1);
    state(false, false, true, false);
    reset();
    assert(handler.persistFeatureUpdate(update(true)) == Result::SAVED);
    assert(LOGGER.enabled && LOGGER.calls == 1);
    state(true, true, true, false);

    // Logger synchronization precedes an independent screen lifecycle change.
    reset(); auto changeScreen = update(true);
    changeScreen["features"]["screenPanel"]["enabled"] = false;
    assert(handler.persistFeatureUpdate(changeScreen) == Result::SAVED);
#if FASTBEE_ENABLE_SCREEN_UI
    assert((events == std::vector<std::string>{"save", "logger:on", "screen:stop"}));
#else
    assert((events == std::vector<std::string>{"save", "logger:on"}));
#endif

    // Failed validation or the first durable write cannot change a driver.
    reset(); manager.nextResult = Manager::ConfigUpdateResult::NO_VALID_ENTRIES;
    assert(handler.persistFeatureUpdate(update(true)) == Result::NO_VALID_ENTRIES);
    assert(LOGGER.calls == 0 && !LOGGER.enabled);
    reset(); manager.nextResult = Manager::ConfigUpdateResult::SAVE_FAILED;
    assert(handler.persistFeatureUpdate(update(true)) == Result::FEATURE_SAVE_FAILED);
    assert(LOGGER.calls == 0 && !LOGGER.enabled);

    // Do not apply logging before a required compatibility mirror commits.
    reset(); network = NetworkManager{}; network.succeeds = false;
    auto mirrored = update(true); mirrored["features"]["mdns"]["enabled"] = false;
    assert(handler.persistFeatureUpdate(mirrored) == Result::MDNS_MIRROR_FAILED);
    assert(LOGGER.calls == 0 && !manager.logging && manager.mdns);
    assert((events == std::vector<std::string>{"save", "mirror", "rollback"}));
    reset(); manager.rollbackSucceeds = false;
    assert(handler.persistFeatureUpdate(mirrored) == Result::MDNS_MIRROR_FAILED);
    assert(LOGGER.calls == 0 && events.back() == "reload");
    reset(); network = NetworkManager{};
    assert(handler.persistFeatureUpdate(mirrored) == Result::SAVED);
    assert((events == std::vector<std::string>{"save", "mirror", "logger:on"}));

    // No network object is required for changes that leave mDNS untouched.
    reset(); FeatureRouteHandler noNetwork{nullptr};
    assert(noNetwork.persistFeatureUpdate(update(true)) == Result::SAVED && LOGGER.enabled);
    reset();
    assert(noNetwork.persistFeatureUpdate(mirrored) == Result::MDNS_MIRROR_FAILED);
    assert(LOGGER.calls == 0 && !manager.logging);

    // Persisted intent is not proof of a successful real logger enable.
    reset(); network = NetworkManager{}; LOGGER.rejectEnable = true;
    assert(handler.persistFeatureUpdate(update(true)) == Result::SAVED);
    assert(manager.logging && !LOGGER.enabled);
    state(true, false, false, true);
    JsonDocument blocked;
    appendFileLoggingApplyState(blocked.to<JsonObject>());
    assert(blocked["fileLogging"]["warning"].as<std::string>() == "NOT_APPLIED");
    reset(); manager.runtimeDisabled = true;
    assert(handler.persistFeatureUpdate(update(true)) == Result::SAVED);
    assert(manager.logging && !LOGGER.enabled);
    state(true, false, false, true);
    blocked.clear();
    appendFileLoggingApplyState(blocked.to<JsonObject>());
    assert(blocked["fileLogging"]["warning"].as<std::string>() == "POLICY_DISABLED");

    // NORMAL recovery must keep the user's latest OFF choice, not saved ON.
    reset(); manager.runtimeDisabled = true;
    HealthMonitor health;
    health._savedFileLogging = true;
    health.recover(MemoryGuardLevel::NORMAL);
    assert(!LOGGER.enabled && LOGGER.calls == 1 && !health._degradationActive);
    assert(health.restoreCalls == 1 && health._criticalStartTime == 0);
    assert(LOGGER.level == health._savedLogLevel);
    assert((events == std::vector<std::string>{"clear", "logger:off"}));

    // Clear only the temporary guard before reading current ON policy.
    reset(); manager.logging = manager.runtimeDisabled = true; health = HealthMonitor{};
    health._savedFileLogging = false;
    health.recover(MemoryGuardLevel::NORMAL);
    assert(LOGGER.enabled && !manager.runtimeDisabled);
    assert((events == std::vector<std::string>{"clear", "logger:on"}));
    for (bool deniedByBuild : {false, true}) {
        reset(); manager.logging = manager.runtimeDisabled = true; health = HealthMonitor{};
        if (deniedByBuild) manager.compiled = false;
        else manager.budget = false;
        health.recover(MemoryGuardLevel::NORMAL);
        assert(!LOGGER.enabled && !manager.runtimeDisabled);
    }

    // Repeated NORMAL samples must not repeatedly apply/restart the logger.
    reset(); health = HealthMonitor{}; health._degradationActive = false;
    health.recover(MemoryGuardLevel::NORMAL);
    assert(LOGGER.calls == 0 && health.restoreCalls == 0);

    // Exercise both real entry points, not a reconstruction of their dispatch.
    for (bool apply : {false, true}) {
        auto invoke = [&](AsyncWebServerRequest* request) {
            if (apply) handler.handleApply(request);
            else handler.handleConfig(request);
        };
        reset(); network = NetworkManager{};
        AsyncWebServerRequest success;
        success.body.set(update(true)); success.body["inputMarker"] = "must not leak into response";
        invoke(&success);
        assert(success.status == 200 && success.response["success"].as<bool>());
        assert(success.response["features"].isNull() && success.response["inputMarker"].isNull());
        const auto logging = success.response["data"]["fileLogging"];
        assert(logging["requested"].as<bool>() && logging["enabled"].as<bool>() && logging["applied"].as<bool>());
        if (apply) {
            assert(success.response["data"]["rebootIn"].as<int>() == 3);
            assert(success.response["data"]["requiresRestart"].isNull());
            assert(success.response["message"].isNull());
            assert(SystemRebooter::calls == 1 && SystemRebooter::delay == 3000);
            assert(SystemRebooter::message == "Feature config applied");
            assert(events[events.size() - 2] == "response" && events.back() == "reboot");
        } else {
            assert(success.response["data"]["requiresRestart"].as<bool>());
            assert(success.response["data"]["rebootIn"].isNull());
            assert(success.response["message"].as<std::string>() == "Feature config saved (reboot required to apply)");
            assert(SystemRebooter::calls == 0 && events.back() == "response");
        }

        reset(); AsyncWebServerRequest unauthenticated;
        unauthenticated.authenticated = false; invoke(&unauthenticated);
        assert(unauthenticated.status == 401 && unauthenticated.parseCalls == 0);
        assert(events.empty() && SystemRebooter::calls == 0 && LOGGER.calls == 0);

        reset(); AsyncWebServerRequest malformed;
        malformed.bodyValid = false; invoke(&malformed);
        assert(malformed.status == 400 && malformed.parseCalls == 1);
        assert(events.empty() && SystemRebooter::calls == 0 && LOGGER.calls == 0);

        for (auto failure : {Manager::ConfigUpdateResult::NO_VALID_ENTRIES, Manager::ConfigUpdateResult::SAVE_FAILED}) {
            reset(); manager.nextResult = failure; AsyncWebServerRequest request;
            request.body.set(update(true)); invoke(&request);
            if (failure == Manager::ConfigUpdateResult::NO_VALID_ENTRIES) {
                assert(request.status == 400 && request.error == "No valid feature entries");
            } else {
                assert(request.status == 500 && request.error == "Failed to save feature config");
            }
            assert(events.empty() && SystemRebooter::calls == 0 && LOGGER.calls == 0);
        }

        reset(); network.succeeds = false; AsyncWebServerRequest mirrorFailure;
        mirrorFailure.body.set(mirrored); invoke(&mirrorFailure);
        assert(mirrorFailure.status == 500 && mirrorFailure.error == "Failed to synchronize mDNS compatibility config");
        assert(!manager.logging && manager.mdns);
        assert(SystemRebooter::calls == 0 && LOGGER.calls == 0);
        assert((events == std::vector<std::string>{"save", "mirror", "rollback"}));

        reset(); network = NetworkManager{}; LOGGER.rejectEnable = true;
        AsyncWebServerRequest deferred;
        deferred.body.set(update(true)); invoke(&deferred);
        assert(deferred.status == 200 && manager.logging && !LOGGER.enabled);
        const auto deferredLogging = deferred.response["data"]["fileLogging"];
        assert(deferredLogging["requested"].as<bool>() && !deferredLogging["enabled"].as<bool>() && !deferredLogging["applied"].as<bool>());
        assert(deferredLogging["warning"].as<std::string>() == "NOT_APPLIED");
    }
}
