#include "network/HttpOutputControl.h"
#include "network/WebHandlerContext.h"
#include "core/PeripheralManager.h"
#include "core/NativeOutputDispatch.h"
#include "core/RfOutputContract.h"
#include "core/MotionOutputDispatch.h"
#include <ArduinoJson.h>
#include <cstdio>
#include <cstring>
#include <limits>

namespace FastBee {
namespace {
namespace OC = OrdinaryControl;

bool fieldsAllowed(AsyncWebServerRequest* request, bool direct, bool scalar = false) {
    if (!request || request->params() > 12) return false;
    for (size_t i = 0; i < request->params(); ++i) {
        const auto* field = request->getParam(i);
        if (!field || !field->isPost() || field->isFile()) return false;
        const String& key = field->name();
        const bool common = key == "id" || key == "holdMs";
        const bool action = scalar ? key == "state" || key == "action" || key == "pwm" ||
            key == "value" || key == "periodMs" || key == "toggle" || key == "intervalMs" : key == "state" || key == "toggle" ||
            key == "action" || key == "intervalMs";
        const bool known = common || (direct ? action || key == "confirmOverride" ||
            key == "controlGeneration" || key == "controlTargetId" ||
            key == "controlConfigEpoch" : key == "value");
        if (!known) return false;
        for (size_t j = 0; j < i; ++j)
            if (request->getParam(j)->name() == key) return false;
    }
    return true;
}
String fieldText(AsyncWebServerRequest* request, const char* name) {
    const auto* field = request->getParam(name, true);
    return field ? field->value() : String();
}
bool unsignedDecimal(const String& text, uint64_t maximum, uint64_t& out) {
    out = 0;
    if (text.isEmpty() || text.length() > 20 || text.length() != std::strlen(text.c_str())) return false;
    for (size_t i = 0; i < text.length(); ++i) {
        const char character = text[i];
        if (character < '0' || character > '9') return false;
        const unsigned digit = static_cast<unsigned>(character - '0');
        if (out > (maximum - digit) / 10U) return false;
        out = out * 10U + digit;
    }
    return true;
}
bool manualHold(AsyncWebServerRequest* request, uint32_t& value) {
    value = OC::DefaultManualHoldMs;
    if (!request->hasParam("holdMs", true)) return true;
    uint64_t parsed = 0;
    if (!unsignedDecimal(fieldText(request, "holdMs"), OC::MaximumHoldMs, parsed) || !parsed) return false;
    value = static_cast<uint32_t>(parsed); return true;
}
uint64_t sessionActor(WebHandlerContext& web, AsyncWebServerRequest* request) {
    const AuthResult auth = web.authenticateRequest(request);
    return auth.success ? auth.outputActor : 0;
}
const char* sourceName(OC::Source source) {
    switch (source) {
        case OC::Source::Rule: return "rule";
        case OC::Source::Mqtt: return "mqtt";
        case OC::Source::Http: return "http";
        case OC::Source::Screen: return "screen";
        default: return "none";
    }
}
const char* phaseName(OC::Phase phase) {
    switch (phase) {
        case OC::Phase::Executing: return "executing";
        case OC::Phase::Held: return "held";
        case OC::Phase::Producing: return "producing";
        case OC::Phase::Retiring: return "retiring";
        case OC::Phase::Fault: return "fault";
        default: return "idle";
    }
}
void sendControl(WebHandlerContext& web, AsyncWebServerRequest* request,
                 const String& id, uint32_t epoch, const OrdinaryControlResult& result,
                 bool success, int status) {
    JsonDocument doc;
    doc["success"] = success; doc["reason"] = result.reason;
    doc["message"] = result.reason;
    if (!success) doc["error"] = result.reason;
    JsonObject data = doc["data"].to<JsonObject>();
    data["id"] = id; data["configEpoch"] = epoch;
    writeOrdinaryControlJson(data["control"].to<JsonObject>(), result);
    if (doc.overflowed()) { web.sendError(request, 503, "CONTROL_RESPONSE_UNCONFIRMED"); return; }
    web.sendJsonResponse(request, status, doc);
}
} // namespace

void writeOrdinaryControlJson(JsonObject control, const OrdinaryControlResult& result) {
    control["targetId"] = result.targetId;
    control["reason"] = result.evaluated ? result.reason : "CONTROL_NOT_ATTEMPTED";
    control["source"] = sourceName(result.actor.source);
    control["phase"] = phaseName(result.phase);
    char decimal[21];
    std::snprintf(decimal, sizeof(decimal), "%llu", static_cast<unsigned long long>(result.generation));
    control["generation"] = decimal; // Never a lossy JavaScript Number.
    std::snprintf(decimal, sizeof(decimal), "%llu", static_cast<unsigned long long>(result.operation));
    control["operation"] = decimal;
    control["holdRemainingMs"] = result.holdRemainingMs;
    control["evaluated"] = result.evaluated;
    control["attemptKnown"] = result.attemptKnown;
    if (result.attemptKnown) control["attempted"] = result.attempted;
    else control["attempted"] = nullptr;
    control["backendOperationId"] = result.backendOperationId;
    control["acknowledgedSteps"] = result.acknowledgedSteps;
    control["accepted"] = result.accepted;
    control["cleanupConfirmed"] = result.cleanupConfirmed;
    control["physicalVerified"] = false;
    control["canConfirmOverride"] = result.evaluated && result.attemptKnown && !result.attempted &&
        result.decision == OC::Decision::Conflict && result.generation &&
        (result.actor.source == OC::Source::Rule || result.actor.source == OC::Source::Mqtt) &&
        (result.phase == OC::Phase::Held || result.phase == OC::Phase::Producing);
}

bool beginHttpRuleOutput(WebHandlerContext& web, AsyncWebServerRequest* request,
                         uint32_t expectedEpoch, OrdinaryControlContext& context) {
    uint32_t hold = 0;
    if (!fieldsAllowed(request, false) || !manualHold(request, hold)) {
        web.sendError(request, 400, "CONTROL_PARAMETERS_INVALID"); return false;
    }
    const uint64_t actor = sessionActor(web, request);
    if (!actor) { web.sendError(request, 401, "CONTROL_SESSION_UNCONFIRMED"); return false; }
    if (!PeripheralManager::getInstance().beginOrdinaryControl(
            OC::Source::Http, actor, hold, false, 0, "", expectedEpoch, {}, context)) {
        web.sendError(request, 409, "CONTROL_CONTEXT_UNCONFIRMED"); return false;
    }
    return true;
}

static bool beginHttpMotionOrPcaOutput(WebHandlerContext& web, AsyncWebServerRequest* request,
    const String& id, uint32_t expectedEpoch, OrdinaryControlContext& context, bool servo) {
    auto fail = [&](const char* reason) { web.sendError(request, 400, reason); return false; };
    if (!request || request->params() > 12 || id.isEmpty() || id.length() > 40 ||
        id.length() != std::strlen(id.c_str())) return fail("CONTROL_PARAMETERS_INVALID");
    for (size_t i = 0; i < request->params(); ++i) {
        const auto* field = request->getParam(i);
        if (!field || !field->isPost() || field->isFile()) return fail("CONTROL_PARAMETERS_INVALID");
        const String& name = field->name();
        const bool commandField = servo ? name == "angle" || name == "pwm" :
            name == "action" || name == "channel" || name == "value";
        if (!commandField && name != "id" && name != "holdMs" &&
            name != "confirmOverride" && name != "controlGeneration" && name != "controlTargetId" &&
            name != "controlConfigEpoch") return fail("CONTROL_PARAMETERS_INVALID");
        for (size_t j = 0; j < i; ++j)
            if (request->getParam(j)->name() == name) return fail("CONTROL_PARAMETERS_INVALID");
    }
    if (fieldText(request, "id") != id) return fail("CONTROL_PARAMETERS_INVALID");
    uint32_t hold = 0;
    if (!manualHold(request, hold)) return fail("CONTROL_PARAMETERS_INVALID");
    const uint64_t actor = sessionActor(web, request);
    if (!actor) { web.sendError(request, 401, "CONTROL_SESSION_UNCONFIRMED"); return false; }
    const String confirmation = fieldText(request, "confirmOverride");
    const bool confirmed = confirmation == "true";
    if (request->hasParam("confirmOverride", true) && confirmation != "false" && !confirmed)
        return fail("CONTROL_CONFIRMATION_INVALID");
    uint64_t generation = 0, reviewEpoch = 0;
    const String reviewTarget = fieldText(request, "controlTargetId");
    if (confirmed) {
        if (reviewTarget != id || !unsignedDecimal(fieldText(request, "controlGeneration"), UINT64_MAX, generation) ||
            !generation || !unsignedDecimal(fieldText(request, "controlConfigEpoch"), UINT32_MAX, reviewEpoch) ||
            !reviewEpoch || reviewEpoch != expectedEpoch) return fail("CONTROL_REVIEW_INVALID");
    } else if (request->hasParam("controlTargetId", true) || request->hasParam("controlGeneration", true) ||
               request->hasParam("controlConfigEpoch", true)) return fail("CONTROL_REVIEW_UNEXPECTED");
    if (!PeripheralManager::getInstance().beginOrdinaryControl(OC::Source::Http, actor, hold,
        confirmed, generation, confirmed ? reviewTarget : String(), expectedEpoch, {}, context)) {
        web.sendError(request, 409, "CONTROL_CONTEXT_UNCONFIRMED"); return false;
    }
    return true;
}


bool beginHttpPcaOutput(WebHandlerContext& web, AsyncWebServerRequest* request,
                       const String& id, uint32_t expectedEpoch, OrdinaryControlContext& context) {
    return beginHttpMotionOrPcaOutput(web, request, id, expectedEpoch, context, false);
}
void handleHttpServoOutput(WebHandlerContext& web, AsyncWebServerRequest* request,
                           const String& id, uint32_t expectedEpoch) {
    auto& pm = PeripheralManager::getInstance();
    PeripheralConfig config;
    uint32_t epoch = 0;
    if (!pm.getPeripheralSnapshot(id, config, &epoch) || epoch != expectedEpoch ||
        config.type != PeripheralType::PWM_SERVO) {
        web.sendError(request, 409, "CONTROL_SOURCE_STALE"); return;
    }
    if (request->hasParam("angle", true) + request->hasParam("pwm", true) != 1) {
        web.sendError(request, 400, "CONTROL_COMMAND_REQUIRED"); return;
    }
    const bool raw = request->hasParam("pwm", true);
    uint32_t duty = 0;
    uint64_t angle = 0;
    if (raw ? fieldText(request, "pwm").isEmpty() ||
              !parseServoRawDuty(config, fieldText(request, "pwm"), duty) :
              !unsignedDecimal(fieldText(request, "angle"), 360, angle) ||
              angle < config.params.servo.minAngle || angle > config.params.servo.maxAngle) {
        web.sendError(request, 400, "CONTROL_PARAMETER_INVALID"); return;
    }
    OrdinaryControlContext context;
    if (!beginHttpMotionOrPcaOutput(web, request, id, expectedEpoch, context, true)) return;
    OrdinaryControlResult result;
    const bool accepted = pm.submitOrdinaryServo(id, raw ? duty : angle, raw, &context, &result);
    pm.finishOrdinaryControl(context, false);
    sendControl(web, request, id, expectedEpoch, result, accepted,
        accepted ? 200 : result.attempted ? 503 : 409);
}

bool beginHttpModbusRelayOutput(WebHandlerContext& web, AsyncWebServerRequest* request,
    const String& canonicalTarget, uint32_t expectedEpoch, OrdinaryControlContext& context) {
    auto fail = [&](const char* reason) { web.sendError(request, 400, reason); return false; };
    if (!request || request->params() > 12 || canonicalTarget.isEmpty()) return fail("CONTROL_PARAMETERS_INVALID");
    for (size_t i = 0; i < request->params(); ++i) {
        const auto* field = request->getParam(i);
        if (!field || !field->isPost() || field->isFile()) return fail("CONTROL_PARAMETERS_INVALID");
        const String& key = field->name();
        if (key != "slaveAddress" && key != "channel" && key != "coilBase" && key != "action" && key != "mode" &&
            key != "holdMs" && key != "confirmOverride" && key != "controlGeneration" && key != "controlTargetId" &&
            key != "controlConfigEpoch") return fail("CONTROL_PARAMETERS_INVALID");
        for (size_t j = 0; j < i; ++j)
            if (request->getParam(j)->name() == key) return fail("CONTROL_PARAMETERS_INVALID");
        if (key == "slaveAddress" || key == "channel" || key == "coilBase") {
            uint64_t number = 0;
            if (!unsignedDecimal(field->value(), key == "slaveAddress" ? 247 : 65535, number) ||
                (key == "slaveAddress" && !number)) return fail("CONTROL_PARAMETERS_INVALID");
        }
    }
    uint32_t hold = 0;
    if (!manualHold(request, hold)) return fail("CONTROL_PARAMETERS_INVALID");
    const uint64_t actor = sessionActor(web, request);
    if (!actor) { web.sendError(request, 401, "CONTROL_SESSION_UNCONFIRMED"); return false; }
    const String confirmation = fieldText(request, "confirmOverride");
    const bool confirmed = confirmation == "true";
    if (request->hasParam("confirmOverride", true) && !confirmed && confirmation != "false")
        return fail("CONTROL_CONFIRMATION_INVALID");
    uint64_t generation = 0, epoch = 0;
    const String reviewTarget = fieldText(request, "controlTargetId");
    if (confirmed) {
        if (reviewTarget != canonicalTarget || !unsignedDecimal(fieldText(request, "controlGeneration"), UINT64_MAX, generation) ||
            !generation || !unsignedDecimal(fieldText(request, "controlConfigEpoch"), UINT32_MAX, epoch) ||
            epoch != expectedEpoch) return fail("CONTROL_REVIEW_INVALID");
    } else if (request->hasParam("controlTargetId", true) || request->hasParam("controlGeneration", true) ||
               request->hasParam("controlConfigEpoch", true)) return fail("CONTROL_REVIEW_UNEXPECTED");
    if (!PeripheralManager::getInstance().beginOrdinaryControl(OC::Source::Http, actor, hold,
        confirmed, generation, confirmed ? reviewTarget : String(), expectedEpoch, {}, context)) {
        web.sendError(request, 409, "CONTROL_CONTEXT_UNCONFIRMED"); return false;
    }
    return true;
}

bool beginHttpMotorOutput(WebHandlerContext& web, AsyncWebServerRequest* request,
    const String& canonicalTarget, uint32_t expectedEpoch, OrdinaryControlContext& context) {
    auto fail = [&](const char* reason) { web.sendError(request, 400, reason); return false; };
    if (!request || request->params() > 12 || canonicalTarget.isEmpty()) return fail("CONTROL_PARAMETERS_INVALID");
    for (size_t i = 0; i < request->params(); ++i) {
        const auto* field = request->getParam(i);
        if (!field || !field->isPost() || field->isFile()) return fail("CONTROL_PARAMETERS_INVALID");
        const String& key = field->name();
        if (key != "slaveAddress" && key != "motorRef" && key != "value" && key != "action" && key != "requestId" &&
            key != "holdMs" && key != "confirmOverride" && key != "controlGeneration" && key != "controlTargetId" &&
            key != "controlConfigEpoch") return fail("CONTROL_PARAMETERS_INVALID");
        for (size_t j = 0; j < i; ++j)
            if (request->getParam(j)->name() == key) return fail("CONTROL_PARAMETERS_INVALID");
        if (key == "slaveAddress" || key == "value") {
            uint64_t number = 0;
            if (!unsignedDecimal(field->value(), key == "slaveAddress" ? 247 : 65535, number) ||
                (key == "slaveAddress" && !number)) return fail("CONTROL_PARAMETERS_INVALID");
        }
    }
    uint32_t hold = 0;
    if (!manualHold(request, hold)) return fail("CONTROL_PARAMETERS_INVALID");
    const uint64_t actor = sessionActor(web, request);
    if (!actor) { web.sendError(request, 401, "CONTROL_SESSION_UNCONFIRMED"); return false; }
    const String confirmation = fieldText(request, "confirmOverride");
    const bool confirmed = confirmation == "true";
    if (request->hasParam("confirmOverride", true) && !confirmed && confirmation != "false")
        return fail("CONTROL_CONFIRMATION_INVALID");
    uint64_t generation = 0, epoch = 0;
    const String reviewTarget = fieldText(request, "controlTargetId");
    if (confirmed) {
        if (reviewTarget != canonicalTarget || !unsignedDecimal(fieldText(request, "controlGeneration"), UINT64_MAX, generation) ||
            !generation || !unsignedDecimal(fieldText(request, "controlConfigEpoch"), UINT32_MAX, epoch) ||
            epoch != expectedEpoch) return fail("CONTROL_REVIEW_INVALID");
    } else if (request->hasParam("controlTargetId", true) || request->hasParam("controlGeneration", true) ||
               request->hasParam("controlConfigEpoch", true)) return fail("CONTROL_REVIEW_UNEXPECTED");
    if (!PeripheralManager::getInstance().beginOrdinaryControl(OC::Source::Http, actor, hold,
        confirmed, generation, confirmed ? reviewTarget : String(), expectedEpoch, {}, context)) {
        web.sendError(request, 409, "CONTROL_CONTEXT_UNCONFIRMED"); return false;
    }
    return true;
}

static void handleHttpNativeOutput(WebHandlerContext& web, AsyncWebServerRequest* request,
                                  const String& id, uint32_t expectedEpoch, bool scalar) {
    auto fail = [&](const char* reason) { web.sendError(request, 400, reason); };
    uint32_t hold = 0;
    if (!fieldsAllowed(request, true, scalar) || fieldText(request, "id") != id ||
        id.isEmpty() || id.length() > 40 || id.length() != std::strlen(id.c_str()) ||
        !manualHold(request, hold)) { fail("CONTROL_PARAMETERS_INVALID"); return; }
    const uint64_t actor = sessionActor(web, request);
    if (!actor) { web.sendError(request, 401, "CONTROL_SESSION_UNCONFIRMED"); return; }
    const unsigned choices = request->hasParam("state", true) + request->hasParam("action", true) +
        request->hasParam("toggle", true) +
        (scalar ? request->hasParam("pwm", true) + request->hasParam("value", true) : 0);
    if (choices != 1) { fail("CONTROL_COMMAND_REQUIRED"); return; }
    DigitalOutputCommand command = DigitalOutputCommand::Low;
    ScalarOutputCommand scalarCommand = ScalarOutputCommand::Pwm;
    uint32_t scalarValue = 0;
    uint64_t interval = 0;
    const String action = fieldText(request, "action");
    if (scalar) {
        PeripheralConfig config;
        uint32_t epoch = 0;
        if (!PeripheralManager::getInstance().getPeripheralSnapshot(id, config, &epoch) ||
            epoch != expectedEpoch || !isOrdinaryScalarTarget(config)) {
            web.sendError(request, 409, "CONTROL_SOURCE_STALE"); return;
        }
        const bool dac = config.type == PeripheralType::DAC;
        scalarCommand = dac ? ScalarOutputCommand::Dac : ScalarOutputCommand::Pwm;
        if (request->hasParam("pwm", true) || request->hasParam("value", true)) {
            if (request->hasParam(dac ? "pwm" : "value", true) ||
                !parseOrdinaryScalarValue(config, fieldText(request, dac ? "value" : "pwm"), scalarValue)) {
                fail("CONTROL_PARAMETER_INVALID"); return;
            }
        } else if (request->hasParam("state", true)) {
            const String state = fieldText(request, "state");
            if ((state != "0" && state != "1") ||
                (state == "1" && !ordinaryScalarHighValue(config, scalarValue))) {
                fail("CONTROL_STATE_INVALID"); return;
            }
        } else if (request->hasParam("toggle", true) || action == "toggle") {
            const String toggle = fieldText(request, "toggle");
            if (dac || (request->hasParam("toggle", true) && toggle != "1" && toggle != "true")) {
                fail("CONTROL_TOGGLE_INVALID"); return;
            }
            scalarCommand = ScalarOutputCommand::Toggle;
        } else if (action == "blink") {
            scalarCommand = ScalarOutputCommand::Blink;
            if ((config.type != PeripheralType::GPIO_PWM_OUTPUT && config.type != PeripheralType::GPIO_DIGITAL_OUTPUT) ||
                !unsignedDecimal(fieldText(request, "intervalMs"), 65535, interval) || !interval) {
                fail("CONTROL_INTERVAL_INVALID"); return;
            }
            scalarValue = static_cast<uint32_t>(interval);
        } else if (action == "breathe") {
            scalarCommand = ScalarOutputCommand::Breathe;
            if ((config.type != PeripheralType::GPIO_PWM_OUTPUT && config.type != PeripheralType::GPIO_ANALOG_OUTPUT) ||
                !unsignedDecimal(fieldText(request, "periodMs"), 65535, interval) || !interval) {
                fail("CONTROL_INTERVAL_INVALID"); return;
            }
            scalarValue = static_cast<uint32_t>(interval);
        } else if (action == "stop") scalarCommand = ScalarOutputCommand::Stop;
        else if (action == "high" || action == "low") {
            if (action == "high" && !ordinaryScalarHighValue(config, scalarValue)) {
                fail("CONTROL_PARAMETER_INVALID"); return;
            }
        } else { fail("CONTROL_COMMAND_INVALID"); return; }
        if (scalarCommand != ScalarOutputCommand::Breathe && request->hasParam("periodMs", true)) {
            fail("CONTROL_INTERVAL_UNEXPECTED"); return;
        }
        if (scalarCommand != ScalarOutputCommand::Blink && request->hasParam("intervalMs", true)) {
            fail("CONTROL_INTERVAL_UNEXPECTED"); return;
        }
    } else {
    if (request->hasParam("state", true)) {
        const String state = fieldText(request, "state");
        if (state != "0" && state != "1") { fail("CONTROL_STATE_INVALID"); return; }
        command = state == "1" ? DigitalOutputCommand::High : DigitalOutputCommand::Low;
    } else if (request->hasParam("toggle", true)) {
        const String toggle = fieldText(request, "toggle");
        if (toggle != "1" && toggle != "true") { fail("CONTROL_TOGGLE_INVALID"); return; }
        command = DigitalOutputCommand::Toggle;
    } else if (action == "high") command = DigitalOutputCommand::High;
    else if (action == "low") command = DigitalOutputCommand::Low;
    else if (action == "toggle") command = DigitalOutputCommand::Toggle;
    else if (action == "blink") command = DigitalOutputCommand::Blink;
    else if (action == "stop") command = DigitalOutputCommand::Stop;
    else { fail("CONTROL_COMMAND_INVALID"); return; }
    if (command == DigitalOutputCommand::Blink) {
        if (!unsignedDecimal(fieldText(request, "intervalMs"), 65535, interval) || !interval) {
            fail("CONTROL_INTERVAL_INVALID"); return;
        }
    } else if (request->hasParam("intervalMs", true)) { fail("CONTROL_INTERVAL_UNEXPECTED"); return; }
    }
    const String confirmation = fieldText(request, "confirmOverride");
    const bool confirmed = confirmation == "true";
    if (request->hasParam("confirmOverride", true) && confirmation != "false" && !confirmed) {
        fail("CONTROL_CONFIRMATION_INVALID"); return;
    }
    uint64_t generation = 0, reviewEpoch = 0;
    const String reviewTarget = fieldText(request, "controlTargetId");
    if (confirmed) {
        if (reviewTarget != id || !unsignedDecimal(fieldText(request, "controlGeneration"), UINT64_MAX, generation) ||
            !generation || !unsignedDecimal(fieldText(request, "controlConfigEpoch"), UINT32_MAX, reviewEpoch) ||
            !reviewEpoch || reviewEpoch != expectedEpoch) { fail("CONTROL_REVIEW_INVALID"); return; }
    } else if (request->hasParam("controlTargetId", true) || request->hasParam("controlGeneration", true) ||
               request->hasParam("controlConfigEpoch", true)) { fail("CONTROL_REVIEW_UNEXPECTED"); return; }
    auto& pm = PeripheralManager::getInstance();
    OrdinaryControlContext context;
    if (!pm.beginOrdinaryControl(OC::Source::Http, actor, hold, confirmed, generation,
            confirmed ? reviewTarget : String(), expectedEpoch, {}, context)) {
        web.sendError(request, 409, "CONTROL_CONTEXT_UNCONFIRMED"); return;
    }
    OrdinaryControlResult result;
    const bool accepted = scalar ? pm.submitOrdinaryScalar(id, scalarCommand, scalarValue, context, result) :
        pm.submitOrdinaryDigital(id, command, static_cast<uint32_t>(interval), context, result);
    pm.finishOrdinaryControl(context, false);
    // A failed/unknown SDK attempt is not a conflict that may be replayed.
    sendControl(web, request, id, expectedEpoch, result, accepted,
        accepted ? 200 : result.attempted ? 503 : 409);
}
void handleHttpNativeDigital(WebHandlerContext& web, AsyncWebServerRequest* request,
                             const String& id, uint32_t expectedEpoch) {
    handleHttpNativeOutput(web, request, id, expectedEpoch, false);
}
void handleHttpNativeScalar(WebHandlerContext& web, AsyncWebServerRequest* request,
                            const String& id, uint32_t expectedEpoch) {
    handleHttpNativeOutput(web, request, id, expectedEpoch, true);
}
void handleHttpRfOutput(WebHandlerContext& web, AsyncWebServerRequest* request,
                        const String& id, uint32_t expectedEpoch) {
    auto fail = [&](const char* reason) { web.sendError(request, 400, reason); };
    if (!request || request->params() > 12 || id.isEmpty() || id.length() > 40 ||
        id.length() != std::strlen(id.c_str())) { fail("CONTROL_PARAMETERS_INVALID"); return; }
    for (size_t i = 0; i < request->params(); ++i) {
        const auto* field = request->getParam(i);
        if (!field || !field->isPost() || field->isFile()) { fail("CONTROL_PARAMETERS_INVALID"); return; }
        const String& key = field->name();
        if (key != "id" && key != "code" && key != "data" && key != "value" && key != "bits" &&
            key != "bitLength" && key != "pulseWidth" && key != "repeat" && key != "holdMs" &&
            key != "confirmOverride" && key != "controlGeneration" && key != "controlTargetId" &&
            key != "controlConfigEpoch") { fail("CONTROL_PARAMETERS_INVALID"); return; }
        for (size_t j = 0; j < i; ++j) if (request->getParam(j)->name() == key) {
            fail("CONTROL_PARAMETERS_INVALID"); return;
        }
    }
    const unsigned choices = request->hasParam("code", true) + request->hasParam("data", true) + request->hasParam("value", true);
    if (fieldText(request, "id") != id || choices != 1 ||
        (request->hasParam("bits", true) && request->hasParam("bitLength", true))) {
        fail("RF_PARAMETERS_INVALID"); return;
    }
    const String code = fieldText(request, request->hasParam("code", true) ? "code" :
        request->hasParam("data", true) ? "data" : "value");
    uint32_t bits = 0, pulseUs = 0, repeat = 0, hold = 0;
    const auto option = [&](const char* key, uint32_t max, uint32_t& value) {
        const String text = fieldText(request, key);
        return !request->hasParam(key, true) || parseRfOutputOption(text.c_str(), text.length(), max, value);
    };
    if (!option("bits", 32, bits) || !option("bitLength", 32, bits) ||
        !option("pulseWidth", 2000, pulseUs) || !option("repeat", 20, repeat) || !manualHold(request, hold)) {
        fail("RF_PARAMETERS_INVALID"); return;
    }
    auto& pm = PeripheralManager::getInstance();
    PeripheralConfig config; uint32_t epoch = 0;
    if (!pm.getPeripheralSnapshot(id, config, &epoch) || epoch != expectedEpoch ||
        config.type != PeripheralType::RF_MODULE || config.params.rf.mode != RF_MODE_TX) {
        web.sendError(request, 409, "CONTROL_SOURCE_STALE"); return;
    }
    RfOutputFrame frame;
    if (!resolveRfOutputFrame(code.c_str(), code.length(), bits, pulseUs, repeat,
        config.params.rf.bitLength, config.params.rf.pulseWidth, config.params.rf.repeat, frame)) {
        fail("RF_PARAMETERS_INVALID"); return;
    }
    const uint64_t actor = sessionActor(web, request);
    if (!actor) { web.sendError(request, 401, "CONTROL_SESSION_UNCONFIRMED"); return; }
    const String confirmation = fieldText(request, "confirmOverride");
    const bool confirmed = confirmation == "true";
    if (request->hasParam("confirmOverride", true) && confirmation != "false" && !confirmed) {
        fail("CONTROL_CONFIRMATION_INVALID"); return;
    }
    uint64_t generation = 0, reviewEpoch = 0;
    const String reviewTarget = fieldText(request, "controlTargetId");
    if (confirmed) {
        if (reviewTarget != id || !unsignedDecimal(fieldText(request, "controlGeneration"), UINT64_MAX, generation) ||
            !generation || !unsignedDecimal(fieldText(request, "controlConfigEpoch"), UINT32_MAX, reviewEpoch) ||
            !reviewEpoch || reviewEpoch != expectedEpoch) { fail("CONTROL_REVIEW_INVALID"); return; }
    } else if (request->hasParam("controlTargetId", true) || request->hasParam("controlGeneration", true) ||
               request->hasParam("controlConfigEpoch", true)) { fail("CONTROL_REVIEW_UNEXPECTED"); return; }
    OrdinaryControlContext context;
    if (!pm.beginOrdinaryControl(OC::Source::Http, actor, hold, confirmed, generation,
            confirmed ? reviewTarget : String(), expectedEpoch, {}, context)) {
        web.sendError(request, 409, "CONTROL_CONTEXT_UNCONFIRMED"); return;
    }
    OrdinaryControlResult result;
    const bool accepted = pm.sendRfCode(id, code, bits, pulseUs, repeat, &context, &result);
    pm.finishOrdinaryControl(context, false);
    sendControl(web, request, id, expectedEpoch, result, accepted,
                accepted ? 200 : result.attempted ? 503 : 409);
}
} // namespace FastBee
