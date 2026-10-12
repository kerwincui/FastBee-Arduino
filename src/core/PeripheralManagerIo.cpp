/**
 * @description Peripheral data I/O, GPIO interrupt, and encoder implementation
 * @author kerwincui
 * @copyright FastBee All rights reserved.
 */

#include "core/PeripheralManager.h"
#include "core/AsyncExecTypes.h"
#include "core/ChipConfig.h"
#include "core/I2CBusManager.h"
#include "core/SpiBusInitializer.h"
#include "peripherals/BoundedStreamRead.h"
#include "systems/LoggerSystem.h"
#include <driver/gpio.h>
#include <Wire.h>
#include <SPI.h>
#include <cstring>

namespace {
constexpr uint32_t ENCODER_MIN_STALE_TIMEOUT_US = 1000000UL;

uint32_t saturatingAddUint32(uint32_t current, size_t amount) {
    const uint32_t remaining = UINT32_MAX - current;
    return amount > remaining ? UINT32_MAX : current + static_cast<uint32_t>(amount);
}

uint16_t boundedUartAvailable(HardwareSerial* serial) {
    if (!serial) return 0;
    const int available = serial->available();
    if (available <= 0) return 0;
    return available > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(available);
}
}  // namespace

// ========== 通用数据读写接口 ==========

