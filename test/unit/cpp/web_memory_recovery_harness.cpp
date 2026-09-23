#include <cstdint>
#include <cstdlib>
#include <iostream>
#include "network/WebMemoryGuard.h"

#define CHECK(condition) do { if (!(condition)) { std::cerr << "check failed at " << __LINE__ << ": " << #condition << "\n"; std::exit(1); } } while (0)
#define ARDUINOJSON_BEGIN_PRIVATE_NAMESPACE namespace json_writer_test {
#define ARDUINOJSON_END_PRIVATE_NAMESPACE }
#define ARDUINOJSON_STRING_BUFFER_SIZE 32
#define ARDUINOJSON_ASSERT(condition) CHECK(condition)
namespace json_writer_test {template<class, class> class Writer;}
#include <ArduinoJson/Serialization/Writers/ArduinoStringWriter.hpp>
struct StatusCache {String json;};
static bool statusRejected(size_t written, size_t expectedSize, const StatusCache& _statusCache) {
#include "production_status_rejected.inc"
}
uint32_t testFree=30000, testLargest=16000;
static unsigned long testNow=100;
unsigned long millis() {return testNow;}
struct SerialStub {template<class... Args> void printf(const char*, Args...) {}} Serial;
class SSERouteHandler {public: int closed=0; void closeAllClients() {++closed;}};
using FastBee::MemoryBudget;
void WebMemoryGuardMiddleware::run(AsyncWebServerRequest*, ArMiddlewareNext) {}
#include "production_maintain.inc"

static void sample(WebMemoryGuardMiddleware& guard, unsigned long now, uint32_t free8, uint32_t largest) {
    testNow=now; testFree=free8; testLargest=largest; guard.maintain();
}
int main() {
    // Real ArduinoJson writer reports all attempted buffer bytes even when
    // destination.concat fails. The production predicate must reject that.
    const std::string payload(100, 'x');
    StatusCache cache;
    cache.json.capacity=31;
    size_t written=0;
    {
        json_writer_test::Writer<String, void> writer(cache.json);
        written=writer.write(reinterpret_cast<const uint8_t*>(payload.data()),payload.size());
    }
    CHECK(written==payload.size()); CHECK(cache.json.length()<payload.size());
    CHECK(statusRejected(written,payload.size(),cache));
    cache.json.capacity=1000;
    {
        json_writer_test::Writer<String, void> writer(cache.json);
        written=writer.write(reinterpret_cast<const uint8_t*>(payload.data()),payload.size());
    }
    CHECK(cache.json.length()==payload.size());
    CHECK(!statusRejected(written,payload.size(),cache));
    CHECK(statusRejected(written-1,payload.size(),cache));
    CHECK(MemoryBudget::WEB_GUARD_MIN_DRAM_FREE==20480);
    CHECK(MemoryBudget::WEB_GUARD_MIN_DRAM_BLK8==4096);
    CHECK(!MemoryBudget::hasWebStreamingHeadroom(21503,7668));
    CHECK(!MemoryBudget::hasWebStreamingHeadroom(28000,6143));
    CHECK(MemoryBudget::hasWebStreamingHeadroom(21504,6144));
    CHECK(MemoryBudget::isSevereWebPressure8(7900,7668,72));
    CHECK(MemoryBudget::isSevereWebPressure8(28000,1400,95));
    WebMemoryGuardMiddleware guard; SSERouteHandler sse; guard.setSseHandler(&sse);
    sample(guard,100,17000,6388); CHECK(!guard.isActive());
    sample(guard,599,17000,6388); CHECK(!guard.isActive());
    sample(guard,600,17000,6388); CHECK(guard.isActive()); CHECK(sse.closed==1);
    // Observed C3 recovery: sufficient total heap but largest remains 7668.
    sample(guard,1000,28000,7668); CHECK(guard.isActive());
    sample(guard,5999,28000,7668); CHECK(guard.isActive());
    sample(guard,6000,28000,7668); CHECK(!guard.isActive());
    // Recovery resets when either dimension dips; brief spikes do not unlock.
    sample(guard,7000,17000,6388); sample(guard,7500,17000,6388); CHECK(guard.isActive());
    sample(guard,8000,28000,7668); sample(guard,12999,21503,7668); CHECK(guard.isActive());
    sample(guard,13000,28000,7668); sample(guard,17999,28000,6143); CHECK(guard.isActive());
    sample(guard,18000,21504,6144); sample(guard,23000,21504,6144); CHECK(!guard.isActive());
    // Legacy ample-block fast recovery and sustained real pressure still work.
    sample(guard,24000,30000,4000); sample(guard,24500,30000,4000); CHECK(guard.isActive());
    sample(guard,25000,21504,9216); CHECK(!guard.isActive());
    sample(guard,26000,17000,6388); sample(guard,26500,17000,6388); CHECK(guard.isActive());
    sample(guard,220000,17000,16000); CHECK(guard.isActive());
    std::cout << "Production guard: activation, stable/fast recovery, interruptions and safety floors passed\n";
}
