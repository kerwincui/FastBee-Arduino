#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_SENSOR_DRIVER

#include "peripherals/SensorDriver.h"
#include "systems/LoggerSystem.h"
#include <DHT.h>
#if defined(FASTBEE_ENABLE_DS18B20) && FASTBEE_ENABLE_DS18B20
#include <OneWire.h>
#include <DallasTemperature.h>
#endif
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
#if defined(FASTBEE_ENABLE_DS18B20) && FASTBEE_ENABLE_DS18B20
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
#else
    (void)pin; (void)index;
    LOGGER.warning("[SensorDriver] DS18B20 not available (FASTBEE_ENABLE_DS18B20=0)");
    return NAN;
#endif
}

// ========== 超声波测距 (HC-SR04) ==========

float SensorDriver::readUltrasonic(uint8_t trigPin, uint8_t echoPin) {
    // 引脚有效性预检
    if (!sensor_pin_valid(trigPin) || !sensor_pin_valid(echoPin)) {
        static unsigned long s_lastWarn = 0;
        unsigned long now = millis();
        if (now - s_lastWarn > 60000) {
            LOGGER.errorf("[SensorDriver] Ultrasonic invalid pins trig=%u echo=%u", (unsigned)trigPin, (unsigned)echoPin);
            s_lastWarn = now;
        }
        return NAN;
    }

    // 查找或创建实例
    auto it = _ultrasonicInstances.find(trigPin);
    if (it == _ultrasonicInstances.end()) {
        // 惰性初始化
        pinMode(trigPin, OUTPUT);
        pinMode(echoPin, INPUT);
        digitalWrite(trigPin, LOW);

        UltrasonicInstance inst;
        inst.trigPin = trigPin;
        inst.echoPin = echoPin;
        inst.initialized = true;
        inst.lastRead = {false, NAN, 0};
        _ultrasonicInstances[trigPin] = inst;
        it = _ultrasonicInstances.find(trigPin);

        LOGGER.infof("[SensorDriver] Ultrasonic initialized trig=%d echo=%d", trigPin, echoPin);
        delayMicroseconds(2);
    }

    UltrasonicInstance& inst = it->second;
    unsigned long now = millis();

    // 缓存有效性检查
    if (inst.lastRead.success && (now - inst.lastRead.timestamp) < ULTRASONIC_MIN_INTERVAL) {
        return inst.lastRead.value;
    }

    // 发送 10us 触发脉冲
    digitalWrite(trigPin, LOW);
    delayMicroseconds(2);
    digitalWrite(trigPin, HIGH);
    delayMicroseconds(10);
    digitalWrite(trigPin, LOW);

    // 测量 Echo 高电平持续时间（超时 30ms ≈ 510cm）
    unsigned long duration = pulseIn(echoPin, HIGH, 30000);

    if (duration == 0) {
        LOGGER.warningf("[SensorDriver] Ultrasonic timeout on trig=%d", trigPin);
        // 返回旧缓存（10s 内有效）
        if (inst.lastRead.success && (now - inst.lastRead.timestamp) < 10000) {
            return inst.lastRead.value;
        }
        inst.lastRead.success = false;
        return NAN;
    }

    // 距离计算：声速 340m/s，往返除以 2
    float distance = (float)duration * 0.034f / 2.0f;

    // 范围校验 (2~400cm)
    if (distance < 2.0f || distance > 400.0f) {
        LOGGER.warningf("[SensorDriver] Ultrasonic out of range: %.1f cm", distance);
        if (inst.lastRead.success && (now - inst.lastRead.timestamp) < 10000) {
            return inst.lastRead.value;
        }
        inst.lastRead.success = false;
        return NAN;
    }

    // 更新缓存
    inst.lastRead.success = true;
    inst.lastRead.value = distance;
    inst.lastRead.timestamp = now;

    LOGGER.debugf("[SensorDriver] Ultrasonic trig=%d dist=%.1f cm", trigPin, distance);
    return distance;
}

// ========== 电流传感器 (ACS712 等) ==========

float SensorDriver::readCurrent(uint8_t pin, const ADCSensorCalibration& cal) {
    if (!sensor_pin_valid(pin)) {
        static unsigned long s_lastWarn = 0;
        unsigned long now = millis();
        if (now - s_lastWarn > 60000) {
            LOGGER.errorf("[SensorDriver] Current sensor invalid pin %u", (unsigned)pin);
            s_lastWarn = now;
        }
        return NAN;
    }

    // 查找或创建实例
    auto it = _adcSensorInstances.find(pin);
    if (it == _adcSensorInstances.end()) {
        // 惰性初始化：配置 ADC 引脚
        analogReadResolution(12);  // 12-bit
        analogSetAttenuation(ADC_11db);  // 0~3.3V 范围

        ADCSensorInstance inst;
        inst.initialized = true;
        inst.type = SensorDriverType::CURRENT;
        inst.cal = cal;
        inst.lastRead = {false, NAN, 0};
        _adcSensorInstances[pin] = inst;
        it = _adcSensorInstances.find(pin);

        LOGGER.infof("[SensorDriver] Current sensor initialized pin=%d sensitivity=%.3f offset=%.3f",
                     pin, cal.sensitivity, cal.offset);
    }

    ADCSensorInstance& inst = it->second;
    unsigned long now = millis();

    // 缓存有效性检查
    if (inst.lastRead.success && (now - inst.lastRead.timestamp) < ADC_SENSOR_MIN_INTERVAL) {
        return inst.lastRead.value;
    }

    // 多次采样平均（减少噪声）
    const int SAMPLES = 10;
    long adcSum = 0;
    for (int i = 0; i < SAMPLES; i++) {
        adcSum += analogRead(pin);
        delayMicroseconds(100);
    }
    float adcAvg = (float)adcSum / SAMPLES;

    // ADC 值转电压
    float voltage = adcAvg * inst.cal.vRef / inst.cal.adcMax;

    // 电压转电流： I = (V - Voffset) / sensitivity
    float current = 0.0f;
    if (inst.cal.sensitivity > 0.001f) {  // 避免除零
        current = (voltage - inst.cal.offset) / inst.cal.sensitivity;
    }

    // 更新缓存
    inst.lastRead.success = true;
    inst.lastRead.value = current;
    inst.lastRead.timestamp = now;

    LOGGER.debugf("[SensorDriver] Current pin=%d adc=%.0f V=%.3f I=%.3fA", pin, adcAvg, voltage, current);
    return current;
}

