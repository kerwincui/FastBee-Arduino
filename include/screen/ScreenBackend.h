#pragma once

#include <Arduino.h>
#include <memory>

#include "core/PeripheralConfig.h"
#include "screen/ScreenBacklight.h"
#include "core/RuntimeResourceSnapshot.h"

namespace FastBee {
namespace Screen {

struct DisplayCapabilities {
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t colorDepth = 16;
    bool partialFlush = true;
    bool brightness = false;
    bool sleep = true;
    bool buzzer = false;
    bool brightnessContinuous = false;
    uint8_t minVisibleBrightnessPercent = MIN_VISIBLE_BRIGHTNESS_PERCENT;
    bool touchWake = false;
};

struct TouchPoint {
    uint16_t x = 0;
    uint16_t y = 0;
    uint8_t contactId = 0;
    bool pressed = false;
    uint32_t timestampMs = 0;
};

class IDisplayBackend {
public:
    virtual ~IDisplayBackend() = default;
    virtual bool begin(const DisplayConfigParams& config, String& error) = 0;
    virtual void end() = 0;
    // Resource-owning backends can refuse cleanup. The owner must retain the
    // instance and retry instead of destroying a still-leased backend.
    virtual bool stop(String& error) {
        setSleep(true);
        end();
        error = "";
        return true;
    }
    virtual bool ready() const = 0;
    // Owner screen task only; HTTP observers use ScreenRuntime's published copy.
    virtual void snapshotBacklightResource(BacklightResourceSnapshot& result) const { result = {}; }
    virtual DisplayCapabilities capabilities() const = 0;
    virtual bool pushPixels(uint16_t x, uint16_t y, uint16_t width,
                            uint16_t height, const uint16_t* rgb565,
                            size_t pixelCount) = 0;
    virtual bool fill(uint16_t color) = 0;
    virtual bool setBrightness(uint8_t percent) = 0;
    virtual bool setSleep(bool sleeping) = 0;
    virtual bool setBuzzer(bool enabled) = 0;
    // Optional housekeeping on the display-owning task, never from an ISR.
    virtual void service(uint32_t nowMs) { (void)nowMs; }
};

class ITouchBackend {
public:
    virtual ~ITouchBackend() = default;
    virtual bool begin(const DisplayConfigParams& config, String& error) = 0;
    virtual void end() = 0;
    virtual bool ready() const = 0;
    virtual bool poll(TouchPoint& point) = 0;
};

std::unique_ptr<IDisplayBackend> createDisplayBackend(
    const DisplayConfigParams& config, String& error);
std::unique_ptr<IDisplayBackend> createRgbDisplayBackend(
    const DisplayConfigParams& config, String& error);
std::unique_ptr<ITouchBackend> createTouchBackend(
    const DisplayConfigParams& config, String& error);

}  // namespace Screen
}  // namespace FastBee
