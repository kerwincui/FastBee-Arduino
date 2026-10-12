#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_IR_REMOTE

#include "peripherals/drivers/IRRemoteDriver.h"
#if FASTBEE_ENABLE_PERIPH_EXEC
#include "core/PeripheralExecution.h"
#include "core/PeriphExecManager.h"
#endif
#include "systems/LoggerSystem.h"
#include "core/FeatureBudget.h"
#include "core/RuntimeFeatureManager.h"

#include <IRrecv.h>
#include <IRutils.h>
#include <ArduinoJson.h>
#include <new>
#include <cstring>
#include <utility>

namespace {
struct IrGuard {
    SemaphoreHandle_t mutex;
    bool held;
    explicit IrGuard(SemaphoreHandle_t value, TickType_t wait = 0)
        : mutex(value), held(value && xSemaphoreTake(value, wait) == pdTRUE) {}
    ~IrGuard() { if (held) xSemaphoreGive(mutex); }
};
struct IrMutation {
    std::atomic_flag& flag;
    bool held;
    explicit IrMutation(std::atomic_flag& value) : flag(value), held(!flag.test_and_set(std::memory_order_acquire)) {}
    ~IrMutation() { if (held) flag.clear(std::memory_order_release); }
};
}

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
    instance.ensureMutex();
    static const FastBee::FeatureInstanceObserver observer{"irRemote", [](const void* context) {
        return static_cast<const IRRemoteDriver*>(context)->observeFeatureInstance();
    }, &instance};
    FastBee::FeatureInstanceRegistry::instance().registerObserver(&observer);
    return instance;
}

bool IRRemoteDriver::ensureMutex() {
    if (_mutex.load(std::memory_order_acquire)) return true;
    const auto created = xSemaphoreCreateMutex();
    if (!created) return false;
    SemaphoreHandle_t expected = nullptr;
    if (!_mutex.compare_exchange_strong(expected, created, std::memory_order_acq_rel,
                                         std::memory_order_acquire)) vSemaphoreDelete(created);
    return true;
}

FastBee::FeatureInstanceSnapshot IRRemoteDriver::observeFeatureInstance() const {
    using namespace FastBee;
    const auto mutex = _mutex.load(std::memory_order_acquire);
    if (!mutex) return FeatureInstanceSnapshot::unavailable(
        FeatureInstanceReason::SynchronizationUnavailable, FeatureInstanceScope::ServiceLifecycle);
    if (xSemaphoreTake(mutex, 0) != pdTRUE)
        return FeatureInstanceSnapshot::lifecycle(FeatureInstanceState::Busy);
    FeatureInstanceSnapshot snapshot;
    if (_initialized && (!_irRecv || !_results)) snapshot = FeatureInstanceSnapshot::unavailable(
        FeatureInstanceReason::InvalidSnapshot, FeatureInstanceScope::ServiceLifecycle);
    else snapshot = FeatureInstanceSnapshot::lifecycle(_initialized ? FeatureInstanceState::Running :
        (_irRecv || _results) ? FeatureInstanceState::Allocated : FeatureInstanceState::Stopped);
    xSemaphoreGive(mutex);
    return snapshot;
}

IRRemoteDriver::~IRRemoteDriver() {
    stop();
}

bool IRRemoteDriver::begin(uint8_t recvPin, uint16_t bufferSize) {
    if (!_mutex) return false;
    if (!FastBee::RuntimeFeatureManager::instance().shouldEnable("irRemote")) {
        LOGGER.info("[IR] Compiled but runtime-disabled, skip init");
        return false;
    }
    if (FastBee::FeatureBudgetRegistry::instance().isDenied("irRemote")) {
        LOGGER.warning("[IR] Disabled by memory budget, skip init");
        return false;
    }
    // Never acquire the rules/storage lock while holding the receiver lock.
    loadKeyMap(); // An unavailable early-boot configuration is retried by check().
    if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
    if (_stopping || _dispatching) { if (_mutex) xSemaphoreGive(_mutex); return false; }
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

    LOGGER.infof("[IR] Receiver initialized on pin %d (buffer=%d, keys=%d)",
                 recvPin, bufferSize, (int)_keyMap.size());
    if (_mutex) xSemaphoreGive(_mutex);
    return true;
}

