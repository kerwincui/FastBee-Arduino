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
#include "utils/FileUtils.h"
#include "utils/ConsumedStringJsonReader.h"
#include "utils/SharedJsonDeserializer.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <cstring>
#include <new>
#include <utility>
#if FASTBEE_ENABLE_PERIPH_EXEC
#include "core/PeriphExecManager.h"
#endif

volatile uint32_t RFLearnDriver::_isrLastEdgeTime = 0;
volatile uint16_t RFLearnDriver::_isrPulseBuffer[RFLearnDriver::ISR_BUFFER_SIZE];
volatile uint8_t RFLearnDriver::_isrPulseCount = 0;
volatile bool RFLearnDriver::_isrFrameReady = false;
namespace {
const char* RF_KEYS_FILE = "/config/rf_keys.json";
constexpr size_t RF_KEYS_MAX_BYTES = 8192;
portMUX_TYPE rfIsrMux = portMUX_INITIALIZER_UNLOCKED;
bool rfIsrEnabled = false;
uint32_t rfIsrGeneration = 0;
struct RfGuard {
    SemaphoreHandle_t mutex; bool held;
    explicit RfGuard(SemaphoreHandle_t value, TickType_t wait = 0)
        : mutex(value), held(value && xSemaphoreTake(value, wait) == pdTRUE) {}
    void release() { if (held) { xSemaphoreGive(mutex); held = false; } }
    ~RfGuard() { release(); }
};
struct RfMutation {
    std::atomic_flag& flag; bool held;
    explicit RfMutation(std::atomic_flag& value) : flag(value), held(!flag.test_and_set(std::memory_order_acquire)) {}
    ~RfMutation() { if (held) flag.clear(std::memory_order_release); }
};
bool validRfName(const String& name) {
    if (name.isEmpty() || name.length() > 32) return false;
    for (size_t i = 0; i < name.length(); ++i) {
        const char c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ' ') continue;
        return false;
    }
    return true;
}
bool parseRfKeyCode(const String& code, uint32_t& value) {
    size_t at = code.startsWith("0x") || code.startsWith("0X") ? 2 : 0;
    if (at == code.length() || code.length() - at > 8) return false;
    value = 0;
    for (; at < code.length(); ++at) {
        const char c = code[at];
        unsigned digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;
        value = (value << 4) | digit;
    }
    return true;
}
bool readRfKeyFile(const char* path, String& raw, bool& exists) {
    raw = ""; exists = false;
    if (!FileUtils::isInitialized()) return false;
    exists = LittleFS.exists(path);
    if (!exists) return true;
    File file = LittleFS.open(path, "r");
    if (!file) return false;
    const size_t size = file.size();
    if (!size || size > RF_KEYS_MAX_BYTES || !raw.reserve(size + 1)) { file.close(); return false; }
    for (size_t i = 0; i < size; ++i) {
        const int next = file.read();
        if (next < 0 || !raw.concat(static_cast<char>(next))) { file.close(); return false; }
    }
    const bool complete = file.available() == 0 && raw.length() == size && raw.length() == std::strlen(raw.c_str());
    file.close(); return complete;
}
using RfJsonReader = FastBee::ConsumedStringJsonReader;
bool parseRfKeys(const String& raw, std::vector<RfKeyEntry>& keys) {
    keys.clear(); if (raw.isEmpty()) return true;
    JsonDocument doc; RfJsonReader reader{raw};
    if (FastBee::deserializeJsonShared(doc, reader) || !reader.onlyWhitespaceRemaining() ||
        doc.overflowed() || !doc.is<JsonArray>() || doc.size() > 40) return false;
    for (JsonVariantConst item : doc.as<JsonArrayConst>()) {
        if (!item.is<JsonObjectConst>() || !item["name"].is<const char*>() || !item["code"].is<const char*>()) return false;
        const JsonString name = item["name"].as<JsonString>(), code = item["code"].as<JsonString>();
        if (name.size() != std::strlen(name.c_str()) || code.size() != std::strlen(code.c_str())) return false;
        if ((!item["bits"].isNull() && !item["bits"].is<uint8_t>()) ||
            (!item["pulse"].isNull() && !item["pulse"].is<uint16_t>())) return false;
        const String codeText(code.c_str());
        RfKeyEntry entry{String(name.c_str()), 0, item["bits"] | uint8_t(24), item["pulse"] | uint16_t(350)};
        if (entry.name.length() != name.size() || !validRfName(entry.name) ||
            codeText.length() != code.size() || !parseRfKeyCode(codeText, entry.code) || entry.bitLength < 12 || entry.bitLength > 32 ||
            entry.pulseWidth < 80 || entry.pulseWidth > 2000 ||
            (entry.bitLength < 32 && (entry.code >> entry.bitLength))) return false;
        for (const auto& old : keys) if (old.name == entry.name) return false;
        keys.push_back(std::move(entry));
    }
    return true;
}
bool serializeRfKeys(const std::vector<RfKeyEntry>& keys, String& output) {
    output = ""; JsonDocument doc; JsonArray array = doc.to<JsonArray>();
    for (const auto& entry : keys) {
        auto object = array.add<JsonObject>();
        object["name"] = entry.name;
        char code[12]; snprintf(code, sizeof(code), "0x%06lX", static_cast<unsigned long>(entry.code));
        object["code"] = code; object["bits"] = entry.bitLength; object["pulse"] = entry.pulseWidth;
    }
    if (doc.overflowed()) return false;
    const size_t expected = measureJson(doc);
    return expected <= RF_KEYS_MAX_BYTES && serializeJson(doc, output) == expected && output.length() == expected;
}
}

