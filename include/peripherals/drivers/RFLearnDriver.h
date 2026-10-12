#ifndef RF_LEARN_DRIVER_H
#define RF_LEARN_DRIVER_H
#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_RF_LEARN
#include <Arduino.h>
#include <atomic>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "core/FeatureInstanceRegistry.h"

// GPIO OOK receiver/learning only; TX remains in PeripheralManagerRf.
// Key mapping uses the existing /config/rf_keys.json array.
enum class RfLearnState : uint8_t { IDLE = 0, LEARNING = 1, CAPTURED = 2, TIMEOUT = 3 };
struct RfKeyEntry { String name; uint32_t code; uint8_t bitLength; uint16_t pulseWidth; };
struct RfStatusSnapshot {
    bool initialized = false, stopping = false, keysLoaded = false;
    uint8_t rxPin = 255, lastBits = 0, learnedBits = 0;
    uint16_t lastPulse = 0, learnedPulse = 0;
    uint32_t lastValue = 0, remainingMs = 0;
    size_t keyCount = 0;
    RfLearnState learnState = RfLearnState::IDLE;
    String lastCode, learnedCode;
};
class RFLearnDriver {
public:
    static RFLearnDriver& getInstance();
    FastBee::FeatureInstanceSnapshot observeFeatureInstance() const;
    bool begin(uint8_t rxPin);
    // False retains receiver ownership while dispatch is in flight or identity differs.
    bool stop(uint8_t expectedPin = 255);
    void check();
    bool getSnapshot(RfStatusSnapshot& snapshot) const;
    bool isInitialized() const;
    uint8_t getRxPin() const;
    String getLastCode() const;
    uint32_t getLastValue() const;
    uint8_t getLastBitLength() const;
    uint16_t getLastPulseWidth() const;
    bool startLearn(uint32_t timeoutMs = 10000);
    bool stopLearn();
    bool isLearning() const;
    RfLearnState getLearnState();
    String getLearnStateName();
    String getLearnedCode() const;
    uint8_t getLearnedBitLength() const;
    uint16_t getLearnedPulseWidth() const;
    uint32_t getLearnRemainingMs();
    bool loadKeyMap();
    size_t getKeyCount() const;
    String resolveKeyName(uint32_t code, uint8_t bitLength) const;
    String getKeysJson() const;
    bool getKeySnapshot(String& json, size_t& count) const;
    bool upsertKey(const String& name, const String& code, uint8_t bitLength, uint16_t pulseWidth, String& error);
    bool deleteKey(const String& name, String* error = nullptr);
    bool clearKeys(String* error = nullptr);
    static volatile uint32_t _isrLastEdgeTime;
    static volatile uint16_t _isrPulseBuffer[];
    static volatile uint8_t _isrPulseCount;
    static volatile bool _isrFrameReady;
private:
    RFLearnDriver() = default;
    ~RFLearnDriver();
    RFLearnDriver(const RFLearnDriver&) = delete;
    RFLearnDriver& operator=(const RFLearnDriver&) = delete;
    static void IRAM_ATTR _isrHandler(void* generation);
    void _updateLearnTimeout();
    bool ensureMutex();
    String _resolveKeyNameLocked(uint32_t code, uint8_t bits) const;
    bool _editKeyMap(const String& name, uint32_t code, uint8_t bits, uint16_t pulse,
                     uint8_t operation, String& error);
    uint8_t _rxPin = 255;
    bool _initialized = false, _stopping = false, _dispatching = false;
    std::atomic<SemaphoreHandle_t> _mutex{nullptr};
    std::atomic_flag _keyMutation = ATOMIC_FLAG_INIT;
    String _lastCode;
    uint32_t _lastValue = 0;
    uint8_t _lastBitLength = 0;
    uint16_t _lastPulseWidth = 0;
    unsigned long _lastEventTime = 0;
    RfLearnState _learnState = RfLearnState::IDLE;
    uint32_t _learnDeadline = 0;
    String _learnedCode;
    uint8_t _learnedBitLength = 0;
    uint16_t _learnedPulseWidth = 0;
    std::vector<RfKeyEntry> _keyMap;
    bool _keyMapLoaded = false;
    static constexpr unsigned long MIN_EVENT_INTERVAL_MS = 300;
    static constexpr size_t MAX_RF_KEYS = 40;
    static constexpr uint8_t ISR_BUFFER_SIZE = 128;
    static constexpr uint16_t SYNC_MIN_US = 3000;
    static constexpr uint8_t MIN_DATA_BITS = 12, MAX_DATA_BITS = 32;
};
#endif // FASTBEE_ENABLE_RF_LEARN
#endif // RF_LEARN_DRIVER_H
