#include "protocols/MotorTravelDraft.h"
#include <cassert>
#include <limits>
#include <string>

int main() {
    const char* fixture = R"({"motorMinPosition":0,"motorMaxPosition":0,"motorTravelDraft":{"version":1,"forwardClearanceMm":222,"reverseClearanceMm":88,"forwardMarginMm":5,"reverseMarginMm":5,"countsPerMm":0}})";
    JsonDocument doc, saved;
    MotorTravelDraft draft, restored;
    assert(!deserializeJson(doc, "{}"));
    assert(!loadMotorTravelDraft(doc.as<JsonObjectConst>(), draft) && !draft.configured);
    assert(!deserializeJson(doc, fixture));
    assert(!loadMotorTravelDraft(doc.as<JsonObjectConst>(), draft) && draft.valid);
    assert(draft.countsPerMm == 0 && draft.forwardClearanceMm == 222);
    saved["motorMinPosition"] = 0; saved["motorMaxPosition"] = 0;
    saveMotorTravelDraft(saved.as<JsonObject>(), draft);
    assert(saved["motorMinPosition"] == 0 && saved["motorMaxPosition"] == 0);
    assert(!loadMotorTravelDraft(saved.as<JsonObjectConst>(), restored));
    assert(saved["motorTravelDraft"] == doc["motorTravelDraft"]);
    assert(restored.reverseClearanceMm == 88 && restored.reverseMarginMm == 5);
    for (const char* field : {"forwardClearanceMm", "reverseClearanceMm", "forwardMarginMm", "reverseMarginMm", "countsPerMm"}) {
        for (const char* invalid : {"null", "true", "false", "\"5\"", "-1", "[]", "{}"}) {
            assert(!deserializeJson(doc, fixture));
            JsonDocument value; assert(!deserializeJson(value, invalid));
            doc["motorTravelDraft"][field].set(value.as<JsonVariantConst>());
            assert(loadMotorTravelDraft(doc.as<JsonObjectConst>(), draft) && !draft.valid);
        }
        assert(!deserializeJson(doc, fixture));
        doc["motorTravelDraft"].remove(field);
        assert(loadMotorTravelDraft(doc.as<JsonObjectConst>(), draft));
        for (double invalid : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
            assert(!deserializeJson(doc, fixture));
            doc["motorTravelDraft"][field] = invalid;
            assert(loadMotorTravelDraft(doc.as<JsonObjectConst>(), draft));
        }
    }
    for (const char* invalid : {"null", "false", "[]", "42"}) {
        assert(!deserializeJson(doc, std::string("{\"motorTravelDraft\":") + invalid + "}"));
        assert(loadMotorTravelDraft(doc.as<JsonObjectConst>(), draft));
    }
    assert(!deserializeJson(doc, fixture));
    doc["motorTravelDraft"]["version"] = 2;
    assert(loadMotorTravelDraft(doc.as<JsonObjectConst>(), draft));
    saveMotorTravelDraft(saved.as<JsonObject>(), draft);
    assert(saved["motorTravelDraft"]["version"] == 0);
    assert(!deserializeJson(doc, fixture));
    doc["motorTravelDraft"]["forwardMarginMm"] = 223;
    assert(loadMotorTravelDraft(doc.as<JsonObjectConst>(), draft));
    doc["motorTravelDraft"]["forwardMarginMm"] = 222;
    assert(!loadMotorTravelDraft(doc.as<JsonObjectConst>(), draft));
    doc["motorTravelDraft"]["reverseMarginMm"] = 88;
    assert(loadMotorTravelDraft(doc.as<JsonObjectConst>(), draft));
    assert(!deserializeJson(doc, fixture));
    doc["motorTravelDraft"]["countsPerMm"] = 3.25;
    doc["motorTravelDraft"]["forwardClearanceMm"] = 42.125;
    assert(!loadMotorTravelDraft(doc.as<JsonObjectConst>(), draft));
    saveMotorTravelDraft(saved.as<JsonObject>(), draft);
    assert(!loadMotorTravelDraft(saved.as<JsonObjectConst>(), restored));
    assert(restored.countsPerMm == 3.25 && restored.forwardClearanceMm == 42.125);
}
