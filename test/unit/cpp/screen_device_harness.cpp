#include <assert.h>
#include <string.h>

#include <memory>

#include "screen/ScreenDevice.h"

using namespace FastBee::Screen;

namespace {

struct BackendTrace {
    int displayBegin = 0;
    int displayEnd = 0;
    int displaySleep = 0;
    int touchBegin = 0;
    int touchEnd = 0;
    bool displayBeginOk = true;
    bool touchBeginOk = true;
    bool touchReady = true;
} trace;

class FakeDisplay final : public IDisplayBackend {
public:
    bool begin(const DisplayConfigParams&, String& error) override {
        ++trace.displayBegin;
        if (!trace.displayBeginOk) error = "DISPLAY_BEGIN_FAILED";
        return trace.displayBeginOk;
    }
    void end() override { assert(trace.displaySleep > 0); ++trace.displayEnd; }
    bool ready() const override { return true; }
    DisplayCapabilities capabilities() const override {
        return DisplayCapabilities{};
    }
    bool pushPixels(uint16_t, uint16_t, uint16_t, uint16_t,
                    const uint16_t*, size_t) override { return true; }
    bool fill(uint16_t) override { return true; }
    bool setBrightness(uint8_t) override { return true; }
    bool setSleep(bool sleeping) override { if(sleeping)++trace.displaySleep;return true; }
    bool setBuzzer(bool) override { return false; }
};

class FakeTouch final : public ITouchBackend {
public:
    bool begin(const DisplayConfigParams&, String& error) override {
        ++trace.touchBegin;
        if (!trace.touchBeginOk) error = "TOUCH_BEGIN_FAILED";
        return trace.touchBeginOk;
    }
    void end() override { ++trace.touchEnd; }
    bool ready() const override { return trace.touchReady; }
    bool poll(TouchPoint&) override { return false; }
};

PeripheralConfig makeDisplay(const char* id, const char* touch = "gt911") {
    PeripheralConfig config;
    config.id = id;
    config.type = PeripheralType::SCREEN_DISPLAY;
    config.enabled = true;
    initializeDisplayDefaults(config);
    strcpy(config.params.display.touchBackend, touch);
    return config;
}

void resetTrace() { trace = BackendTrace{}; }

void testActiveDeviceCannotBeUnconfigured() {
    resetTrace();
    auto& device = ScreenDevice::instance();
    String error;
    assert(device.configure(makeDisplay("panel-a"), error));
    assert(device.activate(error));
    assert(device.active());

    assert(!device.unconfigure("panel-a"));
    assert(device.active());
    assert(device.configured());
    assert(trace.displayEnd == 0);
    assert(trace.touchEnd == 0);

    device.deactivate();
    assert(!device.active());
    assert(trace.displayEnd == 1);
    assert(trace.touchEnd == 1);
    assert(device.unconfigure("panel-a"));
    assert(!device.configured());
}

void testActiveDeviceCannotBeReconfigured() {
    resetTrace();
    auto& device = ScreenDevice::instance();
    String error;
    assert(device.configure(makeDisplay("panel-a", "none"), error));
    assert(device.activate(error));
    assert(!device.configure(makeDisplay("panel-b"), error));
    assert(error == "SCREEN_DEVICE_ACTIVE: deactivate before reconfiguration");
    assert(device.peripheralId() == "panel-a");
    device.deactivate();
    assert(device.unconfigure("panel-a"));
}

void testTouchFailureKeepsReadOnlyDisplay() {
    resetTrace();
    trace.touchBeginOk = false;
    auto& device = ScreenDevice::instance();
    String error;
    assert(device.configure(makeDisplay("panel-a"), error));
    assert(device.activate(error));
    assert(error.isEmpty());
    assert(device.active());
    assert(device.display() && !device.touch());
    assert(trace.displayEnd == 0);
    assert(trace.touchEnd == 1);
    assert(device.touchError() == "TOUCH_BEGIN_FAILED");
    const uint32_t now = millis();
    const int attempts = trace.touchBegin;
    assert(!device.serviceTouch(now));
    assert(!device.serviceTouch(now + 4999U));
    assert(device.serviceTouch(now + 5000U));
    assert(trace.touchBegin == attempts + 1 && !device.touch());
    trace.touchBeginOk = true;
    assert(device.serviceTouch(now + 10000U));
    assert(device.touch() && device.touch()->ready() && device.touchError().isEmpty());
    assert(trace.displayBegin == 1 && trace.displayEnd == 0);
    trace.touchReady = false;
    assert(device.serviceTouch(now + 10001U));
    assert(!device.touch() && device.touchError() == "SCREEN_TOUCH_COMMUNICATION_FAILED");
    trace.touchReady = true;
    assert(!device.serviceTouch(now + 14999U));
    assert(device.serviceTouch(now + 15000U));
    assert(device.touch() && device.touchError().isEmpty());
    device.deactivate();
    assert(trace.displayEnd == 1);
    assert(device.unconfigure("panel-a"));
}

}  // namespace

namespace FastBee {
namespace Screen {

std::unique_ptr<IDisplayBackend> createDisplayBackend(
        const DisplayConfigParams&, String&) {
    return std::unique_ptr<IDisplayBackend>(new FakeDisplay());
}

std::unique_ptr<ITouchBackend> createTouchBackend(
        const DisplayConfigParams& config, String&) {
    if (strcmp(config.touchBackend, "none") == 0) return nullptr;
    return std::unique_ptr<ITouchBackend>(new FakeTouch());
}

}  // namespace Screen
}  // namespace FastBee

int main() {
    testActiveDeviceCannotBeUnconfigured();
    testActiveDeviceCannotBeReconfigured();
    testTouchFailureKeepsReadOnlyDisplay();
    return 0;
}
