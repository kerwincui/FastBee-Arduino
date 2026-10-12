#include "protocols/ModbusRegisterMapping.h"
#include <cassert>
#include <functional>
#include <string>

struct PollTask {
    bool enabled = true;
    uint8_t slaveAddress = 1, mappingCount = 1;
    RegisterMapping mappings[8];
};
struct OneShotResult { uint16_t data[32] = {}, count = 2; };
struct PollTaskCache { bool valid = true; uint16_t values[32] = {}, count = 2; };
struct ModbusHandler {
    struct { uint8_t transferType = 0; struct { uint8_t taskCount = 1; PollTask tasks[8]; } master; } config;
    PollTaskCache _taskCache[8];
    String collectCachedPollData() const;
};
#include "production_modbus_mapping.inc"

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string mode = argv[1];
    uint16_t words[] = {0xffff, 0xffff};
    double value = 123;
    auto decode = [&](uint8_t type, uint8_t order = 0, double scale = 1, size_t count = 2) {
        return FastBee::decodeModbusRegisterValue(words, count, 0, type, order, scale, value);
    };
    if (mode == "legacy") {
        RegisterMapping m; assert(m.wordOrder == 0 && m.scaleFactor == 1 && m.decimalPlaces == 1);
        words[0] = 1; words[1] = 2; assert(decode(2) && value == 65538);
    } else if (mode == "unsigned") {
        assert(decode(2) && value == 4294967295.0); assert(decode(0) && value == 65535);
    } else if (mode == "signed") {
        assert(decode(3) && value == -1); words[0] = 0x8000; words[1] = 0;
        assert(decode(3) && value == -2147483648.0); assert(decode(1) && value == -32768);
    } else if (mode == "float") {
        // Manufacturer V2.4 p3 voltage example, high register first.
        words[0] = 0x4366; words[1] = 0x3334;
        assert(decode(4) && std::fabs(value - 230.2) < .00003);
        words[0] = 0xc148; words[1] = 0; assert(decode(4) && value == -12.5);
    } else if (mode == "orders") {
        for (uint8_t type : {uint8_t(2), uint8_t(3), uint8_t(4)}) {
            words[0] = 0xc148; words[1] = 0x1234; assert(decode(type)); const double expected = value;
            std::swap(words[0], words[1]); assert(decode(type, 1) && value == expected);
        }
    } else if (mode == "invalid") {
        assert(!decode(5) && value == 123); assert(!decode(2, 2)); assert(!decode(0, 1));
        words[0] = 0x7f80; words[1] = 0; assert(!decode(4)); assert(!decode(4, 0, 0));
        words[0] = 0x7fc0; assert(!decode(4));
        assert(!decode(0, 0, std::numeric_limits<double>::infinity()));
        assert(!decode(0, 0, 1e100));
    } else if (mode == "short") {
        assert(!decode(2, 0, 1, 1) && value == 123);
        assert(!decode(0, 0, 1, 0));
        assert(!FastBee::decodeModbusRegisterValue(nullptr, 2, 0, 0, 0, 1, value));
        assert(!FastBee::decodeModbusRegisterValue(words, 2, SIZE_MAX, 4, 0, 1, value));
    } else if (mode == "zero") {
        assert(decode(2, 0, 0) && value == 0); words[0] = words[1] = 0;
        assert(decode(4) && value == 0);
    } else if (mode == "format") {
        RegisterMapping m; m.dataType = 2; m.decimalPlaces = 0;
        char result[96]; assert(FastBee::formatModbusMapping(words, 2, m, result, sizeof(result)));
        assert(std::string(result) == "4294967295");
        assert(!FastBee::formatModbusMapping(words, 2, m, result, 4));
        m.dataType = 4; m.scaleFactor = std::numeric_limits<float>::max(); m.decimalPlaces = 6;
        words[0] = 0x7f7f; words[1] = 0xffff;
        assert(FastBee::formatModbusMapping(words, 2, m, result, sizeof(result)) && std::strlen(result) > 70);
        m.decimalPlaces = 7; assert(!FastBee::formatModbusMapping(words, 2, m, result, sizeof(result)));
    } else if (mode == "parser") {
        JsonDocument doc; RegisterMapping m;
        assert(!deserializeJson(doc, R"({"dataType":4,"wordOrder":1,"scaleFactor":0,"decimalPlaces":0,"sensorId":"voltage","unit":"V"})"));
        assert(FastBee::readModbusRegisterMapping(doc.as<JsonObjectConst>(), 2, m));
        assert(m.wordOrder == 1 && m.scaleFactor == 0 && m.decimalPlaces == 0);
        for (const char* invalid : {R"({"dataType":256})",R"({"dataType":"2"})",R"({"wordOrder":1})",
            R"({"wordOrder":null})",R"({"decimalPlaces":7})",R"({"scaleFactor":null})",R"({"scaleFactor":1e50})",
            R"({"scaleFactor":1e-50})",R"({"sensorId":"1234567890123456"})",R"({"sensorId":"x\"y"})",
            R"({"unit":"12345678"})",R"({"regOffset":1,"dataType":4})",R"({"sensorId":"a\u0000b"})"}) {
            assert(!deserializeJson(doc, invalid)); m.wordOrder = 1;
            assert(!FastBee::readModbusRegisterMapping(doc.as<JsonObjectConst>(), 2, m) && m.wordOrder == 1);
        }
        doc.clear(); auto object = doc.to<JsonObject>(); object["scaleFactor"] = 0;
        assert(FastBee::readModbusRegisterMapping(object, 1, m) && m.wordOrder == 0 && m.scaleFactor == 0);
        for (const char* invalid : {R"({"quantity":null,"mappings":[{}]})", R"({"quantity":"2","mappings":[{}]})",
             R"({"quantity":0,"mappings":[{}]})", R"({"quantity":126,"mappings":[{}]})", R"({"mappings":null})"}) {
            assert(!deserializeJson(doc, invalid));
            assert(!FastBee::validModbusTaskMappings(doc.as<JsonObjectConst>()));
        }
    } else if (mode == "outputs") {
        ModbusHandler handler; auto& task = handler.config.master.tasks[0]; auto& m = task.mappings[0];
        m.dataType = 4; m.wordOrder = 1; m.decimalPlaces = 1; std::strcpy(m.sensorId, "voltage");
        OneShotResult read; read.data[0] = 0x3334; read.data[1] = 0x4366;
        std::memcpy(handler._taskCache[0].values, read.data, sizeof(read.data));
        String callback; const auto report = mappedReport(task, read, true, [&](uint8_t, const String& data) { callback = data; });
        assert(report == callback && report.indexOf("230.2") >= 0);
        assert(handler.collectCachedPollData().indexOf("230.2") >= 0);
        JsonDocument config, output;
        assert(!deserializeJson(config, R"({"mappings":[{"sensorId":"voltage","dataType":4,"wordOrder":1,"decimalPlaces":1}]})"));
        directReport(config.as<JsonObject>(), read, output.to<JsonArray>());
        assert(std::string(output[0]["value"].as<const char*>()) == "230.2");
        read.count = 1; handler._taskCache[0].count = 1; callback = "unchanged";
        assert(mappedReport(task, read, true, [&](uint8_t, const String& data) { callback = data; }) == "[]");
        assert(callback == "unchanged" && handler.collectCachedPollData() == "[]");
        directReport(config.as<JsonObject>(), read, output.to<JsonArray>()); assert(output.size() == 0);
        read.count = 2; read.data[0] = 0; read.data[1] = 0x7fc0;
        assert(mappedReport(task, read, false, {}) == "[]");
    } else return 2;
}
