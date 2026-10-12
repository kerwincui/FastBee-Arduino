#include "screen/ScreenBackend.h"
#include "screen/ScreenBacklight.h"

#include "core/FeatureFlags.h"

#if FASTBEE_ENABLE_SCREEN_UI && defined(ARDUINO_ARCH_ESP32)
#include <SPI.h>
#include "core/SpiBusInitializer.h"
#include <esp_idf_version.h>
#endif

#include <algorithm>
#include <cstring>
#include <new>

namespace FastBee {
namespace Screen {

#if FASTBEE_ENABLE_SCREEN_UI && defined(ARDUINO_ARCH_ESP32)

namespace {

constexpr uint8_t CMD_SWRESET = 0x01;
constexpr uint8_t CMD_SLPIN = 0x10;
constexpr uint8_t CMD_SLPOUT = 0x11;
constexpr uint8_t CMD_NORON = 0x13;
constexpr uint8_t CMD_INVOFF = 0x20;
constexpr uint8_t CMD_INVON = 0x21;
constexpr uint8_t CMD_DISPON = 0x29;
constexpr uint8_t CMD_CASET = 0x2A;
constexpr uint8_t CMD_RASET = 0x2B;
constexpr uint8_t CMD_RAMWR = 0x2C;
constexpr uint8_t CMD_MADCTL = 0x36;
constexpr uint8_t CMD_COLMOD = 0x3A;

class SpiTftDisplayBackend final : public IDisplayBackend {
public:
    bool begin(const DisplayConfigParams& config, String& error) override {
        if (strcasecmp(config.backend, "spi_tft") != 0 ||
            (strcasecmp(config.driver, "st7789") != 0 &&
             strcasecmp(config.driver, "ili9341") != 0)) {
            error = "SCREEN_DISPLAY_BACKEND_UNSUPPORTED";
            return false;
        }
        const uint16_t shortSide = std::min(config.width, config.height);
        const uint16_t longSide = std::max(config.width, config.height);
        if (shortSide == 0 || shortSide > 240 || longSide > 320) {
            error = "SCREEN_DISPLAY_DIMENSIONS_UNSUPPORTED";
            return false;
        }

        SpiBusOperation operation;
        if (!operation.locked()) {
            error = "SPI_BUSY: display initialization lock timeout";
            return false;
        }
        if (_leaseAcquired) {
            error = "SCREEN_DISPLAY_ACTIVE: stop before reinitialization";
            return false;
        }
        if (!SpiBusInitializer::instance().acquire(
                this, static_cast<int8_t>(config.sclk), -1,
                static_cast<int8_t>(config.mosi), static_cast<int8_t>(config.cs), error,
                ResourceConsumerRole::Display)) {
            return false;
        }
        _leaseAcquired = true;
        _config = config;
        _sleeping = false;
        _st7789 = strcasecmp(config.driver, "st7789") == 0;
        _nativeWidth = config.width;
        _nativeHeight = config.height;
        _width = (config.rotation & 1U) ? config.height : config.width;
        _height = (config.rotation & 1U) ? config.width : config.height;
        _spi = &SPI;

        pinMode(static_cast<uint8_t>(_config.cs), OUTPUT);
        pinMode(static_cast<uint8_t>(_config.dc), OUTPUT);
        digitalWrite(static_cast<uint8_t>(_config.cs), HIGH);
        digitalWrite(static_cast<uint8_t>(_config.dc), HIGH);
        if (_config.rst >= 0) {
            pinMode(static_cast<uint8_t>(_config.rst), OUTPUT);
            digitalWrite(static_cast<uint8_t>(_config.rst), HIGH);
        }
        if (_config.backlight >= 0) {
            pinMode(static_cast<uint8_t>(_config.backlight), OUTPUT);
            digitalWrite(static_cast<uint8_t>(_config.backlight),
                         _config.backlightActiveHigh ? LOW : HIGH);
            if (!ledcAttachChannel(static_cast<uint8_t>(_config.backlight),
                                   5000, 8, _config.backlightChannel)) {
                error = "SCREEN_BACKLIGHT_LEDC_INIT_FAILED";
                String cleanupError;
                stop(cleanupError);
                return false;
            }
            _backlightAttached = true;
        }

        hardwareReset();
        const bool initialized = _st7789 ? initializeSt7789() : initializeIli9341();
        if (!initialized) {
            error = "SCREEN_CONTROLLER_INIT_FAILED";
            end();
            return false;
        }
        _ready = true;
        if (_backlightAttached && !setBrightness(_config.brightness)) {
            error = "SCREEN_BACKLIGHT_WRITE_FAILED";
            String cleanupError;
            stop(cleanupError);
            return false;
        }
        error = "";
        return true;
    }

