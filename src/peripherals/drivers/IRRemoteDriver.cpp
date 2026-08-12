#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_IR_REMOTE

#include "peripherals/drivers/IRRemoteDriver.h"
#include "core/PeripheralExecution.h"
#include "core/PeriphExecManager.h"
#include "systems/LoggerSystem.h"
#include "core/FeatureBudget.h"
#include "core/RuntimeFeatureManager.h"

#include <IRrecv.h>
#include <IRutils.h>
#include <ArduinoJson.h>

// 红外按键映射（对码结果）不再单独建 ir_keys.json，
// 统一随外设执行配置 periph_exec.json 的 irKeys 字段持久化（经 PeriphExecManager 读写）

// 将编码字符串（"0x00FF6897" / "00FF6897"）解析为 uint64_t
static bool parseIrCode(const String& in, uint64_t& out) {
    String s = in;
    s.trim();
    if (s.length() == 0) return false;
    if (s.startsWith("0x") || s.startsWith("0X")) s = s.substring(2);
    if (s.length() == 0 || s.length() > 16) return false;
    uint64_t v = 0;
    for (size_t i = 0; i < s.length(); ++i) {
        char c = s[i];
        uint8_t d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = (v << 4) | d;
    }
    out = v;
    return true;
}

// 校验按键名称：非空、长度<=32、仅字母数字下划线短横线
static bool isValidKeyName(const String& name) {
    if (name.length() == 0 || name.length() > 32) return false;
    for (size_t i = 0; i < name.length(); ++i) {
        char c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-') continue;
        return false;
    }
    return true;
}

static String codeToHex(uint64_t value) {
    char hexBuf[20];
    snprintf(hexBuf, sizeof(hexBuf), "0x%08llX", (unsigned long long)value);
    return String(hexBuf);
}

IRRemoteDriver& IRRemoteDriver::getInstance() {
    static IRRemoteDriver instance;
    if (!instance._mutex) {
        instance._mutex = xSemaphoreCreateMutex();
    }
    return instance;
}

IRRemoteDriver::~IRRemoteDriver() {
    stop();
}

bool IRRemoteDriver::begin(uint8_t recvPin, uint16_t bufferSize) {
    if (!FastBee::RuntimeFeatureManager::instance().shouldEnable("irRemote")) {
        LOGGER.info("[IR] Compiled but runtime-disabled, skip init");
        return false;
    }
    if (FastBee::FeatureBudgetRegistry::instance().isDenied("irRemote")) {
        LOGGER.warning("[IR] Disabled by memory budget, skip init");
        return false;
    }
    if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
    if (_initialized) { if (_mutex) xSemaphoreGive(_mutex); return true; }

    IRrecv* recv = new IRrecv(recvPin, bufferSize, 50, true);  // 50us timeout, save buffer
    recv->setUnknownThreshold(12);  // 未知协议最小脉冲数
    recv->enableIRIn();

    _irRecv = recv;
    _results = new decode_results();
    _initialized = true;
    _lastCode = "";
    _lastProtocol = "";
    _lastValue = 0;

    // 加载按键映射（对码结果），不做启动时对码
    loadKeyMap();

    LOGGER.infof("[IR] Receiver initialized on pin %d (buffer=%d, keys=%d)",
                 recvPin, bufferSize, (int)_keyMap.size());
    if (_mutex) xSemaphoreGive(_mutex);
    return true;
}