void IRRemoteDriver::check() {
    if (!_mutex.load(std::memory_order_acquire)) return;
    // 非阻塞获取锁：若 stop()/begin() 正在执行则跳过本轮
    if (_mutex && xSemaphoreTake(_mutex, 0) != pdTRUE) return;
    if (!_initialized || !_irRecv || !_results || _stopping || _dispatching) {
        if (_mutex) xSemaphoreGive(_mutex);
        return;
    }

    // 学习超时检测（即使无解码也需推进状态）
    _updateLearnTimeout();

    // 延迟加载按键映射：IR 外设可能在 PeriphExecManager 初始化前 begin()，
    // 首次读到空表时保持未加载标记，待配置管理器就绪后补加载一次
    if (!_keyMapLoaded) {
        if (_mutex) xSemaphoreGive(_mutex);
        loadKeyMap();
        if (xSemaphoreTake(_mutex, 0) != pdTRUE) return;
        if (!_initialized || !_irRecv || !_results || _stopping || _dispatching) {
            xSemaphoreGive(_mutex); return;
        }
    }

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
        String keyName = _resolveKeyNameLocked(value, protocol);
        String eventData = "{\"protocol\":\"" + protocol +
                           "\",\"code\":\"" + hex +
                           "\",\"bits\":" + String(results->bits);
        if (keyName.length() > 0) {
            eventData += ",\"key\":\"" + keyName + "\"";  // 按键名已校验为安全字符
        }
        eventData += "}";
        // Reserve this exact decode until dispatch returns. stop() refuses to
        // report quiescence or allow restart while this reservation exists.
        recv->resume();
        _dispatching = true;
        if (_mutex) xSemaphoreGive(_mutex);
        struct DispatchTerminal {
            IRRemoteDriver& owner;
            ~DispatchTerminal() {
                IrGuard lock(owner._mutex.load(std::memory_order_acquire), portMAX_DELAY);
                if (lock.held) owner._dispatching = false;
            }
        } terminal{*this};
        PeriphExecManager::getInstance().dispatchEventMatchedRules("ir_code_received", eventData);
        return;
#endif

        recv->resume();
    }
    if (_mutex) xSemaphoreGive(_mutex);
}

bool IRRemoteDriver::stop() {
    IrGuard lock(_mutex.load(std::memory_order_acquire), portMAX_DELAY);
    if (!lock.held) return false;
    if (_dispatching) { _stopping = true; return false; }
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
    _learnDeadline = 0; _learnedCode = ""; _learnedProtocol = ""; _learnedValue = 0;
    _stopping = false;
    LOGGER.info("[IR] Receiver stopped");
    return true;
}

// ============ 学习模式 ============

bool IRRemoteDriver::startLearn(uint32_t timeoutMs) {
    IrGuard lock(_mutex.load(std::memory_order_acquire));
    if (!lock.held || !_initialized || !_irRecv || !_results || _stopping || _dispatching) return false;
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
    return true;
}

bool IRRemoteDriver::stopLearn() {
    IrGuard lock(_mutex.load(std::memory_order_acquire));
    if (!lock.held) return false;
    if (_learnState == IrLearnState::LEARNING) {
        _learnState = IrLearnState::IDLE;
        LOGGER.info("[IR] Learn mode cancelled");
    }
    return true;
}

void IRRemoteDriver::_updateLearnTimeout() {
    if (_learnState == IrLearnState::LEARNING &&
        (int32_t)(millis() - _learnDeadline) >= 0) {
        _learnState = IrLearnState::TIMEOUT;
        LOGGER.info("[IR] Learn timeout");
    }
}

IrLearnState IRRemoteDriver::getLearnState() {
    IrStatusSnapshot snapshot;
    return getSnapshot(snapshot) ? snapshot.learnState : IrLearnState::IDLE;
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
    IrStatusSnapshot snapshot;
    return getSnapshot(snapshot) ? snapshot.remainingMs : 0;
}

