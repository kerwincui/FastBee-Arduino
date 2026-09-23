#pragma once

#include <Arduino.h>
#include <memory>

#include "screen/ScreenBackend.h"

namespace FastBee {
namespace Screen {

// Owns the active physical display/touch pair. configure() is deliberately
// side-effect free: GPIO, buses and buffers are touched only by activate().
class ScreenDevice {
public:
    static ScreenDevice& instance();

    bool configure(const PeripheralConfig& peripheral, String& error);
    bool unconfigure(const String& peripheralId);
    bool activate(String& error);
    void deactivate();

    bool configured() const { return _configured; }
    bool active() const { return _active; }
    const String& peripheralId() const { return _peripheralId; }
    const DisplayConfigParams* config() const {
        return _configured ? &_config : nullptr;
    }
    IDisplayBackend* display() { return _display.get(); }
    ITouchBackend* touch() { return _touch.get(); }
    DisplayCapabilities capabilities() const;

private:
    ScreenDevice() = default;
    ScreenDevice(const ScreenDevice&) = delete;
    ScreenDevice& operator=(const ScreenDevice&) = delete;

    bool _configured = false;
    bool _active = false;
    String _peripheralId;
    DisplayConfigParams _config{};
    std::unique_ptr<IDisplayBackend> _display;
    std::unique_ptr<ITouchBackend> _touch;
};

}  // namespace Screen
}  // namespace FastBee
