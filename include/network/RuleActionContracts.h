#pragma once
#include <ArduinoJson.h>
struct PeriphExecRule;

namespace FastBee {
// Read-only bounded metadata, using the same actual target/parameter contracts.
// Caller must reject an overflowed JsonDocument before sending success.
void appendRuleActionContracts(JsonObject data, const PeriphExecRule& candidate);
} // namespace FastBee
