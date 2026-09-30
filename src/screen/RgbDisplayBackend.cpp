#include "screen/ScreenBackend.h"
#include "screen/ScreenBacklight.h"
#include "screen/RgbRecovery.h"
#include "core/ChipConfig.h"
#include "core/FeatureFlags.h"
#include <algorithm>
#include <cstring>
#include <new>

#if FASTBEE_ENABLE_SCREEN_UI && defined(ARDUINO_ARCH_ESP32) && CHIP_IS_ESP32S3
#include "core/I2CBusManager.h"
#include "systems/LoggerSystem.h"
#include "St7701Type1Init.h"
#include <esp_heap_caps.h>
#include <esp_rom_sys.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#endif

#ifndef FASTBEE_RGB_BOUNCE_LINES
#define FASTBEE_RGB_BOUNCE_LINES 4
#endif

#ifndef FASTBEE_RGB_REQUIRE_BOUNCE
#define FASTBEE_RGB_REQUIRE_BOUNCE 0
#endif

#ifndef FASTBEE_RGB_SCAN_TELEMETRY
#define FASTBEE_RGB_SCAN_TELEMETRY 0
#endif

static_assert(FASTBEE_RGB_BOUNCE_LINES >= 0 && FASTBEE_RGB_BOUNCE_LINES <= 80,
              "FASTBEE_RGB_BOUNCE_LINES must be between 0 and 80");

namespace FastBee { namespace Screen {
#if FASTBEE_ENABLE_SCREEN_UI && defined(ARDUINO_ARCH_ESP32) && CHIP_IS_ESP32S3
namespace {
#if FASTBEE_RGB_SCAN_TELEMETRY == 1
// Public SDK callbacks only. S3 has one RGB controller; callback state must
// stay in internal RAM even when ordinary C++ objects are allocated in PSRAM.
struct RgbScanCounters {
    uint32_t vsyncs = 0, frames = 0, lastVsyncUs = 0;
    uint32_t maxVsyncUs = 0, minVsyncUs = UINT32_MAX;
};
static DRAM_ATTR RgbScanCounters scanCounters;
static DRAM_ATTR portMUX_TYPE scanLock = portMUX_INITIALIZER_UNLOCKED;
static bool IRAM_ATTR scanVsync(esp_lcd_panel_handle_t,
                              const esp_lcd_rgb_panel_event_data_t*, void*) {
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time());
    portENTER_CRITICAL_ISR(&scanLock);
    if (scanCounters.vsyncs) {
        const uint32_t delta = now - scanCounters.lastVsyncUs;
        if (delta > scanCounters.maxVsyncUs) scanCounters.maxVsyncUs = delta;
        if (delta < scanCounters.minVsyncUs) scanCounters.minVsyncUs = delta;
    }
    scanCounters.lastVsyncUs = now;
    ++scanCounters.vsyncs;
    portEXIT_CRITICAL_ISR(&scanLock);
    return false;
}
static bool IRAM_ATTR scanFrame(esp_lcd_panel_handle_t,
                              const esp_lcd_rgb_panel_event_data_t*, void*) {
    portENTER_CRITICAL_ISR(&scanLock);
    ++scanCounters.frames;
    portEXIT_CRITICAL_ISR(&scanLock);
    return false;
}
#endif
class RgbDisplayBackend final : public IDisplayBackend {
public:
    ~RgbDisplayBackend() override { end(); }