    // ScreenDevice must retain this backend until checked stop succeeds.
    void end() override {
        String ignored;
        stop(ignored);
    }

    bool stop(String& error) override {
        error = "";
        if (!_leaseAcquired) return true;
        SpiBusOperation operation;
        if (!operation.locked()) {
            error = "SPI_BUSY: display lease retained; retry stop";
            return false;
        }
        if (_ready && canTransfer()) {
            writeCommand(CMD_SLPIN, nullptr, 0);
            delay(5);
        }
        // Once cleanup begins, a retained backend must not resume rendering.
        _ready = false;
        if (_backlightAttached) {
            ledcWrite(static_cast<uint8_t>(_config.backlight),
                      _config.backlightActiveHigh ? 0 : 255);
            if (!ledcDetach(static_cast<uint8_t>(_config.backlight))) {
                _backlightCleanupFailed = true;
                error = "SCREEN_BACKLIGHT_DETACH_FAILED: display lease retained";
                return false;
            }
            _backlightAttached = false;
            _backlightCleanupFailed = false;
        }
        if (!SpiBusInitializer::instance().release(this, error)) return false;
        releaseExclusivePins();
        // Do not call SPI.end(): W5500 host ownership is not coordinated.
        _leaseAcquired = false;
        _ready = false;
        _sleeping = false;
        _spi = nullptr;
        return true;
    }

    bool ready() const override { return _ready; }
    void snapshotBacklightResource(BacklightResourceSnapshot& result) const override {
        result = {}; result.available = true; result.retained = _backlightAttached;
        result.cleanupFailed = _backlightCleanupFailed;
        if (_backlightAttached) { result.pin = _config.backlight; result.channel = _config.backlightChannel; }
    }

    DisplayCapabilities capabilities() const override {
        DisplayCapabilities result;
        result.width = _width;
        result.height = _height;
        result.colorDepth = 16;
        result.partialFlush = true;
        result.brightness = _config.backlight >= 0;
        result.brightnessContinuous = result.brightness;
        result.minVisibleBrightnessPercent = MIN_VISIBLE_BRIGHTNESS_PERCENT;
        // ScreenRuntime adds the actual touch backend readiness.
        result.touchWake = false;
        result.sleep = true;
        return result;
    }

    bool pushPixels(uint16_t x, uint16_t y, uint16_t width,
                    uint16_t height, const uint16_t* rgb565,
                    size_t pixelCount) override {
        SpiBusOperation operation(20);
        if (!operation.locked() || !canTransfer() || !_ready || !rgb565 || width == 0 || height == 0 ||
            x >= _width || y >= _height || x + width > _width ||
            y + height > _height || pixelCount < static_cast<size_t>(width) * height) {
            return false;
        }
        beginTransaction();
        setAddressWindowLocked(x, y, width, height);
        digitalWrite(static_cast<uint8_t>(_config.dc), HIGH);

        uint8_t bytes[128];
        size_t offset = 0;
        const size_t total = static_cast<size_t>(width) * height;
        while (offset < total) {
            const size_t count = (total - offset) > 64 ? 64 : (total - offset);
            for (size_t i = 0; i < count; ++i) {
                const uint16_t color = rgb565[offset + i];
                bytes[i * 2] = static_cast<uint8_t>(color >> 8);
                bytes[i * 2 + 1] = static_cast<uint8_t>(color);
            }
            _spi->writeBytes(bytes, count * 2);
            offset += count;
        }
        endTransaction();
        return true;
    }

