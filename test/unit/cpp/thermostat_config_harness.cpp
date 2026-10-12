#include "core/ThermostatConfig.h"
#include "network/ThermostatConfigJson.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <limits>
using namespace FastBee;
static const char* validConfig = R"({"sourceRef":"room_temperature","mode":"heat","setpoint":25,"hysteresis":1,"minOnSec":30,"minOffSec":30,"maxAgeSec":30,"recovery":"manual","startup":"fresh","fault":"off"})";
static bool parse(const char* value) { ThermostatConfig cfg; String error; return parseThermostatConfig(value, cfg, error); }
int main(int argc, char** argv) {
    assert(argc == 2);
    const char* mode = argv[1];
    if (!std::strcmp(mode, "schema-json")) {
        JsonDocument doc;
        writeThermostatConfigJson(doc.to<JsonObject>());
        assert(!doc.overflowed());
        std::string text; serializeJson(doc, text);
        std::cout << text;
    } else if (!std::strcmp(mode, "metadata")) {
        JsonDocument doc; doc["sourceRef"] = "room_temperature";
        for (const auto& field : kThermostatNumbers) doc[field.key] = field.defaultValue;
        for (const auto& field : kThermostatPolicies) doc[field.key] = field.defaultValue;
        assert(doc.size() == kThermostatConfigFieldCount);
        auto read = [&]() {
            std::string text; serializeJson(doc, text);
            ThermostatConfig cfg; cfg.sourceRef = "unchanged"; String error;
            bool accepted = parseThermostatConfig(text.c_str(), cfg, error);
            if (!accepted) assert(cfg.sourceRef == "unchanged");
            return accepted;
        };
        assert(read());
        ThermostatSettings defaults;
        assert(defaults.setpoint == doc["setpoint"].as<double>());
        assert(defaults.hysteresis == doc["hysteresis"].as<double>());
        assert(defaults.minOnMs == doc["minOnSec"].as<uint32_t>() * 1000U);
        assert(defaults.minOffMs == doc["minOffSec"].as<uint32_t>() * 1000U);
        assert(defaults.maxAgeMs == doc["maxAgeSec"].as<uint32_t>() * 1000U);
        for (const auto& field : kThermostatNumbers) {
            assert(field.accepts(field.minValue) && field.accepts(field.maxValue));
            for (double value : {field.minValue - 1, field.maxValue + 1,
                    std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
                assert(!field.accepts(value)); doc[field.key] = value; assert(!read());
            }
            if (field.integer) {
                doc[field.key] = field.minValue + 0.5; assert(!read());
                doc[field.key] = field.minValue; assert(read());
                doc[field.key] = field.maxValue; assert(read());
            }
            doc[field.key] = field.defaultValue;
            assert(read());
        }
        for (const auto& field : kThermostatPolicies) {
            for (size_t i = 0; i < field.count; ++i) {
                doc[field.key] = field.choices[i]; assert(read());
            }
            doc[field.key] = "unsupported"; assert(!read());
            doc[field.key] = field.defaultValue;
        }
        doc["sourceRef"] = std::string(kThermostatSourceMaxBytes, 'a'); assert(read());
        doc["sourceRef"] = std::string(kThermostatSourceMaxBytes + 1, 'a'); assert(!read());
        doc["sourceRef"] = "room_temperature";
        doc["setpoint"] = kThermostatNumbers[ThermostatSetpoint].minValue + defaults.hysteresis; assert(read());
        doc["setpoint"] = kThermostatNumbers[ThermostatSetpoint].maxValue - defaults.hysteresis; assert(read());
        doc["setpoint"] = kThermostatNumbers[ThermostatSetpoint].maxValue; assert(!read());
        doc["setpoint"] = defaults.setpoint;
        for (const auto& field : kThermostatNumbers) {
            doc.remove(field.key); assert(!read()); doc[field.key] = field.defaultValue;
        }
        doc["unexpected"] = 1; assert(!read());
    } else if (!std::strcmp(mode, "config")) {
        ThermostatConfig cfg; String error;
        assert(parseThermostatConfig(validConfig, cfg, error));
        assert(cfg.sourceRef == "room_temperature" && !cfg.cooling && !cfg.automaticRecovery);
        assert(cfg.minOffMs == 30000 && cfg.hysteresis == 1);
        assert(!parse("{}"));
        assert(!parse("[]"));
        assert(!parse("{\"sourceRef\":3}"));
    } else if (!std::strcmp(mode, "ranges")) {
        JsonDocument doc; assert(!deserializeJson(doc, validConfig));
        for (const char* key : {"minOnSec", "minOffSec", "maxAgeSec"}) {
            doc[key] = 0; std::string text; serializeJson(doc, text); assert(!parse(text.c_str()));
            doc[key] = 1.5; text.clear(); serializeJson(doc, text); assert(!parse(text.c_str()));
            doc[key] = 30;
        }
        doc["hysteresis"] = 0; std::string text; serializeJson(doc, text); assert(!parse(text.c_str()));
        doc["hysteresis"] = 1; doc["setpoint"] = 125; text.clear(); serializeJson(doc, text); assert(!parse(text.c_str()));
    } else if (!std::strcmp(mode, "policy")) {
        for (const char* key : {"fault", "startup", "recovery", "mode"}) {
            JsonDocument doc; assert(!deserializeJson(doc, validConfig)); doc[key] = "on";
            std::string text; serializeJson(doc, text); assert(!parse(text.c_str()));
        }
        JsonDocument doc; assert(!deserializeJson(doc, validConfig)); doc["pin"] = 12;
        std::string text; serializeJson(doc, text); assert(!parse(text.c_str()));
    } else if (!std::strcmp(mode, "standalone")) {
        PeriphExecRule rule; rule.name = "Temperature"; rule.id = "temperature";
        ExecTrigger trigger; trigger.triggerType = 1; trigger.intervalSec = 1; rule.triggers.push_back(trigger);
        ExecAction action; action.actionType = 39; action.targetPeriphId = "relay"; action.actionValue = validConfig;
        rule.actions.push_back(action); String error;
        assert(validateThermostatRule(rule, error));
        rule.allowPanelControl = true; assert(!validateThermostatRule(rule, error)); rule.allowPanelControl = false;
        rule.actions[0].useReceivedValue = true; assert(!validateThermostatRule(rule, error)); rule.actions[0].useReceivedValue = false;
        rule.actions[0].syncDelayMs = 1; assert(!validateThermostatRule(rule, error)); rule.actions[0].syncDelayMs = 0;
        rule.actions.push_back(action); assert(!validateThermostatRule(rule, error));
    } else return 2;
    return 0;
}
