#ifndef FASTBEE_PERIPHERAL_PREFLIGHT_ROUTE_SUPPORT_H
#define FASTBEE_PERIPHERAL_PREFLIGHT_ROUTE_SUPPORT_H

#include <ArduinoJson.h>
#include "core/PeripheralConfig.h"
#include "core/PeripheralResourcePlanner.h"

class PeripheralManager;

namespace FastBee {

class PeripheralFormFields;

// Acknowledgement bits accepted by the mutation/preflight/import routes.
inline constexpr uint32_t PREFLIGHT_ACK_MASK =
    FastBee::ACK_STRAPPING_PIN |
    FastBee::ACK_BOARD_SHARED_PIN |
    FastBee::ACK_UART0_CONSOLE |
    FastBee::ACK_ADC2_WITH_WIFI;

bool sameText(const char* left, const char* right);
bool compiledBoardProfileSupported();
const BoardProfile* resolveBoardProfile(const String& profileId);

const char* preflightOperationName(PreflightOperation operation);
uint32_t requiredAcknowledgements(const PreflightResult& result);
bool hasHardPreflightError(const PreflightResult& result);

// Preflight response document construction (wire schema lives here).
void addPreflightIssues(JsonArray issues, const PreflightResult& result);
void addCandidateClaims(JsonArray claims, const PreflightResult& result,
                        const char* candidateId);
void addResolvedPins(JsonObject data, const PeripheralConfig& candidate);
void addResourceRows(JsonArray rows, const BoardProfile& profile,
                     const PreflightResult& result);
void addPreflightResponse(JsonDocument& doc, const BoardProfile& profile,
                          const PreflightResult& result,
                          const PeripheralConfig& candidate,
                          PreflightOperation operation, bool success, bool allowed,
                          const char* message = nullptr);

void resetPeripheralTypeDefaults(PeripheralConfig& config);

// MCP23017 group review support: optimistic review token (not an
// authorization credential) and the locked-snapshot group preview document.
String mcpGroupReviewToken(uint64_t version);
bool buildMcpGroupPreview(PeripheralManager& manager, const String& id,
                          JsonDocument& doc, String& error);

// Builds the candidate configuration for the standalone preflight endpoint
// from form fields and the current manager snapshot. Pure parameter
// interpretation; HTTP admission and serialization stay in the route layer.
bool buildPreflightCandidate(const PeripheralFormFields& form,
                             PeripheralManager& manager,
                             PreflightOperation operation,
                             PeripheralConfig& candidate, String& excludeId,
                             String& errorMsg, int& errorStatus);

} // namespace FastBee

#endif // FASTBEE_PERIPHERAL_PREFLIGHT_ROUTE_SUPPORT_H
