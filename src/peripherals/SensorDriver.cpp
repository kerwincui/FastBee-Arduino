#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_SENSOR_DRIVER

#include "peripherals/SensorDriver.h"
#include "core/RuntimeFeatureManager.h"
#include "systems/LoggerSystem.h"
#include <DHT.h>
#if defined(FASTBEE_ENABLE_DS18B20) && FASTBEE_ENABLE_DS18B20
#include <OneWire.h>
#include <DallasTemperature.h>
#endif
#include <soc/gpio_periph.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <atomic>
#include <math.h>
#if defined(ARDUINO_ARCH_ESP32)
#include <esp32-hal-rmt.h>
#include <driver/gpio.h>
#endif

// 引脚有效性预检：避免对无效 pin 反复 new OneWire/DHT 导致内存泄漏
static inline bool sensor_pin_valid(uint8_t pin) {
    if (pin == 255) return false;
    if (pin > GPIO_NUM_MAX - 1) return false;
    return GPIO_IS_VALID_GPIO(pin);
}

static bool adc_calibration_equal(const ADCSensorCalibration& a,
                                  const ADCSensorCalibration& b) {
    return a.vRef == b.vRef && a.sensitivity == b.sensitivity &&
           a.offset == b.offset && a.ratio == b.ratio &&
           a.adcMax == b.adcMax;
}

static bool current_loop_calibration_equal(const CurrentLoopCalibration& a,
                                           const CurrentLoopCalibration& b) {
    return a.shuntResistance == b.shuntResistance &&
           a.inputGain == b.inputGain && a.inputOffset == b.inputOffset &&
           a.loopMinMa == b.loopMinMa && a.loopMaxMa == b.loopMaxMa &&
           a.engineeringMin == b.engineeringMin &&
           a.engineeringMax == b.engineeringMax && a.vRef == b.vRef &&
           a.adcMax == b.adcMax && a.sampleCount == b.sampleCount &&
           a.mapToEngineering == b.mapToEngineering;
}

static bool finite_positive(float value) {
    return !isnan(value) && !isinf(value) && value > 0.0f;
}

// ============ 单总线传感器并发互斥 ============
// 2026-08-10 崩溃根因修复：DHT/DS18B20 均为单总线时序敏感协议，DHT 库的
// 时序临界段（InterruptLock）会关全 CPU 中断。两个 PeriphExec worker 任务
// 同时读取同一传感器时：各自的起始信号互相破坏总线时序，expectPulse 一直
// 等到 _maxcycles 超时（单次约数十 ms，80+ 次调用累计数秒关中断），超过
// 中断看门狗阈值后触发 "Interrupt wdt timeout" panic 整机复位
// （现场：两条定时规则同秒触发并发读 dht11 → Guru Meditation）。
// 用全局总线互斥锁串行化所有物理读取；持锁期间读缓存/读硬件，第二个任务
// 拿锁后双检缓存通常直接命中。拿锁超时则不做强读（避免回到危险路径），
// 直接返回 NAN；不能在未持锁时访问实例 map。
static constexpr uint32_t SENSOR_BUS_LOCK_TIMEOUT_MS = 3000;
static constexpr uint32_t DHT_STALE_CACHE_WINDOW_MS = 10000;
static constexpr uint32_t SENSOR_FAILURE_BACKOFF_INITIAL_MS = 5000;
static constexpr uint32_t SENSOR_FAILURE_BACKOFF_MAX_MS = 60000;
#if defined(ARDUINO_ARCH_ESP32)
// Keep interrupts enabled and bound each edge and the complete transaction
// so a disconnected/stuck DHT line cannot
// enter the Adafruit library's long InterruptLock polling loop.
static constexpr uint32_t DHT_EDGE_TIMEOUT_US = 1500;
static constexpr uint32_t DHT_START_EDGE_TIMEOUT_US = 3000;
static constexpr uint32_t DHT_TRANSACTION_TIMEOUT_US = 150000;
#endif