    bool begin(const DisplayConfigParams& config, String& error) override {
        if (_panel || _controlLease) end();
        if (strcasecmp(config.backend, "rgb") || strcasecmp(config.driver, "st7701") ||
            strcasecmp(config.rgbInitProfile, "st7701_type1")) {
            error = "SCREEN_RGB_PROFILE_UNSUPPORTED";
            return false;
        }
        if (config.width != 480 || config.height != 480 || config.rotation > 3 ||
            config.colorDepth != 16 || config.rgbFrequency < 1000000 ||
            config.rgbFrequency > 16000000 || !config.hsyncPulseWidth || !config.vsyncPulseWidth) {
            error = "SCREEN_RGB_TIMING_INVALID";
            return false;
        }
        const size_t frameBytes = static_cast<size_t>(config.width) * config.height * 2;
        _config = config;
        _width = (config.rotation & 1) ? config.height : config.width;
        _height = (config.rotation & 1) ? config.width : config.height;
        _ch32 = strcasecmp(config.controlBackend, "ch32v003") == 0;
        const bool tca = strcasecmp(config.controlBackend, "tca9554") == 0;
        if (!_ch32 && !tca && config.controlBackend[0] && strcasecmp(config.controlBackend, "none")) {
            error = "SCREEN_RGB_CONTROL_UNSUPPORTED";
            return false;
        }
        if (config.controlBuzzerMask && !_ch32) {
            error = "SCREEN_RGB_BUZZER_CONTROL_UNSUPPORTED";
            return false;
        }
        if (!config.controlTouchSyncValid()) {
            error = "SCREEN_RGB_TOUCH_CONTROL_INVALID"; return false;
        }
        // Configuration/resource planning validates collisions before activation.
        const int8_t commandPins[] = {config.cs, config.sclk, config.mosi};
        for (int8_t pin : commandPins) {
            if (!GPIO_IS_VALID_OUTPUT_GPIO(pin)) { error = "SCREEN_RGB_CONTROL_PIN_INVALID"; return false; }
        }
        if (_ch32 || tca) {
            const uint8_t activeMask = config.controlResetMask |
                config.controlBacklightMask | config.controlEnableMask |
                config.controlBuzzerMask;
            if (!config.controlMasksDisjoint()) {
                error = "SCREEN_RGB_CONTROL_MASK_CONFLICT"; return false;
            }
            if (!I2CBusManager::instance().acquire(this, config.controlSda, config.controlScl,
                                                  config.i2cFrequency, error)) return false;
            _controlLease = true;
            const uint8_t mask = activeMask | config.controlInactiveMask | config.controlTouchIntMask;
            // Change only levels explicitly assigned to this display.
            // Explicit inactive outputs are latched low before output mode is
            // enabled, so a prior firmware's asserted output cannot linger.
            const uint8_t initialOutput = config.controlEnableMask |
                (config.backlightActiveHigh ? 0 : config.controlBacklightMask);
            const bool controlInitialized = _ch32
                ? initializeCh32(mask, initialOutput)
                : updateRegister(outputRegister(), mask, initialOutput) &&
                  updateRegister(0x03, mask, 0);
            if (!controlInitialized) {
                error = "SCREEN_RGB_CONTROL_INIT_FAILED"; end(); return false;
            }
            delay(200);
            if (!updateRegister(outputRegister(), config.controlResetMask, config.controlResetMask)) {
                error = "SCREEN_RGB_RESET_FAILED"; end(); return false;
            }
            if (config.controlTouchIntMask) {
                // GT911 address 0x5D: INT was latched low before RESET rose.
                // Keep it driven low through the >5ms address hold and 50ms
                // synchronization window, then release it for controller IRQs.
                // Direction polarities differ; masked writes retain SYS_EN,
                // buzzer and all other explicitly configured output states.
                delay(60);
                if (!updateRegister(_ch32 ? 0x02 : 0x03, config.controlTouchIntMask,
                                    _ch32 ? 0 : config.controlTouchIntMask)) {
                    error = "SCREEN_RGB_TOUCH_SYNC_FAILED"; end(); return false;
                }
            }
            delay(200);
        }
        // Clear the external output latch even when framebuffer admission
        // fails after an MCU-only reset. Cleanup retains the configured bus
        // long enough to retry OFF before relinquishing it.
        if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) < frameBytes + 4096) {
            error = "SCREEN_RGB_PSRAM_REQUIRED"; end(); return false;
        }
        for (int8_t pin : commandPins) pinMode(pin, OUTPUT);
        digitalWrite(config.cs, HIGH);
        digitalWrite(config.sclk, HIGH);
        _commandPinsActive = true;
        if (config.rst >= 0) {
            pinMode(config.rst, OUTPUT); digitalWrite(config.rst, LOW); delay(20);
            digitalWrite(config.rst, HIGH); delay(120);
        }
        if (config.backlight >= 0) {
            pinMode(config.backlight, OUTPUT);
            digitalWrite(config.backlight, config.backlightActiveHigh ? LOW : HIGH);
            if (!ledcAttachChannel(config.backlight, 5000, 8, config.backlightChannel)) {
                error = "SCREEN_BACKLIGHT_LEDC_INIT_FAILED"; end(); return false;
            }
            _backlightAttached = true;
        }
        for (const auto& command : kSt7701Type1Init) sendCommand(command.command, command.data, command.count);
        sendCommand(0x11); delay(120); sendCommand(0x29);

