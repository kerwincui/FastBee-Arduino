#include "peripherals/drivers/LCD1602Driver.h"
#include "network/LiteralStringMatch.h"
#include "core/I2CBusManager.h"
#include "core/RuntimeFeatureManager.h"
#include <atomic>
#include <cassert>
#include <thread>
#include <limits>
// A regression should fail deterministically, not open Windows crash UI.
#undef assert
#define assert(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "CHECK failed: %s at %s:%d\n", #condition, __FILE__, __LINE__); \
    std::fflush(stderr); std::_Exit(86); \
} } while (false)

using FastBee::I2CBusManager;
static LCD1602Driver& lcd() { return LCD1602Driver::getInstance(); }
static void start(const LCD1602Config& config = LCD1602Config()) {
    assert(lcd().begin(4, 5, config));
    assert(lcd().isInitialized());
    Wire.resetTrace(); lcdDelaysUs.clear();
}
static std::vector<uint8_t> decode(size_t offset, bool data, bool backlight = true) {
    assert((Wire.transfers.size() - offset) % 6 == 0);
    std::vector<uint8_t> nibbles, result;
    for (size_t i = offset; i < Wire.transfers.size(); i += 3) {
        for (size_t j = 0; j < 3; ++j) {
            assert(Wire.transfers[i + j].bytes.size() == 1);
            assert(Wire.transfers[i + j].status == 0);
        }
        const uint8_t low = Wire.transfers[i].bytes[0];
        assert((low & 7) == (data ? 1 : 0));
        assert(bool(low & 8) == backlight);
        assert(Wire.transfers[i + 1].bytes[0] == (low | 4));
        assert(Wire.transfers[i + 2].bytes[0] == low);
        nibbles.push_back(low >> 4);
    }
    for (size_t i = 0; i < nibbles.size(); i += 2)
        result.push_back((nibbles[i] << 4) | nibbles[i + 1]);
    return result;
}
static void bootstrap() {
    assert(lcd().begin(4, 5));
    assert(Wire.transfers.size() == 43);
    assert(Wire.transfers[0].bytes == std::vector<uint8_t>{8});
    const uint8_t expected[] = {0x38, 0x38, 0x38, 0x28};
    for (size_t n = 0; n < 4; ++n) {
        const size_t i = 1 + n * 3;
        assert(Wire.transfers[i].bytes[0] == expected[n]);
        assert(Wire.transfers[i + 1].bytes[0] == (expected[n] | 4));
        assert(Wire.transfers[i + 2].bytes[0] == expected[n]);
    }
    assert(decode(13, false) == std::vector<uint8_t>({0x28, 0x0c, 0x01, 0x06, 0x02}));
    assert(std::count(lcdDelaysUs.begin(), lcdDelaysUs.end(), 2000) == 2);
    assert(std::count(lcdDelaysUs.begin(), lcdDelaysUs.end(), 50000) == 1);
}
static void initFailures(bool shortWrite) {
    for (size_t index = 1; index <= 43; ++index) {
        lcd().end(); Wire.resetTrace();
        if (shortWrite) Wire.shortAt = index; else Wire.failAt = index;
        assert(!lcd().begin(4, 5));
        assert(!lcd().isInitialized());
        assert(Wire.transfers.size() == index);
        assert(I2CBusManager::instance().clientCount() == 0);
        assert(!lcd().print("ignored"));
        assert(Wire.transfers.size() == index);
    }
}
static void printFailure() {
    for (uint8_t status : {1, 2, 3, 4, 5}) {
        for (size_t index = 1; index <= 12; ++index) {
            start(); Wire.failureStatus = status; Wire.failAt = index;
            assert(!lcd().print("AB"));
            assert(!lcd().isInitialized());
            assert(Wire.transfers.size() == index);
            Wire.failAt = 0;
            assert(!lcd().print("must-reinitialize"));
            assert(Wire.transfers.size() == index);
            lcd().end();
            assert(I2CBusManager::instance().clientCount() == 0);
        }
    }
}
static void lineFailures() {
    // Cursor, clearing spaces, second cursor, first and last text nibbles.
    for (size_t index : {1, 6, 7, 102, 103, 108, 109, 120}) {
        start(); Wire.failAt = index;
        assert(!lcd().printLine(1, "AB"));
        assert(!lcd().isInitialized());
        assert(Wire.transfers.size() == index);
        lcd().end();
    }
}
static void dataPulses() {
    for (bool light : {true, false}) {
        LCD1602Config config; config.backlight = light;
        start(config);
        assert(lcd().print("Az0"));
        assert(decode(0, true, light) == std::vector<uint8_t>({'A', 'z', '0'}));
        lcd().end();
    }
}
static void dimensions() {
    LCD1602Config config;
    for (const auto size : std::vector<std::pair<uint8_t,uint8_t>>{{0,2},{16,0},{16,5},{20,2},{255,255}}) {
        config.cols = size.first; config.rows = size.second;
        assert(!lcd().begin(4, 5, config));
        assert(Wire.transfers.empty());
        assert(I2CBusManager::instance().clientCount() == 0);
    }
    config.cols = 20; config.rows = 4; start(config);
    lcd().setCursor(255, 255);
    assert(decode(0, false) == std::vector<uint8_t>{0xe7});
}
static void addresses() {
    LCD1602Config config;
    for (uint8_t address : {1, 0x1f, 0x28, 0x37, 0x40, 0x7f, 0xff}) {
        config.i2cAddress = address;
        assert(!lcd().begin(4, 5, config));
        assert(Wire.transfers.empty());
        assert(I2CBusManager::instance().clientCount() == 0);
    }
}
static void backlightFailure() {
    start(); Wire.failAt = 1;
    assert(!lcd().backlightOff());
    assert(!lcd().isInitialized());
    assert(lcd().isBacklightOn()); // Requested state was not acknowledged.
    assert(!lcd().print("ignored"));
    assert(Wire.transfers.size() == 1);
}
class BusHolder {
    std::atomic<bool> held{false}, stop{false};
    std::thread thread;
public:
    BusHolder() : thread([this] {
        assert(I2CBusManager::instance().lock(100)); held = true;
        while (!stop.load()) std::this_thread::yield();
        I2CBusManager::instance().unlock();
    }) { while (!held.load()) std::this_thread::yield(); }
    ~BusHolder() { stop = true; thread.join(); }
};
static void busyAndRetirement() {
    start();
    {
        BusHolder holder;
        const auto began = std::chrono::steady_clock::now();
        assert(!lcd().print("busy"));
        lcd().end();
        assert(Wire.transfers.empty());
        assert(std::chrono::steady_clock::now() - began < std::chrono::milliseconds(500));
    }
    assert(I2CBusManager::instance().clientCount() == 0);
    assert(lcd().begin(18, 19));
}
static void reinitialize() {
    start(); Wire.failAt = 2;
    assert(!lcd().print("partial"));
    Wire.resetTrace();
    assert(lcd().begin(18, 19));
    assert(I2CBusManager::instance().sda() == 18);
    assert(I2CBusManager::instance().clientCount() == 1);
}
static void featureDisabled() {
    FastBee::RuntimeFeatureManager::instance().enabled = false;
    assert(!lcd().begin(4, 5));
    assert(Wire.transfers.empty());
    assert(I2CBusManager::instance().clientCount() == 0);
}
static void autoDetect() {
    LCD1602Config config; config.i2cAddress = 0;
    Wire.detectedAddress = 0x3f;
    assert(lcd().begin(4, 5, config));
    assert(Wire.transfers[0].address == 0x27 && Wire.transfers[0].status == 2);
    assert(Wire.transfers[1].address == 0x3f && Wire.transfers[1].status == 0);
    lcd().end(); Wire.resetTrace(); Wire.detectedAddress = 0x7e;
    assert(!lcd().begin(4, 5, config));
    assert(I2CBusManager::instance().clientCount() == 0);
    assert(Wire.transfers.size() == 16);
}
static void characterFailure() {
    uint8_t pixels[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    start(); Wire.failAt = 7;
    lcd().createChar(1, pixels);
    assert(!lcd().isInitialized());
    assert(Wire.transfers.size() == 7);
}
static void runtimeShortWrite() {
    for (size_t index = 1; index <= 12; ++index) {
        start(); Wire.shortAt = index;
        assert(!lcd().print("AB"));
        assert(!lcd().isInitialized());
        assert(Wire.transfers.size() == index);
        lcd().end();
        assert(I2CBusManager::instance().clientCount() == 0);
    }
}
static void sharedLease() {
    int peer = 0; String error; auto& bus = I2CBusManager::instance();
    assert(bus.acquire(&peer, 4, 5, 100000, error));
    const unsigned begins = Wire.begins, ends = Wire.ends;
    Wire.failAt = 5;
    assert(!lcd().begin(4, 5));
    assert(bus.clientCount() == 1 && bus.isAcquired(&peer));
    assert(Wire.begins == begins && Wire.ends == ends);
    Wire.resetTrace(); start();
    lcd().end();
    assert(bus.clientCount() == 1 && bus.isAcquired(&peer));
    assert(Wire.ends == ends);
    assert(bus.release(&peer));
}
static void lostLease() {
    start();
    assert(I2CBusManager::instance().release(&lcd()));
    int peer = 0; String error;
    assert(I2CBusManager::instance().acquire(&peer, 4, 5, 100000, error));
    assert(!lcd().print("not-owner"));
    assert(!lcd().isInitialized());
    assert(Wire.transfers.empty());
    lcd().end();
    assert(I2CBusManager::instance().isAcquired(&peer));
    assert(I2CBusManager::instance().release(&peer));
}
static void numericBounds() {
    start();
    assert(lcd().print(1.25f, uint8_t{2}));
    assert(decode(0, true) == std::vector<uint8_t>({'1','.','2','5'}));
    Wire.resetTrace();
    assert(lcd().print(-15, 10));
    assert(decode(0, true) == std::vector<uint8_t>({'-','1','5'}));
    Wire.resetTrace();
    assert(!lcd().print(std::numeric_limits<float>::max(), uint8_t{2}));
    assert(!lcd().print(std::numeric_limits<float>::infinity(), uint8_t{2}));
    assert(!lcd().print(std::numeric_limits<float>::quiet_NaN(), uint8_t{2}));
    assert(!lcd().print(1.0f, uint8_t{255}));
    assert(Wire.transfers.empty());
    assert(lcd().isInitialized()); // Validation errors have no bus side effect.
}
static void allDataBytes() {
    start();
    for (unsigned value = 0; value <= 255; ++value) {
        const char byte = static_cast<char>(value);
        assert(lcd().print(String(&byte, 1)));
        assert(decode(0, true) == std::vector<uint8_t>{static_cast<uint8_t>(value)});
        Wire.resetTrace();
    }
}
static void commandGuards() {
    start();
    lcd().createChar(0, nullptr);
    assert(Wire.transfers.empty());
    assert(!lcd().printLine(2, "bad-row"));
    assert(Wire.transfers.empty());
    lcd().clear(); lcd().home(); lcd().cursor(); lcd().noCursor();
    lcd().blink(); lcd().noBlink(); lcd().autoscroll(); lcd().noAutoscroll();
    assert(decode(0, false) == std::vector<uint8_t>({1,2,0x0e,0x0c,0x0f,0x0c,7,6}));
    lcd().end(); Wire.resetTrace();
    lcd().clear(); lcd().home(); lcd().cursor(); lcd().noCursor();
    lcd().blink(); lcd().noBlink(); lcd().autoscroll(); lcd().noAutoscroll();
    lcd().backlightOn(); lcd().backlightOff(); lcd().setCursor(255,255);
    assert(!lcd().printTwoLines("off","off"));
    assert(Wire.transfers.empty());
}
static void byteTransactionLock() {
    start();
    std::atomic<bool> requested{false}, attempted{false}, entered{false};
    std::thread competitor([&] {
        while (!requested.load()) std::this_thread::yield();
        const bool locked = I2CBusManager::instance().lock(0);
        entered = locked;
        if (locked) I2CBusManager::instance().unlock();
        attempted = true;
    });
    Wire.onEnd = [&] {
        requested = true;
        while (!attempted.load()) std::this_thread::yield();
    };
    assert(lcd().print("A"));
    competitor.join(); Wire.onEnd = nullptr;
    assert(!entered.load());
    assert(decode(0, true) == std::vector<uint8_t>{'A'});
}
static void busBeginFailure() {
    Wire.beginResult = false;
    assert(!lcd().begin(4,5));
    assert(!lcd().isInitialized() && Wire.transfers.empty());
    assert(I2CBusManager::instance().clientCount() == 0);
    Wire.beginResult = true; start();
}
static void concurrentBusyFailure() {
    start();
    std::atomic<bool> firstEntered{false}, releaseFirst{false}, firstResult{true};
    Wire.onEnd = [&] {
        firstEntered = true;
        while (!releaseFirst.load()) std::this_thread::yield();
    };
    std::thread first([&] { firstResult = lcd().print("A"); });
    while (!firstEntered.load()) std::this_thread::yield();
    assert(!lcd().print("B"));
    releaseFirst = true; first.join(); Wire.onEnd = nullptr;
    // The uncertain protocol is quarantined, never continued/retried in place.
    assert(!firstResult.load() && !lcd().isInitialized());
    assert(Wire.transfers.size() == 1);
    Wire.resetTrace();
    assert(lcd().begin(4,5));
}
static void addressConflict() {
    int peer = 0; String error;
    auto& manager = I2CBusManager::instance();
    assert(manager.acquire(&peer, 4, 5, 100000, error));
    assert(manager.claimAddress(&peer, 0x27, error));
    assert(!lcd().begin(4, 5));
    assert(Wire.transfers.empty() && manager.clientCount() == 1);
    assert(manager.ownsAddress(&peer, 0x27));
    assert(manager.release(&peer));
    start();
}

enum class PeripheralType { LCD1602 };
struct RouteRequest {
    String action="clear", text="A"; int row=0, col=0;
    bool enabled=false, hasEnabled=true;
    bool hasParam(const char* key,bool) const { return String(key)=="enabled" && hasEnabled; }
};
struct RouteResult { bool success=false; int status=0; String message; };
struct RouteContext {
    RouteResult& output;
    String getParamValue(RouteRequest* request,const char* key,const char*) {
        return String(key)=="action" ? request->action : request->text;
    }
    int getParamInt(RouteRequest* request,const char* key,int) {
        return String(key)=="row" ? request->row : request->col;
    }
    bool getParamBool(RouteRequest* request,const char*,bool) { return request->enabled; }
    void sendError(RouteRequest*,int status,const String& message) {
        output.success=false; output.status=status; output.message=message;
    }
    void sendSuccess(RouteRequest*,const String& message) {
        output.success=true; output.status=200; output.message=message;
    }
};
static RouteResult route(RouteRequest input) {
    RouteResult response;
    auto invoke=[&]() {
        RouteContext context{response}; auto* ctx=&context; auto* request=&input;
        struct { PeripheralType type=PeripheralType::LCD1602; bool enabled=true; } config;
        constexpr size_t LCD1602_API_MAX_BYTES=128;
        bool success=false; String message;
        // @lcd1602_http_branch@
        // @lcd1602_http_response@
    };
    invoke(); return response;
}
static void checkedRoutes(const std::string& name) {
    RouteRequest request;
    request.action=name=="route_clear" ? "clear" : name=="route_backlight" ? "backlight" : "print";
    const size_t length=request.action=="clear" ? 6 : request.action=="backlight" ? 1 : 12;
    for(size_t fail=1;fail<=length;++fail) {
        start(); Wire.failAt=fail;
        const auto result=route(request);
        assert(!result.success && result.status==400);
        assert(result.message==(request.action=="clear" ? "LCD1602_CLEAR_UNCONFIRMED" :
            request.action=="backlight" ? "LCD1602_BACKLIGHT_UNCONFIRMED" : "LCD1602_PRINT_FAILED"));
        assert(!lcd().isInitialized() && Wire.transfers.size()==fail);
        Wire.failAt=0; assert(!route(request).success && Wire.transfers.size()==fail);
        lcd().end();
    }
    start();
    const auto ok=route(request);
    assert(ok.success && ok.status==200 && Wire.transfers.size()==length);
}
static void checkedRouteBusy() {
    for(const char* action:{"clear","backlight","print"}) {
        start(); RouteRequest request; request.action=action;
        { BusHolder holder; assert(!route(request).success); assert(Wire.transfers.empty()); }
        assert(!lcd().isInitialized()); lcd().end();
    }
}
static void checkedCursorGrouping() {
    start();
    std::atomic<bool> requested{false}, attempted{false}, entered{false};
    std::thread competitor([&] {
        while(!requested.load()) std::this_thread::yield();
        const bool locked=I2CBusManager::instance().lock(0);
        entered=locked; if(locked) I2CBusManager::instance().unlock();
        attempted=true;
    });
    Wire.onEnd=[&] {
        // Compete exactly after cursor's final nibble, before the first character.
        if(Wire.transfers.size()!=6) return;
        requested=true; while(!attempted.load()) std::this_thread::yield();
    };
    assert(lcd().printAt(1,1,"A"));
    competitor.join(); Wire.onEnd=nullptr;
    assert(!entered && Wire.transfers.size()==12);
    Wire.resetTrace(); assert(!lcd().printAt(0,2,"A") && Wire.transfers.empty());
    assert(!lcd().printAt(0,0,String("x\0y",3)) && Wire.transfers.empty());
}

int main(int argc, char** argv) {
    assert(argc == 2); const std::string name(argv[1]);
    if (name == "route_clear" || name == "route_backlight" || name == "route_print") checkedRoutes(name);
    else if (name == "route_busy") checkedRouteBusy();
    else if (name == "cursor_grouping") checkedCursorGrouping();
    else if (name == "bootstrap") bootstrap();
    else if (name == "address_conflict") addressConflict();
    else if (name == "init_nack") initFailures(false);
    else if (name == "init_short_write") initFailures(true);
    else if (name == "print_nack") printFailure();
    else if (name == "line_failures") lineFailures();
    else if (name == "data_pulses") dataPulses();
    else if (name == "dimensions") dimensions();
    else if (name == "addresses") addresses();
    else if (name == "backlight_failure") backlightFailure();
    else if (name == "busy_and_retirement") busyAndRetirement();
    else if (name == "reinitialize") reinitialize();
    else if (name == "feature_disabled") featureDisabled();
    else if (name == "auto_detect") autoDetect();
    else if (name == "character_failure") characterFailure();
    else if (name == "runtime_short_write") runtimeShortWrite();
    else if (name == "shared_lease") sharedLease();
    else if (name == "lost_lease") lostLease();
    else if (name == "numeric_bounds") numericBounds();
    else if (name == "all_data_bytes") allDataBytes();
    else if (name == "command_guards") commandGuards();
    else if (name == "byte_transaction_lock") byteTransactionLock();
    else if (name == "bus_begin_failure") busBeginFailure();
    else if (name == "concurrent_busy_failure") concurrentBusyFailure();
    else return 2;
    lcd().end();
    assert(I2CBusManager::instance().clientCount() == 0);
    return 0;
}