// The mutex is created lazily from the SensorDriver singleton constructor. The
// atomic CAS keeps the fallback safe even if a read arrives before that
// constructor has completed (for example during an unusual startup race).
static SemaphoreHandle_t sensor_bus_mutex() {
    static std::atomic<SemaphoreHandle_t> busMutex(nullptr);
    SemaphoreHandle_t mutex = busMutex.load(std::memory_order_acquire);
    if (mutex) {
        return mutex;
    }

    SemaphoreHandle_t created = xSemaphoreCreateMutex();
    if (!created) {
        return nullptr;
    }

    SemaphoreHandle_t expected = nullptr;
    if (busMutex.compare_exchange_strong(expected, created,
                                         std::memory_order_release,
                                         std::memory_order_acquire)) {
        return created;
    }

    // Another task won the race. Do not leak the unused candidate semaphore.
    vSemaphoreDelete(created);
    return expected;
}

// RAII 总线锁：acquired==false 时调用方必须放弃物理读取
struct SensorBusLock {
    SemaphoreHandle_t mutex;
    bool acquired;
    SensorBusLock() : mutex(sensor_bus_mutex()), acquired(false) {
        if (mutex && xSemaphoreTake(mutex, pdMS_TO_TICKS(SENSOR_BUS_LOCK_TIMEOUT_MS)) == pdTRUE) {
            acquired = true;
        }
    }
    ~SensorBusLock() {
        if (acquired && mutex) {
            xSemaphoreGive(mutex);
        }
    }
};

static inline bool retry_pending(unsigned long now, uint32_t retryAt) {
    // Signed subtraction is wrap-safe for the 32-bit millis() clock as long as
    // no individual backoff exceeds INT32_MAX (ours is at most one minute).
    return retryAt != 0 &&
           static_cast<int32_t>(static_cast<uint32_t>(now) - retryAt) < 0;
}

static uint32_t failure_backoff_ms(uint8_t consecutiveFailures) {
    if (consecutiveFailures == 0) {
        return SENSOR_FAILURE_BACKOFF_INITIAL_MS;
    }

    const uint8_t shift = (consecutiveFailures > 4) ? 4 :
                          static_cast<uint8_t>(consecutiveFailures - 1);
    uint32_t delayMs = SENSOR_FAILURE_BACKOFF_INITIAL_MS << shift;
    if (delayMs > SENSOR_FAILURE_BACKOFF_MAX_MS) {
        delayMs = SENSOR_FAILURE_BACKOFF_MAX_MS;
    }
    return delayMs;
}

static inline float dht_cached_field(const SensorReadResult& reading,
                                     const String& field) {
    return (field == "humidity") ? reading.humidity : reading.temperature;
}

#if defined(ARDUINO_ARCH_ESP32)
// Allocation-free failure evidence. Populate only after pulse capture stops;
// logging or extra GPIO reads inside the timing loops would perturb the frame.
struct DhtReadDiagnostic {
    const char* phase = "ok";
    int bit = -1;
    unsigned long lowUs = 0;
    unsigned long highUs = 0;
    uint32_t elapsedUs = 0;
    int finalLevel = -1;
};
#if SOC_RMT_SUPPORTED
#include "DhtRmtCapture.h"
#endif