        esp_lcd_rgb_panel_config_t panel{};
        panel.clk_src = LCD_CLK_SRC_DEFAULT;
        panel.data_width = 16;
        panel.bits_per_pixel = 16;
        panel.num_fbs = 1;
        panel.flags.fb_in_psram = true;
        panel.dma_burst_size = 64;
        panel.hsync_gpio_num = config.rgbHsync;
        panel.vsync_gpio_num = config.rgbVsync;
        panel.de_gpio_num = config.rgbDe;
        panel.pclk_gpio_num = config.rgbPclk;
        panel.disp_gpio_num = -1;
        for (size_t i = 0; i < 16; ++i) panel.data_gpio_nums[i] = config.rgbDataPins[i];
        panel.timings.pclk_hz = config.rgbFrequency;
        panel.timings.h_res = config.width;
        panel.timings.v_res = config.height;
        panel.timings.hsync_front_porch = config.hsyncFrontPorch;
        panel.timings.hsync_pulse_width = config.hsyncPulseWidth;
        panel.timings.hsync_back_porch = config.hsyncBackPorch;
        panel.timings.vsync_front_porch = config.vsyncFrontPorch;
        panel.timings.vsync_pulse_width = config.vsyncPulseWidth;
        panel.timings.vsync_back_porch = config.vsyncBackPorch;
        panel.timings.flags.hsync_idle_low = config.hsyncIdleLow;
        panel.timings.flags.vsync_idle_low = config.vsyncIdleLow;
        panel.timings.flags.pclk_active_neg = config.pclkActiveNeg;
        // Bounce refill needs both a cache-safe ISR and PSRAM access while
        // LittleFS/NVS writes disable the flash cache. The precompiled Arduino
        // SDK normally provides neither. In that build, use direct EDMA: extra
        // internal buffers would otherwise turn each flash write into an
        // underrun/visible frame shift, even at a low pixel clock.
#if defined(CONFIG_LCD_RGB_ISR_IRAM_SAFE) && CONFIG_LCD_RGB_ISR_IRAM_SAFE && \
    defined(CONFIG_SPIRAM_XIP_FROM_PSRAM) && CONFIG_SPIRAM_XIP_FROM_PSRAM && \
    defined(CONFIG_SPIRAM_FETCH_INSTRUCTIONS) && CONFIG_SPIRAM_FETCH_INSTRUCTIONS && \
    defined(CONFIG_SPIRAM_RODATA) && CONFIG_SPIRAM_RODATA
        constexpr bool cacheSafeBounce = true;
#else
        constexpr bool cacheSafeBounce = false;
#endif
        // IDF allocates two internal buffers. The normal profile keeps the
        // conservative four-line request; isolated panel candidates may raise
        // the line count only after validating the resulting internal-RAM budget.
        constexpr size_t bounceLines = FASTBEE_RGB_BOUNCE_LINES;
        constexpr bool bounceRequired = FASTBEE_RGB_REQUIRE_BOUNCE == 1;
        // Wi-Fi may be between driver stop/start during boot retry. Its
        // released memory is not spare display memory: leave room for network
        // reinitialization and peripheral worker stacks after RGB allocation.
        constexpr size_t internalReserve = 48U * 1024U;
        constexpr size_t largestReserve = 20U * 1024U;
        constexpr size_t driverReserve = 1536U;
        const size_t bouncePixels = static_cast<size_t>(config.width) * bounceLines;
        const size_t bounceBytes = bouncePixels * sizeof(uint16_t) * 2U;
        const uint32_t internalCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        const size_t internalBefore = heap_caps_get_free_size(internalCaps);
        const size_t largestBefore = heap_caps_get_largest_free_block(internalCaps);
        if (bouncePixels && cacheSafeBounce &&
            internalBefore >= internalReserve + bounceBytes + driverReserve &&
            largestBefore >= largestReserve + bounceBytes) {
            panel.bounce_buffer_size_px = bouncePixels;
        }
        LOGGER.infof("Screen RGB request: bounce=%u bytes lines=%u required=%u cacheSafe=%u internal=%u largest=%u",
            static_cast<unsigned>(bounceBytes), static_cast<unsigned>(bounceLines),
            bounceRequired ? 1U : 0U, cacheSafeBounce ? 1U : 0U,
            static_cast<unsigned>(internalBefore), static_cast<unsigned>(largestBefore));
        if (bounceRequired && panel.bounce_buffer_size_px != bouncePixels) {
            error = cacheSafeBounce ? "SCREEN_RGB_BOUNCE_MEMORY_REQUIRED" :
                                      "SCREEN_RGB_BOUNCE_XIP_REQUIRED";
            end();
            return false;
        }
        // Do not invalidate CPU cache behind another core rendering into the
        // framebuffer. The SDK copies coherent cached pixels into its buffers.
        panel.flags.bb_invalidate_cache = false;
        esp_err_t result = esp_lcd_new_rgb_panel(&panel, &_panel);
        if (panel.bounce_buffer_size_px &&
            (result == ESP_ERR_NO_MEM ||
             (result == ESP_OK &&
              (heap_caps_get_free_size(internalCaps) < internalReserve ||
               heap_caps_get_largest_free_block(internalCaps) < largestReserve)))) {
            if (_panel) { esp_lcd_panel_del(_panel); _panel = nullptr; }
            if (bounceRequired) {
                result = ESP_ERR_NO_MEM;
            } else {
                panel.bounce_buffer_size_px = 0;
                result = esp_lcd_new_rgb_panel(&panel, &_panel);
            }
        }
        if (result == ESP_OK) {
            LOGGER.infof("Screen RGB active: bounce=%u bytes lines=%u pclk=%u internal=%u largest=%u",
                static_cast<unsigned>(panel.bounce_buffer_size_px * sizeof(uint16_t) * 2U),
                static_cast<unsigned>(panel.bounce_buffer_size_px / config.width),
                static_cast<unsigned>(config.rgbFrequency),
                static_cast<unsigned>(heap_caps_get_free_size(internalCaps)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(internalCaps)));
        }
#if FASTBEE_RGB_SCAN_TELEMETRY == 1
        if (result == ESP_OK && panel.bounce_buffer_size_px) {
            portENTER_CRITICAL(&scanLock);
            scanCounters = {};
            portEXIT_CRITICAL(&scanLock);
            esp_lcd_rgb_panel_event_callbacks_t callbacks{};
            callbacks.on_vsync = scanVsync;
            callbacks.on_frame_buf_complete = scanFrame;
            result = esp_lcd_rgb_panel_register_event_callbacks(_panel, &callbacks, nullptr);
            _scanAttached = result == ESP_OK;
            _scanReportedAt = millis();
            _scanLastVsyncs = _scanLastFrames = 0;
        }
#endif
        if (result == ESP_OK) result = esp_lcd_panel_reset(_panel);
        if (result == ESP_OK) result = esp_lcd_panel_init(_panel);
        // Let the RGB driver rotate the complete draw rectangle into its PSRAM
        // framebuffer. One submission means one cache synchronization, including
        // at 180 degrees; submitting each source row separately multiplies cache
        // and DMA descriptor maintenance while scanout competes for PSRAM.
        if (result == ESP_OK) result = esp_lcd_panel_swap_xy(_panel, (config.rotation & 1) != 0);
        if (result == ESP_OK) result = esp_lcd_panel_mirror(_panel,
            config.rotation == 1 || config.rotation == 2,
            config.rotation == 2 || config.rotation == 3);
        if (result == ESP_OK && !fastbee_rgb_recovery_attach(_panel, millis())) {
            result = ESP_FAIL;
        }
        if (result != ESP_OK) {
            error = "SCREEN_RGB_INIT_FAILED:" + String(static_cast<int>(result));
            end(); return false;
        }
        _ready = true;
        if (!fill(0) || !setBrightness(config.brightness)) {
            error = "SCREEN_RGB_FIRST_FRAME_FAILED"; end(); return false;
        }
        error = "";
        return true;
    }

