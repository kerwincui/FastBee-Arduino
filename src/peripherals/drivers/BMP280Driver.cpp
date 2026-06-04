#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_I2C_SENSORS

#include "core/interfaces/ISensorDriver.h"
#include "core/DriverRegistry.h"
#include "systems/LoggerSystem.h"

#include <Wire.h>
#include <Adafruit_BMP280.h>

/**
 * @brief BMP280 气压/温度/海拔传感器驱动
 * 
 * 通道定义：
 *   0 - temperature (°C)
 *   1 - pressure (hPa)
 *   2 - altitude (m) - 基于标准大气压(1013.25hPa)估算
 * 
 * 初始化参数（JSON）：
 *   {"addr": 0x76}  - I2C 地址（可选，默认 0x76，备选 0x77）
 *   {"sda": 21, "scl": 22}  - I2C 引脚（可选，使用默认 Wire）
 * 
 * 编译条件：FASTBEE_ENABLE_I2C_SENSORS=1（仅 esp32s3-full）
 */
class BMP280Driver : public ISensorDriver {
public:
    BMP280Driver() = default;
    ~BMP280Driver() override { deinit(); }

    const char* getName() const override { return "BMP280"; }
    uint8_t getChannelCount() const override { return 3; }

    const char* getChannelName(uint8_t channel) const override {
        switch (channel) {
            case 0: return "temperature";
            case 1: return "pressure";
            case 2: return "altitude";
            default: return "unknown";
        }
    }

    const char* getChannelUnit(uint8_t channel) const override {
        switch (channel) {
            case 0: return "°C";
            case 1: return "hPa";
            case 2: return "m";
            default: return "";
        }
    }

    bool init(uint8_t pin, const char* params = nullptr) override {
        if (_initialized) return true;

        _addr = 0x76;  // 默认地址

        // 解析可选参数
        if (params && params[0] == '{') {
            // 简单解析 addr 字段
            const char* addrStr = strstr(params, "\"addr\"");
            if (addrStr) {
                addrStr = strchr(addrStr, ':');
                if (addrStr) {
                    _addr = (uint8_t)strtol(addrStr + 1, nullptr, 0);
                }
            }
        }

        _bmp = new Adafruit_BMP280(&Wire);

        if (!_bmp->begin(_addr)) {
            // 尝试备用地址
            uint8_t altAddr = (_addr == 0x76) ? 0x77 : 0x76;
            if (!_bmp->begin(altAddr)) {
                LOGGER.errorf("[BMP280] Init failed at 0x%02X and 0x%02X", _addr, altAddr);
                delete _bmp;
                _bmp = nullptr;
                return false;
            }
            _addr = altAddr;
        }

        // 配置采样参数（天气监测推荐设置）
        _bmp->setSampling(Adafruit_BMP280::MODE_NORMAL,
                          Adafruit_BMP280::SAMPLING_X2,   // 温度 2x 过采样
                          Adafruit_BMP280::SAMPLING_X16,  // 气压 16x 过采样
                          Adafruit_BMP280::FILTER_X16,    // IIR 滤波 16x
                          Adafruit_BMP280::STANDBY_MS_500); // 500ms 待机

        _initialized = true;
        LOGGER.infof("[BMP280] Initialized at 0x%02X", _addr);
        return true;
    }

    bool read(SensorReading& reading) override {
        if (!_initialized || !_bmp) {
            reading.success = false;
            return false;
        }

        unsigned long now = millis();
        if (_lastReading.success && (now - _lastReading.timestamp) < getMinInterval()) {
            reading = _lastReading;
            return true;
        }

        float temp = _bmp->readTemperature();
        float pressure = _bmp->readPressure() / 100.0f;  // Pa → hPa
        float altitude = _bmp->readAltitude(1013.25f);    // 标准大气压

        if (isnan(temp) || isnan(pressure)) {
            LOGGER.warning("[BMP280] Read failed");
            reading.success = false;
            return false;
        }

        reading.success = true;
        reading.values[0] = temp;
        reading.values[1] = pressure;
        reading.values[2] = altitude;
        reading.channelCount = 3;
        reading.timestamp = now;

        _lastReading = reading;

        LOGGER.debugf("[BMP280] T=%.1f°C P=%.1fhPa Alt=%.1fm", temp, pressure, altitude);
        return true;
    }

    void deinit() override {
        if (_bmp) {
            delete _bmp;
            _bmp = nullptr;
        }
        _initialized = false;
    }

    unsigned long getMinInterval() const override { return 500; }

private:
    Adafruit_BMP280* _bmp = nullptr;
    uint8_t _addr = 0x76;
    bool _initialized = false;
    SensorReading _lastReading;
};

// 自动注册到 DriverRegistry
FASTBEE_REGISTER_SENSOR("BMP280", BMP280Driver);

#endif // FASTBEE_ENABLE_I2C_SENSORS