// Read one DHT frame without the Adafruit DHT library's InterruptLock. The
// protocol is simple pulse-width coding: every bit starts with a ~50us low
// pulse, followed by a short (0) or long (1) high pulse. Consume the current
// pulse: Arduino pulseIn() first discards an already active pulse, which skips
// the response preamble and subsequent bits when used for this continuous frame.
static bool readDhtBounded(uint8_t pin, SensorDriverType type,
                           float& humidity, float& temperature,
                           DhtReadDiagnostic* diagnostic = nullptr) {
    if (diagnostic != nullptr) *diagnostic = DhtReadDiagnostic{};
    const uint32_t transactionStartedUs = micros();
    auto fail = [&](const char* phase, int bit = -1,
                    unsigned long lowUs = 0, unsigned long highUs = 0) -> bool {
        pinMode(pin, INPUT_PULLUP);
        if (diagnostic != nullptr) {
            diagnostic->phase = phase;
            diagnostic->bit = bit;
            diagnostic->lowUs = lowUs;
            diagnostic->highUs = highUs;
            diagnostic->elapsedUs = static_cast<uint32_t>(micros() - transactionStartedUs);
            diagnostic->finalLevel = digitalRead(pin);
        }
        return false;
    };
#if SOC_RMT_SUPPORTED
    uint8_t data[5] = {0, 0, 0, 0, 0};
    if (!readDhtRmtFrame(pin, type == SensorDriverType::DHT11, data, diagnostic)) return false;
#else
    // A held-low line is a common short/disconnected failure and can be
    // rejected before driving the start pulse.
    pinMode(pin, INPUT_PULLUP);
    if (digitalRead(pin) != HIGH) {
        return fail("idle_low");
    }

    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
    if (type == SensorDriverType::DHT11) {
        delay(20);       // DHT11 requires at least 18ms
    } else {
        delay(2);        // DHT22/AM2302 requires at least 1ms
    }
    pinMode(pin, INPUT_PULLUP);
    delayMicroseconds(40);

    const uint32_t startUs = micros();
    auto boundedPulse = [&](uint8_t state, uint32_t edgeTimeoutUs) -> unsigned long {
        const uint32_t elapsedUs = static_cast<uint32_t>(micros() - startUs);
        if (elapsedUs >= DHT_TRANSACTION_TIMEOUT_US) {
            return 0;
        }
        const uint32_t remainingUs = DHT_TRANSACTION_TIMEOUT_US - elapsedUs;
        if (edgeTimeoutUs > remainingUs) {
            edgeTimeoutUs = remainingUs;
        }
        if (edgeTimeoutUs == 0) {
            return 0;
        }
        const uint32_t waitStartUs = micros();
        while (digitalRead(pin) != state) {
            if (static_cast<uint32_t>(micros() - waitStartUs) >= edgeTimeoutUs) {
                return 0;
            }
        }
        const uint32_t pulseStartUs = micros();
        while (digitalRead(pin) == state) {
            if (static_cast<uint32_t>(micros() - waitStartUs) >= edgeTimeoutUs) {
                return 0;
            }
        }
        return static_cast<uint32_t>(micros() - pulseStartUs);
    };

    // Sensor response preamble: low ~80us, high ~80us.
    const unsigned long responseLowUs = boundedPulse(LOW, DHT_START_EDGE_TIMEOUT_US);
    if (responseLowUs == 0) return fail("response_low");
    const unsigned long responseHighUs = boundedPulse(HIGH, DHT_START_EDGE_TIMEOUT_US);
    if (responseHighUs == 0) return fail("response_high", -1, responseLowUs);

    uint8_t data[5] = {0, 0, 0, 0, 0};
    for (uint8_t bit = 0; bit < 40; ++bit) {
        const unsigned long lowUs = boundedPulse(LOW, DHT_EDGE_TIMEOUT_US);
        const unsigned long highUs = boundedPulse(HIGH, DHT_EDGE_TIMEOUT_US);
        // Reject missing/implausible pulses instead of decoding line noise as
        // a valid frame. A healthy DHT low/high pulse is well below 150us.
        if (lowUs == 0 || highUs == 0 || lowUs > 500 || highUs > 500) {
            return fail("bit_pulse", bit, lowUs, highUs);
        }
        data[bit / 8] <<= 1;
        if (highUs > lowUs) {
            data[bit / 8] |= 1;
        }
    }

#endif
    pinMode(pin, INPUT_PULLUP);
    if (data[4] != static_cast<uint8_t>((data[0] + data[1] + data[2] + data[3]) & 0xFF)) {
        return fail("checksum");
    }

    if (type == SensorDriverType::DHT11) {
        humidity = static_cast<float>(data[0]) +
                   static_cast<float>(data[1]) * 0.1f;
        // DHT11 stores the sign in bit 7 of the temperature byte; remove it
        // before composing the magnitude or a negative value is overstated.
        const float magnitude = static_cast<float>(data[2] & 0x7F) +
                                static_cast<float>(data[3]) * 0.1f;
        temperature = (data[2] & 0x80) ? -magnitude : magnitude;
    } else {
        const uint16_t rawHumidity =
            (static_cast<uint16_t>(data[0]) << 8) | data[1];
        const uint16_t rawTemperature =
            (static_cast<uint16_t>(data[2] & 0x7F) << 8) | data[3];
        humidity = static_cast<float>(rawHumidity) * 0.1f;
        temperature = static_cast<float>(rawTemperature) * 0.1f;
        if (data[2] & 0x80) {
            temperature = -temperature;
        }
    }

    // Basic frame sanity checks protect the cache and downstream actions from
    // electrically plausible but invalid noise frames.
    if (isnan(humidity) || isinf(humidity) || isnan(temperature) || isinf(temperature) ||
        humidity < 0.0f || humidity > 100.0f || temperature < -40.0f || temperature > 125.0f) {
        return fail("range");
    }
    return true;
}
#endif

