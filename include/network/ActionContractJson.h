#pragma once
#include <ArduinoJson.h>
#include "core/ActionContractMetadata.h"

namespace FastBee {
inline void writeActionContractJson(JsonObject out, const ActionContractMetadata& value) {
    out["coverage"] = value.covered ? "described" : "unknown";
    out["schema"] = value.schema;
    out["result"] = value.result;
    out["idempotence"] = value.idempotence;
    out["replay"] = value.replay;
    out["timeoutScope"] = value.timeoutScope;
    if (value.timeoutBudgetMs) out["timeoutBudgetMs"] = value.timeoutBudgetMs;
    else out["timeoutBudgetMs"] = nullptr;
    out["fault"] = value.fault;
    out["physicalVerified"] = false;
    // This response is a declaration, never current runtime admission.
    out["runtimeAdmitted"] = false;
}
} // namespace FastBee
