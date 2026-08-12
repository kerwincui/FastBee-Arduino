/**
 * @description: 433MHz OOK 遥控学习驱动实现（EV1527 兼容）
 * @author: kerwincui
 * @copyright: FastBee All rights reserved.
 */

#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_RF_LEARN

#include "peripherals/drivers/RFLearnDriver.h"
#include "core/RuntimeFeatureManager.h"
#include "systems/LoggerSystem.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

#if FASTBEE_ENABLE_PERIPH_EXEC
#include "core/PeriphExecManager.h"
#endif

// ============ 静态成员定义 ============
volatile uint32_t RFLearnDriver::_isrLastEdgeTime = 0;
volatile uint16_t RFLearnDriver::_isrPulseBuffer[RFLearnDriver::ISR_BUFFER_SIZE];
volatile uint8_t RFLearnDriver::_isrPulseCount = 0;
volatile bool RFLearnDriver::_isrFrameReady = false;

static const char* RF_KEYS_FILE = "/config/rf_keys.json";

// ============ 单例 ============
RFLearnDriver& RFLearnDriver::getInstance() {
    static RFLearnDriver instance;
    if (!instance._mutex) {
        instance._mutex = xSemaphoreCreateMutex();
    }
    return instance;
}

RFLearnDriver::~RFLearnDriver() {
    stop();
}

// ============ ISR ============
void IRAM_ATTR RFLearnDriver::_isrHandler() {
    uint32_t now = micros();
    uint32_t duration = now - _isrLastEdgeTime;
    _isrLastEdgeTime = now;

    // 过滤过短的毛刺（<50μs）
    if (duration < 50) return;

    // 限幅到 uint16 范围
    if (duration > 65000) duration = 65000;

    // 检测到超长间隔（>SYNC_MIN_US）：表示前一帧结束，标记帧就绪
    if (duration > SYNC_MIN_US && _isrPulseCount >= (MIN_DATA_BITS * 2)) {
        _isrFrameReady = true;
        return;
    }

    // 超长间隔但数据不够：重置缓冲
    if (duration > SYNC_MIN_US) {
        _isrPulseCount = 0;
        return;
    }

    // 写入缓冲
    if (_isrPulseCount < ISR_BUFFER_SIZE) {
        _isrPulseBuffer[_isrPulseCount++] = (uint16_t)duration;
    } else {
        // 缓冲溢出：标记帧就绪尝试解码
        _isrFrameReady = true;
    }
}

// ============ 初始化 / 停止 ============
bool RFLearnDriver::begin(uint8_t rxPin) {
    // 运行时功能门控：用户可在 Web UI 禁用 rfLearn
    if (!FastBee::RuntimeFeatureManager::instance().shouldEnable("rfLearn")) {
        LOGGER.info("[RFLearn] Compiled but runtime-disabled, skip init");
        return false;
    }
    if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
    if (_initialized) {
        if (_rxPin == rxPin) { if (_mutex) xSemaphoreGive(_mutex); return true; }
        // 引脚变更：先停止再重新初始化
        detachInterrupt(digitalPinToInterrupt(_rxPin));
        _initialized = false;
    }
    _rxPin = rxPin;
    pinMode(_rxPin, INPUT);
    _isrPulseCount = 0;
    _isrFrameReady = false;
    _isrLastEdgeTime = micros();
    attachInterrupt(digitalPinToInterrupt(_rxPin), _isrHandler, CHANGE);
    _initialized = true;
    loadKeyMap();
    LOG_INFOF("[RF-Learn] Initialized on GPIO%u", _rxPin);
    if (_mutex) xSemaphoreGive(_mutex);
    return true;
}

void RFLearnDriver::stop() {
    if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
    if (!_initialized) { if (_mutex) xSemaphoreGive(_mutex); return; }
    detachInterrupt(digitalPinToInterrupt(_rxPin));
    _initialized = false;
    _isrPulseCount = 0;
    _isrFrameReady = false;
    LOG_INFOF("[RF-Learn] Stopped");
    if (_mutex) xSemaphoreGive(_mutex);
}

