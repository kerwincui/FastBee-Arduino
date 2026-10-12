// Real production JSON descriptor and literal parser; no bus or device access.
#include "core/H1ParameterJson.h"
#include "core/Pca9685ConfigSupport.h"
#include <cassert>
#include <iostream>
#include <string>

static const char* valid = R"({"busPeripheralId":"bus","oePeripheralId":"oe","address":64,"channelMask":1,"pwmHz":50,"clockHz":25000000,"minAngle":0,"maxAngle":180,"minPulseUs":500,"maxPulseUs":2500,"openDrain":false})";
int main(int argc, char** argv) {
    assert(argc == 2);
    using namespace FastBee;
    if (!strcmp(argv[1], "emit")) {
        JsonDocument document;
        serializePca9685Fields(document["pca"].to<JsonObject>());
        serializeIoProviderFields(document["mcp"].to<JsonObject>(), true);
        serializeIoProviderFields(document["pcf"].to<JsonObject>(), false);
        assert(!document.overflowed());
        std::string serialized; serializeJson(document, serialized); std::cout << serialized;
        return 0;
    }
    PeripheralConfig config;
    config.id = "pwm"; config.type = PeripheralType::PCA9685_PWM; config.pinCount = 0;
    String error;
    JsonDocument document;
    assert(!deserializeJson(document, valid));
    assert(parsePca9685Config(document.as<JsonObjectConst>(), config, error));
    const auto original = config.params.pca9685;
    const auto reset = [&] { document.clear(); assert(!deserializeJson(document, valid)); };
    const auto rejects = [&] {
        assert(!parsePca9685Config(document.as<JsonObjectConst>(), config, error));
        assert(!error.isEmpty() && !memcmp(&original, &config.params.pca9685, sizeof(original)));
        reset();
    };
    if (!strcmp(argv[1], "typed_fields")) {
        for (const char* key : {"busPeripheralId", "oePeripheralId", "address", "channelMask", "pwmHz", "clockHz",
                               "minAngle", "maxAngle", "minPulseUs", "maxPulseUs", "openDrain"}) {
            document.remove(key); rejects();
        }
        for (const char* key : {"address", "channelMask", "pwmHz", "clockHz", "minAngle", "minPulseUs"}) {
            document[key] = true; rejects(); document[key] = "50"; rejects();
        }
        document["address"] = 64.0; rejects(); // JSON float is not an integer wire type.
        document["channelMask"] = 1.0; rejects();
        document["openDrain"] = 0; rejects();
        document["powerCycleConfirmed"] = true; rejects();
        document["address"] = 112; rejects();
        document["busPeripheralId"] = std::string(41, 'a'); rejects();
    } else if (!strcmp(argv[1], "relationships")) {
        document["maxPulseUs"] = 500.01; rejects();
        document["busPeripheralId"] = "oe"; rejects();
        document["oePeripheralId"] = "pwm"; rejects();
        document["pwmHz"] = 1000; rejects();
        // Valid by the real prescale/tick contract, previously rejected by Web's
        // independent pwmHz>=1 / maxPulseUs<=1e6 caps.
        document["clockHz"] = 500000; document["pwmHz"] = 0.5;
        document["minPulseUs"] = 1000000; document["maxPulseUs"] = 1500000;
        assert(parsePca9685Config(document.as<JsonObjectConst>(), config, error));
        assert(validatePca9685Config(config, error));
    } else if (!strcmp(argv[1], "literal_commands")) {
        Pca9685Command command;
        for (const char* json : {R"({"action":"pulse","channel":0,"value":0})",
                R"({"action":"angle","channel":15,"value":10000})", R"({"action":"duty","channel":0,"value":100})",
                R"({"action":"disable"})", R"({"action":"enable"})", R"({"action":"stop"})"})
            assert(parsePca9685Command(json, command, error));
        uint16_t ticks = 1;
        assert(!makePCA9685PulseTicks(0, 50, ticks)); // parse success != session acceptance.
        for (const char* json : {R"({"action":"prepare"})", R"({"action":"start"})",
                R"({"action":"duty","channel":1.0,"value":0})", R"({"action":"duty","channel":true,"value":0})",
                R"({"action":"duty","channel":1,"value":true})", R"({"action":"duty","channel":1,"value":101})",
                R"({"action":"stop","channel":0})", R"({"action":"enable","powerCycleConfirmed":true})",
                R"({"action":"pulse","channel":0,"value":-0.1})", R"({"action":"angle","channel":0,"value":"$value"})"})
            assert(!parsePca9685Command(json, command, error));
    } else if (!strcmp(argv[1], "io_compatibility")) {
        assert(H1Parameter::acceptsIoInterval(0, true));
        assert(!H1Parameter::acceptsIoInterval(0, false));
        for (double interval : {20., 1000.}) assert(H1Parameter::acceptsIoInterval(interval, false));
        for (double interval : {-1., 19., 1001., 20.5}) assert(!H1Parameter::acceptsIoInterval(interval, true));
        assert(H1Parameter::acceptsReference(H1Parameter::ioBus, "", 0));
        assert(!H1Parameter::acceptsReference(H1Parameter::mcpReset, "", 0));
        assert(!H1Parameter::acceptsReference(H1Parameter::mcpReset, "reset\0redirect", 14));
        for (double address : {31., 40., 32.5}) assert(!H1Parameter::accepts(H1Parameter::ioAddress, address));
    } else assert(false);
}
