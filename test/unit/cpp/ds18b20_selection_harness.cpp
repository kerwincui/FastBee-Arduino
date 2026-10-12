#include "core/Ds18b20Selection.h"
#include <cassert>
#include <cstring>
#include <string>

int main() {
    using FastBee::parseDs18b20Selection;
    uint8_t index = 255, rom[8]{};
    bool addressed = true;
    JsonDocument doc;
    auto parse = [&](const char* json) {
        doc.clear(); assert(!deserializeJson(doc, json));
        return parseDs18b20Selection(doc.as<JsonVariantConst>(), index, rom, addressed);
    };
    assert(!parse("{}") && !index && !addressed);
    assert(!parse("{\"deviceIndex\":15,\"deviceAddress\":\"\"}") && index == 15 && !addressed);
    assert(!parse("{\"deviceAddress\":\"280000000000001E\"}") && addressed);
    const uint8_t expected[8] = {0x28,0,0,0,0,0,0,0x1e};
    assert(std::memcmp(expected, rom, 8) == 0);
    assert(!parse("{\"deviceAddress\":\"280000000000001e\"}") && addressed);
    for (const char* value : {"-1", "16", "256", "4294967296", "1.5", "true", "\"1\"", "{}", "[]"}) {
        const std::string json = std::string("{\"deviceIndex\":") + value + "}";
        const char* error = parse(json.c_str());
        assert(error && std::strcmp(error, "DS18B20_INDEX_INVALID") == 0);
    }
    for (const char* value : {"\"280000000000001f\"", "\"280000000000001\"", "\"280000000000001e00\"",
                              "\"280000000000001G\"", "\"100000000000001e\"", "\"280000000000001e\\u0000\"",
                              "\"28000000\\u00000000001e\"", "true", "28", "[]", "{}"}) {
        const std::string json = std::string("{\"deviceAddress\":") + value + "}";
        const char* error = parse(json.c_str());
        assert(error && std::strcmp(error, "DS18B20_ROM_INVALID") == 0 && !addressed);
    }
    assert(!FastBee::parseDs18b20Address(nullptr, 16, rom));
    assert(!FastBee::parseDs18b20Address("280000000000001e", 16, nullptr));
}
