#ifndef I_SENSOR_DRIVER_H
#define I_SENSOR_DRIVER_H

#include <Arduino.h>

/**
 * @brief 传感器读取结果（通用）
 */
struct SensorReading {
    bool success = false;
    float values[4] = {0};   // 最多 4 通道（温度/湿度/气压/光照等）
    uint8_t channelCount = 0;
    unsigned long timestamp = 0;
    
    float temperature() const { return channelCount > 0 ? values[0] : NAN; }
    float humidity()    const { return channelCount > 1 ? values[1] : NAN; }
    float pressure()    const { return channelCount > 2 ? values[2] : NAN; }
    float extra()       const { return channelCount > 3 ? values[3] : NAN; }
};

/**
 * @brief 传感器驱动抽象接口
 * 
 * 所有新增传感器驱动统一实现此接口，通过 FASTBEE_REGISTER_SENSOR 宏注册。
 * 现有的 DHT/DS18B20 驱动保持原有路径不变。
 * 
 * 生命周期：construct → init() → read() ... → deinit() → destroy
 */
class ISensorDriver {
public:
    virtual ~ISensorDriver() = default;

    /**
     * @brief 获取驱动名称（用于注册和日志）
     */
    virtual const char* getName() const = 0;

    /**
     * @brief 获取驱动支持的通道数
     */
    virtual uint8_t getChannelCount() const = 0;

    /**
     * @brief 获取通道名称（如 "temperature", "humidity"）
     * @param channel 通道索引
     */
    virtual const char* getChannelName(uint8_t channel) const = 0;

    /**
     * @brief 获取通道单位（如 "°C", "%"）
     * @param channel 通道索引
     */
    virtual const char* getChannelUnit(uint8_t channel) const = 0;

    /**
     * @brief 初始化传感器硬件
     * @param pin 主引脚号
     * @param params 附加参数（I2C地址等，JSON 编码字符串，可为空）
     * @return 是否初始化成功
     */
    virtual bool init(uint8_t pin, const char* params = nullptr) = 0;

    /**
     * @brief 读取传感器数据
     * @param reading 输出读取结果
     * @return 是否读取成功
     */
    virtual bool read(SensorReading& reading) = 0;

    /**
     * @brief 释放传感器硬件资源
     */
    virtual void deinit() = 0;

    /**
     * @brief 获取最小读取间隔（毫秒）
     */
    virtual unsigned long getMinInterval() const { return 1000; }
};

#endif // I_SENSOR_DRIVER_H