bool IRRemoteDriver::getSnapshot(IrStatusSnapshot& snapshot) const {
    snapshot = {};
    IrGuard lock(_mutex.load(std::memory_order_acquire));
    if (!lock.held) return false;
    try {
    IrStatusSnapshot value;
    value.initialized = _initialized && !_stopping; value.stopping = _stopping;
    value.keysLoaded = _keyMapLoaded;
    value.learnState = _learnState; value.keyCount = _keyMap.size(); value.lastValue = _lastValue;
    value.lastCode = _lastCode; value.lastProtocol = _lastProtocol;
    value.learnedCode = _learnedCode; value.learnedProtocol = _learnedProtocol;
    if (value.lastCode != _lastCode || value.lastProtocol != _lastProtocol ||
        value.learnedCode != _learnedCode || value.learnedProtocol != _learnedProtocol) return false;
    if (value.stopping) value.learnState = IrLearnState::IDLE;
    if (value.learnState == IrLearnState::LEARNING) {
        const int32_t remaining = static_cast<int32_t>(static_cast<uint32_t>(_learnDeadline) - static_cast<uint32_t>(millis()));
        if (remaining > 0) value.remainingMs = static_cast<uint32_t>(remaining);
        else value.learnState = IrLearnState::TIMEOUT;
    }
    snapshot = std::move(value);
    return true;
    } catch (const std::bad_alloc&) { return false; }
}
String IRRemoteDriver::getLastCode() const { IrStatusSnapshot s; return getSnapshot(s) ? s.lastCode : String(); }
String IRRemoteDriver::getLastProtocol() const { IrStatusSnapshot s; return getSnapshot(s) ? s.lastProtocol : String(); }
uint64_t IRRemoteDriver::getLastValue() const { IrStatusSnapshot s; return getSnapshot(s) ? s.lastValue : 0; }
bool IRRemoteDriver::isInitialized() const { IrStatusSnapshot s; return getSnapshot(s) && s.initialized; }
bool IRRemoteDriver::isLearning() const { IrStatusSnapshot s; return getSnapshot(s) && s.learnState == IrLearnState::LEARNING; }
String IRRemoteDriver::getLearnedCode() const { IrStatusSnapshot s; return getSnapshot(s) ? s.learnedCode : String(); }
String IRRemoteDriver::getLearnedProtocol() const { IrStatusSnapshot s; return getSnapshot(s) ? s.learnedProtocol : String(); }
size_t IRRemoteDriver::getKeyCount() const { IrStatusSnapshot s; return getSnapshot(s) ? s.keyCount : 0; }

// ============ 按键映射 ============

#if FASTBEE_ENABLE_PERIPH_EXEC
static bool parseIrKeys(const String& json, std::vector<IrKeyEntry>& keys) {
    keys.clear();
    if (json.isEmpty()) return true;
    JsonDocument doc;
    if (deserializeJson(doc, json.c_str(), json.length()) || doc.overflowed() || !doc.is<JsonArray>() || doc.size() > 40) return false;
    for (JsonVariantConst item : doc.as<JsonArrayConst>()) {
        if (!item.is<JsonObjectConst>() || !item["name"].is<const char*>() || !item["code"].is<const char*>() ||
            (!item["protocol"].isNull() && !item["protocol"].is<const char*>())) return false;
        const JsonString n = item["name"].as<JsonString>(), c = item["code"].as<JsonString>();
        const JsonString p = item["protocol"].as<JsonString>();
        if (n.size() != std::strlen(n.c_str()) || c.size() != std::strlen(c.c_str()) ||
            (p.c_str() && (p.size() > 32 || p.size() != std::strlen(p.c_str())))) return false;
        String name(n.c_str()), code(c.c_str()), proto(p.c_str() ? p.c_str() : "");
        uint64_t value = 0;
        if (name.length() != n.size() || code.length() != c.size() || proto.length() != p.size() ||
            !isValidKeyName(name) || !parseIrCode(code, value)) return false;
        for (const auto& entry : keys) if (entry.name == name) return false;
        keys.push_back(IrKeyEntry{std::move(name), value, std::move(proto)});
    }
    return true;
}

#endif // FASTBEE_ENABLE_PERIPH_EXEC
String IRRemoteDriver::resolveKeyName(uint64_t value, const String& protocol) const {
    IrGuard lock(_mutex.load(std::memory_order_acquire));
    return lock.held ? _resolveKeyNameLocked(value, protocol) : String();
}
String IRRemoteDriver::_resolveKeyNameLocked(uint64_t value, const String& protocol) const {
    for (const auto& e : _keyMap) {
        if (e.value != value) continue;
        if (e.protocol.length() > 0 && !e.protocol.equalsIgnoreCase(protocol)) continue;
        return e.name;
    }
    return "";
}

static bool serializeIrKeys(const std::vector<IrKeyEntry>& keys, String& out) {
    out = "";
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& e : keys) {
        JsonObject o = arr.add<JsonObject>();
        o["name"] = e.name;
        o["code"] = codeToHex(e.value);
        o["protocol"] = e.protocol;
    }
    if (doc.overflowed()) return false;
    const size_t expected = measureJson(arr);
    return serializeJson(arr, out) == expected && out.length() == expected;
}