// ============ 主循环检查 ============
void RFLearnDriver::check() {
    // 非阻塞获取锁：若 stop()/begin() 正在执行则跳过本轮
    if (_mutex && xSemaphoreTake(_mutex, 0) != pdTRUE) return;
    if (!_initialized) { if (_mutex) xSemaphoreGive(_mutex); return; }

    // 刷新学习超时
    _updateLearnTimeout();

    // 检查是否有帧就绪
    if (!_isrFrameReady) { if (_mutex) xSemaphoreGive(_mutex); return; }

    // 临界区：拷贝缓冲并重置
    noInterrupts();
    uint16_t localBuf[ISR_BUFFER_SIZE];
    uint8_t count = _isrPulseCount;
    for (uint8_t i = 0; i < count; i++) localBuf[i] = _isrPulseBuffer[i];
    _isrPulseCount = 0;
    _isrFrameReady = false;
    interrupts();

    if (count < MIN_DATA_BITS * 2) { if (_mutex) xSemaphoreGive(_mutex); return; }

    // 解码
    // EV1527 格式：数据位交替 HIGH/LOW，每个位占 2 个脉冲
    // bit=1: HIGH=3T, LOW=1T; bit=0: HIGH=1T, LOW=3T
    // 找到第一个有效同步后的数据段

    // 计算平均脉宽（取最短脉冲作为 1T 基准）
    uint16_t minPulse = 65535;
    for (uint8_t i = 0; i < count; i++) {
        if (localBuf[i] < minPulse) minPulse = localBuf[i];
    }
    if (minPulse < 80 || minPulse > 2000) { if (_mutex) xSemaphoreGive(_mutex); return; } // 不合理脉宽

    uint16_t T = minPulse; // 1T 基准
    uint32_t code = 0;
    uint8_t bits = 0;
    uint16_t pulseSum = 0;
    uint8_t pulseSamples = 0;

    // 从缓冲中提取数据位（每 2 个脉冲为一位）
    for (uint8_t i = 0; i + 1 < count && bits < MAX_DATA_BITS; i += 2) {
        uint16_t high = localBuf[i];
        uint16_t low = localBuf[i + 1];

        // 跳过明显异常脉冲
        if (high > T * 6 || low > T * 6) continue;

        uint8_t highRatio = (high + T / 2) / T;
        uint8_t lowRatio = (low + T / 2) / T;

        if (highRatio >= 2 && lowRatio <= 2) {
            // bit = 1 (3:1)
            code = (code << 1) | 1;
            bits++;
            pulseSum += (high + low) / 4;
            pulseSamples++;
        } else if (highRatio <= 2 && lowRatio >= 2) {
            // bit = 0 (1:3)
            code = (code << 1) | 0;
            bits++;
            pulseSum += (high + low) / 4;
            pulseSamples++;
        }
        // 其他比例：噪声，跳过
    }

    if (bits < MIN_DATA_BITS) { if (_mutex) xSemaphoreGive(_mutex); return; }

    uint16_t avgPulse = pulseSamples > 0 ? pulseSum / pulseSamples : T;

    // 防重复：相同编码短时间内不重复处理
    unsigned long now = millis();
    if (code == _lastValue && bits == _lastBitLength &&
        (now - _lastEventTime) < MIN_EVENT_INTERVAL_MS) {
        if (_mutex) xSemaphoreGive(_mutex);
        return;
    }

    // 更新最近解码
    _lastValue = code;
    _lastBitLength = bits;
    _lastPulseWidth = avgPulse;
    char hexBuf[12];
    snprintf(hexBuf, sizeof(hexBuf), "0x%06lX", (unsigned long)code);
    _lastCode = hexBuf;
    _lastEventTime = now;

    LOG_INFOF("[RF-Learn] Decoded: %s (%u bits, pulse=%uus)", _lastCode.c_str(), bits, avgPulse);

    // 学习模式：捕获
    if (_learnState == RfLearnState::LEARNING) {
        _learnedCode = _lastCode;
        _learnedBitLength = bits;
        _learnedPulseWidth = avgPulse;
        _learnState = RfLearnState::CAPTURED;
        if (_mutex) xSemaphoreGive(_mutex);
        return;
    }

    // 正常模式：派发事件
#if FASTBEE_ENABLE_PERIPH_EXEC
    String keyName = resolveKeyName(code, bits);
    String eventData = "{\"code\":\"" + _lastCode + "\",\"bits\":" + String(bits) +
                       ",\"pulse\":" + String(avgPulse);
    if (keyName.length() > 0) {
        eventData += ",\"key\":\"" + keyName + "\"";
    }
    eventData += "}";
    PeriphExecManager::getInstance().dispatchEventMatchedRules("rf_code_received", eventData);
#endif
    if (_mutex) xSemaphoreGive(_mutex);
}

// ============ 学习模式 ============
void RFLearnDriver::startLearn(uint32_t timeoutMs) {
    if (timeoutMs < 1000) timeoutMs = 1000;
    if (timeoutMs > 60000) timeoutMs = 60000;
    _learnState = RfLearnState::LEARNING;
    _learnDeadline = millis() + timeoutMs;
    _learnedCode = "";
    _learnedBitLength = 0;
    _learnedPulseWidth = 0;
    LOG_INFOF("[RF-Learn] Learning started (timeout=%lums)", (unsigned long)timeoutMs);
}

void RFLearnDriver::stopLearn() {
    _learnState = RfLearnState::IDLE;
    _learnedCode = "";
}

RfLearnState RFLearnDriver::getLearnState() {
    _updateLearnTimeout();
    return _learnState;
}

String RFLearnDriver::getLearnStateName() {
    _updateLearnTimeout();
    switch (_learnState) {
        case RfLearnState::LEARNING: return "learning";
        case RfLearnState::CAPTURED: return "captured";
        case RfLearnState::TIMEOUT:  return "timeout";
        default: return "idle";
    }
}

uint32_t RFLearnDriver::getLearnRemainingMs() {
    if (_learnState != RfLearnState::LEARNING) return 0;
    unsigned long now = millis();
    if (now >= _learnDeadline) return 0;
    return (uint32_t)(_learnDeadline - now);
}

