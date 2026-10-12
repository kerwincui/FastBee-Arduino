#pragma once
#include <ArduinoJson.h>
#include "core/H1ParameterContract.h"

namespace FastBee {
inline bool acceptsH1JsonField(const H1Parameter::Field& field, JsonVariantConst value) {
    using H1Parameter::Kind;
    if (field.kind == Kind::Reference) {
        if (!value.is<const char*>()) return false;
        const JsonString text = value.as<JsonString>();
        return H1Parameter::acceptsReference(field, text.c_str(), text.size());
    }
    if (field.kind == Kind::Boolean) return value.is<bool>();
    if (field.kind == Kind::Integer && !value.is<int64_t>() && !value.is<uint64_t>()) return false;
    return !value.is<bool>() && value.is<double>() &&
        H1Parameter::accepts(field, value.as<double>());
}
void serializeH1Field(JsonObject object, const H1Parameter::Field& field);
void serializeIoProviderFields(JsonObject descriptor, bool mcp);
void serializePca9685Fields(JsonObject descriptor);
}