void IRRemoteDriver::check() {
    // 非阻塞获取锁：若 stop()/begin() 正在执行则跳过本轮
    if (_mutex && xSemaphoreTake(_mutex, 0) != pdTRUE) return;
    if (!_initialized || !_irRecv || !_results) {
        if (_mutex) xSemaphoreGive(_mutex);
        return;
    }

    // 学习超时检测（即使无解码也需推进状态）
    _updateLearnTimeout();

    // 延迟加载按键映射：IR 外设可能在 PeriphExecManager 初始化前 begin()，
    // 首次读到空表时保持未加载标记，待配置管理器就绪后补加载一次
    if (!_keyMapLoaded) loadKeyMap();

    IRrecv* recv = static_cast<IRrecv*>(_irRecv);
    decode_results* results = static_cast<decode_results*>(_results);

    if (recv->decode(results)) {
        unsigned long now = millis();

        // 获取协议名称
        String protocol = typeToString(results->decode_type);
        uint64_t value = results->value;

        // 过滤重复码（NEC repeat = 0xFFFFFFFFFFFFFFFF）
        if (value == 0xFFFFFFFFFFFFFFFF || value == 0) {
            recv->resume();
            if (_mutex) xSemaphoreGive(_mutex);
            return;
        }

        // 防抖：同一编码在最小间隔内不重复触发
        if (now - _lastEventTime < MIN_EVENT_INTERVAL_MS &&
            value == _lastValue) {
            recv->resume();
            if (_mutex) xSemaphoreGive(_mutex);
            return;
        }

        // 构建编码字符串
        String hex = codeToHex(value);

        _lastProtocol = protocol;
        _lastValue = value;
        _lastCode = protocol + ":" + hex;
        _lastEventTime = now;

        // 学习模式：仅捕获编码，不派发事件（避免误触已有规则）
        if (_learnState == IrLearnState::LEARNING) {
            _learnedProtocol = protocol;
            _learnedValue = value;
            _learnedCode = hex;
            _learnState = IrLearnState::CAPTURED;
            LOGGER.infof("[IR] Learn captured: %s %s", protocol.c_str(), hex.c_str());
            recv->resume();
            if (_mutex) xSemaphoreGive(_mutex);
            return;
        }

        LOGGER.infof("[IR] Received: %s value=%s bits=%d",
                     protocol.c_str(), hex.c_str(), results->bits);

        // 触发红外编码接收事件（命中按键映射时携带 key 字段）
#if FASTBEE_ENABLE_PERIPH_EXEC
        String keyName = resolveKeyName(value, protocol);
        String eventData = "{\"protocol\":\"" + protocol +
                           "\",\"code\":\"" + hex +
                           "\",\"bits\":" + String(results->bits);
        if (keyName.length() > 0) {
            eventData += ",\"key\":\"" + keyName + "\"";  // 按键名已校验为安全字符
        }
        eventData += "}";
        PeriphExecManager::getInstance().dispatchEventMatchedRules("ir_code_received", eventData);
#endif

        recv->resume();
    }
    if (_mutex) xSemaphoreGive(_mutex);
}

void IRRemoteDriver::stop() {
    if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
    if (_irRecv) {
        IRrecv* recv = static_cast<IRrecv*>(_irRecv);
        _irRecv = nullptr;
        // 注意：不能手动调 disableIRIn()！~IRrecv() 析构函数内部已调用 disableIRIn()，
        // 重复调用会对已 timerEnd() 释放的定时器再次 timerWrite() 导致 NULL 崩溃。
        delete recv;
    }
    if (_results) {
        decode_results* res = static_cast<decode_results*>(_results);
        delete res;
        _results = nullptr;
    }
    _initialized = false;
    _lastCode = "";
    _lastProtocol = "";
    _lastValue = 0;
    _learnState = IrLearnState::IDLE;
    LOGGER.info("[IR] Receiver stopped");
    if (_mutex) xSemaphoreGive(_mutex);
}

// ============ 学习模式 ============

void IRRemoteDriver::startLearn(uint32_t timeoutMs) {
    if (!_initialized) return;
    if (timeoutMs < 1000) timeoutMs = 1000;
    if (timeoutMs > 60000) timeoutMs = 60000;
    _learnState = IrLearnState::LEARNING;
    _learnedCode = "";
    _learnedProtocol = "";
    _learnedValue = 0;
    _learnDeadline = millis() + timeoutMs;
    // 清空可能残留的解码缓冲，避免捕获到进入学习前的旧编码
    if (_irRecv) static_cast<IRrecv*>(_irRecv)->resume();
    LOGGER.infof("[IR] Learn mode started (timeout=%ums)", (unsigned)timeoutMs);
}

void IRRemoteDriver::stopLearn() {
    if (_learnState == IrLearnState::LEARNING) {
        _learnState = IrLearnState::IDLE;
        LOGGER.info("[IR] Learn mode cancelled");
    }
}

void IRRemoteDriver::_updateLearnTimeout() {
    if (_learnState == IrLearnState::LEARNING &&
        (int32_t)(millis() - _learnDeadline) >= 0) {
        _learnState = IrLearnState::TIMEOUT;
        LOGGER.info("[IR] Learn timeout");
    }
}

IrLearnState IRRemoteDriver::getLearnState() {
    _updateLearnTimeout();
    return _learnState;
}

String IRRemoteDriver::getLearnStateName() {
    switch (getLearnState()) {
        case IrLearnState::LEARNING: return "learning";
        case IrLearnState::CAPTURED: return "captured";
        case IrLearnState::TIMEOUT:  return "timeout";
        default:                     return "idle";
    }
}