    bool fill(uint16_t color) override {
        SpiBusOperation operation(20);
        if (!operation.locked() || !canTransfer() || !_ready) return false;
        beginTransaction();
        setAddressWindowLocked(0, 0, _width, _height);
        digitalWrite(static_cast<uint8_t>(_config.dc), HIGH);
        uint8_t bytes[128];
        for (size_t i = 0; i < sizeof(bytes); i += 2) {
            bytes[i] = static_cast<uint8_t>(color >> 8);
            bytes[i + 1] = static_cast<uint8_t>(color);
        }
        size_t remaining = static_cast<size_t>(_width) * _height;
        while (remaining > 0) {
            const size_t count = remaining > 64 ? 64 : remaining;
            _spi->writeBytes(bytes, count * 2);
            remaining -= count;
        }
        endTransaction();
        return true;
    }

    bool setBrightness(uint8_t percent) override {
        SpiBusOperation operation(20);
        if (!operation.locked() || !canTransfer() || !_ready) return false;
        percent = normalizeBrightnessPercent(percent);
        if (!applyBrightness(percent, _sleeping)) return false;
        _brightness = percent;
        return true;
    }

    bool setSleep(bool sleeping) override {
        SpiBusOperation operation(20);
        if (!operation.locked() || !canTransfer() || !_ready) return false;
        if (!writeCommand(sleeping ? CMD_SLPIN : CMD_SLPOUT, nullptr, 0)) return false;
        delay(sleeping ? 5 : 120);
        if (!applyBrightness(_brightness, sleeping)) {
            writeCommand(_sleeping ? CMD_SLPIN : CMD_SLPOUT, nullptr, 0);
            delay(_sleeping ? 5 : 120);
            return false;
        }
        _sleeping = sleeping;
        return true;
    }

    bool setBuzzer(bool) override { return false; }

private:
    // Called only with the operation lock held; no GPIO/transaction on a lost lease.
    bool canTransfer() const {
        return _leaseAcquired && _spi && _spi->bus() &&
               SpiBusInitializer::instance().isAcquired(this);
    }

    bool applyBrightness(uint8_t percent, bool sleeping) {
        if (_config.backlight < 0 || !_backlightAttached) return sleeping || percent == 100;
        return ledcWrite(static_cast<uint8_t>(_config.backlight),
                         backlightDuty(percent, sleeping, _config.backlightActiveHigh));
    }

    void hardwareReset() {
        if (_config.rst < 0) return;
        const uint8_t pin = static_cast<uint8_t>(_config.rst);
        digitalWrite(pin, HIGH);
        delay(5);
        digitalWrite(pin, LOW);
        delay(20);
        digitalWrite(pin, HIGH);
        delay(120);
    }

    void releaseExclusivePins() {
        const int8_t pins[] = {_config.cs, _config.dc, _config.rst, _config.backlight};
        for (int8_t pin : pins) {
            if (pin >= 0) pinMode(static_cast<uint8_t>(pin), INPUT);
        }
    }

    void beginTransaction() {
        _spi->beginTransaction(SPISettings(_config.spiFrequency, MSBFIRST, SPI_MODE0));
        digitalWrite(static_cast<uint8_t>(_config.cs), LOW);
    }

    void endTransaction() {
        digitalWrite(static_cast<uint8_t>(_config.cs), HIGH);
        _spi->endTransaction();
    }

    void writeCommandLocked(uint8_t command, const uint8_t* data, size_t length) {
        digitalWrite(static_cast<uint8_t>(_config.dc), LOW);
        _spi->write(command);
        if (length > 0 && data) {
            digitalWrite(static_cast<uint8_t>(_config.dc), HIGH);
            _spi->writeBytes(const_cast<uint8_t*>(data), length);
        }
    }

    bool writeCommand(uint8_t command, const uint8_t* data, size_t length) {
        SpiBusOperation operation(20);
        if (!operation.locked() || !canTransfer()) return false;
        beginTransaction();
        writeCommandLocked(command, data, length);
        endTransaction();
        return true;
    }

