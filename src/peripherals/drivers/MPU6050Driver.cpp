#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_I2C_SENSORS

#include "core/interfaces/ISensorDriver.h"
#include "core/DriverRegistry.h"
#include "systems/LoggerSystem.h"

#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>

/**
 * @brief MPU6050 六轴惯性测量单元驱动（加速度 + 陀螺仪 + 温度）
 * 
 * 通道定义：
 *   0 - accelX (m/s²) - X 轴加速度
 *   1 - accelY (m/s²) - Y 轴加速度
 *   2 - accelZ (m/s²) - Z 轴加速度
 *   3 - temperature (°C) - 芯片温度
 * 
 * 注：陀螺仪数据（gyroX/Y/Z）可通过 params 中 "mode":"gyro" 切换
 * 默认模式为加速度 + 温度，适用于倾斜检测和运动感知
 * 
 * 初始化参数（JSON）：
 *   {"addr": 0x68}  - I2C 地址（可选，默认 0x68，AD0=HIGH 时为 0x69）
 *   {"mode": "gyro"} - 切换为陀螺仪模式（通道 0-2 变为 gyroX/Y/Z rad/s）
 * 
 * 编译条件：FASTBEE_ENABLE_I2C_SENSORS=1（仅 esp32s3-full）
 */
class MPU6050Driver : public ISensorDriver {
public:
    MPU6050Driver() = default;
    ~MPU6050Driver() override { deinit(); }

    const char* getName() const override { return "MPU6050"; }
    uint8_t getChannelCount() const override { return 4; }

    const char* getChannelName(uint8_t channel) const override {
        if (_gyroMode) {
            switch (channel) {
                case 0: return "gyroX";
                case 1: return "gyroY";
                case 2: return "gyroZ";
                case 3: return "temperature";
                default: return "unknown";
            }
        }
        switch (channel) {
            case 0: return "accelX";
            case 1: return "accelY";
            case 2: return "accelZ";
            case 3: return "temperature";
            default: return "unknown";
        }
    }

    const char* getChannelUnit(uint8_t channel) const override {
        if (_gyroMode) {
            switch (channel) {
                case 0: case 1: case 2: return "rad/s";
                case 3: return "°C";
                default: return "";
            }
        }
        switch (channel) {
            case 0: case 1: case 2: return "m/s²";
            case 3: return "°C";
            default: return "";
        }
    }

    bool init(uint8_t pin, const char* params = nullptr) override {
        if (_initialized) return true;

        _addr = 0x68;
        _gyroMode = false;

        // 解析可选参数
        if (params && params[0] == '{') {
            const char* addrStr = strstr(params, "\"addr\"");
            if (addrStr) {
                addrStr = strchr(addrStr, ':');
                if (addrStr) _addr = (uint8_t)strtol(addrStr + 1, nullptr, 0);
            }
            if (strstr(params, "\"gyro\"")) {
                _gyroMode = true;
            }
        }

        _mpu = new Adafruit_MPU6050();

        if (!_mpu->begin(_addr, &Wire)) {
            // 尝试备用地址
            uint8_t altAddr = (_addr == 0x68) ? 0x69 : 0x68;
            if (!_mpu->begin(altAddr, &Wire)) {
                LOGGER.errorf("[MPU6050] Init failed at 0x%02X and 0x%02X", _addr, altAddr);
                delete _mpu;
                _mpu = nullptr;
                return false;
            }
            _addr = altAddr;
        }

        // 配置测量范围
        _mpu->setAccelerometerRange(MPU6050_RANGE_8_G);     // ±8g
        _mpu->setGyroRange(MPU6050_RANGE_500_DEG);          // ±500°/s
        _mpu->setFilterBandwidth(MPU6050_BAND_21_HZ);       // 低通滤波 21Hz

        _initialized = true;
        LOGGER.infof("[MPU6050] Initialized at 0x%02X mode=%s",
                     _addr, _gyroMode ? "gyro" : "accel");
        return true;
    }

    bool read(SensorReading& reading) override {
        if (!_initialized || !_mpu) {
            reading.success = false;
            return false;
        }

        unsigned long now = millis();
        if (_lastReading.success && (now - _lastReading.timestamp) < getMinInterval()) {
            reading = _lastReading;
            return true;
        }

        sensors_event_t accel, gyro, temp;
        _mpu->getEvent(&accel, &gyro, &temp);

        reading.success = true;
        reading.channelCount = 4;
        reading.timestamp = now;

        if (_gyroMode) {
            reading.values[0] = gyro.gyro.x;
            reading.values[1] = gyro.gyro.y;
            reading.values[2] = gyro.gyro.z;
        } else {
            reading.values[0] = accel.acceleration.x;
            reading.values[1] = accel.acceleration.y;
            reading.values[2] = accel.acceleration.z;
        }
        reading.values[3] = temp.temperature;

        _lastReading = reading;

        LOGGER.debugf("[MPU6050] %s X=%.2f Y=%.2f Z=%.2f T=%.1f°C",
                     _gyroMode ? "Gyro" : "Accel",
                     reading.values[0], reading.values[1],
                     reading.values[2], reading.values[3]);
        return true;
    }

    void deinit() override {
        if (_mpu) {
            delete _mpu;
            _mpu = nullptr;
        }
        _initialized = false;
    }

    unsigned long getMinInterval() const override { return 100; }

private:
    Adafruit_MPU6050* _mpu = nullptr;
    uint8_t _addr = 0x68;
    bool _initialized = false;
    bool _gyroMode = false;
    SensorReading _lastReading;
};

// 自动注册到 DriverRegistry
FASTBEE_REGISTER_SENSOR("MPU6050", MPU6050Driver);

#endif // FASTBEE_ENABLE_I2C_SENSORS
