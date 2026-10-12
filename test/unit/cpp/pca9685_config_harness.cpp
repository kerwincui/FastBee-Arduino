#include <cassert>
#include <cstring>
#include <initializer_list>
#include "core/Pca9685ConfigSupport.h"

static const char* valid = R"({"busPeripheralId":"bus","oePeripheralId":"oe","address":64,"channelMask":1,"pwmHz":50,"clockHz":25000000,"minAngle":0,"maxAngle":180,"minPulseUs":500,"maxPulseUs":2500,"openDrain":false})";
int main(int argc, char** argv) {
    assert(argc == 2);
    PeripheralConfig config;
    config.id = "pwm"; config.type = PeripheralType::PCA9685_PWM; config.pinCount = 0;
    JsonDocument document;
    assert(!deserializeJson(document, valid));
    String error;
    assert(FastBee::parsePca9685Config(document.as<JsonObjectConst>(), config, error));
    const auto original = config.params.pca9685;
    const auto rejects = [&] {
        assert(!FastBee::parsePca9685Config(document.as<JsonObjectConst>(), config, error));
        assert(!error.isEmpty());
        assert(!memcmp(&original, &config.params.pca9685, sizeof(original)));
        document.clear(); assert(!deserializeJson(document, valid));
    };
    if (!strcmp(argv[1], "roundtrip")) {
        JsonDocument serialized;
        FastBee::serializePca9685Config(serialized.to<JsonObject>(), config);
        assert(serialized.size() == 11 && serialized["openDrain"].is<bool>());
        assert(!serialized["powerCycleConfirmed"].is<bool>());
        assert(FastBee::parsePca9685Config(serialized.as<JsonObjectConst>(), config, error));
        assert(!memcmp(&original, &config.params.pca9685, sizeof(original)));
    } else if (!strcmp(argv[1], "schema")) {
        for (const char* key : {"powerCycleConfirmed", "autoEnable", "startupPulse", "token"}) {
            document[key] = true; rejects();
        }
        for (const char* key : {"address", "channelMask", "pwmHz", "clockHz", "openDrain", "busPeripheralId", "oePeripheralId"}) {
            document.remove(key); rejects();
        }
        document["pwmHz"] = "50"; rejects();
        document["openDrain"] = 0; rejects();
        document["address"] = 64.5; rejects();
        document["channelMask"] = 65536; rejects();
    } else if (!strcmp(argv[1], "ranges")) {
        for (int value : {-1, 0, 63, 112, 120, 256}) { document["address"] = value; rejects(); }
        document["channelMask"] = 0; rejects();
        document["pwmHz"] = 0; rejects();
        document["clockHz"] = 50000001; rejects();
        document["pwmHz"] = 1000; rejects(); // configured servo pulse exceeds the shared period
        document["minAngle"] = 180; rejects();
        document["maxPulseUs"] = 500; rejects();
        document["maxPulseUs"] = 500.01; rejects(); // endpoints collapse to same tick
    } else if (!strcmp(argv[1], "references")) {
        document["busPeripheralId"] = "pwm"; rejects();
        document["oePeripheralId"] = "bus"; rejects();
        document["oePeripheralId"] = ""; rejects();
        document["oePeripheralId"] = "12345678901234567890123456789012345678901"; rejects();
        assert(!deserializeJson(document, R"({"busPeripheralId":"bus","oePeripheralId":"oe\u0000redirect","address":64,"channelMask":1,"pwmHz":50,"clockHz":25000000,"minAngle":0,"maxAngle":180,"minPulseUs":500,"maxPulseUs":2500,"openDrain":false})"));
        rejects();
        config.pinCount = 2;
        assert(!FastBee::validatePca9685Config(config, error));
    } else {
        FastBee::Pca9685Command command;
        if (!strcmp(argv[1], "commands")) {
            for (const char* json : {R"({"action":"duty","channel":15,"value":100})", R"({"action":"pulse","channel":0,"value":1500})",
                R"({"action":"angle","channel":0,"value":90})", R"({"action":"enable"})", R"({"action":"disable"})", R"({"action":"stop"})"})
                assert(FastBee::parsePca9685Command(json, command, error));
        } else if (!strcmp(argv[1], "reject_commands")) {
            for (const char* json : {"", "stop", "[]", R"({"action":"prepare"})", R"({"action":"start"})", R"({"action":"enable","powerCycleConfirmed":true})",
                R"({"action":"enable","channel":0})", R"({"action":"stop","periphId":"other"})", R"({"action":"duty","channel":16,"value":0})",
                R"({"action":"duty","channel":-1,"value":0})", R"({"action":"duty","channel":0.5,"value":0})", R"({"action":"duty","channel":0,"value":101})",
                R"({"action":"duty","value":0})", R"({"action":"pulse","channel":0,"value":-1})", R"({"action":"angle","channel":0,"value":"$value"})",
                R"({"action":"stop\u0000extra"})"}) assert(!FastBee::parsePca9685Command(json, command, error));
        } else assert(false);
    }
}
