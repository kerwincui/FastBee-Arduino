#include <Arduino.h>
#undef malloc
#undef free
#include <ArduinoJson.h>
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
void* operator new(size_t size) {
    if (failNextAllocation) { failNextAllocation = false; throw std::bad_alloc(); }
    void* ptr = std::malloc(size ? size : 1);
    if (!ptr) throw std::bad_alloc();
    return ptr;
}
void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr, size_t) noexcept { std::free(ptr); }
static uint32_t clockMs = 1000;
static unsigned long irMillis() { return clockMs; }
#define millis irMillis
using TickType_t = uint32_t;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
constexpr int pdTRUE = 1;
struct FakeMutex { std::timed_mutex lock; };
using SemaphoreHandle_t = FakeMutex*;
static thread_local unsigned irLockDepth = 0;
static SemaphoreHandle_t xSemaphoreCreateMutex() { return new FakeMutex; }
static void vSemaphoreDelete(SemaphoreHandle_t mutex) { delete mutex; }
static int xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t wait) {
    if (!mutex) return 0;
    if (irLockDepth) { CHECK(wait == 0); return 0; }
    const bool locked = wait == 0 ? mutex->lock.try_lock() : mutex->lock.try_lock_for(std::chrono::seconds(2));
    if (locked) ++irLockDepth;
    return locked ? pdTRUE : 0;
}
static void xSemaphoreGive(SemaphoreHandle_t mutex) { CHECK(irLockDepth == 1); --irLockDepth; mutex->lock.unlock(); }
struct Log {
    template<class... T> void infof(const char*, T...) {}
    template<class... T> void warningf(const char*, T...) {}
    void info(const char*) {} void warning(const char*) {}
} LOGGER;
namespace FastBee {
struct RuntimeFeatureManager {
    static RuntimeFeatureManager& instance() { static RuntimeFeatureManager value; return value; }
    bool shouldEnable(const char*) const { return true; }
};
struct FeatureBudgetRegistry {
    static FeatureBudgetRegistry& instance() { static FeatureBudgetRegistry value; return value; }
    bool isDenied(const char*) const { return false; }
};
}
struct decode_results { int decode_type = 1; uint64_t value = 0; uint16_t bits = 32; };
static String typeToString(int) { return "NEC"; }
struct IRrecv {
    inline static int created = 0, destroyed = 0, resumes = 0;
    inline static bool pending = false;
    inline static uint64_t code = 0x12345678;
    inline static std::function<void()> onResume;
    bool alive = true;
    IRrecv(uint8_t, uint16_t, int, bool) { ++created; }
    ~IRrecv() { CHECK(alive); alive = false; ++destroyed; }
    void setUnknownThreshold(int) {} void enableIRIn() {}
    bool decode(decode_results* output) {
        CHECK(alive && irLockDepth == 1);
        if (!pending) return false;
        pending = false; output->value = code; return true;
    }
    void resume() { CHECK(alive && irLockDepth == 1); ++resumes; if (onResume) onResume(); }
};
#if FASTBEE_ENABLE_PERIPH_EXEC
struct PeriphExecManager {
    String official = "[{\"name\":\"power\",\"code\":\"0x12345678\",\"protocol\":\"NEC\"}]";
    bool failStorage = false, ready = true, pendingRestart = false;
    unsigned commits = 0, dispatches = 0;
    String event, payload;
    std::function<void()> afterCopy, beforeCommit, onDispatch;
    static PeriphExecManager& getInstance() { static PeriphExecManager value; return value; }
    bool copyIrKeysJson(String& output) {
        CHECK(irLockDepth == 0); output = "";
        if (!ready || pendingRestart) return false;
        output = official; if (afterCopy) afterCopy(); return true;
    }
    bool setIrKeysJson(const String& input, const String* expected = nullptr) {
        CHECK(irLockDepth == 0);
        if (beforeCommit) beforeCommit();
        if (!ready || pendingRestart || failStorage || (expected && *expected != official)) return false;
        official = input; ++commits; return true;
    }
    void dispatchEventMatchedRules(const String& name, const String& data) {
        CHECK(irLockDepth == 0); ++dispatches; event = name; payload = data;
        if (onDispatch) onDispatch();
    }
};
#endif // FASTBEE_ENABLE_PERIPH_EXEC
#include "ir_declaration.inc"
#include "ir_driver.inc"
struct Param { String text; String value() const { return text; } };
struct AsyncWebServerRequest {
    std::map<String, Param> parameters;
    int status = 0;
    String body;
    bool hasParam(const char* name, bool) const { return parameters.count(name) != 0; }
    Param* getParam(const char* name, bool) { return &parameters.at(name); }
    void send(int code, const char*, const String& response) { status = code; body = response; }
};
struct WebHandlerContext {
    bool authenticated = true, developer = true;
    bool requireAuth(AsyncWebServerRequest* request) { if (!authenticated) request->status = 401; return authenticated; }
    bool requireDeveloperMode(AsyncWebServerRequest* request) { if (!developer) request->status = 403; return developer; }
    void sendError(AsyncWebServerRequest* request, int code, const String& reason) { request->status = code; request->body = reason; }
    void sendSuccess(AsyncWebServerRequest* request, const char* message) { request->status = 200; request->body = message; }
    void sendSuccess(AsyncWebServerRequest* request, JsonDocument& doc) {
        request->status = 200; std::string data; serializeJson(doc, data); request->body = data;
    }
};
class IrRouteHandler {
public:
    explicit IrRouteHandler(WebHandlerContext* context) : ctx(context) {}
    WebHandlerContext* ctx;
    void handleStatus(AsyncWebServerRequest*); void handleLearnStart(AsyncWebServerRequest*);
    void handleLearnStatus(AsyncWebServerRequest*); void handleLearnStop(AsyncWebServerRequest*);
    void handleListKeys(AsyncWebServerRequest*); void handleSaveKey(AsyncWebServerRequest*);
    void handleDeleteKey(AsyncWebServerRequest*); void handleClearKeys(AsyncWebServerRequest*);
};
#include "ir_routes.inc"
struct Rendezvous {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false, release = false;
    void block() {
        std::unique_lock<std::mutex> lock(mutex); entered = true; condition.notify_all();
        CHECK(condition.wait_for(lock, std::chrono::seconds(3), [&] { return release; }));
    }
    void wait() { std::unique_lock<std::mutex> lock(mutex); CHECK(condition.wait_for(lock, std::chrono::seconds(3), [&] { return entered; })); }
    void finish() { std::lock_guard<std::mutex> lock(mutex); release = true; condition.notify_all(); }
};
#if !FASTBEE_ENABLE_PERIPH_EXEC
int main(int argc, char** argv) {
    CHECK(argc == 2 && std::string(argv[1]) == "event_store_disabled");
    auto& ir = IRRemoteDriver::getInstance();
    CHECK(ir.begin(15)); CHECK(!ir.loadKeyMap());
    IrStatusSnapshot snapshot; CHECK(ir.getSnapshot(snapshot) && snapshot.initialized && !snapshot.keysLoaded);
    CHECK(ir.startLearn(1000)); IRrecv::pending = true; ir.check();
    CHECK(ir.getSnapshot(snapshot) && snapshot.learnState == IrLearnState::CAPTURED);
    CHECK(snapshot.learnedCode == "0x12345678");
    clockMs += 151; IRrecv::pending = true; ir.check(); // No Manager type or symbol exists in this build.
    String error;
    CHECK(!ir.upsertKey("power", "22", "NEC", error) && error == "IR_KEYS_STORE_DISABLED");
    CHECK(!ir.deleteKey("power", &error) && error == "IR_KEYS_STORE_DISABLED");
    CHECK(!ir.clearKeys(&error) && error == "IR_KEYS_STORE_DISABLED");
    WebHandlerContext context; IrRouteHandler route(&context); AsyncWebServerRequest request;
    route.handleListKeys(&request); CHECK(request.status == 503);
    route.handleClearKeys(&request); CHECK(request.status == 503);
    CHECK(ir.stop() && IRrecv::created == IRrecv::destroyed);
    puts(argv[1]);
}
#else
int main(int argc, char** argv) {
    CHECK(argc == 2); const std::string mode = argv[1];
    auto& ir = IRRemoteDriver::getInstance(); auto& store = PeriphExecManager::getInstance();
    if (mode != "keys_without_receiver") CHECK(ir.begin(15));
    else CHECK(ir.loadKeyMap());
    const String original = store.official;
    const String cache = ir.getKeysJson();
    CHECK(ir.getKeyCount() == 1 && ir.resolveKeyName(0x12345678, "NEC") == "power");
    String error;
    auto retained = [&] { CHECK(store.official == original); CHECK(ir.getKeysJson() == cache); CHECK(store.commits == 0); };
    auto receive = [&] { IRrecv::pending = true; ir.check(); };
    WebHandlerContext context; IrRouteHandler route(&context); AsyncWebServerRequest request;
    if (mode == "upsert_failure" || mode == "update_failure" || mode == "delete_failure" || mode == "clear_failure") {
        store.failStorage = true;
        if (mode == "upsert_failure") CHECK(!ir.upsertKey("other", "22", "NEC", error));
        if (mode == "update_failure") CHECK(!ir.upsertKey("power", "22", "NEC", error));
        if (mode == "delete_failure") CHECK(!ir.deleteKey("power", &error));
        if (mode == "clear_failure") CHECK(!ir.clearKeys(&error));
        CHECK(error == "IR_KEYS_COMMIT_UNCONFIRMED"); retained();
    } else if (mode == "transaction_success") {
        CHECK(ir.upsertKey("power", "22", "NEC", error)); CHECK(ir.resolveKeyName(0x22, "NEC") == "power");
        CHECK(ir.resolveKeyName(0x12345678, "NEC").isEmpty());
        CHECK(ir.upsertKey("other", "33", "", error)); CHECK(ir.getKeyCount() == 2);
        CHECK(ir.deleteKey("power", &error)); CHECK(ir.getKeyCount() == 1);
        CHECK(!ir.deleteKey("missing", &error) && error == "IR_KEY_NOT_FOUND");
        CHECK(ir.clearKeys(&error)); CHECK(ir.getKeysJson() == "[]" && store.official.isEmpty());
        CHECK(store.commits == 4);
    } else if (mode == "cas_conflict") {
        store.beforeCommit = [&] { store.official = "[{\"name\":\"imported\",\"code\":\"77\"}]"; };
        CHECK(!ir.upsertKey("power", "22", "NEC", error));
        CHECK(error == "IR_KEYS_COMMIT_UNCONFIRMED" && store.official.indexOf("imported") >= 0);
        CHECK(ir.getKeysJson() == cache && store.commits == 0);
    } else if (mode == "pending_import") {
        store.beforeCommit = [&] { store.pendingRestart = true; };
        CHECK(!ir.clearKeys(&error)); retained(); CHECK(!ir.loadKeyMap());
        CHECK(ir.getKeysJson() == cache);
    } else if (mode == "concurrent_edit") {
        Rendezvous gate; store.afterCopy = [&] { gate.block(); };
        bool edited = false; std::thread thread([&] { String issue; edited = ir.upsertKey("other", "22", "NEC", issue); });
        gate.wait(); CHECK(!ir.clearKeys(&error) && error == "IR_KEYS_BUSY");
        CHECK(!ir.loadKeyMap()); CHECK(ir.getKeysJson() == cache);
        gate.finish(); thread.join(); CHECK(edited && ir.getKeyCount() == 2);
    } else if (mode == "invalid_load") {
        for (const char* invalid : {"{", "{}", "[4]", "[{\"name\":\"a\",\"code\":4}]",
             "[{\"name\":\"a\",\"code\":\"11\"},{\"name\":\"a\",\"code\":\"22\"}]",
             "[{\"name\":\"a\\u0000b\",\"code\":\"11\"}]",
             "[{\"name\":\"a\",\"code\":\"11\\u000022\"}]",
             "[{\"name\":\"a\",\"code\":\"11\",\"protocol\":\"N\\u0000EC\"}]"}) {
            store.official = invalid; CHECK(!ir.loadKeyMap()); CHECK(ir.getKeysJson() == cache);
        }
        store.ready = false; CHECK(!ir.loadKeyMap()); CHECK(ir.getKeysJson() == cache);
    } else if (mode == "capacity") {
        store.official = "[";
        for (int i = 0; i < 40; ++i) { if (i) store.official += ","; store.official += "{\"name\":\"k" + String(i) + "\",\"code\":\"11\"}"; }
        store.official += "]"; CHECK(ir.loadKeyMap());
        CHECK(!ir.upsertKey("overflow", "22", "", error) && error == "IR_KEYS_LIMIT");
        CHECK(ir.getKeyCount() == 40); CHECK(ir.upsertKey("k39", "22", "", error));
        const String full = ir.getKeysJson(); store.official = full.substring(0, full.length() - 1) + ",{\"name\":\"overflow\",\"code\":\"33\"}]";
        CHECK(!ir.loadKeyMap()); CHECK(ir.getKeysJson() == full);
    } else if (mode == "memory_failure") {
        store.afterCopy = [&] { failNextAllocation = true; };
        CHECK(!ir.upsertKey("other", "22", "NEC", error)); failNextAllocation = false;
        CHECK(error == "IR_KEYS_MEMORY"); retained();
    } else if (mode == "learning_capture") {
        CHECK(ir.startLearn(1000)); receive();
        IrStatusSnapshot snapshot; CHECK(ir.getSnapshot(snapshot));
        CHECK(snapshot.learnState == IrLearnState::CAPTURED && snapshot.learnedCode == "0x12345678");
        CHECK(store.dispatches == 0); clockMs += 151; receive();
        CHECK(store.dispatches == 1 && store.event == "ir_code_received" && store.payload.indexOf("power") >= 0);
    } else if (mode == "timeout_wrap") {
        clockMs = UINT32_MAX - 499; CHECK(ir.startLearn(1000));
        IrStatusSnapshot snapshot; CHECK(ir.getSnapshot(snapshot) && snapshot.remainingMs == 1000);
        clockMs += 999; CHECK(ir.getSnapshot(snapshot) && snapshot.remainingMs == 1);
        ++clockMs; CHECK(ir.getSnapshot(snapshot) && snapshot.learnState == IrLearnState::TIMEOUT && snapshot.remainingMs == 0);
        ir.check(); CHECK(ir._learnState == IrLearnState::TIMEOUT);
    } else if (mode == "stop_dispatch") {
        Rendezvous gate; store.onDispatch = [&] { gate.block(); };
        std::thread thread(receive); gate.wait();
        CHECK(!ir.stop()); CHECK(IRrecv::destroyed == 0); CHECK(!ir.begin(15)); CHECK(!ir.startLearn());
        IrStatusSnapshot snapshot; CHECK(ir.getSnapshot(snapshot) && snapshot.stopping && !snapshot.initialized);
        ir.check(); CHECK(store.dispatches == 1);
        gate.finish(); thread.join(); CHECK(ir.stop() && IRrecv::destroyed == 1);
        CHECK(ir.begin(15)); CHECK(store.dispatches == 1 && IRrecv::created == 2);
    } else if (mode == "reentrant_dispatch") {
        store.onDispatch = [&] {
            IrStatusSnapshot snapshot; CHECK(ir.getSnapshot(snapshot));
            CHECK(!ir.stop()); CHECK(!ir.startLearn()); CHECK(!ir.begin(15));
        };
        receive(); CHECK(store.dispatches == 1 && IRrecv::destroyed == 0); CHECK(ir.stop());
    } else if (mode == "dispatch_throw") {
        store.onDispatch = [&] { throw std::runtime_error("dispatch fault"); };
        bool caught = false; try { receive(); } catch (const std::runtime_error&) { caught = true; }
        CHECK(caught && !ir._dispatching); CHECK(ir.stop());
    } else if (mode == "learn_stop_race") {
        Rendezvous gate; IRrecv::onResume = [&] { gate.block(); };
        bool learned = false, stopped = false;
        std::thread learner([&] { learned = ir.startLearn(); }); gate.wait();
        std::thread stopper([&] { stopped = ir.stop(); });
        gate.finish(); learner.join(); stopper.join();
        CHECK(learned && stopped && IRrecv::destroyed == 1); CHECK(!ir.startLearn());
    } else if (mode == "snapshot_busy") {
        CHECK(xSemaphoreTake(ir._mutex, 0) == pdTRUE);
        IrStatusSnapshot snapshot; snapshot.initialized = true;
        CHECK(!ir.getSnapshot(snapshot) && !snapshot.initialized);
        String keys = "old"; size_t count = 99;
        CHECK(!ir.getKeySnapshot(keys, count) && keys.isEmpty() && count == 0);
        CHECK(!ir.startLearn() && !ir.stopLearn());
        xSemaphoreGive(ir._mutex);
    } else if (mode == "http_persistence") {
        store.failStorage = true; route.handleClearKeys(&request); CHECK(request.status == 503); retained();
        request.parameters["name"].text = "power"; route.handleDeleteKey(&request); CHECK(request.status == 503); retained();
        request.parameters["code"].text = "22"; route.handleSaveKey(&request); CHECK(request.status == 503); retained();
        store.failStorage = false; request.parameters["name"].text = "missing";
        route.handleDeleteKey(&request); CHECK(request.status == 404);
        request.parameters["name"].text = "bad name"; route.handleDeleteKey(&request); CHECK(request.status == 400);
        route.handleClearKeys(&request); CHECK(request.status == 200 && ir.getKeyCount() == 0);
    } else if (mode == "http_permissions") {
        context.authenticated = false; route.handleClearKeys(&request); CHECK(request.status == 401); retained();
        context.authenticated = true; context.developer = false; route.handleClearKeys(&request); CHECK(request.status == 403); retained();
        route.handleLearnStart(&request); CHECK(request.status == 403);
        CHECK(ir.startLearn()); route.handleLearnStop(&request); CHECK(request.status == 200 && !ir.isLearning());
    } else if (mode == "http_timeout") {
        for (const char* invalid : {"", "-1", "0", "999", "60001", "1000x", "1e4", "1000.0", "4294967296"}) {
            request.parameters["timeoutMs"].text = invalid; route.handleLearnStart(&request);
            CHECK(request.status == 400 && !ir.isLearning());
        }
        request.parameters["timeoutMs"].text = "1000"; route.handleLearnStart(&request);
        CHECK(request.status == 200 && ir.getLearnRemainingMs() == 1000);
        CHECK(ir.stop()); route.handleLearnStart(&request); CHECK(request.status == 409);
    } else if (mode == "keys_without_receiver") {
        ir._keyMapLoaded = false; ir._keyMap.clear();
        route.handleListKeys(&request); CHECK(request.status == 200);
        CHECK(!ir.isInitialized() && ir.getKeyCount() == 1 && IRrecv::created == 0);
    } else if (mode == "http_snapshot") {
        CHECK(xSemaphoreTake(ir._mutex, 0) == pdTRUE);
        route.handleStatus(&request); CHECK(request.status == 503);
        route.handleLearnStatus(&request); CHECK(request.status == 503);
        route.handleListKeys(&request); CHECK(request.status == 503);
        route.handleLearnStop(&request); CHECK(request.status == 503);
        xSemaphoreGive(ir._mutex);
        route.handleListKeys(&request); CHECK(request.status == 200);
        JsonDocument doc; CHECK(!deserializeJson(doc, request.body));
        CHECK(doc["data"]["count"] == 1 && doc["data"]["keys"].size() == 1);
        CHECK(ir.startLearn()); receive(); route.handleLearnStatus(&request); CHECK(request.status == 200);
        CHECK(!deserializeJson(doc, request.body));
        CHECK(doc["learnState"] == "captured" && doc["code"] == "0x12345678" && doc["protocol"] == "NEC");
    } else { CHECK(false); }
    store.afterCopy = {}; store.beforeCommit = {}; store.onDispatch = {}; IRrecv::onResume = {};
    CHECK(ir.stop()); CHECK(IRrecv::created == IRrecv::destroyed); puts(mode.c_str());
}
#endif
