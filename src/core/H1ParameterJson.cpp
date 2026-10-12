#include "core/H1ParameterJson.h"

namespace FastBee {
void serializeH1Field(JsonObject object, const H1Parameter::Field& field) {
    using H1Parameter::Kind;
    object["key"] = field.key;
    object["type"] = field.kind == Kind::Reference ? "reference" :
        field.kind == Kind::Integer ? "integer" : field.kind == Kind::Boolean ? "boolean" : "number";
    object["unit"] = field.unit;
    object["required"] = field.required;
    if (field.kind == Kind::Reference) {
        object["maxBytes"] = field.maxBytes;
    } else if (field.kind == Kind::Boolean) {
        object["default"] = field.defaultValue != 0;
    } else {
        object["default"] = field.defaultValue;
        if (field.hasMinimum) object[field.exclusiveMinimum ? "exclusiveMin" : "min"] = field.minimum;
        if (field.hasMaximum) object["max"] = field.maximum;
    }
}
void serializeIoProviderFields(JsonObject descriptor, bool mcp) {
    using namespace H1Parameter;
    descriptor["configSchemaVersion"] = 1;
    descriptor["defaultAddress"] = ioAddress.defaultValue;
    descriptor["addressMin"] = ioAddress.minimum;
    descriptor["addressMax"] = ioAddress.maximum;
    descriptor["defaultIntervalMs"] = ioInterval.defaultValue;
    descriptor["minIntervalMs"] = ioInterval.minimum;
    descriptor["maxIntervalMs"] = ioInterval.maximum;
    JsonArray fields = descriptor["configFields"].to<JsonArray>();
    serializeH1Field(fields.add<JsonObject>(), ioAddress);
    serializeH1Field(fields.add<JsonObject>(), ioInterval);
    serializeH1Field(fields.add<JsonObject>(), ioBus);
    if (mcp) serializeH1Field(fields.add<JsonObject>(), mcpReset);
}
void serializePca9685Fields(JsonObject descriptor) {
    descriptor["configSchemaVersion"] = 1;
    descriptor["commandSchemaVersion"] = 1;
    JsonArray fields = descriptor["configFields"].to<JsonArray>();
    for (const auto& field : H1Parameter::pcaFields) serializeH1Field(fields.add<JsonObject>(), field);
    descriptor["excludedAddresses"].to<JsonArray>().add(H1Parameter::pcaExcludedAddress);
    JsonArray commands = descriptor["commandDescriptors"].to<JsonArray>();
    for (const auto& command : H1Parameter::commands) {
        JsonObject item = commands.add<JsonObject>();
        item["id"] = command.id;
        JsonArray params = item["fields"].to<JsonArray>();
        if (command.value) {
            serializeH1Field(params.add<JsonObject>(), H1Parameter::channel);
            serializeH1Field(params.add<JsonObject>(), *command.value);
        }
    }
}
}
