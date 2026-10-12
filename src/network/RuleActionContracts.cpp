#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_PERIPH_EXEC
#include "network/RuleActionContracts.h"
#include "network/ActionContractJson.h"
#include "core/PeripheralManager.h"
#include "core/Pca9685ConfigSupport.h"

namespace FastBee {
void appendRuleActionContracts(JsonObject data, const PeriphExecRule& candidate) {
    JsonArray contracts = data["actionContracts"].to<JsonArray>();
    data["actionContractsVersion"] = 1;
    data["actionContractsTruncated"] = candidate.actions.size() > MAX_ACTIONS_PER_RULE;
    auto& manager = PeripheralManager::getInstance();
    // The existing profile limits a valid rule to four actions. Invalid larger
    // inputs never create an unbounded description response.
    for (size_t i = 0; i < candidate.actions.size() && i < MAX_ACTIONS_PER_RULE; ++i) {
        const auto& action = candidate.actions[i];
        ActionContractMetadata description;
        PeripheralConfig target, provider;
        if (manager.getPeripheralSnapshot(action.targetPeriphId, target)) {
            const PeripheralConfig* parent = nullptr;
            if (target.type == PeripheralType::GPIO_DIGITAL_OUTPUT && target.isExpanderChannel() &&
                std::memchr(target.params.gpio.providerId, 0, sizeof(target.params.gpio.providerId)) &&
                manager.getPeripheralSnapshot(target.params.gpio.providerId, provider)) parent = &provider;
            Pca9685Command parsed;
            String error;
            const char* command = nullptr;
            if (target.type == PeripheralType::PCA9685_PWM && !action.useReceivedValue &&
                parsePca9685Command(action.actionValue, parsed, error)) command = parsed.action;
            description = describeActionContract(action.actionType, &target, command, parent);
        }
        JsonObject item = contracts.add<JsonObject>();
        item["index"] = i;
        item["actionType"] = action.actionType;
        writeActionContractJson(item, description);
    }
}
} // namespace FastBee
#endif