bool IRRemoteDriver::loadKeyMap() {
#if !FASTBEE_ENABLE_PERIPH_EXEC
    return false; // No rule-config service is linked; receiver/learning remain usable.
#else
    IrMutation mutation(_keyMutation);
    if (!mutation.held) return false;
    try {
        String source;
        std::vector<IrKeyEntry> candidate;
        if (!PeriphExecManager::getInstance().copyIrKeysJson(source) || !parseIrKeys(source, candidate)) return false;
        IrGuard lock(_mutex.load(std::memory_order_acquire), portMAX_DELAY);
        if (!lock.held) return false;
        _keyMap.swap(candidate); _keyMapLoaded = true;
        return true;
    } catch (const std::bad_alloc&) { return false; }
#endif
}
bool IRRemoteDriver::getKeySnapshot(String& json, size_t& count) const {
    json = ""; count = 0;
    IrGuard lock(_mutex.load(std::memory_order_acquire));
    if (!lock.held || !_keyMapLoaded) return false;
    try {
        if (!serializeIrKeys(_keyMap, json)) { json = ""; return false; }
        count = _keyMap.size(); return true;
    } catch (const std::bad_alloc&) { json = ""; return false; }
}
String IRRemoteDriver::getKeysJson() const {
    String json; size_t count = 0; getKeySnapshot(json, count); return json;
}

bool IRRemoteDriver::upsertKey(const String& name, const String& code,
                               const String& protocol, String& err) {
    err = "";
    if (!isValidKeyName(name)) {
        err = "按键名非法（仅允许字母数字下划线短横线，1-32 字符）";
        return false;
    }
    uint64_t value = 0;
    if (!parseIrCode(code, value)) {
        err = "编码格式非法（应为十六进制，如 0x00FF6897）";
        return false;
    }
    if (protocol.length() > 32 || protocol.length() != std::strlen(protocol.c_str())) {
        err = "IR_KEY_PROTOCOL_INVALID"; return false;
    }
    return _editKeyMap(name, value, protocol, 0, err);
}

bool IRRemoteDriver::deleteKey(const String& name, String* error) {
    String ignored;
    return _editKeyMap(name, 0, "", 1, error ? *error : ignored);
}

bool IRRemoteDriver::clearKeys(String* error) {
    String ignored;
    return _editKeyMap("", 0, "", 2, error ? *error : ignored);
}

bool IRRemoteDriver::_editKeyMap(const String& name, uint64_t value, const String& protocol,
                                 uint8_t operation, String& error) {
#if !FASTBEE_ENABLE_PERIPH_EXEC
    (void)name; (void)value; (void)protocol; (void)operation;
    error = "IR_KEYS_STORE_DISABLED"; return false;
#else
    error = "";
    if (operation != 2 && !isValidKeyName(name)) { error = "IR_KEY_NAME_INVALID"; return false; }
    if (!_mutex.load(std::memory_order_acquire)) { error = "IR_UNAVAILABLE"; return false; }
    IrMutation mutation(_keyMutation);
    if (!mutation.held) { error = "IR_KEYS_BUSY"; return false; }
    try {
        String source, json;
        std::vector<IrKeyEntry> candidate;
        auto& manager = PeriphExecManager::getInstance();
        if (!manager.copyIrKeysJson(source) || !parseIrKeys(source, candidate)) {
            error = "IR_KEYS_SOURCE_UNCONFIRMED"; return false;
        }
        size_t found = 0;
        while (found < candidate.size() && candidate[found].name != name) ++found;
        if (operation == 2) candidate.clear();
        else if (operation == 1) {
            if (found == candidate.size()) { error = "IR_KEY_NOT_FOUND"; return false; }
            candidate.erase(candidate.begin() + found);
        } else {
            IrKeyEntry entry{name, value, protocol};
            if (entry.name != name || entry.protocol != protocol) { error = "IR_KEYS_MEMORY"; return false; }
            if (found < candidate.size()) candidate[found] = std::move(entry);
            else {
                if (candidate.size() >= MAX_IR_KEYS) { error = "IR_KEYS_LIMIT"; return false; }
                candidate.push_back(std::move(entry));
            }
        }
        if (!candidate.empty() && !serializeIrKeys(candidate, json)) { error = "IR_KEYS_MEMORY"; return false; }
        // The same rules lock compares the observed value and commits the
        // existing atomic configuration writer. Imports cannot be overwritten.
        if (!manager.setIrKeysJson(json, &source)) { error = "IR_KEYS_COMMIT_UNCONFIRMED"; return false; }
        IrGuard lock(_mutex.load(std::memory_order_acquire), portMAX_DELAY);
        if (!lock.held) { error = "IR_KEYS_PUBLISH_UNCONFIRMED"; return false; }
        _keyMap.swap(candidate); _keyMapLoaded = true;
        return true;
    } catch (const std::bad_alloc&) { error = "IR_KEYS_MEMORY"; return false; }
#endif
}

#endif // FASTBEE_ENABLE_IR_REMOTE
