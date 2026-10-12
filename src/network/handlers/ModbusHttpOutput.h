#pragma once
#include <cstring>
#include <initializer_list>
#include "core/PeripheralManager.h"
#include "network/WebHandlerContext.h"
#include "network/HttpOutputControl.h"
#include "protocols/ModbusHandler.h"
#include "utils/ConsumedStringJsonReader.h"
#include "utils/SharedJsonDeserializer.h"

// Private helpers for authenticated form endpoints. Neither origin nor output
// ownership is accepted from the request. The caller holds the handler lease.
namespace ModbusHttp {
inline bool fields(WebHandlerContext& web, AsyncWebServerRequest* request,
                   std::initializer_list<const char*> allowed) {
    if (!request || request->params() > 16) {
        web.sendBadRequest(request, "CONTROL_PARAMETERS_INVALID"); return false;
    }
    for (size_t i = 0; i < request->params(); ++i) {
        const auto* item = request->getParam(i);
        bool known = item && item->isPost() && !item->isFile() &&
            item->name().length() == std::strlen(item->name().c_str());
        if (known) {
            known = item->name() == "holdMs";
            for (const char* name : allowed) known = known || item->name() == name;
            for (size_t j = 0; j < i; ++j)
                if (request->getParam(j)->name() == item->name()) known = false;
        }
        if (!known) { web.sendBadRequest(request, "CONTROL_PARAMETERS_INVALID"); return false; }
    }
    return true;
}
inline bool decimal(const String& text, uint32_t low, uint32_t high, uint32_t& value) {
    value = 0;
    if (text.isEmpty() || text.length() > 10 || text.length() != std::strlen(text.c_str())) return false;
    for (size_t i = 0; i < text.length(); ++i) {
        const char c = text[i];
        if (c < '0' || c > '9') return false;
        const uint32_t digit = static_cast<uint32_t>(c - '0');
        if (digit > high || value > (high - digit) / 10U) return false;
        value = value * 10U + digit;
    }
    return value >= low && value <= high;
}
inline bool number(WebHandlerContext& web, AsyncWebServerRequest* request, const char* name,
                   uint32_t fallback, uint32_t low, uint32_t high, uint32_t& value) {
    const auto* item = request->getParam(name, true);
    if (!item) { value = fallback; return true; }
    if (decimal(item->value(), low, high, value)) return true;
    web.sendBadRequest(request, String("Invalid numeric parameter: ") + name); return false;
}
// Track consumed bytes so ArduinoJson's successful first value cannot hide
// a second array or trailing garbage in a write request.
using JsonReader = FastBee::ConsumedStringJsonReader;
inline bool registerArray(const String& text, JsonDocument& values) {
    JsonReader reader{text};
    return !FastBee::deserializeJsonShared(values, reader) && reader.complete() &&
        !values.overflowed() && values.is<JsonArray>();
}
inline bool registers(FastBee::ModbusOutputFrame& frame, uint8_t slave, uint16_t start,
                      JsonArrayConst values,
                      FastBee::ModbusFrameAccess access = FastBee::ModbusFrameAccess::Standard) {
    if (values.size() == 0 || values.size() > 123 || uint32_t(start) + values.size() > 65536U) return false;
    uint8_t bytes[253]{};
    bytes[0] = slave; bytes[1] = 16; bytes[2] = start >> 8; bytes[3] = start & 255;
    bytes[4] = 0; bytes[5] = static_cast<uint8_t>(values.size()); bytes[6] = values.size() * 2;
    for (size_t i = 0; i < values.size(); ++i) {
        if (!values[i].is<uint16_t>()) return false;
        const uint16_t value = values[i].as<uint16_t>();
        bytes[7 + 2 * i] = value >> 8; bytes[8 + 2 * i] = value & 255;
    }
    return frame.assign(bytes, 7 + 2 * values.size(), access);
}

class Output {
public:
    Output(WebHandlerContext& web, AsyncWebServerRequest* request, ModbusHandler& bus)
        : web_(web), request_(request), bus_(bus) {}
    ~Output() { if (begun_) PeripheralManager::getInstance().finishOrdinaryControl(context_); }
    bool begin(bool maintenance = false) {
        if (maintenance && request_->hasParam("holdMs", true)) {
            web_.sendBadRequest(request_, "Maintenance does not accept an output hold"); return false;
        }
        uint32_t hold = 0;
        if (!number(web_, request_, "holdMs", FastBee::OrdinaryControl::DefaultManualHoldMs,
                    1, FastBee::OrdinaryControl::MaximumHoldMs, hold)) return false;
        if (maintenance) hold = 0;
        const auto auth = web_.authenticateRequest(request_);
        if (!auth.success || !auth.outputActor) {
            web_.sendError(request_, 401, "CONTROL_SESSION_UNCONFIRMED"); return false;
        }
        begun_ = maintenance ? PeripheralManager::getInstance().beginOrdinaryModbusMaintenance(
            auth.outputActor, FastBee::SensorConfigEpoch::current(), context_) :
            PeripheralManager::getInstance().beginOrdinaryControl(
            FastBee::OrdinaryControl::Source::Http, auth.outputActor, hold, false, 0, "",
            FastBee::SensorConfigEpoch::current(), {}, context_);
        if (!begun_) web_.sendError(request_, 409, "CONTROL_CONTEXT_UNCONFIRMED");
        return begun_;
    }
    OneShotResult submit(const FastBee::ModbusOutputFrame& frame) {
        FastBee::ModbusControlTarget target;
        OneShotResult result;
        if (!begun_ || !frame.valid() || !bus_.snapshotOrdinaryFrame(frame, target) ||
            (bound_ && (target.instance != binding_.instance || target.configuration != binding_.configuration ||
                        target.slave != binding_.slave))) {
            result.error = ONESHOT_CONTROL_REJECTED;
            result.controlReason = "CONTROL_FRAME_MAPPING_UNCONFIRMED";
            control_ = {}; control_.evaluated = true; control_.reason = result.controlReason;
            return result;
        }
        if (!bound_) { binding_ = target; bound_ = true; }
        result = PeripheralManager::getInstance().submitOrdinaryModbusFrame(
            bus_, target, frame, context_, control_);
        attempted_ = attempted_ || result.outputAttempted;
        if (result.error == ONESHOT_SUCCESS) {
            record(frame.bytes[1], static_cast<uint16_t>((frame.bytes[2] << 8) | frame.bytes[3]),
                   static_cast<uint16_t>((result.data[4] << 8) | result.data[5]));
        }
        return result;
    }
    void append(JsonObject data) const {
        data["outputAttempted"] = attempted_;
        data["commandAcknowledged"] = acknowledged_ != 0;
        data["acknowledgedCommandCount"] = acknowledged_;
        data["physicalOutputVerified"] = false;
        data["configEpoch"] = context_.configEpoch;
        FastBee::writeOrdinaryControlJson(data["control"].to<JsonObject>(), control_);
        data["control"]["canConfirmOverride"] = false; // No review flow on these legacy forms.
        JsonArray steps = data["acknowledgedSteps"].to<JsonArray>();
        for (unsigned i = 0; i < acknowledged_ && i < 32; ++i) {
            JsonObject step = steps.add<JsonObject>();
            step["function"] = prefix_[i].function; step["address"] = prefix_[i].address;
            step[prefix_[i].function == 15 || prefix_[i].function == 16 ? "quantity" : "value"] = prefix_[i].value;
            step["commandAcknowledged"] = true; step["physicalOutputVerified"] = false;
        }
    }
    OneShotResult relay(const FastBee::ModbusControlTarget& target, uint8_t function,
                        uint16_t address, uint16_t value, bool toggle = false) {
        OneShotResult result;
        if (!begun_ || (bound_ && (target.instance != binding_.instance ||
            target.configuration != binding_.configuration || target.slave != binding_.slave))) {
            result.error = ONESHOT_CONTROL_REJECTED;
            result.controlReason = "CONTROL_FRAME_MAPPING_UNCONFIRMED"; return result;
        }
        if (!bound_) { binding_ = target; bound_ = true; }
        result = PeripheralManager::getInstance().submitOrdinaryModbusRelay(
            bus_, target, function, address, value, context_, control_, 0, toggle);
        attempted_ = attempted_ || result.outputAttempted;
        if (result.error == ONESHOT_SUCCESS)
            record(function, address, static_cast<uint16_t>((result.data[4] << 8) | result.data[5]));
        return result;
    }
    void fail(const OneShotResult& result) const {
        JsonDocument doc;
        doc["success"] = false;
        doc["error"] = result.controlReason ? result.controlReason : "MODBUS_COMMAND_UNCONFIRMED";
        doc["errorCode"] = static_cast<unsigned>(result.error);
        doc["exceptionCode"] = result.exceptionCode;
        JsonObject data = doc["data"].to<JsonObject>();
        append(data);
        data["resultUnknown"] = result.outputAttempted && result.error != ONESHOT_EXCEPTION;
        data["manualReviewRequired"] = attempted_;
        web_.sendJsonResponse(request_, attempted_ ? 502 : 409, doc);
    }
    const FastBee::ModbusControlTarget& binding() const { return binding_; }
    void expect(const FastBee::ModbusControlTarget& target) { binding_ = target; bound_ = true; }
    const FastBee::OrdinaryControlContext& context() const { return context_; }
private:
    void record(uint8_t function, uint16_t address, uint16_t value) {
        if (acknowledged_ < 32) prefix_[acknowledged_] = {function, address, value};
        ++acknowledged_;
    }
    struct Step { uint8_t function; uint16_t address, value; };
    Step prefix_[32]{};
    WebHandlerContext& web_;
    AsyncWebServerRequest* request_;
    ModbusHandler& bus_;
    FastBee::OrdinaryControlContext context_;
    FastBee::OrdinaryControlResult control_;
    FastBee::ModbusControlTarget binding_;
    bool begun_ = false, bound_ = false, attempted_ = false;
    unsigned acknowledged_ = 0;
};
} // namespace ModbusHttp
