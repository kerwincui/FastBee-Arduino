#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_SENSOR_DRIVER

#include "peripherals/SensorDriver.h"
#include "systems/LoggerSystem.h"
#include <DHT.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <soc/gpio_periph.h>

// 引脚有效性预检：避免对无效 pin 反复 new OneWire/DHT 导致内存泄漏
static inline bool sensor_pin_valid(uint8_t pin) {
    if (pin == 255) return false;
    if (pin > GPIO_NUM_MAX - 1) return false;
    return GPIO_IS_VALID_GPIO(pin);
}

SensorDriver& SensorDriver::getInstance() {
    static SensorDriver instance;
    return instance;
}

SensorDriver::~SensorDriver() {
    releaseAll();
}

float SensorDriver::readDHT(uint8_t pin, SensorDriverType type, const String& field) {
    // 引脚有效性预检：避免对无效 pin 反复 new DHT 导致内存泄漏
    if (!sensor_pin_valid(pin)) {
        static unsigned long s_lastWarn = 0;
        unsigned long now = millis();
        if (now - s_lastWarn > 60000) {
            LOGGER.errorf("[SensorDriver] DHT invalid pin %u, refuse to init (suppressed for 60s)", (unsigned)pin);
            s_lastWarn = now;
        }
        return NAN;
    }

    // 查找或创建 DHT 实例
    auto it = _dhtInstances.find(pin);
    if (it == _dhtInstances.end()) {
        // 惰性初始化
        uint8_t dhtType = (type == SensorDriverType::DHT11) ? DHT11 : DHT22;
        DHT* dht = new DHT(pin, dhtType);
        dht->begin();
        
        DHTInstance inst;
        inst.dht = dht;
        inst.type = type;
        inst.lastRead = {false, NAN, NAN, 0};
        _dhtInstances[pin] = inst;
        it = _dhtInstances.find(pin);
        
        LOGGER.infof("[SensorDriver] DHT%s initialized on pin %d",
                     type == SensorDriverType::DHT11 ? "11" : "22", pin);
        
        // 首次初始化后等待传感器稳定
        delay(100);
    }

    DHTInstance& inst = it->second;
    DHT* dht = static_cast<DHT*>(inst.dht);
    unsigned long now = millis();

    // 缓存有效性检查：避免过于频繁读取
    if (inst.lastRead.success && (now - inst.lastRead.timestamp) < DHT_MIN_INTERVAL) {
        // 使用缓存值
        if (field == "humidity") return inst.lastRead.humidity;
        return inst.lastRead.temperature;
    }

    // 执行实际读取
    float humidity = dht->readHumidity();
    float temperature = dht->readTemperature();

    if (isnan(humidity) || isnan(temperature)) {
        LOGGER.warningf("[SensorDriver] DHT read failed on pin %d", pin);
        // 如果有旧缓存且未超过 10s，仍返回旧值
        if (inst.lastRead.success && (now - inst.lastRead.timestamp) < 10000) {
            if (field == "humidity") return inst.lastRead.humidity;
            return inst.lastRead.temperature;
        }
        inst.lastRead.success = false;
        return NAN;
    }

    // 更新缓存
    inst.lastRead.success = true;
    inst.lastRead.temperature = temperature;
    inst.lastRead.humidity = humidity;
    inst.lastRead.timestamp = now;

    LOGGER.debugf("[SensorDriver] DHT pin=%d temp=%.1f humi=%.1f", pin, temperature, humidity);

    if (field == "humidity") return humidity;
    return temperature;
}

float SensorDriver::readDS18B20(uint8_t pin, uint8_t index) {
    // 引脚有效性预检：避免对无效 pin 反复 new OneWire/DallasTemperature 导致内存泄漏
    if (!sensor_pin_valid(pin)) {
        static unsigned long s_lastWarn = 0;
        unsigned long now = millis();
        if (now - s_lastWarn > 60000) {
            LOGGER.errorf("[SensorDriver] DS18B20 invalid pin %u, refuse to init (suppressed for 60s)", (unsigned)pin);
            s_lastWarn = now;
        }
        return NAN;
    }

    // 查找或创建 DS18B20 实例
    auto it = _ds18b20Instances.find(pin);
    if (it == _ds18b20Instances.end()) {
        // 惰性初始化
        OneWire* ow = new OneWire(pin);
        DallasTemperature* sensors = new DallasTemperature(ow);
        sensors->begin();
        // 设置为 12 位精度（默认），但使用非阻塞模式
        sensors->setWaitForConversion(true);  // 在异步任务中可以阻塞等待

        DS18B20Instance inst;
        inst.oneWire = ow;
        inst.sensors = sensors;
        inst.lastRead = {false, NAN, NAN, 0};
        _ds18b20Instances[pin] = inst;
        it = _ds18b20Instances.find(pin);

        LOGGER.infof("[SensorDriver] DS18B20 initialized on pin %d, devices=%d",
                     pin, sensors->getDeviceCount());
    }

    DS18B20Instance& inst = it->second;
    DallasTemperature* sensors = static_cast<DallasTemperature*>(inst.sensors);
    unsigned long now = millis();

    // 缓存有效性检查
    if (inst.lastRead.success && (now - inst.lastRead.timestamp) < DS18B20_MIN_INTERVAL) {
        return inst.lastRead.temperature;
    }

    // 发起温度转换并读取
    sensors->requestTemperatures();
    float temperature = sensors->getTempCByIndex(index);

    if (temperature == DEVICE_DISCONNECTED_C || temperature == -127.0f) {
        LOGGER.warningf("[SensorDriver] DS18B20 read failed on pin %d index %d", pin, index);
        // 返回旧缓存（10s 内有效）
        if (inst.lastRead.success && (now - inst.lastRead.timestamp) < 10000) {
            return inst.lastRead.temperature;
        }
        inst.lastRead.success = false;
        return NAN;
    }

    // 更新缓存
    inst.lastRead.success = true;
    inst.lastRead.temperature = temperature;
    inst.lastRead.timestamp = now;

    LOGGER.debugf("[SensorDriver] DS18B20 pin=%d idx=%d temp=%.2f", pin, index, temperature);
    return temperature;
}

void SensorDriver::release(uint8_t pin) {
    // 释放 DHT
    auto dhtIt = _dhtInstances.find(pin);
    if (dhtIt != _dhtInstances.end()) {
        delete static_cast<DHT*>(dhtIt->second.dht);
        _dhtInstances.erase(dhtIt);
    }
    // 释放 DS18B20
    auto dsIt = _ds18b20Instances.find(pin);
    if (dsIt != _ds18b20Instances.end()) {
        delete static_cast<DallasTemperature*>(dsIt->second.sensors);
        delete static_cast<OneWire*>(dsIt->second.oneWire);
        _ds18b20Instances.erase(dsIt);
    }
}

void SensorDriver::releaseAll() {
    for (auto& pair : _dhtInstances) {
        delete static_cast<DHT*>(pair.second.dht);
    }
    _dhtInstances.clear();

    for (auto& pair : _ds18b20Instances) {
        delete static_cast<DallasTemperature*>(pair.second.sensors);
        delete static_cast<OneWire*>(pair.second.oneWire);
    }
    _ds18b20Instances.clear();
}

#endif // FASTBEE_ENABLE_SENSOR_DRIVER
