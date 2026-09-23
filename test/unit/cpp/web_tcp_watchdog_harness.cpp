#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>

#define CHECK(condition) do { if (!(condition)) { std::cerr << "check failed at " << __LINE__ << ": " << #condition << "\n"; std::exit(1); } } while (0)
template<class... Args> void testLog(const char*, Args...) {}
#define LOG_DEBUG(...) testLog(__VA_ARGS__)
#define LOG_DEBUGF(...) testLog(__VA_ARGS__)
#define LOG_WARNINGF(...) testLog(__VA_ARGS__)
#define LOG_INFOF(...) testLog(__VA_ARGS__)

static uint32_t clockNow = 0;
uint32_t millis() { return clockNow; }
constexpr int WL_CONNECTED = 3;
constexpr int MALLOC_CAP_DEFAULT = 0;
struct WifiStub { int state = WL_CONNECTED; int status() const { return state; } } WiFi;
struct EspStub { uint32_t getFreeHeap() const { return 30000; } } ESP;
uint32_t heap_caps_get_largest_free_block(int) { return 18000; }
struct SseStub {
    unsigned clients = 0, closes = 0;
    unsigned clientCount() const { return clients; }
    void closeAllClients() { ++closes; }
};

class WebConfigManager {
public:
#include "production_request_timestamp.inc"
    bool isRunning = true;
    SseStub* sseRouteHandler = nullptr;
    static constexpr unsigned long NO_REQUEST_WATCHDOG_MS = 120000UL;
    unsigned reaped = 0, events = 0;
    bool requestDuringPcbCount = false;
    unsigned long getLastWebRequestAgeMs() const;
    uint16_t countTcpConnections(uint8_t* timeWait) const { *timeWait = 2; return 3; }
    uint16_t countTcpConnectionsOnLocalPort(uint16_t port, uint8_t* timeWait) {
        CHECK(port == 80);
        *timeWait = 2;
        if (requestDuringPcbCount) lastRequestSeenMs = clockNow;
        return 3;
    }
    uint8_t abortPcbsOnLocalPort(uint16_t port) {
        CHECK(port == 80);
        ++reaped;
        return 3;
    }
    void recordRecoveryEvent(const char*, const char*, unsigned long,
                             uint32_t, uint32_t, unsigned) { ++events; }
    void getTcpDiag(uint16_t& total, uint8_t& timeWait, unsigned long& lastReqAgeMs) const {
#include "production_request_diag.inc"
    }
    void watchdog(unsigned long maintenanceNow) {
#include "production_request_watchdog.inc"
    }
};
#include "production_request_age.inc"

static void checkAge(WebConfigManager& manager, unsigned long expected) {
    CHECK(manager.getLastWebRequestAgeMs() == expected);
    uint16_t total = 0;
    uint8_t timeWait = 0;
    unsigned long age = 0;
    manager.getTcpDiag(total, timeWait, age);
    CHECK(age == expected && total == 3 && timeWait == 2);
}

int main() {
    WebConfigManager manager;
    // A new request arrives after maintenance captured its clock. The old
    // expression produced UINT32_MAX-1 and killed all active Web connections.
    const uint32_t oldMaintenance = 1642600;
    manager.lastRequestSeenMs = oldMaintenance + 2;
    clockNow = oldMaintenance + 3;
    CHECK(uint32_t(oldMaintenance - uint32_t(manager.lastRequestSeenMs.load())) > 4000000000U);
    checkAge(manager, 1);
    manager.watchdog(oldMaintenance);
    CHECK(manager.reaped == 0 && manager.events == 0);

    // millis() rollover remains a small positive elapsed time, including
    // a request timestamp captured after the maintenance clock wrapped.
    manager.lastRequestSeenMs = 0xfffffff0U;
    clockNow = 20;
    checkAge(manager, 36);
    manager.watchdog(0xffffffefU);
    manager.lastRequestSeenMs = 3;
    clockNow = 4;
    manager.watchdog(0xfffffffeU);
    CHECK(manager.reaped == 0);

    // Genuine inactivity at the existing threshold still reaps port 80.
    manager.lastRequestSeenMs = 100;
    clockNow = 120099;
    manager.watchdog(clockNow);
    CHECK(manager.reaped == 0);
    clockNow = 120100;
    manager.watchdog(clockNow);
    CHECK(manager.reaped == 1 && manager.events == 1);

    // An arriving request while counting PCBs invalidates the old decision.
    manager.lastRequestSeenMs = 100;
    clockNow = 240100;
    manager.requestDuringPcbCount = true;
    manager.watchdog(clockNow);
    CHECK(manager.reaped == 1 && manager.events == 1);
    manager.requestDuringPcbCount = false;

    // Existing long-connection and disconnected-network exemptions remain.
    SseStub sse;
    sse.clients = 1;
    manager.sseRouteHandler = &sse;
    manager.lastRequestSeenMs = 100;
    manager.watchdog(clockNow);
    CHECK(manager.reaped == 1 && sse.closes == 0);
    manager.sseRouteHandler = nullptr;
    manager.lastRequestSeenMs = 100;
    WiFi.state = 0;
    manager.watchdog(clockNow);
    CHECK(manager.reaped == 1);
    WiFi.state = WL_CONNECTED;

    manager.lastRequestSeenMs = 0;
    checkAge(manager, 0);
    manager.watchdog(clockNow);
    CHECK(manager.reaped == 1);
    manager.lastRequestSeenMs = 100;
    manager.isRunning = false;
    manager.watchdog(clockNow);
    CHECK(manager.reaped == 1);
    manager.isRunning = true;

    // Model frequent upload chunks across >120 seconds with the real idle
    // decision. Each callback refreshes activity; the request never idles.
    for (uint32_t tick = 250000; tick < 550000; tick += 200) {
        manager.lastRequestSeenMs = tick;
        clockNow = tick + 1;
        manager.watchdog(tick - 10);
    }
    CHECK(manager.reaped == 1 && manager.events == 1);
    std::cout << "Production Web watchdog: cross-task clocks, wraparound, activity recheck, OTA chunks and stale recovery passed\n";
}
