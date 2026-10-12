#include "protocols/MotorProtocol.h"
#include <cassert>
#include <string>

int main() {
    const char* generic = R"({"motorRegs":[100,100,100,101,102],"motorProtocol":{
        "version":1,"readFunction":4,"writeFunction":16,"speedReadRegister":0,
        "pulseReadRegister":65535,"forwardValue":1,"reverseValue":2,"stopValue":0,"finitePulseMove":true}})";
    JsonDocument doc;
    MotorProtocol profile;
    assert(!deserializeJson(doc, "{}"));
    assert(!loadMotorProtocol(doc.as<JsonObjectConst>(), profile) && !profile.configured && profile.valid);
    assert(!deserializeJson(doc, generic));
    assert(!loadMotorProtocol(doc.as<JsonObjectConst>(), profile) && profile.valid);
    assert(profile.readFunction == 4 && profile.writeFunction == 16 && profile.stopValue == 0);
    assert(profile.speedReadRegister == 0 && profile.pulseReadRegister == 65535);
    JsonDocument saved;
    saved["motorRegs"].set(doc["motorRegs"]);
    saveMotorProtocol(saved.as<JsonObject>(), profile);
    MotorProtocol restored;
    assert(!loadMotorProtocol(saved.as<JsonObjectConst>(), restored) && restored.stopValue == 0);
    assert(saved["motorProtocol"] == doc["motorProtocol"]);
    doc["motorProtocol"]["positionTracking"] = "commanded";
    assert(!loadMotorProtocol(doc.as<JsonObjectConst>(), profile) && profile.commandPosition);
    saveMotorProtocol(saved.as<JsonObject>(), profile);
    assert(!loadMotorProtocol(saved.as<JsonObjectConst>(), restored) && restored.commandPosition);
    doc["motorMinPosition"] = -400; doc["motorMaxPosition"] = 400;
    doc["motorProtocol"]["startupPosition"] = 0;
    assert(!loadMotorProtocol(doc.as<JsonObjectConst>(), profile) && profile.startupReference && profile.startupPosition == 0);
    saved["motorMinPosition"] = -400; saved["motorMaxPosition"] = 400;
    saveMotorProtocol(saved.as<JsonObject>(), profile);
    assert(!loadMotorProtocol(saved.as<JsonObjectConst>(), restored) && restored.startupReference && restored.startupPosition == 0);
    doc["motorProtocol"]["startupPosition"] = 401;
    assert(loadMotorProtocol(doc.as<JsonObjectConst>(), profile) && !profile.valid);
    doc["motorProtocol"]["startupPosition"] = "0";
    assert(loadMotorProtocol(doc.as<JsonObjectConst>(), profile) && !profile.valid);
    doc["motorProtocol"]["startupPosition"] = 0;
    doc["motorProtocol"]["positionTracking"] = "conservative";
    assert(loadMotorProtocol(doc.as<JsonObjectConst>(), profile) && !profile.valid);
    doc["motorProtocol"].remove("startupPosition");
    doc["motorProtocol"]["positionTracking"] = "commanded";
    doc["motorProtocol"]["finitePulseMove"] = false;
    assert(loadMotorProtocol(doc.as<JsonObjectConst>(), profile) && !profile.valid);
    doc["motorProtocol"]["positionTracking"] = "unknown";
    assert(loadMotorProtocol(doc.as<JsonObjectConst>(), profile) && !profile.valid);
    doc["motorProtocol"]["positionTracking"] = true;
    assert(loadMotorProtocol(doc.as<JsonObjectConst>(), profile) && !profile.valid);
    for (const char* field : {"version", "readFunction", "writeFunction", "speedReadRegister", "pulseReadRegister",
                             "forwardValue", "reverseValue", "stopValue", "finitePulseMove"}) {
        assert(!deserializeJson(doc, generic)); doc["motorProtocol"].remove(field);
        assert(loadMotorProtocol(doc.as<JsonObjectConst>(), profile) && !profile.valid);
    }
    for (const char* malformed : {"null", "false", "[]", "42", "\"profile\""}) {
        std::string text = std::string("{\"motorProtocol\":") + malformed + "}";
        assert(!deserializeJson(doc, text));
        assert(loadMotorProtocol(doc.as<JsonObjectConst>(), profile) && profile.configured && !profile.valid);
    }
    for (const char* field : {"speedReadRegister", "pulseReadRegister", "forwardValue", "reverseValue", "stopValue"}) {
        for (const char* value : {"-1", "65536", "0.5", "true", "\"1\"", "null"}) {
            assert(!deserializeJson(doc, generic));
            JsonDocument bad; assert(!deserializeJson(bad, value)); doc["motorProtocol"][field].set(bad.as<JsonVariantConst>());
            assert(loadMotorProtocol(doc.as<JsonObjectConst>(), profile) && !profile.valid);
        }
    }
    for (unsigned fc : {0U, 1U, 5U, 255U, 259U}) {
        assert(!deserializeJson(doc, generic)); doc["motorProtocol"]["readFunction"] = fc;
        assert(loadMotorProtocol(doc.as<JsonObjectConst>(), profile));
    }
    for (unsigned fc : {0U, 3U, 5U, 15U, 262U}) {
        assert(!deserializeJson(doc, generic)); doc["motorProtocol"]["writeFunction"] = fc;
        assert(loadMotorProtocol(doc.as<JsonObjectConst>(), profile));
    }
    assert(!deserializeJson(doc, generic)); doc["motorProtocol"]["reverseValue"] = 1;
    assert(loadMotorProtocol(doc.as<JsonObjectConst>(), profile));
    assert(!deserializeJson(doc, generic)); doc["motorRegs"][3] = 100;
    assert(loadMotorProtocol(doc.as<JsonObjectConst>(), profile));
    assert(!deserializeJson(doc, generic)); doc["motorRegs"][4] = 101;
    assert(loadMotorProtocol(doc.as<JsonObjectConst>(), profile));
    assert(!deserializeJson(doc, generic)); doc["motorProtocol"]["finitePulseMove"] = false;
    assert(!loadMotorProtocol(doc.as<JsonObjectConst>(), profile) && !profile.finitePulseMove);
    doc["motorProtocol"]["version"] = 2;
    assert(loadMotorProtocol(doc.as<JsonObjectConst>(), profile));
    saveMotorProtocol(saved.as<JsonObject>(), profile);
    assert(saved["motorProtocol"]["version"] == 0);
}
