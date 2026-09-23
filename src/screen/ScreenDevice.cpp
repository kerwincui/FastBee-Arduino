#include "screen/ScreenDevice.h"

#include <cstring>

#include "core/FeatureFlags.h"
#include "systems/LoggerSystem.h"

namespace FastBee {
namespace Screen {

ScreenDevice& ScreenDevice::instance() {
    static ScreenDevice device;
    return device;
}

bool ScreenDevice::configure(const PeripheralConfig& peripheral, String& error) {
    if (peripheral.type != PeripheralType::SCREEN_DISPLAY) {
        error = "SCREEN_CONFIG_TYPE_INVALID";
        return false;
    }
    if (_active) {
        error = "SCREEN_DEVICE_ACTIVE: deactivate before reconfiguration";
        return false;
    }
    _peripheralId = peripheral.id;
    memcpy(&_config, &peripheral.params.display, sizeof(_config));
    _configured = true;
    error = "";
    return true;
}

bool ScreenDevice::unconfigure(const String& peripheralId) {
    if (!_configured || _peripheralId != peripheralId) return false;
    // Backend ownership belongs to ScreenRuntime's UI task. A caller must
    // first request and confirm task shutdown; releasing here would race its
    // touch polling and rendering calls.
    if (_active) return false;
    memset(&_config, 0, sizeof(_config));
    _peripheralId = "";
    _configured = false;
    return true;
}

bool ScreenDevice::activate(String& error) {
#if !FASTBEE_ENABLE_SCREEN_UI
    error = "SCREEN_UI_NOT_COMPILED";
    return false;
#else
    if (_active) {
        error = "";
        return true;
    }
    if (!_configured) {
        error = "SCREEN_DISPLAY_NOT_CONFIGURED";
        return false;
    }

    _display = createDisplayBackend(_config, error);
    if (!_display || !_display->begin(_config, error)) {
        if (error.isEmpty()) error = "SCREEN_DISPLAY_INIT_FAILED";
        _display.reset();
        return false;
    }

    _touch = createTouchBackend(_config, error);
    if (strcasecmp(_config.touchBackend, "none") != 0) {
        if (!_touch || !_touch->begin(_config, error)) {
            if (error.isEmpty()) error = "SCREEN_TOUCH_INIT_FAILED";
            LOG_WARNINGF("ScreenDevice: touch unavailable (%s); keeping display visible in read-only mode",
                         error.c_str());
            if (_touch) _touch->end();
            _touch.reset();
            // Touch is optional. A missing/unresponsive controller must not
            // hide useful data or strand a screen in an unwakeable black state.
            // ScreenRuntime suppresses idle sleep when no touch backend exists.
        }
    }

    if (!_display->setBrightness(_config.brightness)) {
        LOG_WARNING("ScreenDevice: configured brightness could not be applied");
    }
    _active = true;
    error = "";
    LOG_INFOF("ScreenDevice: activated '%s' backend=%s driver=%s %ux%u touch=%s",
              _peripheralId.c_str(), _config.backend, _config.driver,
              static_cast<unsigned>(_config.width),
              static_cast<unsigned>(_config.height), _config.touchBackend);
    return true;
#endif
}

void ScreenDevice::deactivate() {
    if (_touch) {
        _touch->end();
        _touch.reset();
    }
    if (_display) {
        _display->setSleep(true);
        _display->end();
        _display.reset();
    }
    _active = false;
}

DisplayCapabilities ScreenDevice::capabilities() const {
    return _display ? _display->capabilities() : DisplayCapabilities{};
}

}  // namespace Screen
}  // namespace FastBee