    void setAddressWindowLocked(uint16_t x, uint16_t y,
                                uint16_t width, uint16_t height) {
        uint16_t xOffset = 0;
        uint16_t yOffset = 0;
        if (_st7789 && _nativeWidth == 240 && _nativeHeight == 240) {
            if (_config.rotation & 1U) xOffset = 80;
            else yOffset = 80;
        }
        const uint16_t x0 = x + xOffset;
        const uint16_t y0 = y + yOffset;
        const uint16_t x1 = x0 + width - 1;
        const uint16_t y1 = y0 + height - 1;
        const uint8_t columns[] = {
            static_cast<uint8_t>(x0 >> 8), static_cast<uint8_t>(x0),
            static_cast<uint8_t>(x1 >> 8), static_cast<uint8_t>(x1)
        };
        const uint8_t rows[] = {
            static_cast<uint8_t>(y0 >> 8), static_cast<uint8_t>(y0),
            static_cast<uint8_t>(y1 >> 8), static_cast<uint8_t>(y1)
        };
        writeCommandLocked(CMD_CASET, columns, sizeof(columns));
        writeCommandLocked(CMD_RASET, rows, sizeof(rows));
        writeCommandLocked(CMD_RAMWR, nullptr, 0);
    }

    uint8_t madctl() const {
        if (_st7789) {
            static const uint8_t values[4] = {0x00, 0x60, 0xC0, 0xA0};
            return static_cast<uint8_t>(values[_config.rotation & 3U] | 0x08U);
        }
        static const uint8_t values[4] = {0x48, 0x28, 0x88, 0xE8};
        return values[_config.rotation & 3U];
    }

    bool initializeSt7789() {
        if (!writeCommand(CMD_SWRESET, nullptr, 0)) return false;
        delay(150);
        if (!writeCommand(CMD_SLPOUT, nullptr, 0)) return false;
        delay(120);
        const uint8_t colorMode = 0x55;
        const uint8_t memoryAccess = madctl();
        const uint8_t porch[] = {0x0C, 0x0C, 0x00, 0x33, 0x33};
        const uint8_t gate = 0x35;
        const uint8_t vcom = 0x19;
        const uint8_t lcm = 0x2C;
        const uint8_t vdvVrh = 0x01;
        const uint8_t vrh = 0x12;
        const uint8_t vdv = 0x20;
        const uint8_t frame = 0x0F;
        const uint8_t power[] = {0xA4, 0xA1};
        if (!writeCommand(CMD_COLMOD, &colorMode, 1)) return false;
        if (!writeCommand(CMD_MADCTL, &memoryAccess, 1)) return false;
        if (!writeCommand(0xB2, porch, sizeof(porch))) return false;
        if (!writeCommand(0xB7, &gate, 1)) return false;
        if (!writeCommand(0xBB, &vcom, 1)) return false;
        if (!writeCommand(0xC0, &lcm, 1)) return false;
        if (!writeCommand(0xC2, &vdvVrh, 1)) return false;
        if (!writeCommand(0xC3, &vrh, 1)) return false;
        if (!writeCommand(0xC4, &vdv, 1)) return false;
        if (!writeCommand(0xC6, &frame, 1)) return false;
        if (!writeCommand(0xD0, power, sizeof(power))) return false;
        if (!writeCommand(CMD_INVON, nullptr, 0)) return false;
        if (!writeCommand(CMD_NORON, nullptr, 0)) return false;
        delay(10);
        if (!writeCommand(CMD_DISPON, nullptr, 0)) return false;
        delay(120);
        return true;
    }

