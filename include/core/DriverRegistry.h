#ifndef DRIVER_REGISTRY_H
#define DRIVER_REGISTRY_H

#include <Arduino.h>
#include "core/interfaces/ISensorDriver.h"

/**
 * @brief 驱动注册表（静态数组，零运行时分配）
 * 
 * 使用静态构造函数模式实现"热插拔"注册：
 * - 新驱动只需在 .cpp 文件末尾使用 FASTBEE_REGISTER_SENSOR(name, ClassName)
 * - 链接时自动注册到全局表，无需修改 PeripheralManager
 * - 最多支持 8 种第三方驱动（可调整 MAX_SENSOR_DRIVERS）
 */

namespace FastBee {

// 驱动工厂函数类型
using SensorDriverFactory = ISensorDriver* (*)();

// 驱动注册条目
struct SensorDriverEntry {
    const char* name;            // 驱动名称（如 "SHT31", "BMP280"）
    SensorDriverFactory factory; // 工厂函数
};

// 最大注册驱动数
static constexpr size_t MAX_SENSOR_DRIVERS = 8;

/**
 * @brief 传感器驱动注册表（Meyers' Singleton）
 */
class DriverRegistry {
public:
    static DriverRegistry& getInstance() {
        static DriverRegistry instance;
        return instance;
    }

    /**
     * @brief 注册驱动（由宏在静态初始化阶段调用）
     * @param name 驱动名称
     * @param factory 工厂函数
     * @return 是否注册成功
     */
    bool registerDriver(const char* name, SensorDriverFactory factory) {
        if (!name || !factory) return false;
        if (_count >= MAX_SENSOR_DRIVERS) return false;
        // 检查重名
        for (size_t i = 0; i < _count; i++) {
            if (equalsName(_drivers[i].name, name)) return false;
        }
        _drivers[_count++] = {name, factory};
        return true;
    }

    /**
     * @brief 通过名称创建驱动实例
     * @param name 驱动名称
     * @return 驱动实例指针（调用者负责 delete），未找到返回 nullptr
     */
    ISensorDriver* createDriver(const char* name) const {
        if (!name) return nullptr;
        for (size_t i = 0; i < _count; i++) {
            if (equalsName(_drivers[i].name, name)) {
                return _drivers[i].factory();
            }
        }
        return nullptr;
    }

    /**
     * @brief 获取已注册驱动数量
     */
    size_t getDriverCount() const { return _count; }

    /**
     * @brief 获取驱动条目（用于枚举）
     */
    const SensorDriverEntry* getDriverEntry(size_t index) const {
        return (index < _count) ? &_drivers[index] : nullptr;
    }

    /**
     * @brief 检查驱动是否已注册
     */
    bool hasDriver(const char* name) const {
        if (!name) return false;
        for (size_t i = 0; i < _count; i++) {
            if (equalsName(_drivers[i].name, name)) return true;
        }
        return false;
    }

private:
    DriverRegistry() = default;
    static bool equalsName(const char* a, const char* b) {
        if (!a || !b) return false;
        while (*a && *b) {
            char ca = *a;
            char cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca = char(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = char(cb - 'A' + 'a');
            if (ca != cb) return false;
            ++a;
            ++b;
        }
        return *a == '\0' && *b == '\0';
    }
    SensorDriverEntry _drivers[MAX_SENSOR_DRIVERS] = {};
    size_t _count = 0;
};

/**
 * @brief 驱动自动注册辅助类（配合宏使用）
 */
struct SensorDriverRegistrar {
    SensorDriverRegistrar(const char* name, SensorDriverFactory factory) {
        DriverRegistry::getInstance().registerDriver(name, factory);
    }
};

} // namespace FastBee

/**
 * @brief 传感器驱动热插拔注册宏
 * 
 * 用法：在驱动 .cpp 文件末尾添加：
 *   FASTBEE_REGISTER_SENSOR("SHT31", SHT31Driver)
 * 
 * 效果：编译链接后自动注册到 DriverRegistry，无需修改任何核心代码
 */
#define FASTBEE_REGISTER_SENSOR(name, DriverClass) \
    static ISensorDriver* _fastbee_create_##DriverClass() { return new DriverClass(); } \
    static FastBee::SensorDriverRegistrar _fastbee_reg_##DriverClass(name, _fastbee_create_##DriverClass)

#endif // DRIVER_REGISTRY_H
