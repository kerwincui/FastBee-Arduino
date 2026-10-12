#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#include <functional>
#include <cstring>
#include <freertos/task.h>
#include <esp_private/esp_ipc.h>
#include "systems/TaskStackSnapshot.h"
#include "network/TaskStackSnapshotJson.h"
using namespace FastBee;
static int64_t clockUs = 1000000;
static unsigned ipcCalls[2]{}, infoCalls = 0;
static void (*callbacks[2])(void*){};
static void* arguments[2]{};
static thread_local uint32_t callbackCore = 0;
static std::string mode;
static bool busy0 = false;
static uint32_t freeBytes = 50000;
static unsigned sdkScans = 0;
int xPortGetCoreID() { return callbackCore; }
int64_t esp_timer_get_time() { return clockUs; }
void vTaskGetInfo(void* task, TaskStatus_t* out, int scan, unsigned state) {
    assert(task == nullptr && scan == pdFALSE && state == eRunning);
    ++infoCalls;
    out->pcTaskName = reinterpret_cast<const char*>(1);
    out->pxStackBase = reinterpret_cast<void*>(1);
    out->xHandle = reinterpret_cast<void*>(1);
    out->xTaskNumber = mode == "duplicate" ? 101 : callbackCore + 101;
    out->uxCurrentPriority = 24;
}
int esp_ipc_call_nonblocking(uint32_t core, void (*callback)(void*), void* arg) {
    assert(core < 2); ++ipcCalls[core];
    if (busy0 && core == 0) return ESP_FAIL;
    if (mode == "sdk_reject") return ESP_ERR_INVALID_STATE;
    if (mode == "pending" || mode == "wrong_core") { callbacks[core] = callback; arguments[core] = arg; }
    else { callbackCore = core; callback(arg); }
    return ESP_OK;
}
// All blocking, trace-ID, borrowed-name, and extra watermark APIs are forbidden.
int esp_ipc_call(uint32_t, void (*)(void*), void*) { std::abort(); }
int esp_ipc_call_blocking(uint32_t, void (*)(void*), void*) { std::abort(); }
unsigned uxTaskGetTaskNumber(void*) { std::abort(); }
const char* pcTaskGetName(void*) { std::abort(); }
unsigned uxTaskGetStackHighWaterMark(void*) { std::abort(); }
unsigned long millis() { return static_cast<uint32_t>(clockUs / 1000); }
size_t heap_caps_get_free_size(uint32_t) { return freeBytes; }
size_t heap_caps_get_largest_free_block(uint32_t) { return 20000; }
void* heap_caps_calloc(size_t n, size_t size, uint32_t) { return std::calloc(n, size); }
void heap_caps_free(void* pointer) { std::free(pointer); }
unsigned uxTaskGetNumberOfTasks() { return 2; }
int xTaskGetSchedulerState() { return 1; }
void* xTaskGetIdleTaskHandleForCore(int) { return nullptr; }
void* xTimerGetTimerDaemonTaskHandle() { return nullptr; }
unsigned uxTaskGetSystemState(TaskStatus_t* out, unsigned, unsigned*) {
    ++sdkScans;
    for (unsigned i = 0; i < 2; ++i) {
        out[i].xHandle = reinterpret_cast<void*>(1);
        out[i].pcTaskName = reinterpret_cast<const char*>(1);
        out[i].pxStackBase = reinterpret_cast<void*>(1);
        out[i].xTaskNumber = i + 101;
        out[i].usStackHighWaterMark = i ? 260 : 252;
        out[i].uxCurrentPriority = 24;
    }
    clockUs += 50;
    return 2;
}
static void finish(uint32_t core) { callbackCore = core; callbacks[core](arguments[core]); }
struct SnapshotParameter {
    std::string key, data;
    bool file = false, post = false;
    const std::string& name() const { return key; }
    const std::string& value() const { return data; }
    bool isFile() const { return file; }
};
struct AsyncWebServerRequest {
    std::vector<SnapshotParameter> fields;
    mutable unsigned reads = 0;
    size_t params() const { ++reads; return fields.size(); }
    const SnapshotParameter* getParam(size_t i) const { return i < fields.size() ? &fields[i] : nullptr; }
};
constexpr int HTTP_POST = 2;
struct AsyncWebServer {
    std::function<void(AsyncWebServerRequest*)> callback;
    void on(const char* path, int method, std::function<void(AsyncWebServerRequest*)> fn) {
        assert(std::strcmp(path, "/api/system/task-stack-snapshot") == 0 && method == HTTP_POST);
        callback = fn;
    }
};
static unsigned errors = 0, responses = 0;
static std::string response;
struct WebHandlerContext {
    bool auth = true, dev = true;
    unsigned authCalls = 0, devCalls = 0;
    bool requireAuth(AsyncWebServerRequest*) { ++authCalls; return auth; }
    bool requireDeveloperMode(AsyncWebServerRequest*) { ++devCalls; return dev; }
    void sendError(AsyncWebServerRequest*, int code, const char*) { assert(code == 400); ++errors; }
};
struct HealthMonitor { bool isMemorySevere() const { return false; } };
struct FastBeeFramework {
    static FastBeeFramework* getInstance() { static FastBeeFramework value; return &value; }
    HealthMonitor* getHealthMonitor() { static HealthMonitor value; return &value; }
};
namespace HandlerUtils {
void sendJsonError(AsyncWebServerRequest*, int, const char*, int) { std::abort(); }
void sendJsonStream(AsyncWebServerRequest*, JsonDocument& doc) {
    ++responses; response.clear(); serializeJson(doc, response);
}
}
namespace FastBee {
JsonDocument makeJsonDocument(size_t) { return JsonDocument(); }
// @route@
}
int main(int argc, char** argv) {
    assert(argc == 2); mode = argv[1];
    if (mode.find("route_") == 0) {
        AsyncWebServer server; WebHandlerContext context; AsyncWebServerRequest request;
        setupTaskStackSnapshotRoute(&server, &context); assert(server.callback);
        request.fields.push_back({"ipcMode", "observe-only", false, false});
        if (mode == "route_auth") {
            context.auth = false; server.callback(&request);
            assert(context.authCalls == 1 && context.devCalls == 0 && request.reads == 0);
            context.auth = true; context.dev = false; server.callback(&request);
            assert(context.devCalls == 1 && request.reads == 0 && !responses && !errors);
            assert(!ipcCalls[0] && !ipcCalls[1] && !sdkScans);
            return 0;
        }
        if (mode == "route_invalid") {
            const std::vector<std::vector<SnapshotParameter>> invalid{
                {{"ipcMode", ""}}, {{"ipcMode", "enable"}}, {{"other", "observe-only"}},
                {{"ipcMode", "observe-only", true}},
                {{"ipcMode", "observe-only"}, {"ipcMode", "observe-only"}},
                {{"ipcMode", "observe-only", false, false}, {"ipcMode", "observe-only", false, true}}};
            for (const auto& fields : invalid) { request.fields = fields; server.callback(&request); }
            assert(errors == invalid.size() && !responses && !sdkScans && !ipcCalls[0] && !ipcCalls[1]);
            return 0;
        }
        if (mode == "route_observe_ready" || mode == "route_observe_busy") {
            busy0 = mode == "route_observe_busy";
            const auto initialized = sampleIpcTaskOwners(clockUs, true);
            assert(initialized.status == (busy0 ? IpcOwnerStatus::Partial : IpcOwnerStatus::Observed));
            busy0 = false; clockUs += 31000000;
        }
        if (mode == "route_default") request.fields.clear();
        // Exercise both query and form transport representations.
        for (unsigned transport = 0; transport < 2; ++transport) {
            if (!request.fields.empty()) request.fields[0].post = transport == 1;
            server.callback(&request);
            assert(errors == 0 && responses == transport + 1 && sdkScans == transport + 1);
            JsonDocument doc; assert(!deserializeJson(doc, response) && doc["success"] == true);
            if (mode == "route_observe_cold") {
                assert(!ipcCalls[0] && !ipcCalls[1] && infoCalls == 0);
                assert(doc["data"]["ipcAttribution"]["status"] == "unknown");
                assert(doc["data"]["ipcAttribution"]["cores"][0]["reason"] == "EXPLICIT_REQUEST_REQUIRED");
                assert(doc["data"]["tasks"][0]["sdkOwner"].isNull());
            } else {
                assert(ipcCalls[0] == 1 && ipcCalls[1] == 1);
                if (mode == "route_observe_busy") {
                    assert(doc["data"]["ipcAttribution"]["reason"] == "IPC_IDENTITIES_PARTIAL");
                    assert(doc["data"]["ipcAttribution"]["cores"][0]["reason"] == "IPC_BUSY_EXPLICIT_REQUEST_REQUIRED");
                } else {
                    assert(doc["data"]["ipcAttribution"]["status"] == "observed");
                    assert(doc["data"]["tasks"][0]["sdkOwner"] == "ipc-core0");
                }
            }
            assert(doc["data"]["tasks"][0]["highWaterBytes"] == 252);
            clockUs += 31000000;
        }
        return 0;
    }
    if (mode == "stress") {
        for (unsigned round = 0; round < 300; ++round) {
            IpcTaskOwnerSlot slot;
            std::atomic<unsigned> claims{0};
            std::vector<std::thread> workers;
            for (unsigned i = 0; i < 8; ++i) workers.emplace_back([&] { if (slot.begin(1000000)) ++claims; });
            for (auto& worker : workers) worker.join();
            assert(claims == 1);
            std::atomic<bool> done{false};
            std::thread publisher([&] { slot.publish(100 + round, true, 1000100); done.store(true); });
            for (unsigned i = 0; i < 8; ++i) workers[i] = std::thread([&] {
                do {
                    const auto row = slot.read(1000200);
                    assert(row.status == IpcOwnerStatus::Pending || row.status == IpcOwnerStatus::Observed);
                    if (row.status == IpcOwnerStatus::Observed) assert(row.taskNumber == 100 + round && row.generation == 1);
                } while (!done.load());
            });
            publisher.join(); for (auto& worker : workers) worker.join();
            assert(!slot.begin(2000000) && slot.read(2000000).taskNumber == 100 + round);
        }
        return 0;
    }
    if (mode == "clock" || mode == "wrap") {
        IpcTaskOwnerSlot slot;
        assert(!slot.begin(-1));
        const auto start = mode == "wrap" ? static_cast<int64_t>(UINT32_MAX - 500ULL) * 1000 : 1000000;
        assert(slot.begin(start));
        assert(slot.read(-1).status == IpcOwnerStatus::ClockUnavailable);
        assert(slot.read(start - 1000).status == IpcOwnerStatus::ClockUnavailable);
        assert(slot.read(start + 1000000).status == IpcOwnerStatus::TimedOut);
        slot.publish(101, true, start - 1);
        assert(slot.read(start + 1000001).status == IpcOwnerStatus::ClockUnavailable);
        return 0;
    }
    if (mode == "unsupported") {
        const auto out = sampleIpcTaskOwners(clockUs, true);
        assert(out.status == IpcOwnerStatus::Unsupported && out.fixedRamBytes == 0 && !ipcCalls[0] && !ipcCalls[1]);
        return 0;
    }
    if (mode == "memory" || mode == "severe" || mode == "capture_clock") {
        if (mode == "memory") freeBytes = 10000;
        if (mode == "capture_clock") clockUs = -1;
        const auto out = captureTaskStackSnapshot(mode == "severe");
        assert(out.status != TaskStackCaptureStatus::Observed && !ipcCalls[0] && !ipcCalls[1] && !sdkScans);
        return 0;
    }
    if (mode == "capture") {
        const auto out = captureTaskStackSnapshot(false);
        assert(out.status == TaskStackCaptureStatus::Observed && out.ipcAttribution.status == IpcOwnerStatus::Observed);
        assert(out.entries[0].permanentOwner == TaskStackPermanentOwner::IpcCore0);
        assert(out.entries[1].permanentOwner == TaskStackPermanentOwner::IpcCore1);
        assert(out.entries[0].highWaterBytes == 252 && out.entries[1].highWaterBytes == 260);
        JsonDocument document; writeTaskStackSnapshotJson(document.to<JsonObject>(), out);
        assert(document["tasks"][0]["sdkOwner"] == "ipc-core0" && document["tasks"][0]["name"].isNull());
        assert(document["ipcAttribution"]["callerMayChangeSdkPriority"] == true);
        assert(infoCalls == 2 && sdkScans == 1);
        return 0;
    }
    if (mode == "busy") busy0 = true;
    auto out = sampleIpcTaskOwners(clockUs, true);
    assert(ipcCalls[0] == 1 && ipcCalls[1] == 1);
    if (mode == "busy") {
        assert(out.status == IpcOwnerStatus::Partial && out.cores[0].status == IpcOwnerStatus::Busy);
        sampleIpcTaskOwners(clockUs, false); assert(ipcCalls[0] == 1);
        busy0 = false;
        out = sampleIpcTaskOwners(clockUs, true);
        assert(out.status == IpcOwnerStatus::Observed && ipcCalls[0] == 2 && ipcCalls[1] == 1);
    } else if (mode == "sdk_reject") {
        assert(out.cores[0].status == IpcOwnerStatus::SdkUnavailable && infoCalls == 0);
    } else if (mode == "duplicate") {
        assert(out.status == IpcOwnerStatus::InvalidIdentity);
    } else if (mode == "wrong_core") {
        callbackCore = 1; callbacks[0](arguments[0]); finish(1);
        out = sampleIpcTaskOwners(clockUs, false);
        assert(out.status == IpcOwnerStatus::Partial && out.cores[0].status == IpcOwnerStatus::InvalidIdentity);
    } else if (mode == "pending") {
        clockUs += 1100000;
        out = sampleIpcTaskOwners(clockUs, true);
        assert(out.cores[0].status == IpcOwnerStatus::TimedOut && ipcCalls[0] == 1 && ipcCalls[1] == 1);
        const auto originalTimedOutWindow = out;
        const auto timedOutCaptureUs = clockUs;
        clockUs += 100;
        finish(0); finish(1);
        const auto old = sampleIpcTaskOwners(timedOutCaptureUs, false);
        assert(old.status == IpcOwnerStatus::Partial && old.cores[0].status == IpcOwnerStatus::PublishedAfterCapture);
        assert(originalTimedOutWindow.cores[0].status == IpcOwnerStatus::TimedOut);
        out = sampleIpcTaskOwners(clockUs, false);
        assert(out.status == IpcOwnerStatus::Observed && out.cores[0].publishedLate && out.cores[0].generation == 1);
    } else {
        assert(out.status == IpcOwnerStatus::Observed && out.cores[0].taskNumber == 101 && out.cores[1].taskNumber == 102);
        assert(out.fixedRamBytes >= 2 * sizeof(IpcTaskOwnerSlot) && out.callerMayChangeSdkPriority);
    }
    sampleIpcTaskOwners(clockUs, true);
    if (mode != "sdk_reject") assert(ipcCalls[0] == (mode == "busy" ? 2U : 1U) && ipcCalls[1] == 1);
}
