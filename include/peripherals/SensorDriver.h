#ifndef SENSOR_DRIVER_H
#define SENSOR_DRIVER_H

#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_SENSOR_DRIVER

#include <Arduino.h>
#include <map>

/**
 * @brief 传感器驱动管理器
 * 
 * 管理 DHT11/DHT22/DS18B20 传感器的初始化和数据读取。
 * 设计原则：
 * - 惰性初始化：首次读取时自动初始化对应引脚的传感器
 * - 缓存机制：避免过于频繁的读取（DHT 最小间隔 2s，DS18B20 转换需 750ms）
 * - 线程安全：异步任务中调用时不阻塞主循环（已在独立 FreeRTOS 任务中运行）
 */

// 传感器读取结果
struct SensorReadResult {
    bool success;
    float temperature;    // 温度 (°C)
    float humidity;       // 湿度 (%) - 仅 DHT 有效
    unsigned long timestamp;  // 读取时间戳 (millis)
};

// 传感器类型
enum class SensorDriverType : uint8_t {
    DHT11 = 0,
    DHT22 = 1,
    DS18B20 = 2
};

class SensorDriver {
public:
    static SensorDriver& getInstance();

    /**
     * @brief 读取 DHT 传感器数据
     * @param pin GPIO 引脚号
     * @param type DHT11 或 DHT22
     * @param field "temperature" 或 "humidity"
     * @return 读取值，失败返回 NAN
     */
    float readDHT(uint8_t pin, SensorDriverType type, const String& field);

    /**
     * @brief 读取 DS18B20 温度
     * @param pin OneWire 数据引脚
     * @param index 总线上设备索引（默认 0，多设备时指定）
     * @return 温度值 (°C)，失败返回 NAN
     */
    float readDS18B20(uint8_t pin, uint8_t index = 0);

    /**
     * @brief 释放指定引脚的传感器资源
     */
    void release(uint8_t pin);

    /**
     * @brief 释放所有传感器资源
     */
    void releaseAll();

private:
    SensorDriver() = default;
    ~SensorDriver();
    SensorDriver(const SensorDriver&) = delete;
    SensorDriver& operator=(const SensorDriver&) = delete;

    // DHT 实例缓存 (key = pin)
    struct DHTInstance {
        void* dht;          // DHT* (避免头文件暴露)
        SensorDriverType type;
        SensorReadResult lastRead;
    };
    std::map<uint8_t, DHTInstance> _dhtInstances;

    // DS18B20 实例缓存 (key = pin)
    struct DS18B20Instance {
        void* oneWire;      // OneWire*
        void* sensors;      // DallasTemperature*
        SensorReadResult lastRead;
    };
    std::map<uint8_t, DS18B20Instance> _ds18b20Instances;

    // DHT 最小读取间隔 (ms)
    static constexpr unsigned long DHT_MIN_INTERVAL = 2000;
    // DS18B20 最小读取间隔 (ms)
    static constexpr unsigned long DS18B20_MIN_INTERVAL = 1000;
};

#endif // FASTBEE_ENABLE_SENSOR_DRIVER
#endif // SENSOR_DRIVER_H
