#pragma once
#include "core/FeatureFlags.h"
#include <Arduino.h>
#include <ArduinoJson.h>

struct MotorControlState {
    uint16_t speed = 0;
    uint16_t pulse = 0;
    uint8_t precision = 0;
    uint32_t updatedMs = 0;
    bool known = false;
    bool pending = false;
    char lastAction[16]{};
    char error[128]{};
    char readError[128]{};
    uint32_t operationId = 0, commandUpdatedMs = 0;
    bool commandKnown = false, commandSuccess = false;
    char lastAcknowledgedAction[16]{};
    uint32_t acknowledgedMs = 0;
    bool softLimitEnabled = false;
    bool referenceKnown = false, stationaryConfirmed = false;
    bool forwardReady = false, reverseReady = false;
    int32_t possiblePositionMin = 0, possiblePositionMax = 0;
    char travelToken[24]{}, travelReason[64]{};
    int32_t minPosition = 0, maxPosition = 0, estimatedPosition = 0;
};

// One bounded command worker shared by the dashboard and physical panel.
// References bind the slave address AND configuration fingerprint, never a
// mutable index into the device list. A command acknowledgement is not motion.
class MotorControlService {
public:
    static MotorControlService& instance();
    void appendChoices(JsonArray out);
    bool validReference(const String& ref);
    bool submit(const String& ref, const String& action, int value,
                const String& requestId, uint32_t& operationId, String& error);
    bool operation(uint32_t id, JsonObject out, bool* busy = nullptr);
    // Execution rules retain their legacy register value / bounded-pulse
    // semantics, but share the same scheduler, stop priority and result path.
    bool submitRule(const String& ref, const String& action, int value,
                    const String& requestId, uint32_t budgetMs, uint16_t timeoutMs,
                    uint32_t& operationId, String& error);
    bool state(const String& ref, MotorControlState& out);
    bool confirmTravel(const String& ref, const String& token, bool reference,
                       int32_t position, bool stationary, String& error);
    String referenceForSlave(uint8_t slave);
private:
    MotorControlService() = default;
    bool submitImpl(const String& ref, const String& action, int value,
                    const String& requestId, uint32_t budgetMs, uint16_t timeoutMs,
                    bool legacyValue, uint32_t& operationId, String& error);
};

#if !FASTBEE_ENABLE_MODBUS || !defined(ARDUINO_ARCH_ESP32)
inline MotorControlService& MotorControlService::instance() { static MotorControlService s; return s; }
inline void MotorControlService::appendChoices(JsonArray) {}
inline bool MotorControlService::validReference(const String&) { return false; }
inline bool MotorControlService::submit(const String&, const String&, int, const String&, uint32_t&, String& error) { error = "Motor control unavailable"; return false; }
inline bool MotorControlService::operation(uint32_t, JsonObject, bool* busy) { if (busy) *busy = true; return false; }
inline bool MotorControlService::submitRule(const String&, const String&, int, const String&, uint32_t, uint16_t, uint32_t&, String& error) { error = "Motor control unavailable"; return false; }
inline bool MotorControlService::state(const String&, MotorControlState&) { return false; }
inline bool MotorControlService::confirmTravel(const String&, const String&, bool, int32_t, bool, String& error) { error = "Motor control unavailable"; return false; }
inline String MotorControlService::referenceForSlave(uint8_t) { return String(); }
#endif
