#pragma once
#include <ArduinoJson.h>
#include "core/SdRecorderConfigMetadata.h"
#include "network/FixedJsonField.h"
namespace FastBee {
inline void writeSdRecorderConfigJson(JsonObject out) {
    setFixedJsonField(out, "schema", 1);
    setFixedJsonField(out, "sourceMaxBytes", 96);
    JsonArray numbers = out["numbers"].to<JsonArray>();
    for (const auto& parameter : kSdRecorderNumbers) {
        JsonObject number = numbers.add<JsonObject>();
        setFixedJsonField(number, "key", parameter.key);
        setFixedJsonField(number, "min", parameter.min);
        setFixedJsonField(number, "max", parameter.max);
        setFixedJsonField(number, "defaultValue", parameter.defaultValue);
        setFixedJsonField(number, "integer", true);
    }
}
} // namespace FastBee