uint32_t IRRemoteDriver::getLearnRemainingMs() {
    _updateLearnTimeout();
    if (_learnState != IrLearnState::LEARNING) return 0;
    int32_t rem = (int32_t)(_learnDeadline - millis());
    return rem > 0 ? (uint32_t)rem : 0;
}

// ============ 按键映射 ============

bool IRRemoteDriver::loadKeyMap() {
    _keyMap.clear();
    // PeriphExecManager 未初始化（如 IR 外设在系统启动早期 begin）时保持未加载标记，
    // 由 check() 在配置管理器就绪后补加载
    String keysJson = PeriphExecManager::getInstance().getIrKeysJson();
    if (keysJson.isEmpty() && !PeriphExecManager::getInstance().getRulesMutex()) {
        return false;
    }
    _keyMapLoaded = true;
    if (keysJson.isEmpty()) return true;  // 无配置视为空表
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, keysJson);
    if (err || !doc.is<JsonArray>()) {
        LOGGER.warningf("[IR] irKeys parse error: %s", err.c_str());
        return false;
    }
    for (JsonObject o : doc.as<JsonArray>()) {
        String name = o["name"] | "";
        String code = o["code"] | "";
        String proto = o["protocol"] | "";
        uint64_t value = 0;
        if (!isValidKeyName(name) || !parseIrCode(code, value)) continue;
        if (_keyMap.size() >= MAX_IR_KEYS) break;
        _keyMap.push_back(IrKeyEntry{name, value, proto});
    }
    return true;
}

String IRRemoteDriver::resolveKeyName(uint64_t value, const String& protocol) const {
    for (const auto& e : _keyMap) {
        if (e.value != value) continue;
        if (e.protocol.length() > 0 && !e.protocol.equalsIgnoreCase(protocol)) continue;
        return e.name;
    }
    return "";
}

String IRRemoteDriver::getKeysJson() const {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& e : _keyMap) {
        JsonObject o = arr.add<JsonObject>();
        o["name"] = e.name;
        o["code"] = codeToHex(e.value);
        o["protocol"] = e.protocol;
    }
    String out;
    serializeJson(arr, out);
    return out;
}

bool IRRemoteDriver::upsertKey(const String& name, const String& code,
                               const String& protocol, String& err) {
    if (!_keyMapLoaded) loadKeyMap();
    if (!isValidKeyName(name)) {
        err = "按键名非法（仅允许字母数字下划线短横线，1-32 字符）";
        return false;
    }
    uint64_t value = 0;
    if (!parseIrCode(code, value)) {
        err = "编码格式非法（应为十六进制，如 0x00FF6897）";
        return false;
    }
    // 更新已有同名按键
    for (auto& e : _keyMap) {
        if (e.name == name) {
            e.value = value;
            e.protocol = protocol;
            return _persistKeyMap();
        }
    }
    if (_keyMap.size() >= MAX_IR_KEYS) {
        err = "按键数量已达上限（" + String((int)MAX_IR_KEYS) + "）";
        return false;
    }
    _keyMap.push_back(IrKeyEntry{name, value, protocol});
    return _persistKeyMap();
}

bool IRRemoteDriver::deleteKey(const String& name) {
    if (!_keyMapLoaded) loadKeyMap();
    bool removed = false;
    for (size_t i = 0; i < _keyMap.size(); ++i) {
        if (_keyMap[i].name == name) {
            _keyMap.erase(_keyMap.begin() + i);
            removed = true;
            break;
        }
    }
    if (!removed) return false;
    return _persistKeyMap();
}

bool IRRemoteDriver::clearKeys() {
    _keyMap.clear();
    _keyMapLoaded = true;
    // 清空：写入空数组并持久化（PeriphExecManager 会同步移除配置中的 irKeys 字段）
    PeriphExecManager::getInstance().setIrKeysJson("");
    LOGGER.info("[IR] All IR keys cleared");
    return true;
}

bool IRRemoteDriver::_persistKeyMap() {
    if (_keyMap.empty()) {
        return PeriphExecManager::getInstance().setIrKeysJson("");
    }
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& e : _keyMap) {
        JsonObject o = arr.add<JsonObject>();
        o["name"] = e.name;
        o["code"] = codeToHex(e.value);
        o["protocol"] = e.protocol;
    }
    String json;
    serializeJson(arr, json);
    bool ok = PeriphExecManager::getInstance().setIrKeysJson(json);
    if (ok) LOGGER.infof("[IR] IR keys saved to periph_exec.json (%d keys)", (int)_keyMap.size());
    else LOGGER.warning("[IR] Failed to save IR keys to periph_exec.json");
    return ok;
}

#endif // FASTBEE_ENABLE_IR_REMOTE