void RFLearnDriver::_updateLearnTimeout() {
    if (_learnState == RfLearnState::LEARNING && millis() >= _learnDeadline) {
        _learnState = RfLearnState::TIMEOUT;
        LOG_INFOF("[RF-Learn] Learning timeout");
    }
}

// ============ 按键映射 ============
bool RFLearnDriver::loadKeyMap() {
    _keyMap.clear();
    _keyMapLoaded = true;

    if (!LittleFS.exists(RF_KEYS_FILE)) return true; // 无文件=空表

    File f = LittleFS.open(RF_KEYS_FILE, "r");
    if (!f) return false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        LOG_WARNINGF("[RF-Learn] Failed to parse rf_keys.json: %s", err.c_str());
        return false;
    }

    JsonArray arr = doc.as<JsonArray>();
    for (JsonObject obj : arr) {
        RfKeyEntry entry;
        entry.name = obj["name"] | "";
        const char* codeStr = obj["code"] | "0";
        entry.code = (uint32_t)strtoul(codeStr, nullptr, 16);
        entry.bitLength = obj["bits"] | 24;
        entry.pulseWidth = obj["pulse"] | 350;
        if (entry.name.length() > 0) {
            _keyMap.push_back(entry);
        }
    }
    LOG_INFOF("[RF-Learn] Loaded %u keys", (unsigned)_keyMap.size());
    return true;
}

String RFLearnDriver::resolveKeyName(uint32_t code, uint8_t bitLength) const {
    for (const auto& e : _keyMap) {
        if (e.code == code && e.bitLength == bitLength) return e.name;
    }
    return "";
}

String RFLearnDriver::getKeysJson() const {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& e : _keyMap) {
        JsonObject obj = arr.add<JsonObject>();
        obj["name"] = e.name;
        char hex[12];
        snprintf(hex, sizeof(hex), "0x%06lX", (unsigned long)e.code);
        obj["code"] = hex;
        obj["bits"] = e.bitLength;
        obj["pulse"] = e.pulseWidth;
    }
    String out;
    serializeJson(doc, out);
    return out;
}

bool RFLearnDriver::upsertKey(const String& name, const String& code, uint8_t bitLength, uint16_t pulseWidth, String& err) {
    if (name.length() == 0 || name.length() > 32) {
        err = "按键名称长度需在 1-32 字符之间";
        return false;
    }
    // 校验名称安全字符
    for (size_t i = 0; i < name.length(); i++) {
        char c = name[i];
        if (!isalnum(c) && c != '_' && c != '-' && c != ' ') {
            err = "名称仅支持字母、数字、下划线、连字符和空格";
            return false;
        }
    }

    uint32_t codeVal = (uint32_t)strtoul(code.c_str(), nullptr, 16);
    if (codeVal == 0 && !code.startsWith("0x0") && !code.equals("0")) {
        err = "无效的编码值";
        return false;
    }
    if (bitLength < MIN_DATA_BITS || bitLength > MAX_DATA_BITS) {
        err = "位长需在 12-32 之间";
        return false;
    }

    if (_keyMap.size() >= MAX_RF_KEYS) {
        // 检查是否为更新
        bool found = false;
        for (auto& e : _keyMap) {
            if (e.name == name) { found = true; break; }
        }
        if (!found) {
            err = "按键数量已达上限 (" + String(MAX_RF_KEYS) + ")";
            return false;
        }
    }

    // 更新或新增
    for (auto& e : _keyMap) {
        if (e.name == name) {
            e.code = codeVal;
            e.bitLength = bitLength;
            e.pulseWidth = pulseWidth;
            return _persistKeyMap();
        }
    }
    RfKeyEntry entry;
    entry.name = name;
    entry.code = codeVal;
    entry.bitLength = bitLength;
    entry.pulseWidth = pulseWidth;
    _keyMap.push_back(entry);
    return _persistKeyMap();
}

bool RFLearnDriver::deleteKey(const String& name) {
    for (auto it = _keyMap.begin(); it != _keyMap.end(); ++it) {
        if (it->name == name) {
            _keyMap.erase(it);
            return _persistKeyMap();
        }
    }
    return false;
}

bool RFLearnDriver::clearKeys() {
    _keyMap.clear();
    return _persistKeyMap();
}

bool RFLearnDriver::_persistKeyMap() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& e : _keyMap) {
        JsonObject obj = arr.add<JsonObject>();
        obj["name"] = e.name;
        char hex[12];
        snprintf(hex, sizeof(hex), "0x%06lX", (unsigned long)e.code);
        obj["code"] = hex;
        obj["bits"] = e.bitLength;
        obj["pulse"] = e.pulseWidth;
    }

    File f = LittleFS.open(RF_KEYS_FILE, "w");
    if (!f) {
        LOG_ERRORF("[RF-Learn] Cannot open rf_keys.json for writing");
        return false;
    }
    serializeJson(doc, f);
    f.close();
    return true;
}

#endif // FASTBEE_ENABLE_RF_LEARN
