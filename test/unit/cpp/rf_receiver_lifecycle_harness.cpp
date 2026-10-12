#include <Arduino.h>
#undef malloc
#undef free
#include <ArduinoJson.h>
#include "utils/ConsumedStringJsonReader.h"
#include "utils/SharedJsonDeserializer.h"
#include <LittleFS.h>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <new>
#include <stdexcept>
#include <utility>
#include "core/FeatureInstanceRegistry.h"
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); std::exit(86); } } while (false)
inline void convertToJson(const String& value, JsonVariant output) { output.set(std::string(value)); }
inline void convertFromJson(JsonVariantConst value, String& output) { output = value.as<std::string>(); }
static thread_local bool failNextAllocation = false;
void* operator new(size_t size) { if (failNextAllocation) { failNextAllocation = false; throw std::bad_alloc(); }
    void* p = std::malloc(size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
static uint32_t clockMs = 1000, clockUs = 1000000;
static unsigned long rfMillis() { return clockMs; }
static unsigned long rfMicros() { return clockUs; }
#define millis rfMillis
#define micros rfMicros
#define IRAM_ATTR
using TickType_t = uint32_t;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
constexpr int pdTRUE = 1;
struct FakeMutex { std::timed_mutex mutex; };
using SemaphoreHandle_t = FakeMutex*;
static thread_local unsigned rxLockDepth = 0;
static SemaphoreHandle_t xSemaphoreCreateMutex() { return new FakeMutex; }
static void vSemaphoreDelete(SemaphoreHandle_t mutex) { delete mutex; }
static int xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t wait) {
    if (!mutex) return 0;
    if (rxLockDepth) { CHECK(wait == 0); return 0; }
    const bool held = wait == 0 ? mutex->mutex.try_lock() : mutex->mutex.try_lock_for(std::chrono::seconds(2));
    if (held) ++rxLockDepth;
    return held ? pdTRUE : 0;
}
static void xSemaphoreGive(SemaphoreHandle_t mutex) { CHECK(rxLockDepth == 1); --rxLockDepth; mutex->mutex.unlock(); }
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
static thread_local unsigned isrLockDepth = 0;
static void portENTER_CRITICAL(portMUX_TYPE* mutex) { CHECK(isrLockDepth == 0); mutex->lock(); ++isrLockDepth; }
static void portEXIT_CRITICAL(portMUX_TYPE* mutex) { CHECK(isrLockDepth == 1); --isrLockDepth; mutex->unlock(); }
#define portENTER_CRITICAL_ISR portENTER_CRITICAL
#define portEXIT_CRITICAL_ISR portEXIT_CRITICAL
constexpr int INPUT = 1, CHANGE = 2;
static void (*attachedCallback)(void*) = nullptr;
static void* attachedArgument = nullptr;
static int attaches = 0, detaches = 0, configuredPin = -1;
static int digitalPinToInterrupt(int pin) { return pin; }
static void pinMode(int pin, int) { CHECK(rxLockDepth == 1); configuredPin = pin; }
static void attachInterruptArg(int, void (*callback)(void*), void* argument, int) {
    CHECK(rxLockDepth == 1); attachedCallback = callback; attachedArgument = argument; ++attaches;
}
static void detachInterrupt(int) { CHECK(rxLockDepth == 1); attachedCallback = nullptr; ++detaches; }
namespace FastBee { struct RuntimeFeatureManager {
    static RuntimeFeatureManager& instance() { static RuntimeFeatureManager value; return value; }
    bool shouldEnable(const char*) const { return true; }
}; }
static bool filesystemReady = true;
struct FileUtils {
    static bool isInitialized() { CHECK(rxLockDepth == 0); return filesystemReady; }
    static File openAtomicWrite(const String& path) { CHECK(rxLockDepth == 0); return LittleFS.open(path + ".tmp", "w"); }
    static bool commitAtomicWrite(const String&, const String&);
};
#define LOG_ERRORF(...) do {} while (false)
#include "rf_file_commit.inc"
static unsigned dispatches = 0;
static String lastEvent, lastPayload;
static std::function<void()> onDispatch;
#if FASTBEE_ENABLE_PERIPH_EXEC
struct PeriphExecManager {
    static PeriphExecManager& getInstance() { static PeriphExecManager instance; return instance; }
    void dispatchEventMatchedRules(const String& event, const String& payload) {
        CHECK(rxLockDepth == 0 && isrLockDepth == 0); ++dispatches; lastEvent = event; lastPayload = payload;
        if (onDispatch) onDispatch();
    }
};
#endif
#include "rf_declaration.inc"
#include "rf_driver.inc"
#define LOG_INFOF(...) do {} while (false)
constexpr uint8_t RF_MODE_RX = 1;
enum class PeripheralType { RF_MODULE };
struct RfPmConfig {
    PeripheralType type = PeripheralType::RF_MODULE;
    uint8_t pinCount = 1, pins[1] = {16};
    struct { struct { uint8_t mode = RF_MODE_RX; } rf; } params;
};
static bool pmInitializeReceiver(const RfPmConfig& config) {
    const uint8_t pin = config.pins[0];
#include "rf_pm_init.inc"
    return config.params.rf.mode == RF_MODE_RX;
}
static bool pmStopReceiver(const RfPmConfig& config) {
#include "rf_pm_stop.inc"
    return true;
}
struct Param { String text; String value() const { return text; } };
struct AsyncWebServerRequest {
    std::map<String, Param> parameters; int status = 0; String body;
    bool hasParam(const char* name, bool) const { return parameters.count(name) != 0; }
    Param* getParam(const char* name, bool) { return &parameters.at(name); }
    void send(int code, const char*, const String& response) { status = code; body = response; }
};
struct WebHandlerContext {
    bool authenticated = true, developer = true;
    bool requireAuth(AsyncWebServerRequest* r) { if (!authenticated) r->status = 401; return authenticated; }
    bool requireDeveloperMode(AsyncWebServerRequest* r) { if (!developer) r->status = 403; return developer; }
    void sendError(AsyncWebServerRequest* r, int code, const String& error) { r->status = code; r->body = error; }
    void sendSuccess(AsyncWebServerRequest* r, const char* message) { r->status = 200; r->body = message; }
    void sendSuccess(AsyncWebServerRequest* r, JsonDocument& doc) { r->status = 200; std::string out; serializeJson(doc, out); r->body = out; }
};
class RfRouteHandler {
public:
    explicit RfRouteHandler(WebHandlerContext* value) : ctx(value) {}
    WebHandlerContext* ctx;
    void handleStatus(AsyncWebServerRequest*); void handleLearnStart(AsyncWebServerRequest*);
    void handleLearnStatus(AsyncWebServerRequest*); void handleLearnStop(AsyncWebServerRequest*);
    void handleListKeys(AsyncWebServerRequest*); void handleSaveKey(AsyncWebServerRequest*);
    void handleDeleteKey(AsyncWebServerRequest*); void handleClearKeys(AsyncWebServerRequest*);
};
#include "rf_routes.inc"
struct Rendezvous {
    std::mutex mutex; std::condition_variable condition; bool entered = false, release = false;
    void block() { std::unique_lock<std::mutex> lock(mutex); entered = true; condition.notify_all();
        CHECK(condition.wait_for(lock, std::chrono::seconds(3), [&] { return release; })); }
    void wait() { std::unique_lock<std::mutex> lock(mutex); CHECK(condition.wait_for(lock, std::chrono::seconds(3), [&] { return entered; })); }
    void finish() { std::lock_guard<std::mutex> lock(mutex); release = true; condition.notify_all(); }
};
static void feed(uint32_t code = 0xA1B2C3, void* token = nullptr) {
    auto callback = RFLearnDriver::_isrHandler;
    if (!token) token = attachedArgument;
    clockUs += 4000; callback(token);
    for (int i = 23; i >= 0; --i) {
        clockUs += ((code >> i) & 1) ? 1050 : 350; callback(token);
        clockUs += ((code >> i) & 1) ? 350 : 1050; callback(token);
    }
    clockUs += 4000; callback(token);
}
int main(int argc, char** argv) {
    CHECK(argc == 2); const std::string mode = argv[1];
    const std::string path = "/config/rf_keys.json";
    const std::string original = "[{\"name\":\"power\",\"code\":\"0xA1B2C3\",\"bits\":24,\"pulse\":350}]";
    LittleFS.files[path] = original;
    auto& rf = RFLearnDriver::getInstance(); CHECK(rf.begin(16));
    const String cache = rf.getKeysJson(); CHECK(rf.getKeyCount() == 1);
    auto retained = [&] { CHECK(LittleFS.files.at(path) == original && rf.getKeysJson() == cache); };
    auto receive = [&] { feed(); rf.check(); };
    WebHandlerContext context; RfRouteHandler route(&context); AsyncWebServerRequest request;
    String error;
    if (mode == "open_failure" || mode == "short_write" || mode == "rename_failure" ||
        mode == "update_failure" || mode == "delete_failure" || mode == "clear_failure") {
        if (mode == "short_write") LittleFS.writeLimit = 8;
        else if (mode == "rename_failure") LittleFS.failRename = path + ".tmp";
        else LittleFS.failOpen = path + ".tmp";
        if (mode == "delete_failure") CHECK(!rf.deleteKey("power", &error));
        else if (mode == "clear_failure") CHECK(!rf.clearKeys(&error));
        else CHECK(!rf.upsertKey(mode == "update_failure" ? "power" : "other", "22", 24, 350, error));
        CHECK(error.startsWith("RF_KEYS_")); retained();
    } else if (mode == "transaction_success") {
        CHECK(rf.upsertKey("power", "22", 24, 350, error)); CHECK(rf.resolveKeyName(0x22, 24) == "power");
        CHECK(rf.upsertKey("other", "33", 24, 350, error)); CHECK(rf.getKeyCount() == 2);
        CHECK(rf.deleteKey("power", &error)); CHECK(rf.getKeyCount() == 1);
        CHECK(!rf.deleteKey("missing", &error) && error == "RF_KEY_NOT_FOUND");
        CHECK(rf.clearKeys(&error)); CHECK(LittleFS.files.at(path) == "[]" && rf.getKeysJson() == "[]");
    } else if (mode == "source_conflict") {
        LittleFS.onRead = [&](size_t offset) { if (offset == 1 && LittleFS.exists((path + ".tmp").c_str())) LittleFS.files[path] = "[]"; };
        CHECK(!rf.clearKeys(&error)); CHECK(error == "RF_KEYS_VERIFY_OR_SOURCE_CONFLICT");
        CHECK(LittleFS.files.at(path) == "[]" && rf.getKeysJson() == cache);
    } else if (mode == "concurrent_edit") {
        Rendezvous gate; bool blocked = false;
        LittleFS.onRead = [&](size_t offset) { if (offset == 1 && !blocked) { blocked = true; gate.block(); } };
        bool edited = false; std::thread thread([&] { String issue; edited = rf.upsertKey("other", "22", 24, 350, issue); });
        gate.wait(); CHECK(!rf.clearKeys(&error) && error == "RF_KEYS_BUSY"); CHECK(!rf.loadKeyMap());
        CHECK(rf.getKeysJson() == cache); gate.finish(); thread.join(); CHECK(edited && rf.getKeyCount() == 2);
    } else if (mode == "invalid_load") {
        for (const char* input : {"{", "{}", "[4]", "[] trailing", "[]{}", "[{\"name\":\"a\",\"code\":4}]",
            "[{\"name\":\"a\",\"code\":\"22\",\"bits\":280}]",
            "[{\"name\":\"a\",\"code\":\"22junk\"}]",
            "[{\"name\":\"a\\u0000b\",\"code\":\"22\"}]",
            "[{\"name\":\"a\",\"code\":\"22\"},{\"name\":\"a\",\"code\":\"33\"}]"}) {
            LittleFS.files[path] = input; CHECK(!rf.loadKeyMap()); CHECK(rf.getKeysJson() == cache);
        }
        LittleFS.files[path] = std::string(8193, ' '); CHECK(!rf.loadKeyMap()); CHECK(rf.getKeysJson() == cache);
    } else if (mode == "not_mounted") {
        filesystemReady = false; CHECK(!rf.loadKeyMap()); CHECK(!rf.clearKeys(&error)); retained(); filesystemReady = true;
    } else if (mode == "key_validation") {
        for (const char* code : {"", "-1", "1junk", "0x100000000", "0x1 2", "FFFFFFFFF"})
            CHECK(!rf.upsertKey("other", code, 24, 350, error));
        CHECK(!rf.upsertKey("other", "0xFFFF", 12, 350, error));
        CHECK(!rf.upsertKey("other", "22", 24, 79, error)); CHECK(!rf.upsertKey("other", "22", 24, 2001, error)); retained();
    } else if (mode == "capacity") {
        std::string full = "["; for (int i = 0; i < 40; ++i) { if (i) full += ',';
            full += "{\"name\":\"k" + std::to_string(i) + "\",\"code\":\"22\"}"; } full += ']';
        LittleFS.files[path] = full; CHECK(rf.loadKeyMap());
        CHECK(!rf.upsertKey("overflow", "22", 24, 350, error) && error == "RF_KEYS_LIMIT");
        CHECK(rf.upsertKey("k39", "33", 24, 350, error)); CHECK(rf.getKeyCount() == 40);
    } else if (mode == "learning") {
        CHECK(rf.startLearn(1000)); receive(); RfStatusSnapshot snapshot; CHECK(rf.getSnapshot(snapshot));
        CHECK(snapshot.learnState == RfLearnState::CAPTURED && snapshot.learnedCode == "0xA1B2C3");
        CHECK(snapshot.learnedBits == 24 && snapshot.learnedPulse == 350 && dispatches == 0);
        clockMs += 301; receive();
#if FASTBEE_ENABLE_PERIPH_EXEC
        CHECK(dispatches == 1 && lastEvent == "rf_code_received" && lastPayload.indexOf("power") >= 0);
#else
        CHECK(dispatches == 0);
#endif
    } else if (mode == "timeout_wrap") {
        clockMs = UINT32_MAX - 499; CHECK(rf.startLearn(1000)); CHECK(rf.getLearnRemainingMs() == 1000);
        clockMs += 999; CHECK(rf.getLearnRemainingMs() == 1); ++clockMs;
        CHECK(rf.getLearnState() == RfLearnState::TIMEOUT && rf.getLearnRemainingMs() == 0);
        rf.check(); CHECK(rf._learnState == RfLearnState::TIMEOUT);
    } else if (mode == "old_isr_generation") {
        void* old = attachedArgument; CHECK(rf.stop(16)); CHECK(rf.begin(17)); CHECK(attachedArgument != old);
        feed(0xA1B2C3, old); CHECK(rf._isrPulseCount == 0 && !rf._isrFrameReady); rf.check(); CHECK(rf.getLastCode().isEmpty());
        feed(); rf.check(); CHECK(rf.getLastCode() == "0xA1B2C3");
    } else if (mode == "singleton_identity") {
        CHECK(!rf.begin(17) && configuredPin == 16 && attaches == 1); CHECK(!rf.stop(17) && rf.isInitialized());
        CHECK(rf.stop(16)); CHECK(rf.begin(17) && attaches == 2);
    } else if (mode == "generation_exhausted") {
        CHECK(rf.stop(16)); rfIsrGeneration = UINT32_MAX;
        CHECK(!rf.begin(17) && attaches == 1 && configuredPin == 16);
    } else if (mode == "pm_lifecycle") {
        RfPmConfig config; config.pins[0] = 17;
        CHECK(!pmInitializeReceiver(config) && configuredPin == 16 && attaches == 1);
        CHECK(!pmStopReceiver(config) && rf.isInitialized()); config.pins[0] = 16;
#if FASTBEE_ENABLE_PERIPH_EXEC
        onDispatch = [&] { CHECK(!pmStopReceiver(config) && detaches == 0); };
        receive(); CHECK(dispatches == 1); onDispatch = {};
#endif
        CHECK(pmStopReceiver(config) && detaches == 1);
        config.pins[0] = 17; CHECK(pmInitializeReceiver(config) && attaches == 2 && configuredPin == 17);
    } else if (mode == "stop_dispatch") {
        Rendezvous gate; onDispatch = [&] { gate.block(); }; std::thread thread(receive); gate.wait();
        CHECK(!rf.stop(16) && detaches == 0); CHECK(!rf.begin(17) && !rf.startLearn());
        RfStatusSnapshot s; CHECK(rf.getSnapshot(s) && s.stopping && !s.initialized);
        gate.finish(); thread.join(); CHECK(rf.stop(16) && detaches == 1); CHECK(rf.begin(17));
    } else if (mode == "reentrant_dispatch") {
        onDispatch = [&] { RfStatusSnapshot s; CHECK(rf.getSnapshot(s)); CHECK(!rf.stop(16) && !rf.begin(17)); };
        receive(); CHECK(dispatches == 1); CHECK(rf.stop(16));
    } else if (mode == "dispatch_throw") {
        onDispatch = [] { throw std::runtime_error("dispatch fault"); };
        bool caught = false; try { receive(); } catch (const std::runtime_error&) { caught = true; }
        CHECK(caught && !rf._dispatching && rf.stop());
    } else if (mode == "snapshot_busy") {
        CHECK(xSemaphoreTake(rf._mutex, 0)); RfStatusSnapshot s; s.initialized = true;
        CHECK(!rf.getSnapshot(s) && !s.initialized); CHECK(!rf.startLearn() && !rf.stopLearn());
        String json = "old"; size_t count = 2; CHECK(!rf.getKeySnapshot(json, count) && json.isEmpty() && count == 0);
        xSemaphoreGive(rf._mutex);
    } else if (mode == "http_failure") {
        LittleFS.failOpen = path + ".tmp"; route.handleClearKeys(&request); CHECK(request.status == 503); retained();
        request.parameters["name"].text = "power"; route.handleDeleteKey(&request); CHECK(request.status == 503); retained();
        request.parameters["code"].text = "22"; route.handleSaveKey(&request); CHECK(request.status == 503); retained();
        LittleFS.failOpen = ""; request.parameters["name"].text = "missing"; route.handleDeleteKey(&request); CHECK(request.status == 404);
    } else if (mode == "http_parameters") {
        request.parameters["name"].text = "other"; request.parameters["code"].text = "22";
        for (const char* value : {"280", "-1", "1.0", "65560", "24x", "0"}) {
            request.parameters["bits"].text = value; route.handleSaveKey(&request); CHECK(request.status == 400); retained(); }
        request.parameters["bits"].text = "24"; request.parameters["pulse"].text = "65886";
        route.handleSaveKey(&request); CHECK(request.status == 400); retained();
        request.parameters["timeoutMs"].text = "4294967296"; route.handleLearnStart(&request); CHECK(request.status == 400);
        request.parameters["timeoutMs"].text = "1000"; route.handleLearnStart(&request); CHECK(request.status == 200);
        CHECK(rf.stop()); route.handleLearnStart(&request); CHECK(request.status == 409);
    } else if (mode == "http_permissions") {
        context.authenticated = false; route.handleClearKeys(&request); CHECK(request.status == 401); retained();
        context.authenticated = true; context.developer = false; route.handleClearKeys(&request); CHECK(request.status == 403); retained();
        CHECK(rf.startLearn()); route.handleLearnStop(&request); CHECK(request.status == 200 && !rf.isLearning());
    } else if (mode == "http_snapshot") {
        CHECK(xSemaphoreTake(rf._mutex, 0)); route.handleStatus(&request); CHECK(request.status == 503);
        route.handleLearnStatus(&request); CHECK(request.status == 503); route.handleListKeys(&request); CHECK(request.status == 503);
        route.handleLearnStop(&request); CHECK(request.status == 503); xSemaphoreGive(rf._mutex);
        route.handleListKeys(&request); CHECK(request.status == 200); JsonDocument doc; CHECK(!deserializeJson(doc, request.body));
        CHECK(doc["data"]["count"] == 1 && doc["data"]["keys"].size() == 1);
    } else if (mode == "memory_failure") {
        failNextAllocation = true; CHECK(!rf.upsertKey("other", "22", 24, 350, error)); failNextAllocation = false;
        CHECK(error == "RF_KEYS_MEMORY"); retained();
    } else CHECK(false);
    LittleFS.onRead = {}; onDispatch = {}; CHECK(rf.stop()); CHECK(attaches == detaches); puts(mode.c_str());
}