bool PeripheralManager::writeData(const String& id, const uint8_t* data, size_t len) {
    if (!data || !len) return false;
    RecursiveMutexGuard lock(_mutex);
    if (!lock.isLocked()) return false;
    auto config = getPeripheral(id);
    if (!config || !config->enabled) {
        LOG_WARNINGF("writeData: Peripheral '%s' not found or disabled", id.c_str());
        return false;
    }

    bool success = false;

    switch (config->type) {
        // GPIO 数字输出
        case PeripheralType::GPIO_DIGITAL_OUTPUT:
            if (len >= 1) {
                GPIOState state = (data[0] != 0) ? GPIOState::STATE_HIGH : GPIOState::STATE_LOW;
                success = writePin(id, state);
            }
            break;

        // PWM / 模拟输出
        case PeripheralType::GPIO_PWM_OUTPUT:
        case PeripheralType::GPIO_ANALOG_OUTPUT:
            if (len >= 2) {
                uint32_t dutyCycle = data[0] | (data[1] << 8);
                if (len >= 4) {
                    dutyCycle |= (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
                }
                success = writePWM(id, dutyCycle);
            } else if (len == 1) {
                // 单字节作为 0-255 占空比
                uint32_t maxVal = (1U << config->params.gpio.pwmResolution) - 1;
                uint32_t dutyCycle = (data[0] * maxVal) / 255;
                success = writePWM(id, dutyCycle);
            }
            break;

        // 舵机统一使用角度语义；单字节和小端双字节都表示角度。
        case PeripheralType::PWM_SERVO:
            if (len >= 2) {
                const int16_t angle = static_cast<int16_t>(
                    static_cast<uint16_t>(data[0]) |
                    (static_cast<uint16_t>(data[1]) << 8));
                success = writeServoAngle(id, angle);
            } else if (len == 1) {
                success = writeServoAngle(id, data[0]);
            }
            break;

        // DAC 输出
        case PeripheralType::DAC:
            // The raw data API cannot supply trusted source/operation identity.
            // Setup still uses checked writeManagedDac; ordinary commands must
            // use submitOrdinaryScalar and cannot silently adopt an idle target.
            success = false;
            break;

        // UART 发送
        case PeripheralType::UART: {
            auto state = runtimeStates.find(id);
            if (state == runtimeStates.end() || state->second.memorySuspended) {
                if (state != runtimeStates.end()) state->second.lastError = "UART_NOT_READY";
                break;
            }
            if (HardwareSerial* serial = getUartSerial(id)) {
                const size_t accepted = serial->write(data, len);
                success = accepted == len;
                state->second.lastError = success ? "" : "UART_WRITE_INCOMPLETE";
                // Count even a partially accepted write. This is a local driver
                // acknowledgement, not evidence of peer receipt or execution.
                const size_t bounded = accepted < len ? accepted : len;
                state->second.state.comm.bytesSent = saturatingAddUint32(state->second.state.comm.bytesSent, bounded);
                if (bounded) state->second.lastActivity = millis();
                state->second.state.comm.isConnected = true;
                state->second.state.comm.rxAvailable = boundedUartAvailable(serial);
            } else {
                state->second.lastError = "UART_NOT_OWNED";
                state->second.state.comm.isConnected = false;
            }
            break;
        }

        // I2C 写入
        case PeripheralType::I2C:
            if (config->params.i2c.isMaster && config->params.i2c.address > 0) {
                FastBee::I2CBusLock busLock(20);
                TwoWire* wire = busLock.locked() ? getManagedI2cWire(id) : nullptr;
                if (!wire) break;
                wire->beginTransmission(config->params.i2c.address);
                const size_t written = wire->write(data, len);
                success = (wire->endTransmission() == 0 && written == len);
            }
            break;

        // SPI 传输
        case PeripheralType::SPI: {
            if (!data || len == 0) break;
            if (config->pinCount < 4 || config->pins[3] == 255 ||
                !isValidPin(config->pins[3]) ||
                isReservedPin(config->pins[3]) ||
                isInputOnlyPin(config->pins[3])) {
                LOG_WARNINGF("writeData: SPI '%s' has no usable CS pin", id.c_str());
                break;
            }
            FastBee::SpiBusOperation operation(20);
            String spiError;
            if (!operation.locked()) spiError = "SPI_BUSY: write deferred";
            if (!operation.locked() || !FastBee::SpiBusInitializer::instance().checkNamed(
                    id, config->pins[2], config->pins[0], config->pins[1], config->pins[3], spiError)) {
                auto state = runtimeStates.find(id);
                if (state != runtimeStates.end()) state->second.lastError = spiError;
                break;
            }
            const uint8_t cs = config->pins[3];
            SPI.beginTransaction(SPISettings(config->params.spi.frequency ? config->params.spi.frequency : 1000000,
                config->params.spi.msbFirst ? MSBFIRST : LSBFIRST,
                config->params.spi.mode));
            digitalWrite(cs, LOW);
            for (size_t i = 0; i < len; i++) {
                SPI.transfer(data[i]);
            }
            digitalWrite(cs, HIGH);
            SPI.endTransaction();
            success = true;
            auto state = runtimeStates.find(id);
            if (state != runtimeStates.end()) state->second.lastError = "";
            break;
        }

        case PeripheralType::RF_MODULE:
            // Binary/printable legacy writeData has no authenticated actor,
            // epoch or operation. It cannot fabricate an RF output grant.
            success = false;
            break;

        default:
            LOG_WARNINGF("writeData: Unsupported peripheral type %d", static_cast<int>(config->type));
            break;
    }

    // 更新运行时状态
    if (success && runtimeStates.find(id) != runtimeStates.end()) {
        PeripheralRuntimeState& runtime = runtimeStates[id];
        runtime.lastActivity = millis();
        if (config->isCommunicationPeripheral()) {
            if (config->type != PeripheralType::UART) {
                runtime.state.comm.bytesSent = saturatingAddUint32(runtime.state.comm.bytesSent, len);
            }
            if (config->type == PeripheralType::UART) {
                HardwareSerial* serial = getUartSerial(id);
                runtime.state.comm.isConnected = serial != nullptr;
                runtime.state.comm.rxAvailable = boundedUartAvailable(serial);
            }
        }
    }

    return success;
}

bool PeripheralManager::readData(const String& id, uint8_t* buffer, size_t& len) {
    if (!buffer || !len) { len = 0; return false; }
    RecursiveMutexGuard lock(_mutex);
    if (!lock.isLocked()) { len = 0; return false; }
    auto config = getPeripheral(id);
    if (!config || !config->enabled) {
        LOG_WARNINGF("readData: Peripheral '%s' not found or disabled", id.c_str());
        len = 0;
        return false;
    }

    bool success = false;
    size_t maxLen = len;
    len = 0;

    switch (config->type) {
        // GPIO 数字输入
        case PeripheralType::GPIO_DIGITAL_INPUT:
        case PeripheralType::GPIO_DIGITAL_INPUT_PULLUP:
        case PeripheralType::GPIO_DIGITAL_INPUT_PULLDOWN:
        case PeripheralType::GPIO_INTERRUPT_RISING:
        case PeripheralType::GPIO_INTERRUPT_FALLING:
        case PeripheralType::GPIO_INTERRUPT_CHANGE:
            if (maxLen >= 1) {
                GPIOState state = readPin(id);
                if (state == GPIOState::STATE_UNDEFINED) break;
                buffer[0] = (state == GPIOState::STATE_HIGH) ? 1 : 0;
                len = 1;
                success = true;
            }
            break;

        // ADC / 模拟输入
        case PeripheralType::GPIO_ANALOG_INPUT:
        case PeripheralType::ADC:
            if (maxLen >= 2) {
                AnalogReadSample sample;
                if (!readAnalogSample(id, sample)) break;
                const uint16_t value = sample.raw;
                buffer[0] = value & 0xFF;
                buffer[1] = (value >> 8) & 0xFF;
                len = 2;
                success = true;
            }
            break;

        // UART 接收
        case PeripheralType::UART: {
            auto state = runtimeStates.find(id);
            if (state == runtimeStates.end() || state->second.memorySuspended) {
                if (state != runtimeStates.end()) state->second.lastError = "UART_NOT_READY";
                break;
            }
            if (HardwareSerial* serial = getUartSerial(id)) {
                const uint32_t generation = getUartReceiveGeneration(id);
                const auto receiveCurrent = [&] {
                    return generation && getUartReceiveGeneration(id) == generation;
                };
                success = FastBee::readBufferedStreamChecked<256>(*serial, buffer, maxLen, len, receiveCurrent);
                if (!receiveCurrent()) {
                    success = false; len = 0;
                    state->second.lastError = "UART_RX_FAULT_LATCHED: explicit UART restart required";
                    state->second.state.comm.isConnected = false;
                } else state->second.lastError = success ? "" : "UART_READ_FAILED";
            } else {
                state->second.lastError = "UART_NOT_OWNED";
                state->second.state.comm.isConnected = false;
            }
            break;
        }

        // I2C 读取
        case PeripheralType::I2C:
            if (config->params.i2c.isMaster && config->params.i2c.address > 0) {
                FastBee::I2CBusLock busLock(20);
                TwoWire* wire = busLock.locked() ? getManagedI2cWire(id) : nullptr;
                if (!wire) break;
                size_t requestLen = (maxLen < 32) ? maxLen : 32;  // I2C 一次最多读取 32 字节
                const size_t received = wire->requestFrom(config->params.i2c.address, (uint8_t)requestLen);
                if (received != requestLen) FastBee::discardBufferedStream(*wire);
                else success = FastBee::readExactBufferedStream<32>(*wire, buffer, requestLen);
                if (success) len = requestLen;
                auto state = runtimeStates.find(id);
                if (state != runtimeStates.end()) state->second.lastError = success ? "" : "I2C_READ_INCOMPLETE";
            }
            break;

        // SPI 读取（需要先发送才能读取）
        case PeripheralType::SPI: {
            if (!buffer || maxLen == 0) break;
            if (config->pinCount < 4 || config->pins[3] == 255 ||
                !isValidPin(config->pins[3]) ||
                isReservedPin(config->pins[3]) ||
                isInputOnlyPin(config->pins[3])) {
                LOG_WARNINGF("readData: SPI '%s' has no usable CS pin", id.c_str());
                break;
            }
            FastBee::SpiBusOperation operation(20);
            String spiError;
            if (!operation.locked()) spiError = "SPI_BUSY: read deferred";
            if (!operation.locked() || !FastBee::SpiBusInitializer::instance().checkNamed(
                    id, config->pins[2], config->pins[0], config->pins[1], config->pins[3], spiError)) {
                auto state = runtimeStates.find(id);
                if (state != runtimeStates.end()) state->second.lastError = spiError;
                break;
            }
            const uint8_t cs = config->pins[3];
            SPI.beginTransaction(SPISettings(config->params.spi.frequency ? config->params.spi.frequency : 1000000,
                config->params.spi.msbFirst ? MSBFIRST : LSBFIRST,
                config->params.spi.mode));
            digitalWrite(cs, LOW);
            for (size_t i = 0; i < maxLen; i++) {
                buffer[i] = SPI.transfer(0xFF);  // 发送 dummy 字节读取数据
            }
            digitalWrite(cs, HIGH);
            SPI.endTransaction();
            len = maxLen;
            success = true;
            auto state = runtimeStates.find(id);
            if (state != runtimeStates.end()) state->second.lastError = "";
            break;
        }

        case PeripheralType::RADAR_SENSOR:
            if (maxLen >= 1) {
                bool detected = false;
                success = readRadarState(id, detected);
                if (success) {
                    buffer[0] = detected ? 1 : 0;
                    len = 1;
                }
            }
            break;

        case PeripheralType::RF_MODULE:
            if (maxLen >= 1) {
                bool level = false;
                success = readRfLevel(id, level);
                if (success) {
                    buffer[0] = level ? 1 : 0;
                    len = 1;
                }
            }
            break;

        default:
            LOG_WARNINGF("readData: Unsupported peripheral type %d", static_cast<int>(config->type));
            break;
    }

    // 更新运行时状态
    if (success && len > 0 && runtimeStates.find(id) != runtimeStates.end()) {
        PeripheralRuntimeState& runtime = runtimeStates[id];
        runtime.lastActivity = millis();
        if (config->isCommunicationPeripheral()) {
            runtime.state.comm.bytesReceived = saturatingAddUint32(
                runtime.state.comm.bytesReceived, len);
            if (config->type == PeripheralType::UART) {
                HardwareSerial* serial = getUartSerial(id);
                runtime.state.comm.isConnected = serial != nullptr;
                runtime.state.comm.rxAvailable = boundedUartAvailable(serial);
            }
        }
    }

    return success;
}

bool PeripheralManager::writeString(const String& id, const String& data) {
    return writeData(id, (const uint8_t*)data.c_str(), data.length());
}

String PeripheralManager::readString(const String& id) {
    uint8_t buffer[256];
    size_t len = sizeof(buffer);
    if (readData(id, buffer, len)) {
        return String((char*)buffer, len);
    }
    return "";
}

bool PeripheralManager::getEncoderReading(const String& id, EncoderReading& reading) {
    RecursiveMutexGuard lock(_mutex);
    auto configIt = peripherals.find(id);
    if (configIt == peripherals.end() || configIt->second.type != PeripheralType::ENCODER ||
        !configIt->second.enabled) {
        return false;
    }

    EncoderIsrSlot* slot = findEncoderSlot(id);
    if (!encoderSlotReady(slot)) return false;

    int64_t count = 0;
    int64_t delta = 0;
    int8_t direction = 0;
    uint32_t lastEdgeMicros = 0;
    uint32_t edgePeriodMicros = 0;
    portENTER_CRITICAL(&_encoderMux);
    count = slot->count;
    delta = slot->delta;
    direction = slot->direction;
    lastEdgeMicros = slot->lastEdgeMicros;
    edgePeriodMicros = slot->edgePeriodMicros;
    slot->delta = 0;
    portEXIT_CRITICAL(&_encoderMux);

    float frequencyHz = 0.0f;
    if (lastEdgeMicros != 0 && edgePeriodMicros != 0) {
        uint32_t staleTimeoutUs = ENCODER_MIN_STALE_TIMEOUT_US;
        if (edgePeriodMicros > ENCODER_MIN_STALE_TIMEOUT_US / 2) {
            staleTimeoutUs = edgePeriodMicros > UINT32_MAX / 2
                ? UINT32_MAX : edgePeriodMicros * 2;
        }
        if (static_cast<uint32_t>(micros() - lastEdgeMicros) <= staleTimeoutUs) {
            frequencyHz = 1000000.0f / static_cast<float>(edgePeriodMicros);
        }
    }

    reading.count = count;
    reading.delta = delta;
    reading.direction = direction;
    reading.frequencyHz = frequencyHz;
    reading.rpm = configIt->second.params.encoder.resolution > 0
        ? frequencyHz * 60.0f /
              static_cast<float>(configIt->second.params.encoder.resolution)
        : 0.0f;

    auto stateIt = runtimeStates.find(id);
    if (stateIt != runtimeStates.end()) {
        stateIt->second.state.encoder.count = reading.count;
        stateIt->second.state.encoder.delta = reading.delta;
        stateIt->second.state.encoder.direction = reading.direction;
        stateIt->second.state.encoder.frequencyHz = reading.frequencyHz;
        stateIt->second.state.encoder.rpm = reading.rpm;
        if (reading.delta != 0) stateIt->second.lastActivity = millis();
    }
    return true;
}

bool PeripheralManager::resetEncoder(const String& id) {
    RecursiveMutexGuard lock(_mutex);
    auto configIt = peripherals.find(id);
    if (configIt == peripherals.end() || configIt->second.type != PeripheralType::ENCODER) {
        return false;
    }

    EncoderIsrSlot* slot = findEncoderSlot(id);
    auto stateIt = runtimeStates.find(id);
    if (!slot && configIt->second.enabled) return false;
    if (slot && !encoderSlotReady(slot)) return false;

    portENTER_CRITICAL(&_encoderMux);
    if (slot) {
        slot->count = 0;
        slot->delta = 0;
        slot->direction = 0;
        slot->lastEdgeMicros = 0;
        slot->edgePeriodMicros = 0;
    }
    if (stateIt != runtimeStates.end()) {
        memset(&stateIt->second.state.encoder, 0, sizeof(stateIt->second.state.encoder));
    }
    portEXIT_CRITICAL(&_encoderMux);

    if (stateIt != runtimeStates.end()) stateIt->second.lastActivity = millis();
    LOG_INFOF("Peripheral Manager: Encoder '%s' counter and timing reset", id.c_str());
    return true;
}

bool PeripheralManager::attachInterrupt(uint8_t pin, GPIOInterruptCallback callback) {
    String id = getPinPeripheralId(pin);
    if (id.isEmpty()) return false;
    return attachInterrupt(id, callback);
}

bool PeripheralManager::attachInterrupt(const String& peripheralId, GPIOInterruptCallback callback) {
    // 线程安全（T4 修复）：旧实现在 getPeripheral 释放锁后继续解引用返回的裸指针，
    // 并发 removePeripheral 会使指针悬垂；改为持锁内直接访问容器
    RecursiveMutexGuard lock(_mutex);
    if (!lock.isLocked()) return false;
    auto it = peripherals.find(peripheralId);
    if (it == peripherals.end() || !it->second.isGPIOPeripheral()) return false;
    PeripheralConfig& config = it->second;

    uint8_t pin = config.getPrimaryPin();
    // 引脚验证（T3）：attach 前验证引脚合法性，避免向非法 GPIO 注册中断
    if (pin == 255 || !isValidPin(pin)) return false;

    uint8_t mode = CHANGE;
    if (config.type == PeripheralType::GPIO_INTERRUPT_RISING) {
        mode = config.params.gpio.invertLogic ? FALLING : RISING;
    } else if (config.type == PeripheralType::GPIO_INTERRUPT_FALLING) {
        mode = config.params.gpio.invertLogic ? RISING : FALLING;
    }

    if (!config.enabled || !_managedPinIo.attach(config.id.c_str(), pin, isrHandler, (void*)(uintptr_t)pin, mode)) return false;
    config.params.gpio.interruptCallback = callback;

    auto rs = runtimeStates.find(peripheralId);
    if (rs != runtimeStates.end()) {
        rs->second.state.gpio.interruptAttached = true;
    }

    return true;
}

bool PeripheralManager::popDigitalInputInterruptEvent(uint8_t& pin, GPIOState& state) {
    RecursiveMutexGuard lock(_mutex);
    if (_digitalInterruptCount == 0) return false;

    const DigitalInputInterruptEvent& event = _digitalInterruptEvents[_digitalInterruptHead];
    pin = event.pin;
    state = event.state;
    _digitalInterruptHead = (_digitalInterruptHead + 1) % ISR_QUEUE_SIZE;
    _digitalInterruptCount--;
    return true;
}

bool PeripheralManager::detachInterrupt(uint8_t pin) {
    String id = getPinPeripheralId(pin);
    if (id.isEmpty()) return false;
    return detachInterrupt(id);
}

bool PeripheralManager::detachInterrupt(const String& peripheralId) {
    // 同 attachInterrupt：持锁内直接访问容器，避免锁外解引用裸指针
    RecursiveMutexGuard lock(_mutex);
    if (!lock.isLocked()) return false;
    auto it = peripherals.find(peripheralId);
    if (it == peripherals.end() || !it->second.isGPIOPeripheral()) return false;
    PeripheralConfig& config = it->second;

    uint8_t pin = config.getPrimaryPin();
    if (pin == 255 || !isValidPin(pin)) return false;

    if (!_managedPinIo.detach(config.id.c_str(), pin)) return false;
    config.params.gpio.interruptCallback = nullptr;

    auto rs = runtimeStates.find(peripheralId);
    if (rs != runtimeStates.end()) {
        rs->second.state.gpio.interruptAttached = false;
    }

    return true;
}

// ========== 编码器中断处理 ==========
// 编码器使用正交解码原理：
// - A相和B相相差90度相位
// - 根据A相上升/下降沿时B相的状态判断转向
// - 顺时针：计数+1；逆时针：计数-1

// 静态槽位存储：ISR 只触碰固定内存，不做任何堆分配
PeripheralManager::EncoderIsrSlot PeripheralManager::_encoderSlots[PeripheralManager::MAX_ENCODER_SLOTS];
portMUX_TYPE PeripheralManager::_encoderMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t PeripheralManager::_nextEncoderGeneration = 0;

void PeripheralManager::snapshotEncoderResources(FastBee::EncoderResourceSnapshot& result) const {
    result = FastBee::EncoderResourceSnapshot{};
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked()) { result.status = FastBee::ResourceObservation::Busy; return; }
    static_assert(MAX_ENCODER_SLOTS == FastBee::EncoderResourceSnapshot::CAPACITY, "snapshot capacity");
    // Task mutations of identity/pins/used hold _mutex. The ISR only reads
    // these fields; counters are intentionally omitted, so no ISR spinlock is
    // acquired and no GPIO/interrupt API is invoked by this observation.
    for (size_t i = 0; i < MAX_ENCODER_SLOTS; ++i) {
        const auto& slot = _encoderSlots[i];
        if (!slot.used) continue;
        auto& lease = result.leases[result.count++];
        lease.slot = static_cast<uint8_t>(i);
        lease.pinA = slot.pinA; lease.pinB = slot.pinB;
        // Do not invent a truncated identity if a malformed legacy slot exists.
        const size_t length = slot.id.length();
        if (length < sizeof(lease.id) && std::strlen(slot.id.c_str()) == length)
            std::memcpy(lease.id, slot.id.c_str(), length);
        FastBee::GpioResourceSnapshot::Lease phase;
        if (_managedPinIo.observeGpio(slot.pinA, phase)) {
            lease.interruptAttached = phase.interruptAttached;
            lease.interruptCleanupPending = phase.interruptCleanupPending;
            lease.fault = phase.fault;
            lease.cleanupPending = phase.cleanupPending;
        }
        if (_managedPinIo.observeGpio(slot.pinB, phase)) {
            lease.fault = lease.fault || phase.fault;
            lease.cleanupPending = lease.cleanupPending || phase.cleanupPending;
        }
        lease.active = encoderSlotReady(&slot);
        lease.fault = lease.fault || slot.fault;
        lease.cleanupPending = lease.cleanupPending || slot.cleanupPending;
    }
}