SensorDriver::SensorDriver() {
    // Force one-time mutex creation during singleton startup rather than on a
    // worker's first physical read. The CAS in sensor_bus_mutex() remains a
    // defensive guard for unusual early-call races.
    (void)sensor_bus_mutex();
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

    // 总线互斥：实例 map 访问、缓存检查与物理读取全部在锁内，
    // 防止并发读破坏单总线时序触发中断看门狗复位（见文件头注释）
    SensorBusLock busLock;
    if (!busLock.acquired) {
        LOGGER.warningf("[SensorDriver] DHT bus lock timeout on pin %d, skip physical read", pin);
        // The instance map is owned by the bus lock. Returning NAN here keeps
        // the timeout path from racing a concurrent map update/initialization.
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

    // A failed DHT transaction can spend tens of milliseconds with interrupts
    // disabled. Do not immediately start another physical transaction from a
    // second rule/field; serve a still-fresh stale value when available.
    if (!inst.lastRead.success && retry_pending(now, inst.nextRetryAt)) {
        if (inst.lastRead.timestamp != 0 &&
            (now - inst.lastRead.timestamp) < DHT_STALE_CACHE_WINDOW_MS) {
            return dht_cached_field(inst.lastRead, field);
        }
        return NAN;
    }

    // 缓存有效性检查：避免过于频繁读取
    if (inst.lastRead.success && (now - inst.lastRead.timestamp) < DHT_MIN_INTERVAL) {
        // 使用缓存值
        return dht_cached_field(inst.lastRead, field);
    }

    // 执行实际读取。ESP32 使用有界连续脉冲采集，避免 Adafruit DHT 库
    // 在异常线路上进入长时间 InterruptLock；其他 Arduino 平台保留原库路径。
    float humidity = NAN;
    float temperature = NAN;
#if defined(ARDUINO_ARCH_ESP32)
    (void)dht; // The object still owns begin()/lifetime compatibility.
    DhtReadDiagnostic diagnostic;
    const bool boundedReadOk = readDhtBounded(pin, type, humidity, temperature, &diagnostic);
    if (!boundedReadOk) {
        humidity = NAN;
        temperature = NAN;
    }
#else
    humidity = dht->readHumidity();
    // If the first transaction failed, avoid repeating a timing-critical read
    // on libraries/configurations that do not cache a failed result.
    temperature = isnan(humidity) ? NAN : dht->readTemperature();
#endif

    if (isnan(humidity) || isnan(temperature)) {
#if defined(ARDUINO_ARCH_ESP32)
        LOGGER.warningf("[SensorDriver] DHT read failed on pin %d phase=%s bit=%d lowUs=%lu highUs=%lu elapsedUs=%lu line=%d",
                        pin, diagnostic.phase, diagnostic.bit, diagnostic.lowUs, diagnostic.highUs,
                        static_cast<unsigned long>(diagnostic.elapsedUs), diagnostic.finalLevel);
#else
        LOGGER.warningf("[SensorDriver] DHT read failed on pin %d", pin);
#endif
        const bool hasStaleValue =
            inst.lastRead.success &&
            (now - inst.lastRead.timestamp) < DHT_STALE_CACHE_WINDOW_MS;
        const float staleValue = dht_cached_field(inst.lastRead, field);

        if (inst.consecutiveFailures < 255) {
            ++inst.consecutiveFailures;
        }
        inst.lastRead.success = false;
        inst.nextRetryAt = static_cast<uint32_t>(now) +
                           failure_backoff_ms(inst.consecutiveFailures);

        // Keep the last successful value/timestamp for the bounded stale
        // fallback, while marking the transaction itself as failed.
        return hasStaleValue ? staleValue : NAN;
    }

    // 更新缓存
    inst.lastRead.success = true;
    inst.lastRead.temperature = temperature;
    inst.lastRead.humidity = humidity;
    inst.lastRead.timestamp = now;
    inst.consecutiveFailures = 0;
    inst.nextRetryAt = 0;

    LOGGER.debugf("[SensorDriver] DHT pin=%d temp=%.1f humi=%.1f", pin, temperature, humidity);

    if (field == "humidity") return humidity;
    return temperature;
}

float SensorDriver::readDS18B20(uint8_t pin, uint8_t index) {
#if defined(FASTBEE_ENABLE_DS18B20) && FASTBEE_ENABLE_DS18B20
    // 运行时功能门控：用户可在 Web UI 禁用 ds18b20
    if (!FastBee::RuntimeFeatureManager::instance().shouldEnable("ds18b20")) {
        return NAN;
    }

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
    // 总线互斥：OneWire 位读写同样关中断，并发访问会破坏时序（同 DHT 修复）
    SensorBusLock busLock;
    if (!busLock.acquired) {
        LOGGER.warningf("[SensorDriver] DS18B20 bus lock timeout on pin %d, skip physical read", pin);
        // Do not inspect _ds18b20Instances without the bus lock; map access
        // races with lazy initialization and can corrupt the pooled node.
        return NAN;
    }
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

    if (!inst.lastRead.success && retry_pending(now, inst.nextRetryAt)) {
        if (inst.lastRead.timestamp != 0 &&
            (now - inst.lastRead.timestamp) < DHT_STALE_CACHE_WINDOW_MS) {
            return inst.lastRead.temperature;
        }
        return NAN;
    }

    // 缓存有效性检查
    if (inst.lastRead.success && (now - inst.lastRead.timestamp) < DS18B20_MIN_INTERVAL) {
        return inst.lastRead.temperature;
    }

    // 发起温度转换并读取
    sensors->requestTemperatures();
    float temperature = sensors->getTempCByIndex(index);

    if (temperature == DEVICE_DISCONNECTED_C || temperature == -127.0f) {
        LOGGER.warningf("[SensorDriver] DS18B20 read failed on pin %d index %d", pin, index);
        const bool hasStaleValue =
            inst.lastRead.success &&
            (now - inst.lastRead.timestamp) < DHT_STALE_CACHE_WINDOW_MS;
        const float staleValue = inst.lastRead.temperature;
        if (inst.consecutiveFailures < 255) {
            ++inst.consecutiveFailures;
        }
        inst.lastRead.success = false;
        inst.nextRetryAt = static_cast<uint32_t>(now) +
                           failure_backoff_ms(inst.consecutiveFailures);
        return hasStaleValue ? staleValue : NAN;
    }

    // 更新缓存
    inst.lastRead.success = true;
    inst.lastRead.temperature = temperature;
    inst.lastRead.timestamp = now;
    inst.consecutiveFailures = 0;
    inst.nextRetryAt = 0;

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

    // 总线互斥：pulseIn() 内部关中断忙等 ~30ms，并发读取会互相干扰时序
    // 产生错误距离值（同 DHT/DS18B20 单总线时序保护，见文件头注释）
    SensorBusLock busLock;
    if (!busLock.acquired) {
        LOGGER.warningf("[SensorDriver] Ultrasonic bus lock timeout on trig=%d, skip physical read", trigPin);
        // The instance map is protected by the bus lock; do not read it from
        // this timeout path while another task may be initializing an entry.
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
    // A pin can be re-used after a rule/configuration change. Refresh the
    // calibration and invalidate the cached value instead of serving a value
    // calculated with the previous sensor type.
    if (inst.type != SensorDriverType::CURRENT || !adc_calibration_equal(inst.cal, cal)) {
        inst.type = SensorDriverType::CURRENT;
        inst.cal = cal;
        inst.lastRead = {false, NAN, 0};
    }
    if (!finite_positive(inst.cal.vRef) || inst.cal.adcMax == 0 ||
        !finite_positive(inst.cal.sensitivity)) {
        LOGGER.warningf("[SensorDriver] Current calibration invalid on pin %d", pin);
        inst.lastRead = {false, NAN, millis()};
        return NAN;
    }
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
    current = (voltage - inst.cal.offset) / inst.cal.sensitivity;

    // 更新缓存
    inst.lastRead.success = true;
    inst.lastRead.value = current;
    inst.lastRead.timestamp = now;

    LOGGER.debugf("[SensorDriver] Current pin=%d adc=%.0f V=%.3f I=%.3fA", pin, adcAvg, voltage, current);
    return current;
}

// ========== 4-20 mA current loop (shunt resistor) ==========

float SensorDriver::readCurrentLoop(uint8_t pin, const CurrentLoopCalibration& cal) {
    if (!sensor_pin_valid(pin)) {
        static unsigned long s_lastWarn = 0;
        const unsigned long now = millis();
        if (now - s_lastWarn > 60000UL) {
            LOGGER.errorf("[SensorDriver] 4-20mA invalid pin %u", (unsigned)pin);
            s_lastWarn = now;
        }
        return NAN;
    }

    // Reject malformed configuration before touching the ADC. This avoids
    // divide-by-zero and keeps a bad remote rule from poisoning the cache.
    if (!finite_positive(cal.shuntResistance) || !finite_positive(cal.inputGain) ||
        !finite_positive(cal.vRef) || cal.adcMax == 0 ||
        !finite_positive(cal.loopMaxMa - cal.loopMinMa) ||
        isnan(cal.engineeringMin) || isinf(cal.engineeringMin) ||
        isnan(cal.engineeringMax) || isinf(cal.engineeringMax) ||
        cal.engineeringMin == cal.engineeringMax) {
        LOGGER.warningf("[SensorDriver] 4-20mA calibration invalid on pin %d", pin);
        return NAN;
    }

    auto it = _adcSensorInstances.find(pin);
    if (it == _adcSensorInstances.end()) {
        analogReadResolution(12);
        analogSetAttenuation(ADC_11db);

        ADCSensorInstance inst;
        inst.initialized = true;
        inst.type = SensorDriverType::CURRENT_LOOP;
        inst.cal = ADCSensorCalibration();
        inst.loopCal = cal;
        inst.lastRead = {false, NAN, 0};
        _adcSensorInstances[pin] = inst;
        it = _adcSensorInstances.find(pin);
        LOGGER.infof("[SensorDriver] 4-20mA initialized pin=%d shunt=%.2f ohm range=%.2f..%.2f mA",
                     pin, cal.shuntResistance, cal.loopMinMa, cal.loopMaxMa);
    }

    ADCSensorInstance& inst = it->second;
    if (inst.type != SensorDriverType::CURRENT_LOOP ||
        !current_loop_calibration_equal(inst.loopCal, cal)) {
        inst.type = SensorDriverType::CURRENT_LOOP;
        inst.loopCal = cal;
        inst.lastRead = {false, NAN, 0};
    }

    const unsigned long now = millis();
    if (inst.lastRead.success &&
        (now - inst.lastRead.timestamp) < ADC_SENSOR_MIN_INTERVAL) {
        return inst.lastRead.value;
    }

    uint8_t sampleCount = cal.sampleCount;
    if (sampleCount == 0) sampleCount = 1;
    if (sampleCount > 32) sampleCount = 32;
    uint32_t adcSum = 0;
    for (uint8_t i = 0; i < sampleCount; ++i) {
        adcSum += static_cast<uint32_t>(analogRead(pin));
        delayMicroseconds(100);
    }

    const float adcAvg = static_cast<float>(adcSum) / static_cast<float>(sampleCount);
    const float measuredVoltage = adcAvg * cal.vRef / static_cast<float>(cal.adcMax);
    const float shuntVoltage = (measuredVoltage - cal.inputOffset) / cal.inputGain;
    const float currentMa = shuntVoltage * 1000.0f / cal.shuntResistance;
    const float toleranceMa = 1.0f;

    if (isnan(currentMa) || isinf(currentMa) ||
        currentMa < (cal.loopMinMa - toleranceMa) ||
        currentMa > (cal.loopMaxMa + toleranceMa)) {
        inst.lastRead = {false, NAN, now};
        static unsigned long s_lastRangeWarn = 0;
        if (now - s_lastRangeWarn > 30000UL) {
            LOGGER.warningf("[SensorDriver] 4-20mA pin=%d out of range: %.3f mA (expected %.3f..%.3f)",
                            pin, currentMa, cal.loopMinMa, cal.loopMaxMa);
            s_lastRangeWarn = now;
        }
        return NAN;
    }

    float value = currentMa;
    if (cal.mapToEngineering) {
        const float clampedMa = constrain(currentMa, cal.loopMinMa, cal.loopMaxMa);
        value = cal.engineeringMin +
                (clampedMa - cal.loopMinMa) *
                (cal.engineeringMax - cal.engineeringMin) /
                (cal.loopMaxMa - cal.loopMinMa);
    }

    inst.lastRead = {true, value, now};
    LOGGER.debugf("[SensorDriver] 4-20mA pin=%d adc=%.0f V=%.3f I=%.3f mA value=%.3f",
                  pin, adcAvg, measuredVoltage, currentMa, value);
    return value;
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

// ========== NTC 热敏电阻 (分压器 + B 参数法) ==========

float SensorDriver::readNTC(uint8_t pin, const NTCSensorCalibration& cal) {
    if (!sensor_pin_valid(pin)) {
        static unsigned long s_lastWarn = 0;
        unsigned long now = millis();
        if (now - s_lastWarn > 60000) {
            LOGGER.errorf("[SensorDriver] NTC sensor invalid pin %u", (unsigned)pin);
            s_lastWarn = now;
        }
        return NAN;
    }

    // 查找或创建实例
    auto it = _ntcInstances.find(pin);
    if (it == _ntcInstances.end()) {
        analogReadResolution(12);
        analogSetAttenuation(ADC_11db);

        NTCSensorInstance inst;
        inst.initialized = true;
        inst.cal = cal;
        inst.lastRead = {false, NAN, 0};
        _ntcInstances[pin] = inst;
        it = _ntcInstances.find(pin);

        LOGGER.infof("[SensorDriver] NTC initialized pin=%d series=%.0f nominal=%.0f B=%.0f",
                     pin, cal.seriesResistor, cal.nominalResistance, cal.bCoefficient);
    }

    NTCSensorInstance& inst = it->second;
    unsigned long now = millis();

    // 缓存有效性检查
    if (inst.lastRead.success && (now - inst.lastRead.timestamp) < ADC_SENSOR_MIN_INTERVAL) {
        return inst.lastRead.value;
    }

    // 多次采样中值滤波（NTC 对脉冲噪声敏感，中值更稳健）
    const int SAMPLES = 10;
    int samples[SAMPLES];
    for (int i = 0; i < SAMPLES; i++) {
        samples[i] = analogRead(pin);
        delayMicroseconds(100);
    }
    // 简单插入排序后取中值
    for (int i = 1; i < SAMPLES; i++) {
        int key = samples[i];
        int j = i - 1;
        while (j >= 0 && samples[j] > key) {
            samples[j + 1] = samples[j];
            j--;
        }
        samples[j + 1] = key;
    }
    float adcVal = (float)samples[SAMPLES / 2];

    // 防除零/极端值
    if (adcVal <= 1.0f || adcVal >= (float)inst.cal.adcMax - 1.0f) {
        LOGGER.warningf("[SensorDriver] NTC pin=%d adc=%.0f out of range", pin, adcVal);
        return NAN;
    }

    // 电路：VCC --[R_series]-- ADC --[NTC]-- GND
    // NTC 电压 = adc * vRef / adcMax；NTC 电阻 = series * Vntc / (vRef - Vntc)
    float vNtc = adcVal * inst.cal.vRef / inst.cal.adcMax;
    float rNtc = inst.cal.seriesResistor * vNtc / (inst.cal.vRef - vNtc);
    if (rNtc <= 0.0f) {
        return NAN;
    }

    // B 参数法: 1/T = 1/T0 + ln(R/R0)/B
    float tempK = 1.0f / (
        (1.0f / (inst.cal.nominalTempC + 273.15f)) +
        (logf(rNtc / inst.cal.nominalResistance) / inst.cal.bCoefficient));
    float tempC = tempK - 273.15f;

    // 更新缓存
    inst.lastRead.success = true;
    inst.lastRead.value = tempC;
    inst.lastRead.timestamp = now;

    LOGGER.debugf("[SensorDriver] NTC pin=%d adc=%.0f R=%.0f T=%.2fC", pin, adcVal, rNtc, tempC);
    return tempC;
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
    // 释放 NTC 热敏电阻
    _ntcInstances.erase(pin);
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
    _ntcInstances.clear();
}

#endif // FASTBEE_ENABLE_SENSOR_DRIVER
