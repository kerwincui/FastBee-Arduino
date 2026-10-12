// Real route body and serializer; only HTTP/lock/planner/manager dependencies
// are substituted. Real registries are exercised by their lifecycle harnesses.
#include <Arduino.h>
#include "network/RuntimePinResourceJson.h"
#include <cassert>
#include <cstring>
#include <vector>
#include <memory>
#include <new>
#define FASTBEE_ENABLE_SENSOR_DRIVER 1
#define FASTBEE_ENABLE_SCREEN_UI 1
static bool resultBusy=false, managerBusy=false, authenticated=true, lowMemory=false;
static int lockDepth=0, snapshots=0, copiedConfigs=0;
static bool unsupported=false, comparison=false;
static bool workspaceOom=false;
static int workspaceLive=0, workspaceCreated=0;
static JsonDocument response;
class AsyncWebServerRequest {};
struct LockState { bool busy; };
class MutexGuard {
public:
    explicit MutexGuard(void*, unsigned timeout) : locked(!resultBusy) { assert(timeout==0); if(locked)++lockDepth; }
    ~MutexGuard() { if(locked)--lockDepth; }
    bool isLocked() const { return locked; }
private: bool locked;
};
class RecursiveMutexGuard {
public:
    explicit RecursiveMutexGuard(void*, unsigned timeout) : locked(!managerBusy) { assert(timeout==0); if(locked)++lockDepth; }
    ~RecursiveMutexGuard() { if(locked)--lockDepth; }
    bool isLocked() const { return locked; }
private: bool locked;
};
struct PeripheralConfig { bool enabled=true; };
namespace FastBee {
struct BoardProfile { const char* key; const char* displayName; unsigned version; };
const BoardProfile activeProfile{"active", "Active board", 1}, comparedProfile{"compared", "Comparison board", 2};
const BoardProfile& getCompiledBoardProfile() { return activeProfile; }
struct PreflightResult {
    bool claimsTruncated=false, issuesTruncated=false;
    PreflightResult() { assert(lockDepth==2); ++workspaceLive; ++workspaceCreated; }
    ~PreflightResult() { --workspaceLive; }
    static void* operator new(size_t bytes, const std::nothrow_t&) noexcept {
        return workspaceOom ? nullptr : ::operator new(bytes, std::nothrow);
    }
    static void operator delete(void* pointer) noexcept { ::operator delete(pointer); }
};
enum class ClaimState { ACTIVE, DORMANT };
constexpr int ACK_NONE=0;
class PeripheralResourcePlanner {
public:
    PeripheralResourcePlanner(const BoardProfile&, int, PreflightResult&, const PeripheralConfig*, size_t) {}
    void addPeripheral(const PeripheralConfig&, ClaimState) {}
};
class I2CBusManager {
public:
    static I2CBusManager& instance() { static I2CBusManager m;return m; }
    void snapshotResources(I2CResourceSnapshot& value) {
        assert(lockDepth==0 && workspaceLive==0);++snapshots;value.count=1;value.leases[0].retiring=true;
    }
};
class SpiBusInitializer {
public:
    static SpiBusInitializer& instance() { static SpiBusInitializer m;return m; }
    void snapshotResources(SpiResourceSnapshot& value) {
        assert(lockDepth==0);++snapshots;value.status=ResourceObservation::Busy;
    }
};
class UartPortLease {
public:
    static void snapshotResources(UartResourceSnapshot& value, uint8_t count) {
        assert(lockDepth==0);++snapshots;value.portCount=count;value.generations[1]=5;
    }
};
void snapshotAdcResources(AdcResourceSnapshot& value) {
    assert(lockDepth==0); ++snapshots; value.observed=true; value.unitCount=1;
    value.units[0].retained=true; value.units[0].cleanupFailed=true;
}
namespace Screen {
class ScreenRuntime {
public: static void snapshotBacklightResources(BacklightResourceSnapshot& value) {
    assert(lockDepth==0);++snapshots;value.available=true;value.retained=true;value.pin=45;value.channel=3;
} };
}
}
class PeripheralManager {
public:
    static PeripheralManager& getInstance() { static PeripheralManager m;return m; }
    void* getMutex() { return this; }
    std::vector<PeripheralConfig> getAllPeripherals() { assert(lockDepth==2);++copiedConfigs;return {PeripheralConfig{}}; }
    void snapshotControlOutputResources(FastBee::ControlOutputResourceSnapshot& value) {
        assert(lockDepth==0);++snapshots;
        if(managerBusy)value.status=FastBee::ResourceObservation::Busy;
    }
    void snapshotEncoderResources(FastBee::EncoderResourceSnapshot& value) {
        assert(lockDepth==0);++snapshots;
        if(managerBusy)value.status=FastBee::ResourceObservation::Busy;
        else { value.count=1;value.leases[0].pinA=4;value.leases[0].pinB=5; }
    }
    void snapshotLedcResources(FastBee::LedcResourceSnapshot& value) {
        assert(lockDepth==0);++snapshots;
        if(managerBusy)value.status=FastBee::ResourceObservation::Busy;
        else { value.count=1;value.leases[0].pin=18;value.leases[0].cleanupFailed=true; }
    }
    void snapshotGpioResources(FastBee::GpioResourceSnapshot& value) {
        assert(lockDepth==0); ++snapshots;
        if(managerBusy)value.status=FastBee::ResourceObservation::Busy;
        else { value.count=1;value.leases[0].pin=4;value.leases[0].fault=true; }
    }
    void snapshotDacResources(FastBee::DacResourceSnapshot& value) {
        assert(lockDepth==0); ++snapshots; value.supported=false;
        if(managerBusy)value.status=FastBee::ResourceObservation::Busy;
    }
    void snapshotNeoPixelResources(FastBee::NeoPixelResourceSnapshot& value) {
        assert(lockDepth==0); ++snapshots; value.supported=true;
        if(managerBusy)value.status=FastBee::ResourceObservation::Busy;
        else { value.objectAllocated=true; value.projectPinRetained=true; value.pin=18; }
    }
    void snapshotStepperResources(FastBee::StepperResourceSnapshot& value) {
        assert(lockDepth==0); ++snapshots;
        if(managerBusy)value.status=FastBee::ResourceObservation::Busy;
        else { value.count=1; value.leases[0].retainedMask=5; value.leases[0].cleanupPending=true; }
    }
};
class SensorDriver {
public:
    static void snapshotDhtRmtResources(FastBee::DhtRmtResourceSnapshot& value) {
        assert(lockDepth==0);++snapshots;value.supported=true;value.retainedPin=15;
    }
};
struct Context {
    void* networkManager=nullptr;
    int errorCode=0;
    bool requireAuth(AsyncWebServerRequest*) { return authenticated; }
    String getParamValue(AsyncWebServerRequest*,const char*,const char*) { return ""; }
    void sendError(AsyncWebServerRequest*,int code,const char*) { errorCode=code; }
};
using WebHandlerContext = Context;
bool compiledBoardProfileSupported() { return true; }
const FastBee::BoardProfile* resolveBoardProfile(const String&) {
    return unsupported?nullptr:comparison?&FastBee::comparedProfile:&FastBee::activeProfile;
}
bool sameText(const char* a,const char* b) { return std::strcmp(a,b)==0; }
int makePreflightContext(const FastBee::BoardProfile&,int,void*) { return 0; }
void addResourceRows(JsonArray rows,const FastBee::BoardProfile&,const FastBee::PreflightResult&) {
    auto row=rows.add<JsonObject>();row["used"]=4;row["available"]=3;
}
void addPreflightIssues(JsonArray,const FastBee::PreflightResult&) {}
namespace HandlerUtils {
bool checkLowMemory(AsyncWebServerRequest*,size_t) { return lowMemory; }
void sendJsonStream(AsyncWebServerRequest*,JsonDocument& doc) { assert(!doc.overflowed());response=doc; }
}
class PeripheralRouteHandler {
public:
    Context* ctx;
    void* preflightMutex=this;
    void handleGetPeripheralResources(AsyncWebServerRequest* request);
};
#include "resource_route_under_test.inc"
int main() {
    Context context;PeripheralRouteHandler route;route.ctx=&context;AsyncWebServerRequest request;
    route.handleGetPeripheralResources(&request);
    assert(response["success"]==true && snapshots==13 && copiedConfigs==1);
    assert(response["data"]["runtime"]["neopixel"]["leases"][0]["pin"]==18);
    assert(response["data"]["runtime"]["stepper"]["leases"][0]["retainedMask"]==5);
    assert(response["data"]["runtime"]["adc"]["units"][0]["cleanupFailed"]==true);
    assert(response["data"]["runtime"]["gpio"]["leases"][0]["fault"]==true);
    assert(response["data"]["runtime"]["dac"]["status"]=="unsupported");
    assert(response["data"]["runtime"]["ledc"]["leases"][0]["cleanupFailed"]==true);
    assert(response["data"]["configurationStatus"]=="observed");
    assert(response["data"]["claimScope"]=="configuration" && response["data"]["runtimeReservationsIncluded"]==false);
    assert(response["data"]["resources"][0]["used"]==4 && response["data"]["runtime"]["i2c"]["leaseCount"]==1);
    assert(response["data"]["runtime"]["spi"]["leaseCount"].isNull());
    resultBusy=true;
    route.handleGetPeripheralResources(&request);
    assert(response["data"]["configurationStatus"]=="busy" && response["data"]["resources"].isNull());
    assert(snapshots==26 && copiedConfigs==1 && context.errorCode==0);
    resultBusy=false;managerBusy=true;
    route.handleGetPeripheralResources(&request);
    assert(response["data"]["configurationStatus"]=="busy" && response["data"]["claimsTruncated"].isNull());
    assert(response["data"]["runtime"]["controlOutput"]["status"]=="busy");
    assert(response["data"]["runtime"]["encoder"]["leaseCount"].isNull());
    assert(response["data"]["runtime"]["ledc"]["leaseCount"].isNull());
    assert(response["data"]["runtime"]["gpio"]["leaseCount"].isNull());
    assert(response["data"]["runtime"]["dac"]["leaseCount"].isNull());
    assert(response["data"]["runtime"]["neopixel"]["leaseCount"].isNull());
    assert(response["data"]["runtime"]["stepper"]["leaseCount"].isNull());
    assert(response["data"]["runtime"]["dhtRmt"]["leases"][0]["cleanupFailed"]==true);
    assert(response["data"]["runtime"]["uart"]["leaseCount"]==1 && copiedConfigs==1);
    managerBusy=false;comparison=true;
    route.handleGetPeripheralResources(&request);
    assert(response["data"]["profileId"]=="compared" && response["data"]["comparison"]==true);
    assert(response["data"]["runtime"]["activeProfileId"]=="active");
    const int observed=snapshots;
    authenticated=false;route.handleGetPeripheralResources(&request);assert(snapshots==observed);
    authenticated=true;lowMemory=true;route.handleGetPeripheralResources(&request);assert(snapshots==observed);
    lowMemory=false;unsupported=true;route.handleGetPeripheralResources(&request);
    assert(snapshots==observed && context.errorCode==422 && lockDepth==0);
    unsupported=false;comparison=false;context.errorCode=0;
    for (unsigned repeat=0;repeat<100;++repeat) {
        route.handleGetPeripheralResources(&request);
        assert(response["success"]==true && lockDepth==0 && workspaceLive==0);
    }
    const int beforeOom=snapshots,beforeConfigs=copiedConfigs,beforeCreated=workspaceCreated;
    response.clear();workspaceOom=true;
    route.handleGetPeripheralResources(&request);
    assert(context.errorCode==503 && response.isNull() && lockDepth==0 && workspaceLive==0);
    assert(snapshots==beforeOom && copiedConfigs==beforeConfigs && workspaceCreated==beforeCreated);
    workspaceOom=false;resultBusy=true;context.errorCode=0;
    route.handleGetPeripheralResources(&request);
    assert(context.errorCode==0 && workspaceCreated==beforeCreated && workspaceLive==0);
    resultBusy=false;route.handleGetPeripheralResources(&request);
    assert(response["success"]==true && workspaceLive==0 && lockDepth==0);
}