PeripheralManager::EncoderIsrSlot* PeripheralManager::findEncoderSlot(const String& id) {
    for (auto& slot : _encoderSlots) {
        if (slot.used && slot.id == id) return &slot;
    }
    return nullptr;
}

PeripheralManager::EncoderIsrSlot* PeripheralManager::allocEncoderSlot(const String& id,
                                                                       uint8_t pinA, uint8_t pinB) {
    if (id.length() == 0 || id.length() >= FastBee::ResourceConsumerIdentity::ID_CAPACITY ||
        std::strlen(id.c_str()) != id.length() || findEncoderSlot(id) ||
        _nextEncoderGeneration >= (UINT32_MAX >> 2)) return nullptr;
    static_assert(MAX_ENCODER_SLOTS == 4 && sizeof(uintptr_t) >= sizeof(uint32_t), "encoder token capacity");
    EncoderIsrSlot* slot = nullptr;
    for (auto& s : _encoderSlots) {
        if (!s.used) { slot = &s; break; }
    }
    if (!slot) return nullptr;
    slot->id = id;  // String 赋值在临界区外（关中断时不可 malloc）
    if (slot->id != id) { slot->id = ""; return nullptr; }
    portENTER_CRITICAL(&_encoderMux);
    slot->token = (++_nextEncoderGeneration << 2) | static_cast<uint32_t>(slot - _encoderSlots);
    slot->active = false;
    slot->fault = false;
    slot->cleanupPending = false;
    slot->pinA = pinA;
    slot->pinB = pinB;
    slot->count = 0;
    slot->delta = 0;
    slot->direction = 0;
    slot->lastEdgeMicros = 0;
    slot->edgePeriodMicros = 0;
    slot->used = true;
    portEXIT_CRITICAL(&_encoderMux);
    return slot;
}