    void end() override {
        // The active-high buzzer can remain latched across an MCU reset.
        // Clear it before giving up the shared CH32 lease.
        if (_controlLease && _ch32 && _config.controlBuzzerMask) {
            for (uint8_t attempt = 0; attempt < 3; ++attempt) {
                if (setBuzzer(false)) break;
                delay(2);
            }
        }
        if (_ready || _controlLease || _backlightAttached) applyBrightness(_brightness, true);
        _ready = false;
        if (_panel) {
#if FASTBEE_RGB_SCAN_TELEMETRY == 1
            if (_scanAttached) {
                esp_lcd_rgb_panel_event_callbacks_t callbacks{};
                esp_lcd_rgb_panel_register_event_callbacks(_panel, &callbacks, nullptr);
                _scanAttached = false;
            }
#endif
            fastbee_rgb_recovery_detach(_panel);
            esp_lcd_panel_del(_panel);
            _panel = nullptr;
        }
        if (_backlightAttached) { ledcDetach(_config.backlight); _backlightAttached = false; }
        if (_commandPinsActive) {
            digitalWrite(_config.cs, HIGH);
            for (int8_t pin : {_config.cs, _config.sclk, _config.mosi, _config.rst, _config.backlight})
                if (pin >= 0) pinMode(pin, INPUT);
            _commandPinsActive = false;
        }
        if (_controlLease) {
            if (!I2CBusManager::instance().release(this)) I2CBusManager::instance().deferRelease(this);
            _controlLease = false;
        }
        _sleeping = false;
    }
    bool ready() const override { return _ready; }
    DisplayCapabilities capabilities() const override {
        DisplayCapabilities caps;
        caps.width = _width; caps.height = _height;
        caps.brightness = _config.backlight >= 0 || _ch32 || _config.controlBacklightMask != 0;
        // RGB refresh remains active; sleep blanks backlight without losing pixels.
        caps.sleep = caps.brightness;
        caps.brightnessContinuous = _config.backlight >= 0 || _ch32;
        caps.minVisibleBrightnessPercent = MIN_VISIBLE_BRIGHTNESS_PERCENT;
        // Wake availability belongs to the successfully initialized touch
        // backend and is composed by ScreenRuntime, not guessed from display pins.
        caps.touchWake = false;
        caps.buzzer = _ch32 && _controlLease && _config.controlBuzzerMask != 0;
        return caps;
    }
    bool pushPixels(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                    const uint16_t* pixels, size_t count) override {
        if (!_ready || !pixels || !width || !height || x + width > _width || y + height > _height ||
            count < static_cast<size_t>(width) * height) return false;
        return esp_lcd_panel_draw_bitmap(_panel, x, y, x + width, y + height, pixels) == ESP_OK;
    }
    bool fill(uint16_t color) override {
        if (!_ready) return false;
        void* frame = nullptr;
        if (esp_lcd_rgb_panel_get_frame_buffer(_panel, 1, &frame) != ESP_OK || !frame) return false;
        std::fill_n(static_cast<uint16_t*>(frame), static_cast<size_t>(_config.width) * _config.height, color);
        // A uniform frame needs no rotation. Submit the driver's own buffer
        // once for cache synchronization, avoiding 480 draw/cache operations
        // and a permanently retained 960-byte internal line buffer.
        return esp_lcd_panel_draw_bitmap(_panel, 0, 0, _config.width, _config.height, frame) == ESP_OK;
    }
    bool setBrightness(uint8_t percent) override {
        percent = normalizeBrightnessPercent(percent);
        if (!applyBrightness(percent, _sleeping)) return false;
        _brightness = percent;
        return true;
    }
    bool setSleep(bool sleeping) override {
        if (!_ready || !capabilities().sleep) return false;
        if (!applyBrightness(_brightness, sleeping)) return false;
        _sleeping = sleeping;
        return true;
    }
    bool setBuzzer(bool enabled) override {
        if (!_controlLease || !_ch32 || !_config.controlBuzzerMask) return false;
        return updateRegister(outputRegister(), _config.controlBuzzerMask,
                              enabled ? _config.controlBuzzerMask : 0);
    }
    void service(uint32_t nowMs) override {
#if FASTBEE_RGB_SCAN_TELEMETRY == 1
        if (!_scanAttached || nowMs - _scanReportedAt < 30000U) return;
        RgbScanCounters current;
        portENTER_CRITICAL(&scanLock);
        current = scanCounters;
        scanCounters.maxVsyncUs = 0;
        scanCounters.minVsyncUs = UINT32_MAX;
        portEXIT_CRITICAL(&scanLock);
        // Counts measure scan progress, not physical display acceptance or an
        // exact underrun count. No allocations/file logging in either callback.
        Serial.printf("[RGBSCAN] ms=%lu vsync=%lu frames=%lu interval_us=%lu..%lu internal=%u largest=%u\n",
            static_cast<unsigned long>(nowMs - _scanReportedAt),
            static_cast<unsigned long>(current.vsyncs - _scanLastVsyncs),
            static_cast<unsigned long>(current.frames - _scanLastFrames),
            static_cast<unsigned long>(current.minVsyncUs),
            static_cast<unsigned long>(current.maxVsyncUs),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        _scanReportedAt = nowMs;
        _scanLastVsyncs = current.vsyncs;
        _scanLastFrames = current.frames;
#else
        (void)nowMs;
#endif
    }
private:
    bool applyBrightness(uint8_t percent, bool sleeping) {
        const uint8_t duty = backlightDuty(percent, sleeping, _config.backlightActiveHigh);
        if (_backlightAttached) return ledcWrite(_config.backlight, duty);
        if (_controlLease && _ch32) return writeRegister(0x05, duty);
        if (_controlLease && _config.controlBacklightMask)
            return updateRegister(outputRegister(), _config.controlBacklightMask,
                (!sleeping) == _config.backlightActiveHigh ? _config.controlBacklightMask : 0);
        return true;
    }

    bool initializeCh32(uint8_t outputMask, uint8_t initialOutput) {
        I2CBusLock lock;
        TwoWire* wire = lock.locked() ? I2CBusManager::instance().wire(this) : nullptr;
        uint8_t input = 0;
        if (!wire || !readLocked(wire, 0x04, input)) return false;
        const uint8_t output = (input & ~outputMask) | (initialOutput & outputMask);
        if (!writeLocked(wire, 0x03, output)) return false;
        _ch32Output = output;
        // The CH32 driver owns the complete I2C address. Direction/output are
        // write-only in the vendor protocol; their unknown prior directions
        // cannot be inherited. Undeclared pins stay inputs (including TP_INT),
        // with their sampled levels retained in the output latch.
        if (!writeLocked(wire, 0x02, outputMask)) return false;
        _ch32Direction = outputMask;
        LOGGER.infof("Screen CH32: input=0x%02X direction=0x%02X output=0x%02X", input, _ch32Direction, _ch32Output);
        return true;
    }

    uint8_t outputRegister() const { return _ch32 ? 0x03 : 0x01; }
    bool readLocked(TwoWire* wire, uint8_t reg, uint8_t& value) {
        wire->beginTransmission(_config.controlAddress); wire->write(reg);
        if (wire->endTransmission(false) || wire->requestFrom(_config.controlAddress, static_cast<uint8_t>(1)) != 1) return false;
        value = static_cast<uint8_t>(wire->read()); return true;
    }
    bool writeLocked(TwoWire* wire, uint8_t reg, uint8_t value) {
        wire->beginTransmission(_config.controlAddress); wire->write(reg); wire->write(value);
        return wire->endTransmission() == 0;
    }
    bool writeRegister(uint8_t reg, uint8_t value) {
        I2CBusLock lock;
        TwoWire* wire = lock.locked() ? I2CBusManager::instance().wire(this) : nullptr;
        return wire && writeLocked(wire, reg, value);
    }
    bool updateRegister(uint8_t reg, uint8_t mask, uint8_t value) {
        if (!mask) return true;
        I2CBusLock lock;
        TwoWire* wire = lock.locked() ? I2CBusManager::instance().wire(this) : nullptr;
        if (!wire) return false;
        if (_ch32 && (reg == 0x02 || reg == 0x03)) {
            uint8_t& shadow = reg == 0x02 ? _ch32Direction : _ch32Output;
            const uint8_t next = (shadow & ~mask) | (value & mask);
            if (!writeLocked(wire, reg, next)) return false;
            shadow = next;
            return true;
        }
        uint8_t current;
        return readLocked(wire, reg, current) &&
            writeLocked(wire, reg, static_cast<uint8_t>((current & ~mask) | (value & mask)));
    }
    void sendWord(bool data, uint8_t value) {
        uint16_t word = (data ? 0x100 : 0) | value;
        for (int bit = 8; bit >= 0; --bit) {
            digitalWrite(_config.sclk, LOW);
            digitalWrite(_config.mosi, (word >> bit) & 1);
            delayMicroseconds(1); digitalWrite(_config.sclk, HIGH); delayMicroseconds(1);
        }
    }
    void sendCommand(uint8_t command, const uint8_t* data = nullptr, uint8_t count = 0) {
        digitalWrite(_config.cs, LOW); sendWord(false, command);
        for (uint8_t i = 0; i < count; ++i) sendWord(true, data[i]);
        digitalWrite(_config.cs, HIGH);
    }
    DisplayConfigParams _config{};
    esp_lcd_panel_handle_t _panel = nullptr;
    uint16_t _width = 0, _height = 0;
    uint8_t _brightness = 0;
    uint8_t _ch32Direction = 0, _ch32Output = 0;
    bool _ready = false, _ch32 = false, _controlLease = false;
    bool _commandPinsActive = false, _backlightAttached = false, _sleeping = false;
#if FASTBEE_RGB_SCAN_TELEMETRY == 1
    bool _scanAttached = false;
    uint32_t _scanReportedAt = 0, _scanLastVsyncs = 0, _scanLastFrames = 0;
#endif
};
}  // namespace
#endif

std::unique_ptr<IDisplayBackend> createRgbDisplayBackend(const DisplayConfigParams& config, String& error) {
#if FASTBEE_ENABLE_SCREEN_UI && defined(ARDUINO_ARCH_ESP32) && CHIP_IS_ESP32S3
    if (!strcasecmp(config.backend, "rgb") && !strcasecmp(config.driver, "st7701")) {
        std::unique_ptr<IDisplayBackend> backend(new (std::nothrow) RgbDisplayBackend());
        if (!backend) error = "SCREEN_DISPLAY_ALLOCATION_FAILED";
        return backend;
    }
    error = "SCREEN_DISPLAY_BACKEND_UNSUPPORTED";
#else
    (void)config;
    error = "SCREEN_RGB_REQUIRES_ESP32S3";
#endif
    return nullptr;
}
} }  // namespace FastBee::Screen
