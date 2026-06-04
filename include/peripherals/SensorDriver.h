#ifndef SENSOR_DRIVER_H
#define SENSOR_DRIVER_H

#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_SENSOR_DRIVER

#include <Arduino.h>
#include <map>

/**
 * @brief 传感器驱动管理器
 * 
 * 管理 DHT11/DHT22/DS18B20/超声波/电流/电压传感器的初始化和数据读取。
 * 设计原则：
 * - 惰性初始化：首次读取时自动初始化对应引脚的传感器
 * - 缓存机制：避免过于频繁的读取（DHT 2s，DS18B20 1s，ADC 200ms，超声波 100ms）
 * - 线程安全：异步任务中调用时不阻塞主循环（已在独立 FreeRTOS 任务中运行）
 * - 线性校准：电流/电压传感器支持参数化转换公式
 */

// 传感器读取结果
struct SensorReadResult {
    bool success;
    float temperature;    // 温度 (°C)
    float humidity;       // 湿度 (%) - 仅 DHT 有效
    unsigned long timestamp;  // 读取时间戳 (millis)
};

// 通用传感器读取结果（用于超声波/电流/电压等）
struct SensorValueResult {
    bool success;
    float value;              // 读取值（单位取决于传感器类型：cm/A/V）
    unsigned long timestamp;  // 读取时间戳 (millis)
};

// ADC 传感器校准参数
struct ADCSensorCalibration {
    float vRef;           // ADC 参考电压 (V)，默认 3.3
    float sensitivity;    // 电流传感器灵敏度 (V/A)，如 ACS712-20A: 0.100
    float offset;         // 零偏移电压 (V)，如 ACS712 的 VCC/2 = 1.65
    float ratio;          // 电压分压比，如 R1=30k,R2=7.5k 则 ratio=(30+7.5)/7.5=5.0
    uint16_t adcMax;      // ADC 最大值，默认 4095 (12-bit)

    ADCSensorCalibration()
        : vRef(3.3f), sensitivity(0.100f), offset(1.65f),
          ratio(1.0f), adcMax(4095) {}
};

// 传感器类型
enum class SensorDriverType : uint8_t {
    DHT11 = 0,
    DHT22 = 1,
    DS18B20 = 2,
    ULTRASONIC = 3,   // HC-SR04 超声波
    CURRENT = 4,      // 电流型 (ACS712 等)
    VOLTAGE = 5       // 电压型 (分压器 ADC)
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
     * @brief 读取超声波距离 (HC-SR04)
     * @param trigPin Trig 触发引脚
     * @param echoPin Echo 回响引脚
     * @return 距离 (cm)，失败返回 NAN，有效范围 2~400cm
     */
    float readUltrasonic(uint8_t trigPin, uint8_t echoPin);

    /**
     * @brief 读取电流传感器 (ACS712 等)
     * @param pin ADC 引脚
     * @param cal 校准参数（灵敏度、零偏移等）
     * @return 电流值 (A)，失败返回 NAN
     */
    float readCurrent(uint8_t pin, const ADCSensorCalibration& cal = ADCSensorCalibration());

    /**
     * @brief 读取电压传感器 (分压器 ADC)
     * @param pin ADC 引脚
     * @param cal 校准参数（分压比等）
     * @return 电压值 (V)，失败返回 NAN
     */
    float readVoltage(uint8_t pin, const ADCSensorCalibration& cal = ADCSensorCalibration());

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

    // 超声波实例缓存 (key = trigPin)
    struct UltrasonicInstance {
        uint8_t trigPin;
        uint8_t echoPin;
        bool initialized;
        SensorValueResult lastRead;
    };
    std::map<uint8_t, UltrasonicInstance> _ultrasonicInstances;

    // ADC 传感器实例缓存 (key = pin)
    struct ADCSensorInstance {
        bool initialized;
        SensorDriverType type;     // CURRENT 或 VOLTAGE
        ADCSensorCalibration cal;  // 校准参数
        SensorValueResult lastRead;
    };
    std::map<uint8_t, ADCSensorInstance> _adcSensorInstances;

    // DHT 最小读取间隔 (ms)
    static constexpr unsigned long DHT_MIN_INTERVAL = 2000;
    // DS18B20 最小读取间隔 (ms)
    static constexpr unsigned long DS18B20_MIN_INTERVAL = 1000;
    // 超声波最小读取间隔 (ms)
    static constexpr unsigned long ULTRASONIC_MIN_INTERVAL = 100;
    // ADC 传感器最小读取间隔 (ms)
    static constexpr unsigned long ADC_SENSOR_MIN_INTERVAL = 200;
};

#endif // FASTBEE_ENABLE_SENSOR_DRIVER
#endif // SENSOR_DRIVER_H
