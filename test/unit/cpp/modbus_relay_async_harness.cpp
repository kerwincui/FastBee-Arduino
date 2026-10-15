#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
using String = std::string;
using BaseType_t = int;
struct Semaphore { bool available = true; };
using SemaphoreHandle_t = Semaphore*;
using TaskHandle_t = void*;
constexpr int pdTRUE = 1, pdPASS = 1, portMAX_DELAY = -1;
static Semaphore slot;
static bool notified = false, createSucceeds = true, allocationFails = false;
static void (*pendingTask)(void*) = nullptr;
static void* pendingArgument = nullptr;
static int pauses = 0, calls = 0, deletedTasks = 0;
// The throwing form must be replaced too: without it, throwing-new expressions
// in the exe (make_shared control blocks, non-SSO strings) resolve to
// libstdc++-6.dll's operator new while the deletes below free() from the exe's
// CRT — a cross-domain free the Windows heap reports as 0xC0000374.
void* operator new(size_t size) {
    if (void* p = std::malloc(size)) return p;
    throw std::bad_alloc();
}
void* operator new(size_t size, const std::nothrow_t&) noexcept {
    return allocationFails ? nullptr : std::malloc(size);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
SemaphoreHandle_t xSemaphoreCreateCounting(int, int) { return &slot; }
int xSemaphoreTake(SemaphoreHandle_t s, int timeout) {
    assert(timeout == 0);
    if (!s || !s->available) return 0;
    s->available = false;
    return pdTRUE;
}
void xSemaphoreGive(SemaphoreHandle_t s) { assert(!s->available); s->available = true; }
int xTaskCreate(void (*function)(void*), const char*, uint32_t stack, void* arg, int, TaskHandle_t* task) {
    assert(stack == 7168 && pauses == 0);
    if (!createSucceeds) return 0;
    pendingTask = function; pendingArgument = arg; *task = arg;
    return pdPASS;
}
void xTaskNotifyGive(TaskHandle_t) { assert(pauses == 1); notified = true; }
void ulTaskNotifyTake(int, int) { assert(notified); }
void vTaskDelete(void*) { ++deletedTasks; }
uint32_t uxTaskGetStackHighWaterMark(void*) { return 2000; }
struct { void printf(const char*, ...) {} } Serial;
namespace Protocols { constexpr int MODBUS_MAX_WRITE_COILS = 32; }
namespace FastBee { namespace MemoryBudget {
uint32_t freeBytes = 30000, blockBytes = 10000;
uint32_t dram8Free() { return freeBytes; }
uint32_t dram8Largest() { return blockBytes; }
}}
struct Client { bool online = true; bool connected() const { return online; } };
struct AsyncWebServerRequest;
using AsyncWebServerRequestPtr = std::weak_ptr<AsyncWebServerRequest>;
struct AsyncWebServerRequest : std::enable_shared_from_this<AsyncWebServerRequest> {
    Client connection;
    int status = 0;
    Client* client() { return &connection; }
    AsyncWebServerRequestPtr pause() { ++pauses; return shared_from_this(); }
};
static std::shared_ptr<AsyncWebServerRequest>* disconnectDuringOperation = nullptr;
struct WebHandlerContext {
    bool authenticated = true;
    bool requireAuth(AsyncWebServerRequest* request) {
        if (!authenticated) request->status = 401;
        return authenticated;
    }
};
namespace HandlerUtils {
void sendJsonError(AsyncWebServerRequest* r, int status, const char*, int) { r->status = status; }
}
struct ModbusRouteHandler {
    WebHandlerContext* ctx;
    using RelayHandler = void (ModbusRouteHandler::*)(AsyncWebServerRequest*);
    struct RelayJob;
    void dispatchRelayRequest(AsyncWebServerRequest*, RelayHandler);
    static void relayTask(void*);
    void execute(AsyncWebServerRequest* r) {
        ++calls;
        if (disconnectDuringOperation) {
            const auto held = std::weak_ptr<AsyncWebServerRequest>(*disconnectDuringOperation);
            r->connection.online = false;
            disconnectDuringOperation->reset();
            // Simulate the server releasing its last owner on disconnect.
            // The worker must retain the request and its parameters until done.
            assert(!held.expired());
        }
        r->status = 200;
    }
};
struct ModbusHandler {
    Semaphore bus;
    SemaphoreHandle_t _oneShotSemaphore = &bus;
    String _lastTxHex = "06 06", _lastRxHex = "06 03";
    bool getDebugSnapshot(String&, String&);
};
#include "relay_methods.inc"
int main() {
    WebHandlerContext context;
    ModbusRouteHandler owner{&context};
    auto r = std::make_shared<AsyncWebServerRequest>();
    const auto send = [&]() { owner.dispatchRelayRequest(r.get(), &ModbusRouteHandler::execute); };
    context.authenticated = false; send(); assert(r->status == 401 && pauses == 0);
    context.authenticated = true;
    slot.available = false; send(); assert(r->status == 503 && pauses == 0);
    slot.available = true;
    FastBee::MemoryBudget::blockBytes = 7000; send(); assert(slot.available && pauses == 0);
    FastBee::MemoryBudget::blockBytes = 10000;
    FastBee::MemoryBudget::freeBytes = 15000; send(); assert(slot.available && pauses == 0);
    FastBee::MemoryBudget::freeBytes = 30000;
    allocationFails = true; send(); assert(slot.available && pauses == 0);
    allocationFails = false;
    createSucceeds = false; send(); assert(slot.available && pauses == 0);
    createSucceeds = true; send(); assert(!slot.available && notified && calls == 0);
    auto second = std::make_shared<AsyncWebServerRequest>();
    owner.dispatchRelayRequest(second.get(), &ModbusRouteHandler::execute);
    assert(second->status == 503 && calls == 0);
    pendingTask(pendingArgument); assert(slot.available && calls == 1 && r->status == 200);
    pauses = 0; notified = false; send(); r.reset();
    pendingTask(pendingArgument); assert(slot.available && calls == 1);
    pauses = 0; notified = false; r = std::make_shared<AsyncWebServerRequest>(); send();
    r->connection.online = false; pendingTask(pendingArgument);
    assert(slot.available && calls == 1 && deletedTasks == 3);
    pauses = 0; notified = false; r = std::make_shared<AsyncWebServerRequest>(); send();
    const auto releasedAfterTask = std::weak_ptr<AsyncWebServerRequest>(r);
    disconnectDuringOperation = &r;
    pendingTask(pendingArgument);
    assert(slot.available && calls == 2 && deletedTasks == 4 && releasedAfterTask.expired());
    disconnectDuringOperation = nullptr;
    assert(validRelayChannel(6, 3, 0));
    assert(validRelayChannel(247, 0, 65535));
    assert(!validRelayChannel(262, 0, 0));
    assert(!validRelayChannel(6, -1, 0));
    assert(!validRelayChannel(6, 32, 0));
    assert(!validRelayChannel(6, 1, 65535));
    assert(!validRelayChannel(6, 0, -1));
    ModbusHandler bus; String tx, rx;
    bus.bus.available = false; assert(!bus.getDebugSnapshot(tx, rx));
    assert(tx.empty() && rx.empty());
    bus.bus.available = true; assert(bus.getDebugSnapshot(tx, rx));
    bus._lastTxHex = "different transaction";
    assert(tx == "06 06" && rx == "06 03" && bus.bus.available);
}