// ========== 电压传感器 (分压器 ADC) ==========

float SensorDriver::readVoltage(uint8_t pin, const ADCSensorCalibration& cal) {
    if (!sensor_pin_valid(pin)) {
        static unsigned long s_lastWarn = 0;
        unsigned long now = millis();
        if (now - s_lastWarn > 60000) {
            LOGGER.errorf("[SensorDriver] Voltage sensor invalid pin %u", (unsigned)pin);
            s_lastWarn = now;
        }
        return NAN;
    }

    // 查找或创建实例
    auto it = _adcSensorInstances.find(pin);
    if (it == _adcSensorInstances.end()) {
        analogReadResolution(12);
        analogSetAttenuation(ADC_11db);

        ADCSensorInstance inst;
        inst.initialized = true;
        inst.type = SensorDriverType::VOLTAGE;
        inst.cal = cal;
        inst.lastRead = {false, NAN, 0};
        _adcSensorInstances[pin] = inst;
        it = _adcSensorInstances.find(pin);

        LOGGER.infof("[SensorDriver] Voltage sensor initialized pin=%d ratio=%.2f",
                     pin, cal.ratio);
    }

    ADCSensorInstance& inst = it->second;
    unsigned long now = millis();

    // 缓存有效性检查
    if (inst.lastRead.success && (now - inst.lastRead.timestamp) < ADC_SENSOR_MIN_INTERVAL) {
        return inst.lastRead.value;
    }

    // 多次采样平均
    const int SAMPLES = 10;
    long adcSum = 0;
    for (int i = 0; i < SAMPLES; i++) {
        adcSum += analogRead(pin);
        delayMicroseconds(100);
    }
    float adcAvg = (float)adcSum / SAMPLES;

    // ADC 值转电压（ADC 测量的是分压后的电压）
    float measuredVoltage = adcAvg * inst.cal.vRef / inst.cal.adcMax;

    // 实际电压 = 测量电压 * 分压比
    float actualVoltage = measuredVoltage * inst.cal.ratio;

    // 更新缓存
    inst.lastRead.success = true;
    inst.lastRead.value = actualVoltage;
    inst.lastRead.timestamp = now;

    LOGGER.debugf("[SensorDriver] Voltage pin=%d adc=%.0f measV=%.3f actualV=%.2fV",
                 pin, adcAvg, measuredVoltage, actualVoltage);
    return actualVoltage;
}

// ========== 资源释放 ==========

void SensorDriver::release(uint8_t pin) {
    // 释放 DHT
    auto dhtIt = _dhtInstances.find(pin);
    if (dhtIt != _dhtInstances.end()) {
        delete static_cast<DHT*>(dhtIt->second.dht);
        _dhtInstances.erase(dhtIt);
    }
    // 释放 DS18B20
#if defined(FASTBEE_ENABLE_DS18B20) && FASTBEE_ENABLE_DS18B20
    auto dsIt = _ds18b20Instances.find(pin);
    if (dsIt != _ds18b20Instances.end()) {
        delete static_cast<DallasTemperature*>(dsIt->second.sensors);
        delete static_cast<OneWire*>(dsIt->second.oneWire);
        _ds18b20Instances.erase(dsIt);
    }
#endif
    // 释放超声波
    _ultrasonicInstances.erase(pin);
    // 释放 ADC 传感器
    _adcSensorInstances.erase(pin);
}

void SensorDriver::releaseAll() {
    for (auto& pair : _dhtInstances) {
        delete static_cast<DHT*>(pair.second.dht);
    }
    _dhtInstances.clear();

    for (auto& pair : _ds18b20Instances) {
#if defined(FASTBEE_ENABLE_DS18B20) && FASTBEE_ENABLE_DS18B20
        delete static_cast<DallasTemperature*>(pair.second.sensors);
        delete static_cast<OneWire*>(pair.second.oneWire);
#endif
    }
    _ds18b20Instances.clear();

    _ultrasonicInstances.clear();
    _adcSensorInstances.clear();
}

#endif // FASTBEE_ENABLE_SENSOR_DRIVER
