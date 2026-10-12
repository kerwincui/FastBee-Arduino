#include <cmath>
using std::isnan; using std::isinf;
#include "core/ActionContractMetadata.h"
#include "network/ActionContractJson.h"
#include "core/Pca9685ConfigSupport.h"
#include "core/H1ParameterJson.h"
#include <cassert>
#include <cstring>
#include <map>
// The transport/storage seam is substituted. Wrapper bounds and the literal
// parser are compiled verbatim from production, without driver/bus methods.
class PeripheralManager {
public:
    std::map<std::string,PeripheralConfig> configs;
    unsigned reads=0;
    static PeripheralManager& getInstance() { static PeripheralManager manager; return manager; }
    bool getPeripheralSnapshot(const String& name, PeripheralConfig& out) {
        ++reads;
        const auto found=configs.find(name.c_str());
        if(found==configs.end()) return false;
        out=found->second; return true;
    }
};
namespace FastBee {
// @parser@
// @wrapper@
}
using FastBee::describeActionContract;
static uint8_t id(ExecActionType action) { return static_cast<uint8_t>(action); }
static void unknown(const FastBee::ActionContractMetadata& value) {
    assert(!value.covered && !std::strcmp(value.schema,"unknown") && !value.timeoutBudgetMs);
}
int main() {
    using A=ExecActionType;
    for(unsigned a=0;a<256;++a) unknown(describeActionContract(a,nullptr));
    PeripheralConfig target;
    target.type=PeripheralType::GPIO_DIGITAL_OUTPUT;
    target.params.gpio.ioSource=0; target.params.gpio.passive=false;
    for(auto action:{A::ACTION_HIGH,A::ACTION_LOW,A::ACTION_HIGH_INVERTED,A::ACTION_LOW_INVERTED}) {
        const auto value=describeActionContract(id(action),&target);
        assert(value.covered && !std::strcmp(value.idempotence,"absolute_command_same_config"));
    }
    assert(!std::strcmp(describeActionContract(id(A::ACTION_GPIO_TOGGLE),&target).idempotence,"non_idempotent"));
    unknown(describeActionContract(id(A::ACTION_BLINK),&target));
    target.params.gpio.passive=true;
    unknown(describeActionContract(id(A::ACTION_HIGH),&target));
    target.params.gpio.passive=false;
    target.params.gpio.ioSource=1;
    std::strcpy(target.params.gpio.providerId,"expander");
    unknown(describeActionContract(id(A::ACTION_HIGH),&target));
    PeripheralConfig provider; provider.type=PeripheralType::IO_EXPANDER; provider.id="expander";
    std::strcpy(provider.params.ioExpander.driver,"MCP23017");
    const auto mcp=describeActionContract(id(A::ACTION_HIGH),&target,nullptr,&provider);
    assert(mcp.covered && !std::strcmp(mcp.fault,"mcp23017_faulted_no_latch_replay"));
    provider.id="other"; unknown(describeActionContract(id(A::ACTION_HIGH),&target,nullptr,&provider));
    provider.id="expander"; std::strcpy(provider.params.ioExpander.driver,"PCF8574");
    unknown(describeActionContract(id(A::ACTION_HIGH),&target,nullptr,&provider));

    target=PeripheralConfig{}; target.type=PeripheralType::DAC;
    const auto dac=describeActionContract(id(A::ACTION_SET_DAC),&target);
    assert(dac.covered && !std::strcmp(dac.result,"sdk_ack_only"));
    target.type=PeripheralType::UART; unknown(describeActionContract(id(A::ACTION_SET_DAC),&target));

    target=PeripheralConfig{}; target.type=PeripheralType::SENSOR_GENERIC;
    std::strcpy(target.params.sensor.driver,"ADS1115");
    target.params.sensor.mux=4; target.params.sensor.fullScaleMv=4096; target.params.sensor.dataRate=128;
    const auto ads=describeActionContract(id(A::ACTION_SENSOR_READ),&target);
    assert(ads.covered && ads.timeoutBudgetMs==66);
    assert(!std::strcmp(ads.idempotence,"cache_aware_sample_with_original_age"));
    target.params.sensor.dataRate=8;
    assert(describeActionContract(id(A::ACTION_SENSOR_READ),&target).timeoutBudgetMs==300);
    target.params.sensor.dataRate=0; unknown(describeActionContract(id(A::ACTION_SENSOR_READ),&target));
    target.params.sensor.dataRate=128; std::strcpy(target.params.sensor.driver,"INA226");
    unknown(describeActionContract(id(A::ACTION_SENSOR_READ),&target));

    target=PeripheralConfig{}; target.type=PeripheralType::PCA9685_PWM;
    unknown(describeActionContract(id(A::ACTION_CALL_PERIPHERAL),&target));
    for(const char* command:{"start","prepare","reset","unknown"})
        unknown(describeActionContract(id(A::ACTION_CALL_PERIPHERAL),&target,command));
    for(const char* command:{"enable","disable","duty","pulse","angle"}) {
        const auto value=describeActionContract(id(A::ACTION_CALL_PERIPHERAL),&target,command);
        assert(value.covered && !std::strcmp(value.replay,"no_automatic_replay"));
    }
    assert(!std::strcmp(describeActionContract(id(A::ACTION_CALL_PERIPHERAL),&target,"stop").replay,
                       "release_cleanup_only_after_stop"));

    target=PeripheralConfig{}; target.type=PeripheralType::GPIO_DIGITAL_OUTPUT;
    target.pinCount=1; target.params.gpio.ioSource=0; target.params.gpio.passive=false;
    target.params.gpio.invertLogic=false; target.params.gpio.initialState=GPIOState::STATE_HIGH;
    const auto thermostat=describeActionContract(id(A::ACTION_THERMOSTAT),&target);
    assert(thermostat.covered && !std::strcmp(thermostat.result,"controller_status_only"));
    target.params.gpio.initialState=GPIOState::STATE_LOW;
    unknown(describeActionContract(id(A::ACTION_THERMOSTAT),&target));
    unknown(describeActionContract(id(A::ACTION_MODBUS_POLL),&target));

    JsonDocument doc;
    FastBee::writeActionContractJson(doc.to<JsonObject>(),ads);
    assert(!doc.overflowed());
    assert(doc["timeoutBudgetMs"].as<unsigned>()==66);
    assert(!doc["physicalVerified"].as<bool>() && !doc["runtimeAdmitted"].as<bool>());
    FastBee::writeActionContractJson(doc.to<JsonObject>(),{});
    assert(!std::strcmp(doc["coverage"].as<const char*>(),"unknown"));
    assert(doc["timeoutBudgetMs"].isNull()); // Reusing object cannot retain old budget.

    auto& pm=PeripheralManager::getInstance();
    PeripheralConfig pca; pca.id="pca"; pca.type=PeripheralType::PCA9685_PWM;
    pm.configs["pca"]=pca;
    PeriphExecRule rule;
    ExecAction action; action.actionType=id(A::ACTION_CALL_PERIPHERAL); action.targetPeriphId="pca";
    action.actionValue="{\"action\":\"stop\"}";
    rule.actions.push_back(action);
    FastBee::appendRuleActionContracts(doc.to<JsonObject>(),rule);
    assert(doc["actionContractsVersion"].as<unsigned>()==1 && !doc["actionContractsTruncated"].as<bool>());
    assert(doc["actionContracts"].size()==1 && doc["actionContracts"][0]["index"].as<unsigned>()==0);
    assert(!strcmp(doc["actionContracts"][0]["replay"].as<const char*>(),"release_cleanup_only_after_stop"));
    for(const char* bad:{"{\"action\":\"duty\",\"channel\":16,\"value\":1}",
                         "{\"action\":\"stop\",\"extra\":true}","{\"action\":\"start\"}"}) {
        rule.actions[0].actionValue=bad;
        FastBee::appendRuleActionContracts(doc.to<JsonObject>(),rule);
        assert(!strcmp(doc["actionContracts"][0]["coverage"].as<const char*>(),"unknown"));
    }
    rule.actions[0].actionValue="{\"action\":\"stop\"}";
    rule.actions[0].useReceivedValue=true;
    FastBee::appendRuleActionContracts(doc.to<JsonObject>(),rule);
    assert(!strcmp(doc["actionContracts"][0]["coverage"].as<const char*>(),"unknown"));
    rule.actions[0].targetPeriphId="missing";
    FastBee::appendRuleActionContracts(doc.to<JsonObject>(),rule);
    assert(!strcmp(doc["actionContracts"][0]["coverage"].as<const char*>(),"unknown"));
    rule.actions.assign(MAX_ACTIONS_PER_RULE+20,action); pm.reads=0;
    FastBee::appendRuleActionContracts(doc.to<JsonObject>(),rule);
    assert(doc["actionContractsTruncated"].as<bool>() && doc["actionContracts"].size()==MAX_ACTIONS_PER_RULE);
    assert(pm.reads==MAX_ACTIONS_PER_RULE);
    rule.actions.clear();
    FastBee::appendRuleActionContracts(doc.to<JsonObject>(),rule);
    assert(doc["actionContracts"].size()==0 && !doc["actionContractsTruncated"].as<bool>());
}
