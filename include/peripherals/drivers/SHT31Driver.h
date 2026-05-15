#ifndef SHT31_DRIVER_H
#define SHT31_DRIVER_H

#include "core/interfaces/ISensorDriver.h"
#include "core/DriverRegistry.h"
#include <Wire.h>

/**
 * @brief SHT31 温湿度传感器驱动（示例）
 * 
 * 演示如何通过 ISensorDriver 接口 + FASTBEE_REGISTER_SENSOR 宏
 * 实现热插拔式传感器驱动注册。
 * 
 * 硬件接线：I2C（SDA/SCL），默认地址 0x44
 * 通道：[0] temperature (°C), [1] humidity (%)
 */
class SHT31Driver : public ISensorDriver {
public:
    const char* getName() const override { return "SHT31"; }
    uint8_t getChannelCount() const override { return 2; }
    
    const char* getChannelName(uint8_t channel) const override {
        static const char* names[] = {"temperature", "humidity"};
        return (channel < 2) ? names[channel] : "unknown";
    }
    
    const char* getChannelUnit(uint8_t channel) const override {
        static const char* units[] = {"°C", "%"};
        return (channel < 2) ? units[channel] : "";
    }
    
    bool init(uint8_t pin, const char* params) override {
        // pin 参数在 I2C 传感器中不直接使用（使用默认 I2C 总线）
        // params 可以包含 JSON 格式的 I2C 地址覆盖，如 {"addr": "0x45"}
        _addr = 0x44;  // 默认地址
        if (params && strlen(params) > 0) {
            // 简单解析地址参数
            String p(params);
            int idx = p.indexOf("0x");
            if (idx >= 0) {
                _addr = (uint8_t)strtol(p.c_str() + idx, nullptr, 16);
            }
        }
        
        Wire.beginTransmission(_addr);
        uint8_t err = Wire.endTransmission();
        _initialized = (err == 0);
        return _initialized;
    }
    
    bool read(SensorReading& reading) override {
        if (!_initialized) return false;
        
        // 发送单次测量命令（高精度）
        Wire.beginTransmission(_addr);
        Wire.write(0x24);  // MSB
        Wire.write(0x00);  // LSB - 高重复性
        if (Wire.endTransmission() != 0) return false;
        
        delay(15);  // 等待测量完成
        
        Wire.requestFrom(_addr, (uint8_t)6);
        if (Wire.available() < 6) return false;
        
        uint8_t data[6];
        for (int i = 0; i < 6; i++) data[i] = Wire.read();
        
        // CRC 校验（简化：跳过）
        uint16_t rawTemp = (data[0] << 8) | data[1];
        uint16_t rawHum  = (data[3] << 8) | data[4];
        
        reading.values[0] = -45.0f + 175.0f * (float)rawTemp / 65535.0f;
        reading.values[1] = 100.0f * (float)rawHum / 65535.0f;
        reading.channelCount = 2;
        reading.success = true;
        reading.timestamp = millis();
        return true;
    }
    
    void deinit() override {
        _initialized = false;
    }
    
    unsigned long getMinInterval() const override { return 500; }

private:
    uint8_t _addr = 0x44;
    bool _initialized = false;
};

// 热插拔注册：编译链接时自动注册到 DriverRegistry
FASTBEE_REGISTER_SENSOR("SHT31", SHT31Driver);

#endif // SHT31_DRIVER_H
