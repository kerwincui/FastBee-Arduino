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
    char error[64]{};
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
    bool operation(uint32_t id, JsonObject out);
    // Execution rules retain their legacy register value / bounded-pulse
    // semantics, but share the same scheduler, stop priority and result path.
    bool submitRule(const String& ref, const String& action, int value,
                    const String& requestId, uint32_t budgetMs, uint16_t timeoutMs,
                    uint32_t& operationId, String& error);
    bool state(const String& ref, MotorControlState& out);
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
inline bool MotorControlService::operation(uint32_t, JsonObject) { return false; }
inline bool MotorControlService::submitRule(const String&, const String&, int, const String&, uint32_t, uint16_t, uint32_t&, String& error) { error = "Motor control unavailable"; return false; }
inline bool MotorControlService::state(const String&, MotorControlState&) { return false; }
inline String MotorControlService::referenceForSlave(uint8_t) { return String(); }
#endif