RFLearnDriver& RFLearnDriver::getInstance() {
    static RFLearnDriver instance;
    instance.ensureMutex();
    static const FastBee::FeatureInstanceObserver observer{"rfLearn", [](const void* context) {
        return static_cast<const RFLearnDriver*>(context)->observeFeatureInstance();
    }, &instance};
    FastBee::FeatureInstanceRegistry::instance().registerObserver(&observer);
    return instance;
}
bool RFLearnDriver::ensureMutex() {
    if (_mutex.load(std::memory_order_acquire)) return true;
    const auto created = xSemaphoreCreateMutex();
    if (!created) return false;
    SemaphoreHandle_t expected = nullptr;
    if (!_mutex.compare_exchange_strong(expected, created, std::memory_order_acq_rel,
                                         std::memory_order_acquire)) vSemaphoreDelete(created);
    return true;
}
FastBee::FeatureInstanceSnapshot RFLearnDriver::observeFeatureInstance() const {
    using namespace FastBee;
    const auto mutex = _mutex.load(std::memory_order_acquire);
    if (!mutex) return FeatureInstanceSnapshot::unavailable(
        FeatureInstanceReason::SynchronizationUnavailable, FeatureInstanceScope::ServiceLifecycle);
    if (xSemaphoreTake(mutex, 0) != pdTRUE)
        return FeatureInstanceSnapshot::lifecycle(FeatureInstanceState::Busy);
    const auto snapshot = FeatureInstanceSnapshot::lifecycle(
        _initialized ? FeatureInstanceState::Running : FeatureInstanceState::Stopped);
    xSemaphoreGive(mutex); return snapshot;
}
RFLearnDriver::~RFLearnDriver() { stop(); }
void IRAM_ATTR RFLearnDriver::_isrHandler(void* generation) {
    portENTER_CRITICAL_ISR(&rfIsrMux);
    do {
        if (!rfIsrEnabled || static_cast<uint32_t>(reinterpret_cast<uintptr_t>(generation)) != rfIsrGeneration) break;
        const uint32_t now = micros();
        uint32_t duration = now - _isrLastEdgeTime; _isrLastEdgeTime = now;
        if (_isrFrameReady || duration < 50) break;
        if (duration > 65000) duration = 65000;
        if (duration > SYNC_MIN_US && _isrPulseCount >= MIN_DATA_BITS * 2) { _isrFrameReady = true; break; }
        if (duration > SYNC_MIN_US) { _isrPulseCount = 0; break; }
        if (_isrPulseCount < ISR_BUFFER_SIZE) _isrPulseBuffer[_isrPulseCount++] = static_cast<uint16_t>(duration);
        else _isrFrameReady = true;
    } while (false);
    portEXIT_CRITICAL_ISR(&rfIsrMux);
}
bool RFLearnDriver::begin(uint8_t rxPin) {
    if (!_mutex || !FastBee::RuntimeFeatureManager::instance().shouldEnable("rfLearn")) return false;
    loadKeyMap(); // Storage is never called while holding the receiver lock.
    RfGuard guard(_mutex.load(std::memory_order_acquire), portMAX_DELAY);
    if (!guard.held || _stopping || _dispatching) return false;
    if (_initialized) return _rxPin == rxPin; // Never steal the singleton from another configured pin.
    portENTER_CRITICAL(&rfIsrMux);
    if (rfIsrGeneration == UINT32_MAX) { portEXIT_CRITICAL(&rfIsrMux); return false; }
    const uint32_t generation = ++rfIsrGeneration;
    rfIsrEnabled = false; _isrPulseCount = 0; _isrFrameReady = false; _isrLastEdgeTime = micros();
    portEXIT_CRITICAL(&rfIsrMux);
    _rxPin = rxPin; pinMode(rxPin, INPUT);
    attachInterruptArg(digitalPinToInterrupt(rxPin), _isrHandler,
                       reinterpret_cast<void*>(static_cast<uintptr_t>(generation)), CHANGE);
    portENTER_CRITICAL(&rfIsrMux); rfIsrEnabled = true; portEXIT_CRITICAL(&rfIsrMux);
    _initialized = true; _lastCode = ""; _lastValue = 0; _lastBitLength = 0; _lastPulseWidth = 0; _lastEventTime = 0;
    _learnState = RfLearnState::IDLE; _learnDeadline = 0;
    return true; // Arduino attach API has no SDK acknowledgement; this is not physical reception evidence.
}
bool RFLearnDriver::stop(uint8_t expectedPin) {
    RfGuard guard(_mutex.load(std::memory_order_acquire), portMAX_DELAY);
    if (!guard.held) return false;
    if (_initialized && expectedPin != 255 && expectedPin != _rxPin) return false;
    if (_dispatching) { _stopping = true; return false; }
    portENTER_CRITICAL(&rfIsrMux);
    rfIsrEnabled = false; _isrPulseCount = 0; _isrFrameReady = false;
    portEXIT_CRITICAL(&rfIsrMux);
    if (_initialized) detachInterrupt(digitalPinToInterrupt(_rxPin));
    _initialized = false; _stopping = false; _rxPin = 255;
    _learnState = RfLearnState::IDLE; _learnDeadline = 0; _learnedCode = ""; _learnedBitLength = 0; _learnedPulseWidth = 0;
    _lastCode = ""; _lastValue = 0; _lastBitLength = 0; _lastPulseWidth = 0;
    return true;
}
void RFLearnDriver::check() {
    RfGuard guard(_mutex.load(std::memory_order_acquire));
    if (!guard.held || !_initialized || _stopping || _dispatching) return;
    _updateLearnTimeout();
    uint16_t localBuf[ISR_BUFFER_SIZE]; uint8_t count = 0;
    portENTER_CRITICAL(&rfIsrMux);
    if (_isrFrameReady) {
        count = _isrPulseCount;
        for (uint8_t i = 0; i < count; ++i) localBuf[i] = _isrPulseBuffer[i];
        _isrPulseCount = 0; _isrFrameReady = false;
    }
    portEXIT_CRITICAL(&rfIsrMux);
    if (count < MIN_DATA_BITS * 2) return;
    uint16_t minPulse = 65535;
    for (uint8_t i = 0; i < count; ++i) if (localBuf[i] < minPulse) minPulse = localBuf[i];
    if (minPulse < 80 || minPulse > 2000) return;
    const uint16_t T = minPulse;
    uint32_t code = 0, pulseSum = 0; uint8_t bits = 0, samples = 0;
    for (uint8_t i = 0; i + 1 < count && bits < MAX_DATA_BITS; i += 2) {
        const uint16_t high = localBuf[i], low = localBuf[i + 1];
        if (high > T * 6 || low > T * 6) continue;
        const uint8_t highRatio = (high + T / 2) / T, lowRatio = (low + T / 2) / T;
        if (highRatio >= 2 && lowRatio <= 2) code = (code << 1) | 1;
        else if (highRatio <= 2 && lowRatio >= 2) code <<= 1;
        else continue;
        ++bits; pulseSum += (high + low) / 4; ++samples;
    }
    if (bits < MIN_DATA_BITS) return;
    const uint16_t pulse = samples ? pulseSum / samples : T;
    const uint32_t now = millis();
    if (code == _lastValue && bits == _lastBitLength && uint32_t(now - _lastEventTime) < MIN_EVENT_INTERVAL_MS) return;
    char hex[12]; snprintf(hex, sizeof(hex), "0x%06lX", static_cast<unsigned long>(code));
    _lastCode = hex; _lastValue = code; _lastBitLength = bits; _lastPulseWidth = pulse; _lastEventTime = now;
    if (_learnState == RfLearnState::LEARNING) {
        _learnedCode = _lastCode; _learnedBitLength = bits; _learnedPulseWidth = pulse; _learnState = RfLearnState::CAPTURED;
        return;
    }
#if FASTBEE_ENABLE_PERIPH_EXEC
    JsonDocument doc; doc["code"] = _lastCode; doc["bits"] = bits; doc["pulse"] = pulse;
    const String key = _resolveKeyNameLocked(code, bits); if (!key.isEmpty()) doc["key"] = key;
    if (doc.overflowed()) return;
    String data; const size_t expected = measureJson(doc);
    if (serializeJson(doc, data) != expected || data.length() != expected) return;
    _dispatching = true; guard.release();
    struct Terminal {
        RFLearnDriver& driver;
        ~Terminal() { RfGuard lock(driver._mutex.load(std::memory_order_acquire), portMAX_DELAY); if (lock.held) driver._dispatching = false; }
    } terminal{*this};
    PeriphExecManager::getInstance().dispatchEventMatchedRules("rf_code_received", data);
#endif
}
bool RFLearnDriver::startLearn(uint32_t timeoutMs) {
    RfGuard guard(_mutex.load(std::memory_order_acquire));
    if (!guard.held || !_initialized || _stopping || _dispatching) return false;
    if (timeoutMs < 1000) timeoutMs = 1000;
    if (timeoutMs > 60000) timeoutMs = 60000;
    portENTER_CRITICAL(&rfIsrMux); _isrPulseCount = 0; _isrFrameReady = false; _isrLastEdgeTime = micros(); portEXIT_CRITICAL(&rfIsrMux);
    _learnState = RfLearnState::LEARNING; _learnDeadline = uint32_t(millis()) + timeoutMs;
    _learnedCode = ""; _learnedBitLength = 0; _learnedPulseWidth = 0; return true;
}
bool RFLearnDriver::stopLearn() {
    RfGuard guard(_mutex.load(std::memory_order_acquire)); if (!guard.held) return false;
    _learnState = RfLearnState::IDLE; _learnDeadline = 0; _learnedCode = ""; _learnedBitLength = 0; _learnedPulseWidth = 0; return true;
}
void RFLearnDriver::_updateLearnTimeout() {
    if (_learnState == RfLearnState::LEARNING && static_cast<int32_t>(uint32_t(millis()) - _learnDeadline) >= 0) _learnState = RfLearnState::TIMEOUT;
}
bool RFLearnDriver::getSnapshot(RfStatusSnapshot& snapshot) const {
    snapshot = {}; RfGuard guard(_mutex.load(std::memory_order_acquire)); if (!guard.held) return false;
    try {
        RfStatusSnapshot value; value.initialized = _initialized && !_stopping; value.stopping = _stopping;
        value.keysLoaded = _keyMapLoaded; value.rxPin = _rxPin; value.keyCount = _keyMap.size(); value.lastValue = _lastValue;
        value.lastCode = _lastCode; value.lastBits = _lastBitLength; value.lastPulse = _lastPulseWidth;
        value.learnedCode = _learnedCode; value.learnedBits = _learnedBitLength; value.learnedPulse = _learnedPulseWidth;
        if (value.lastCode != _lastCode || value.learnedCode != _learnedCode) return false;
        value.learnState = _stopping ? RfLearnState::IDLE : _learnState;
        if (value.learnState == RfLearnState::LEARNING) {
            const int32_t remaining = static_cast<int32_t>(_learnDeadline - uint32_t(millis()));
            if (remaining > 0) value.remainingMs = remaining; else value.learnState = RfLearnState::TIMEOUT;
        }
        snapshot = std::move(value); return true;
    } catch (const std::bad_alloc&) { return false; }
}
bool RFLearnDriver::isInitialized() const { RfStatusSnapshot s; return getSnapshot(s) && s.initialized; }
uint8_t RFLearnDriver::getRxPin() const { RfStatusSnapshot s; return getSnapshot(s) ? s.rxPin : 255; }
String RFLearnDriver::getLastCode() const { RfStatusSnapshot s; return getSnapshot(s) ? s.lastCode : String(); }
uint32_t RFLearnDriver::getLastValue() const { RfStatusSnapshot s; return getSnapshot(s) ? s.lastValue : 0; }
uint8_t RFLearnDriver::getLastBitLength() const { RfStatusSnapshot s; return getSnapshot(s) ? s.lastBits : 0; }
uint16_t RFLearnDriver::getLastPulseWidth() const { RfStatusSnapshot s; return getSnapshot(s) ? s.lastPulse : 0; }
bool RFLearnDriver::isLearning() const { RfStatusSnapshot s; return getSnapshot(s) && s.learnState == RfLearnState::LEARNING; }
RfLearnState RFLearnDriver::getLearnState() { RfStatusSnapshot s; return getSnapshot(s) ? s.learnState : RfLearnState::IDLE; }
String RFLearnDriver::getLearnStateName() {
    switch (getLearnState()) { case RfLearnState::LEARNING: return "learning"; case RfLearnState::CAPTURED: return "captured"; case RfLearnState::TIMEOUT: return "timeout"; default: return "idle"; }
}
String RFLearnDriver::getLearnedCode() const { RfStatusSnapshot s; return getSnapshot(s) ? s.learnedCode : String(); }
uint8_t RFLearnDriver::getLearnedBitLength() const { RfStatusSnapshot s; return getSnapshot(s) ? s.learnedBits : 0; }
uint16_t RFLearnDriver::getLearnedPulseWidth() const { RfStatusSnapshot s; return getSnapshot(s) ? s.learnedPulse : 0; }
uint32_t RFLearnDriver::getLearnRemainingMs() { RfStatusSnapshot s; return getSnapshot(s) ? s.remainingMs : 0; }
size_t RFLearnDriver::getKeyCount() const { RfStatusSnapshot s; return getSnapshot(s) ? s.keyCount : 0; }
String RFLearnDriver::_resolveKeyNameLocked(uint32_t code, uint8_t bits) const {
    for (const auto& entry : _keyMap) if (entry.code == code && entry.bitLength == bits) return entry.name;
    return "";
}
String RFLearnDriver::resolveKeyName(uint32_t code, uint8_t bits) const {
    RfGuard guard(_mutex.load(std::memory_order_acquire)); return guard.held ? _resolveKeyNameLocked(code, bits) : String();
}
bool RFLearnDriver::loadKeyMap() {
    RfMutation mutation(_keyMutation); if (!mutation.held) return false;
    try {
        String raw; bool exists; std::vector<RfKeyEntry> candidate;
        if (!readRfKeyFile(RF_KEYS_FILE, raw, exists) || !parseRfKeys(raw, candidate)) return false;
        RfGuard guard(_mutex.load(std::memory_order_acquire), portMAX_DELAY); if (!guard.held) return false;
        _keyMap.swap(candidate); _keyMapLoaded = true; return true;
    } catch (const std::bad_alloc&) { return false; }
}
bool RFLearnDriver::getKeySnapshot(String& json, size_t& count) const {
    json = ""; count = 0; RfGuard guard(_mutex.load(std::memory_order_acquire));
    if (!guard.held || !_keyMapLoaded) return false;
    try { if (!serializeRfKeys(_keyMap, json)) { json = ""; return false; } count = _keyMap.size(); return true; }
    catch (const std::bad_alloc&) { json = ""; return false; }
}
String RFLearnDriver::getKeysJson() const { String json; size_t count; getKeySnapshot(json, count); return json; }
bool RFLearnDriver::upsertKey(const String& name, const String& code, uint8_t bits, uint16_t pulse, String& error) {
    error = ""; uint32_t value;
    if (!validRfName(name)) { error = "RF_KEY_NAME_INVALID"; return false; }
    if (!parseRfKeyCode(code, value) || bits < MIN_DATA_BITS || bits > MAX_DATA_BITS ||
        (bits < 32 && (value >> bits)) || pulse < 80 || pulse > 2000) { error = "RF_KEY_PARAMETERS_INVALID"; return false; }
    return _editKeyMap(name, value, bits, pulse, 0, error);
}
bool RFLearnDriver::deleteKey(const String& name, String* error) { String ignored; return _editKeyMap(name, 0, 0, 0, 1, error ? *error : ignored); }
bool RFLearnDriver::clearKeys(String* error) { String ignored; return _editKeyMap("", 0, 0, 0, 2, error ? *error : ignored); }
bool RFLearnDriver::_editKeyMap(const String& name, uint32_t code, uint8_t bits, uint16_t pulse, uint8_t operation, String& error) {
    error = "";
    if (operation != 2 && !validRfName(name)) { error = "RF_KEY_NAME_INVALID"; return false; }
    if (!_mutex.load(std::memory_order_acquire)) { error = "RF_KEYS_UNAVAILABLE"; return false; }
    RfMutation mutation(_keyMutation); if (!mutation.held) { error = "RF_KEYS_BUSY"; return false; }
    try {
        String source, candidateJson; bool existed; std::vector<RfKeyEntry> candidate;
        if (!readRfKeyFile(RF_KEYS_FILE, source, existed) || !parseRfKeys(source, candidate)) { error = "RF_KEYS_SOURCE_UNCONFIRMED"; return false; }
        size_t found = 0; while (found < candidate.size() && candidate[found].name != name) ++found;
        if (operation == 2) candidate.clear();
        else if (operation == 1) {
            if (found == candidate.size()) { error = "RF_KEY_NOT_FOUND"; return false; }
            candidate.erase(candidate.begin() + found);
        } else {
            RfKeyEntry entry{name, code, bits, pulse}; if (entry.name != name) { error = "RF_KEYS_MEMORY"; return false; }
            if (found < candidate.size()) candidate[found] = std::move(entry);
            else { if (candidate.size() >= MAX_RF_KEYS) { error = "RF_KEYS_LIMIT"; return false; } candidate.push_back(std::move(entry)); }
        }
        if (!serializeRfKeys(candidate, candidateJson)) { error = "RF_KEYS_MEMORY"; return false; }
        const String temporary = String(RF_KEYS_FILE) + ".tmp";
        if (temporary != "/config/rf_keys.json.tmp") { error = "RF_KEYS_MEMORY"; return false; }
        File output = FileUtils::openAtomicWrite(RF_KEYS_FILE);
        if (!output) { error = "RF_KEYS_STORAGE_UNCONFIRMED"; return false; }
        const size_t written = output.write(reinterpret_cast<const uint8_t*>(candidateJson.c_str()), candidateJson.length());
        output.flush(); output.close();
        String verified, latest; bool stagedExists, latestExists;
        if (written != candidateJson.length() || !readRfKeyFile(temporary.c_str(), verified, stagedExists) ||
            !stagedExists || verified != candidateJson || !readRfKeyFile(RF_KEYS_FILE, latest, latestExists) ||
            latestExists != existed || latest != source) {
            LittleFS.remove(temporary); error = "RF_KEYS_VERIFY_OR_SOURCE_CONFLICT"; return false;
        }
        if (!FileUtils::commitAtomicWrite(temporary, RF_KEYS_FILE)) { error = "RF_KEYS_COMMIT_UNCONFIRMED"; return false; }
        RfGuard guard(_mutex.load(std::memory_order_acquire), portMAX_DELAY);
        if (!guard.held) { error = "RF_KEYS_PUBLISH_UNCONFIRMED"; return false; }
        _keyMap.swap(candidate); _keyMapLoaded = true; return true;
    } catch (const std::bad_alloc&) { error = "RF_KEYS_MEMORY"; return false; }
}
#endif // FASTBEE_ENABLE_RF_LEARN