    bool initializeIli9341() {
        if (!writeCommand(CMD_SWRESET, nullptr, 0)) return false;
        delay(120);
        const uint8_t ef[] = {0x03, 0x80, 0x02};
        const uint8_t cf[] = {0x00, 0xC1, 0x30};
        const uint8_t ed[] = {0x64, 0x03, 0x12, 0x81};
        const uint8_t e8[] = {0x85, 0x00, 0x78};
        const uint8_t cb[] = {0x39, 0x2C, 0x00, 0x34, 0x02};
        const uint8_t f7 = 0x20;
        const uint8_t ea[] = {0x00, 0x00};
        const uint8_t c0 = 0x23;
        const uint8_t c1 = 0x10;
        const uint8_t c5[] = {0x3E, 0x28};
        const uint8_t c7 = 0x86;
        const uint8_t memoryAccess = madctl();
        const uint8_t colorMode = 0x55;
        const uint8_t b1[] = {0x00, 0x18};
        const uint8_t b6[] = {0x08, 0x82, 0x27};
        const uint8_t f2 = 0x00;
        const uint8_t gamma = 0x01;
        const uint8_t e0[] = {0x0F,0x31,0x2B,0x0C,0x0E,0x08,0x4E,0xF1,0x37,0x07,0x10,0x03,0x0E,0x09,0x00};
        const uint8_t e1[] = {0x00,0x0E,0x14,0x03,0x11,0x07,0x31,0xC1,0x48,0x08,0x0F,0x0C,0x31,0x36,0x0F};
        if (!writeCommand(0xEF, ef, sizeof(ef))) return false;
        if (!writeCommand(0xCF, cf, sizeof(cf))) return false;
        if (!writeCommand(0xED, ed, sizeof(ed))) return false;
        if (!writeCommand(0xE8, e8, sizeof(e8))) return false;
        if (!writeCommand(0xCB, cb, sizeof(cb))) return false;
        if (!writeCommand(0xF7, &f7, 1)) return false;
        if (!writeCommand(0xEA, ea, sizeof(ea))) return false;
        if (!writeCommand(0xC0, &c0, 1)) return false;
        if (!writeCommand(0xC1, &c1, 1)) return false;
        if (!writeCommand(0xC5, c5, sizeof(c5))) return false;
        if (!writeCommand(0xC7, &c7, 1)) return false;
        if (!writeCommand(CMD_MADCTL, &memoryAccess, 1)) return false;
        if (!writeCommand(CMD_COLMOD, &colorMode, 1)) return false;
        if (!writeCommand(0xB1, b1, sizeof(b1))) return false;
        if (!writeCommand(0xB6, b6, sizeof(b6))) return false;
        if (!writeCommand(0xF2, &f2, 1)) return false;
        if (!writeCommand(0x26, &gamma, 1)) return false;
        if (!writeCommand(0xE0, e0, sizeof(e0))) return false;
        if (!writeCommand(0xE1, e1, sizeof(e1))) return false;
        if (!writeCommand(CMD_INVOFF, nullptr, 0)) return false;
        if (!writeCommand(CMD_SLPOUT, nullptr, 0)) return false;
        delay(120);
        if (!writeCommand(CMD_DISPON, nullptr, 0)) return false;
        delay(120);
        return true;
    }

    DisplayConfigParams _config{};
    SPIClass* _spi = nullptr;
    uint16_t _nativeWidth = 0;
    uint16_t _nativeHeight = 0;
    uint16_t _width = 0;
    uint16_t _height = 0;
    bool _st7789 = false;
    bool _backlightAttached = false;
    bool _backlightCleanupFailed = false;
    bool _sleeping = false;
    uint8_t _brightness = 100;
    bool _ready = false;
    bool _leaseAcquired = false;
};

}  // namespace

#endif

std::unique_ptr<IDisplayBackend> createDisplayBackend(
        const DisplayConfigParams& config, String& error) {
#if FASTBEE_ENABLE_SCREEN_UI && defined(ARDUINO_ARCH_ESP32)
    if (strcasecmp(config.backend, "rgb") == 0) {
        return createRgbDisplayBackend(config, error);
    }
    if (strcasecmp(config.backend, "spi_tft") == 0 &&
        (strcasecmp(config.driver, "st7789") == 0 ||
         strcasecmp(config.driver, "ili9341") == 0)) {
        std::unique_ptr<IDisplayBackend> backend(new (std::nothrow) SpiTftDisplayBackend());
        if (!backend) error = "SCREEN_DISPLAY_ALLOCATION_FAILED";
        return backend;
    }
    error = "SCREEN_DISPLAY_BACKEND_UNSUPPORTED";
#else
    (void)config;
    error = "SCREEN_UI_NOT_AVAILABLE_ON_TARGET";
#endif
    return nullptr;
}

}  // namespace Screen
}  // namespace FastBee