void PeripheralManager::releaseEncoderSlot(const String& id) {
    EncoderIsrSlot* slot = findEncoderSlot(id);
    if (!slot) return;
    if (slot->active || _managedPinIo.hasGpio(slot->pinA) || _managedPinIo.hasGpio(slot->pinB)) return;
    portENTER_CRITICAL(&_encoderMux);
    slot->active = false;
    slot->used = false;
    slot->token = 0;
    slot->fault = false;
    slot->cleanupPending = false;
    slot->pinA = 255;
    slot->pinB = 255;
    slot->count = 0;
    slot->delta = 0;
    slot->direction = 0;
    slot->lastEdgeMicros = 0;
    slot->edgePeriodMicros = 0;
    portEXIT_CRITICAL(&_encoderMux);
    slot->id = "";  // String 释放在临界区外
}

void IRAM_ATTR PeripheralManager::handleEncoderInterrupt(void* arg) {
    const uintptr_t token = reinterpret_cast<uintptr_t>(arg);
    if (token == 0 || token != static_cast<uint32_t>(token) || (token >> 2) == 0) return;
    EncoderIsrSlot* slot = &_encoderSlots[token & 3U];
    portENTER_CRITICAL_ISR(&_encoderMux);
    if (slot->used && slot->active && slot->token == token) {
        // CHANGE 同时包含 A 相上升沿和下降沿，因此必须联合读取 A/B；
        // 只看 B 相会在相邻边沿产生相反结论并导致计数错误。
        const bool phaseA = gpio_get_level(static_cast<gpio_num_t>(slot->pinA)) != 0;
        const bool phaseB = gpio_get_level(static_cast<gpio_num_t>(slot->pinB)) != 0;
        const int8_t step = (phaseA == phaseB) ? 1 : -1;
        const uint32_t nowMicros = micros();
        if (slot->lastEdgeMicros != 0) {
            slot->edgePeriodMicros = nowMicros - slot->lastEdgeMicros;
        }
        slot->lastEdgeMicros = nowMicros;
        if ((step > 0 && slot->count < INT64_MAX) || (step < 0 && slot->count > INT64_MIN)) slot->count += step;
        if ((step > 0 && slot->delta < INT64_MAX) || (step < 0 && slot->delta > INT64_MIN)) slot->delta += step;
        slot->direction = step;
    }
    portEXIT_CRITICAL_ISR(&_encoderMux);
}
