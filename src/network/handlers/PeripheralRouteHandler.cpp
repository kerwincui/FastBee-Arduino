#include "./network/handlers/PeripheralRouteHandler.h"
#include "network/LiteralStringMatch.h"
#include "PeripheralRouteReadSupport.h"
#include "./network/handlers/HandlerUtils.h"
#include "network/JsonFragmentsResponse.h"
#include "network/FixedJsonField.h"
#include "network/NetworkManager.h"
#include "network/NetworkResourceConfig.h"
#include "./network/WebHandlerContext.h"
#include "core/FeatureFlags.h"
#include "core/AsyncExecTypes.h"
#include "core/ChipConfig.h"
#include "core/PeripheralManager.h"
#include "core/ResourceProfile.h"
#include "core/SensorDriverCache.h"
#include "core/ActionValueContract.h"
#include "core/DacWriteAccess.h"
#include "core/PeripheralRouteMapper.h"
#include "core/PeripheralPreflightRouteSupport.h"
#include "core/PeripheralParamApply.h"
#include "core/PeripheralFormFields.h"
#include "../../core/PeripheralManagerConfigHelpers.h"
#include "network/HttpOutputControl.h"
#include <cmath>
#include <cstdlib>
#include "core/DriverRegistry.h"
#include "core/RuntimeFeatureManager.h"
#include "core/I2CBusManager.h"
#include "peripherals/SDCardManager.h"
#include "network/SDCardStatusJson.h"
#include "network/RuntimeResourceJson.h"
#include "network/RuntimePinResourceJson.h"
#include "core/SpiBusInitializer.h"
#include "core/SdAccess.h"
#if FASTBEE_ENABLE_SCREEN_UI
#include "screen/ScreenRuntime.h"
#endif
#if FASTBEE_ENABLE_SENSOR_DRIVER
#include "peripherals/SensorDriver.h"
#endif
#if FASTBEE_ENABLE_DS1302
#include "peripherals/drivers/DS1302Driver.h"
#include "core/RtcAccess.h"
#endif
#if FASTBEE_ENABLE_LCD1602
#include "peripherals/drivers/LCD1602Driver.h"
#endif
#if FASTBEE_ENABLE_RFID
#include "peripherals/drivers/RFIDDriver.h"
#endif
#include <ArduinoJson.h>
#include <Wire.h>
#include <WiFi.h>
#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <math.h>
#include <new>
#include <vector>
#include <esp_system.h>
#include "systems/LoggerSystem.h"

namespace {
using namespace FastBee::PeripheralRouteReadDetail;

// JSON/param helpers now live in core mapping/preflight units; the route
// layer keeps only HTTP admission, adapters and response serialization.
using FastBee::PREFLIGHT_ACK_MASK;
using FastBee::parseStrictHex;
using FastBee::addAnalogInputParams;
using FastBee::addCandidateClaims;
using FastBee::addDigitalInputParams;
using FastBee::addDisplayParams;
using FastBee::addEncoderParams;
using FastBee::addGenericPeripheralFields;
using FastBee::addPassiveBuzzerFlag;
using FastBee::addPeripheralDiagnosticCapability;
using FastBee::addPeripheralListIdentity;
using FastBee::addPeripheralListParams;
using FastBee::addP1PeripheralParams;
using FastBee::addPreflightIssues;
using FastBee::addPreflightResponse;
using FastBee::addResolvedPins;
using FastBee::addResourceRows;
using FastBee::addServoParams;
using FastBee::addUartParams;
using FastBee::applyAddTypeParams;
using FastBee::applyAnalogInputParams;
using FastBee::applyDigitalInputParams;
using FastBee::applyDisplayRequestParameters;
using FastBee::applyIoPeripheralRequestParameters;
using FastBee::applyP1PeripheralParams;
using FastBee::applyPeripheralRequestParameters;
using FastBee::applyServoParams;
using FastBee::applyUpdateTypeParams;
using FastBee::buildMcpGroupPreview;
using FastBee::buildPreflightCandidate;
using FastBee::bytesToHex;
using FastBee::calculateDayOfWeek;
using FastBee::clampParamInt;
using FastBee::compiledBoardProfileSupported;
using FastBee::daysInMonth;
using FastBee::escapePeriphJsonString;
using FastBee::hasHardPreflightError;
using FastBee::isAnalogInputPeripheral;
using FastBee::isDigitalInputPeripheral;
using FastBee::isListMemoryCriticallyLow;
using FastBee::mcpGroupReviewToken;
using FastBee::mutationErrorStatus;
using FastBee::normalizeSensorSampleInterval;
using FastBee::parsePeripheralPins;
using FastBee::preflightOperationName;
using FastBee::readDisplayRequestInteger;
using FastBee::readIoFormInteger;
using FastBee::requiredAcknowledgements;
using FastBee::resetPeripheralTypeDefaults;
using FastBee::resolveBoardProfile;
using FastBee::sameText;
using FastBee::shouldForceCompactList;
using FastBee::userConfigurablePeripheralTypeError;

constexpr size_t LCD1602_API_MAX_BYTES = 128;

bool allocatePreflightWorkspace(WebHandlerContext* ctx, AsyncWebServerRequest* request,
                                std::unique_ptr<FastBee::PreflightResult>& workspace) {
    // The caller already owns both planner and configuration locks. Keep this
    // large scratch result off callback stacks and out of idle resident RAM.
    workspace.reset(new (std::nothrow) FastBee::PreflightResult);
    if (workspace) return true;
    ctx->sendError(request, 503, "PREFLIGHT_MEMORY_UNAVAILABLE: resource planner workspace unavailable");
    return false;
}

// Adapts HTTP form fields to the core-side parameter parsing contract while
// preserving the route layer's lookup semantics (POST body first, then GET).
class WebPeripheralFormFields final : public FastBee::PeripheralFormFields {
public:
    WebPeripheralFormFields(WebHandlerContext* ctx, AsyncWebServerRequest* request)
        : ctx(ctx), request(request) {}
    bool has(const char* name) const override {
        return request->hasParam(name, true) || request->hasParam(name);
    }
    String value(const char* name, const char* fallback) const override {
        return ctx->getParamValue(request, name, fallback);
    }
    int intValue(const char* name, int fallback) const override {
        return ctx->getParamInt(request, name, fallback);
    }
    bool boolValue(const char* name, bool fallback) const override {
        return ctx->getParamBool(request, name, fallback);
    }
private:
    WebHandlerContext* ctx;
    AsyncWebServerRequest* request;
};

// Legacy channel-integer reader for the peripheral write route. Parameter
// interpretation itself lives in the core form-field parser.
bool readIoRequestInteger(WebHandlerContext* ctx, AsyncWebServerRequest* request,
                          const char* name, uint32_t fallback, uint32_t minimum,
                          uint32_t maximum, uint32_t& value, String& error,
                          bool allowHex = false) {
    WebPeripheralFormFields form(ctx, request);
    return FastBee::readIoFormInteger(form, name, fallback, minimum, maximum,
                                      value, error, allowHex);
}

bool parsePreflightOperation(const String& value,
                             FastBee::PreflightOperation& operation) {
    if (FastBee::stringEqualsIgnoreCaseLiteral(value, "add")) {
        operation = FastBee::PreflightOperation::ADD;
        return true;
    }
    if (FastBee::stringEqualsIgnoreCaseLiteral(value, "update")) {
        operation = FastBee::PreflightOperation::UPDATE;
        return true;
    }
    if (FastBee::stringEqualsIgnoreCaseLiteral(value, "enable")) {
        operation = FastBee::PreflightOperation::ENABLE;
        return true;
    }
    return false;
}

FastBee::PreflightContext makePreflightContext(
        const FastBee::BoardProfile& profile, uint32_t acknowledgements,
        void* networkManager) {
    FastBee::PreflightContext context;
    context.wifiActive = WiFi.getMode() != WIFI_MODE_NULL;
    context.uart0ConsoleActive = true;
    context.usbSerialJtagActive =
        profile.id == FastBee::BoardProfileId::ESP32_C6_DEVKITC_1 ||
        profile.id == FastBee::BoardProfileId::ESP32_C3_GENERIC;
    context.jtagActive = false;
    context.acknowledgements = acknowledgements & PREFLIGHT_ACK_MASK;
    if (networkManager) {
        context.ethernet = FastBee::ethernetResourceConfig(
            static_cast<FBNetworkManager*>(networkManager)->getConfig());
        context.cellular = FastBee::cellularResourceConfig(
            static_cast<FBNetworkManager*>(networkManager)->getConfig());
    }
    return context;
}


void sendJsonWithStatus(AsyncWebServerRequest* request, int status,
                         JsonDocument& doc) {
    const size_t responseSize = measureJson(doc);
    if (HandlerUtils::checkResponseMemory(request, responseSize)) return;
    String payload;
    payload.reserve(responseSize + 1);
    serializeJson(doc, payload);
    HandlerUtils::sendWithClose(
        request, request->beginResponse(status, "application/json", payload));
}

bool sendBlockedPreflight(AsyncWebServerRequest* request,
                          const FastBee::BoardProfile& profile,
                          const FastBee::PreflightResult& result,
                          const PeripheralConfig& candidate,
                          FastBee::PreflightOperation operation) {
    if (result.allowed && !result.claimsTruncated && !result.issuesTruncated) {
        return false;
    }
    JsonDocument doc;
    doc["code"] = "PERIPHERAL_PREFLIGHT_BLOCKED";
    addPreflightResponse(doc, profile, result, candidate, operation, false,
                         false,
                         "Peripheral resource preflight blocked the change");
    sendJsonWithStatus(request, 409, doc);
    return true;
}
}

PeripheralRouteHandler::PeripheralRouteHandler(WebHandlerContext* ctx)
    : ctx(ctx) {
    preflightMutex = xSemaphoreCreateMutex();
}

PeripheralRouteHandler::~PeripheralRouteHandler() {
    if (preflightMutex) {
        vSemaphoreDelete(preflightMutex);
        preflightMutex = nullptr;
    }
}

void PeripheralRouteHandler::setupRoutes(AsyncWebServer* server) {
    // 注意：更具体的路由必须放在通用路由之前

    server->on("/api/boards/profiles", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
                  handleGetBoardProfiles(request);
              });

    server->on("/api/peripherals/resources", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
                  handleGetPeripheralResources(request);
              });

    server->on("/api/peripherals/preflight", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
                  handlePreflightPeripheral(request);
              });

    server->on("/api/peripherals/config/preflight", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleImportConfiguration(request, false); });
    server->on("/api/peripherals/config/import", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleImportConfiguration(request, true); });

    server->on("/api/peripherals/types", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetPeripheralTypes(request); });

    server->on("/api/peripherals/enable", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleEnablePeripheral(request); });

    server->on("/api/peripherals/disable", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleDisablePeripheral(request); });

    server->on("/api/peripherals/status", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetPeripheralStatus(request); });

    server->on("/api/peripherals/read", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleReadPeripheral(request); });

    server->on("/api/peripherals/validate-pins", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleValidatePins(request); });

    server->on("/api/peripherals/write", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleWritePeripheral(request); });

    server->on("/api/peripherals/", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetPeripheral(request); });

    server->on("/api/peripherals/", HTTP_PUT,
              [this](AsyncWebServerRequest* request) { handleUpdatePeripheral(request); });

    server->on("/api/peripherals/update", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleUpdatePeripheral(request); });

    server->on("/api/peripherals/", HTTP_DELETE,
              [this](AsyncWebServerRequest* request) { handleDeletePeripheral(request); });

    server->on("/api/peripherals", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetPeripherals(request); });

    server->on("/api/peripherals", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleAddPeripheral(request); });
}


void PeripheralRouteHandler::handleGetPeripheralResources(
        AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (HandlerUtils::checkLowMemory(request, 24576)) return;

    const String requestedProfile =
        ctx->getParamValue(request, "profileId", "");
    const FastBee::BoardProfile* profile =
        resolveBoardProfile(requestedProfile);
    if (!profile) {
        ctx->sendError(request, 422,
                       "UNSUPPORTED_BOARD_PROFILE: unknown or unavailable profile");
        return;
    }

    PeripheralManager& manager = PeripheralManager::getInstance();
    const FastBee::BoardProfile* active = compiledBoardProfileSupported()
        ? &FastBee::getCompiledBoardProfile() : nullptr;
    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    FastBee::setFixedJsonField(data, "profileId", profile->key);
    FastBee::setFixedJsonField(data, "profileName", profile->displayName);
    FastBee::setFixedJsonField(data, "profileVersion", profile->version);
    FastBee::setFixedJsonField(data, "activeProfileId", active ? active->key : "");
    FastBee::setFixedJsonField(data, "comparison", active == nullptr || !sameText(profile->key, active->key));
    FastBee::setFixedJsonField(data, "claimScope", "configuration");
    FastBee::setFixedJsonField(data, "networkClaims", "w5500,cellular");
    // The legacy resource rows remain configuration-only. Runtime leases are
    // separate and always refer to the active firmware, even for comparisons.
    FastBee::setFixedJsonField(data, "runtimeReservationsIncluded", false);
    FastBee::setFixedJsonField(data, "configuredUseMeaning", "enabled_configuration");
    FastBee::setFixedJsonField(data, "availableMeaning", "configuration_only_not_runtime_free");
    FastBee::setFixedJsonField(data, "configurationStatus", "busy");
    FastBee::setFixedJsonField(data, "resources", nullptr);
    FastBee::setFixedJsonField(data, "diagnostics", nullptr);
    {
        MutexGuard resultLock(preflightMutex, 0);
        if (resultLock.isLocked()) {
            RecursiveMutexGuard managerLock(manager.getMutex(), 0);
            if (managerLock.isLocked()) {
                std::unique_ptr<FastBee::PreflightResult> workspace;
                if (!allocatePreflightWorkspace(ctx, request, workspace)) return;
                FastBee::PreflightResult& preflightResult = *workspace;
                const std::vector<PeripheralConfig> existing = manager.getAllPeripherals();
                FastBee::PeripheralResourcePlanner planner(
                    *profile, makePreflightContext(*profile, FastBee::ACK_NONE, ctx->networkManager),
                    preflightResult, existing.data(), existing.size());
                for (const PeripheralConfig& config : existing) {
                    planner.addPeripheral(config, config.enabled ? FastBee::ClaimState::ACTIVE
                                                                 : FastBee::ClaimState::DORMANT);
                }
                FastBee::setFixedJsonField(data, "configurationStatus", "observed");
                FastBee::setFixedJsonField(data, "claimsTruncated", preflightResult.claimsTruncated);
                FastBee::setFixedJsonField(data, "issuesTruncated", preflightResult.issuesTruncated);
                addResourceRows(data["resources"].to<JsonArray>(), *profile, preflightResult);
                addPreflightIssues(data["diagnostics"].to<JsonArray>(), preflightResult);
            }
        }
    }
    JsonObject runtime = data["runtime"].to<JsonObject>();
    FastBee::writeRuntimeResourceHeader(runtime, active ? active->key : "");
    // Each zero-wait registry observation ends before the next begins. No
    // planner/manager lock is held while observing another resource manager.
    {
        FastBee::I2CResourceSnapshot snapshot;
        FastBee::I2CBusManager::instance().snapshotResources(snapshot);
        FastBee::writeI2CResourceJson(runtime["i2c"].to<JsonObject>(), snapshot);
    }
    {
        FastBee::SpiResourceSnapshot snapshot;
        FastBee::SpiBusInitializer::instance().snapshotResources(snapshot);
        FastBee::writeSpiResourceJson(runtime["spi"].to<JsonObject>(), snapshot);
    }
    {
        FastBee::UartResourceSnapshot snapshot;
        FastBee::UartPortLease::snapshotResources(snapshot, CHIP_UART_COUNT);
        FastBee::writeUartResourceJson(runtime["uart"].to<JsonObject>(), snapshot);
    }
    {
        FastBee::ControlOutputResourceSnapshot snapshot;
        manager.snapshotControlOutputResources(snapshot);
        FastBee::writeControlOutputResourceJson(runtime["controlOutput"].to<JsonObject>(), snapshot);
    }
    {
        FastBee::EncoderResourceSnapshot snapshot;
        manager.snapshotEncoderResources(snapshot);
        FastBee::writeEncoderResourceJson(runtime["encoder"].to<JsonObject>(), snapshot);
    }
    {
        FastBee::LedcResourceSnapshot snapshot;
        manager.snapshotLedcResources(snapshot);
        FastBee::writeLedcResourceJson(runtime["ledc"].to<JsonObject>(), snapshot);
    }
    {
        FastBee::BacklightResourceSnapshot snapshot;
#if FASTBEE_ENABLE_SCREEN_UI
        FastBee::Screen::ScreenRuntime::snapshotBacklightResources(snapshot);
#endif
        FastBee::writeBacklightResourceJson(runtime["backlightLedc"].to<JsonObject>(), snapshot, millis());
    }
    {
        FastBee::DhtRmtResourceSnapshot snapshot;
#if FASTBEE_ENABLE_SENSOR_DRIVER
        SensorDriver::snapshotDhtRmtResources(snapshot);
#endif
        FastBee::writeDhtRmtResourceJson(runtime["dhtRmt"].to<JsonObject>(), snapshot);
    }
    FastBee::writeManagedPinResources(runtime, manager);
    HandlerUtils::sendJsonStream(request, doc);
}

void PeripheralRouteHandler::handleImportConfiguration(AsyncWebServerRequest* request, bool commit) {
    if (!ctx->requireAuth(request) || !ctx->requireDeveloperMode(request)) return;
    if (!compiledBoardProfileSupported()) {
        ctx->sendError(request, 501, "CONFIG_IMPORT_BOARD_UNSUPPORTED"); return;
    }
    if (!request->hasParam("content", true)) {
        ctx->sendBadRequest(request, "CONFIG_IMPORT_CONTENT_REQUIRED"); return;
    }
    const String& content = request->getParam("content", true)->value();
    if (content.isEmpty() || content.length() > 24 * 1024 || memchr(content.c_str(), '\0', content.length())) {
        ctx->sendBadRequest(request, "CONFIG_IMPORT_SIZE_OR_CONTENT"); return;
    }
    if (HandlerUtils::checkLowMemory(request, content.length() * 2 + 24576)) return;
    uint32_t acknowledgements = 0;
    if (request->hasParam("acknowledgements", true)) {
        const String& value = request->getParam("acknowledgements", true)->value();
        if (value.isEmpty() || value.length() > 2) {
            ctx->sendBadRequest(request, "CONFIG_IMPORT_ACK_INVALID"); return;
        }
        for (size_t i = 0; i < value.length(); ++i) {
            if (value[i] < '0' || value[i] > '9') {
                ctx->sendBadRequest(request, "CONFIG_IMPORT_ACK_INVALID"); return;
            }
            acknowledgements = acknowledgements * 10 + static_cast<unsigned>(value[i] - '0');
        }
        if (acknowledgements & ~PREFLIGHT_ACK_MASK) {
            ctx->sendBadRequest(request, "CONFIG_IMPORT_ACK_INVALID"); return;
        }
    }
    MutexGuard resultLock(preflightMutex, pdMS_TO_TICKS(50));
    auto& manager = PeripheralManager::getInstance();
    RecursiveMutexGuard managerLock(manager.getMutex(), pdMS_TO_TICKS(50));
    if (!resultLock.isLocked() || !managerLock.isLocked() || manager.configurationImportPending()) {
        ctx->sendError(request, 409, "CONFIG_IMPORT_BUSY_OR_PENDING_RESTART"); return;
    }
    const uint64_t version = manager.getConfigurationReviewVersion();
    char versionText[17];
    snprintf(versionText, sizeof(versionText), "%016llx", static_cast<unsigned long long>(version));
    if (!version || (commit && (!request->hasParam("expectedVersion", true) ||
        request->getParam("expectedVersion", true)->value() != versionText))) {
        ctx->sendError(request, 409, "CONFIG_IMPORT_REVISION_CONFLICT"); return;
    }
    FastBeeJsonDocLarge document;
    if (deserializeJson(document, content.c_str(), content.length())) {
        ctx->sendBadRequest(request, "CONFIG_IMPORT_JSON_INVALID"); return;
    }
    std::vector<PeripheralConfig> candidates;
    String error;
    const auto& profile = FastBee::getCompiledBoardProfile();
    std::unique_ptr<FastBee::PreflightResult> workspace;
    if (!allocatePreflightWorkspace(ctx, request, workspace)) return;
    FastBee::PreflightResult& preflightResult = *workspace;
    const bool valid = manager.prepareConfigurationImport(document, candidates,
        makePreflightContext(profile, commit ? acknowledgements : 0, ctx->networkManager), preflightResult, error);
    if (!valid) {
        ctx->sendError(request, 422, error); return;
    }
    if (commit && (!preflightResult.allowed || preflightResult.requiresConfirmation)) {
        ctx->sendError(request, 409, "CONFIG_IMPORT_RESOURCE_CONFIRMATION_REQUIRED"); return;
    }
    if (commit && !manager.commitConfigurationImport(document, version, error)) {
        ctx->sendError(request, 503, error); return;
    }
    JsonDocument response;
    response["name"] = "peripherals.json";
    response["size"] = content.length();
    response["peripheralCount"] = document["peripherals"].size();
    response["version"] = versionText;
    response["profileId"] = profile.key;
    response["allowed"] = true;
    response["requiresConfirmation"] = preflightResult.requiresConfirmation;
    response["requiredAcknowledgements"] = requiredAcknowledgements(preflightResult);
    response["saved"] = commit;
    response["applied"] = false;
    response["restartRequired"] = commit;
    addPreflightIssues(response["issues"].to<JsonArray>(), preflightResult);
    ctx->sendSuccess(request, response);
}

void PeripheralRouteHandler::handlePreflightPeripheral(
        AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;
    if (!compiledBoardProfileSupported()) {
        ctx->sendError(request, 501,
                       "UNSUPPORTED_BOARD_PROFILE: no mutation profile for this build");
        return;
    }

    FastBee::PreflightOperation operation;
    const String operationText =
        ctx->getParamValue(request, "operation", "add");
    if (!parsePreflightOperation(operationText, operation)) {
        ctx->sendError(request, 400,
                       "INVALID_PREFLIGHT_OPERATION: expected add, update or enable");
        return;
    }

    MutexGuard resultLock(preflightMutex, pdMS_TO_TICKS(500));
    if (!resultLock.isLocked()) {
        ctx->sendError(request, 503, "PREFLIGHT_BUSY: resource planner is busy");
        return;
    }
    PeripheralManager& manager = PeripheralManager::getInstance();
    RecursiveMutexGuard managerLock(manager.getMutex(), pdMS_TO_TICKS(500));
    if (!managerLock.isLocked()) {
        ctx->sendError(request, 503, "PREFLIGHT_BUSY: peripheral state is busy");
        return;
    }

    WebPeripheralFormFields form(ctx, request);
    PeripheralConfig candidate;
    String excludeId;
    String errorMsg;
    int errorStatus = 400;
    if (!FastBee::buildPreflightCandidate(form, manager, operation, candidate,
                                          excludeId, errorMsg, errorStatus)) {
        ctx->sendError(request, errorStatus, errorMsg);
        return;
    }

    int requestedAcks = ctx->getParamInt(request, "acknowledgements", 0);
    if (requestedAcks < 0) requestedAcks = 0;
    const uint32_t acknowledgements =
        static_cast<uint32_t>(requestedAcks) & PREFLIGHT_ACK_MASK;
    const FastBee::BoardProfile& profile = FastBee::getCompiledBoardProfile();
    std::unique_ptr<FastBee::PreflightResult> workspace;
    if (!allocatePreflightWorkspace(ctx, request, workspace)) return;
    FastBee::PreflightResult& preflightResult = *workspace;
    const std::vector<PeripheralConfig> existing = manager.getAllPeripherals();
    FastBee::preflightPeripheralChange(
        profile, existing.data(), existing.size(), candidate, operation,
        excludeId, makePreflightContext(profile, acknowledgements, ctx->networkManager),
        preflightResult);

    JsonDocument doc;
    const bool allowed = !hasHardPreflightError(preflightResult);
    addPreflightResponse(doc, profile, preflightResult, candidate, operation,
                         true, allowed);
    HandlerUtils::sendJsonStream(request, doc);
}

void PeripheralRouteHandler::handleGetPeripherals(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    PeripheralManager& pm = PeripheralManager::getInstance();

    // 如果传了 id 参数，只返回单条外设（避免编辑时加载全部导致内存不足）
    if (request->hasParam("id")) {
        String periphId = request->getParam("id")->value();
        PeripheralConfig config;
        if (!pm.getPeripheralSnapshot(periphId, config)) {
            request->send(404, "application/json", "{\"success\":false,\"message\":\"Peripheral not found\"}");
            return;
        }

        JsonDocument doc;
        addPeripheralListIdentity(doc, config);
        addPeripheralDiagnosticCapability(doc.as<JsonObject>(), config);
        addPassiveBuzzerFlag(doc, config);

        addPeripheralListParams(doc, config);
        addGenericPeripheralFields(doc.as<JsonObject>(), config);

        JsonArray pins = doc["pins"].to<JsonArray>();
        for (int i = 0; i < config.pinCount && i < 8; i++) {
            if (config.pins[i] != 255) {
                pins.add(config.pins[i]);
            }
        }

        PeripheralRuntimeState runtimeState;
        if (pm.getRuntimeStateSnapshot(periphId, runtimeState)) {
            doc["status"] = static_cast<int>(runtimeState.status);
            doc["memorySuspended"] = runtimeState.memorySuspended;
            if (config.type == PeripheralType::UART) {
                addUartRuntime(doc.as<JsonObject>(), runtimeState, config.enabled);
            }
            if (config.type == PeripheralType::ENCODER) {
                EncoderReading reading;
                if (pm.getEncoderReading(periphId, reading)) {
                    addEncoderReading(doc.as<JsonObject>(), reading);
                }
            }
        } else {
            doc["status"] = 0;
            doc["memorySuspended"] = false;
        }

        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", "{\"success\":true,\"data\":" + response + "}");
        return;
    }

    if (HandlerUtils::rejectHeavyRequestOnPressure(request, "Peripheral list", MemoryGuardLevel::CRITICAL, 5)) {
        return;
    }

    String typeFilter = ctx->getParamValue(request, "type", "");
    String categoryFilter = ctx->getParamValue(request, "category", "");
    bool enabledOnly = ctx->getParamValue(request, "enabledOnly", "0") == "1";
    bool requestedCompact = ctx->getParamValue(request, "compact", "0") == "1";
    bool degraded = shouldForceCompactList();
    bool compact = requestedCompact || degraded;

    // 使用指针向量代替深拷贝，大幅减少内存占用（38个外设：指针~152B vs 拷贝~6KB）
    std::vector<const PeripheralConfig*> ptrs;
    ptrs.reserve(40);  // 预估外设数量

    if (!typeFilter.isEmpty()) {
        PeripheralType type = parsePeripheralType(typeFilter.c_str());
        pm.forEachPeripheral([&ptrs, type, enabledOnly](const PeripheralConfig& p) {
            if (p.type == type &&
                p.type != PeripheralType::MODBUS_DEVICE &&
                (!enabledOnly || p.enabled)) {
                ptrs.push_back(&p);
            }
        });
    } else if (!categoryFilter.isEmpty()) {
        PeripheralCategory category = PeripheralCategory::CATEGORY_GPIO;
        if (categoryFilter == "communication") category = PeripheralCategory::CATEGORY_COMMUNICATION;
        else if (categoryFilter == "gpio") category = PeripheralCategory::CATEGORY_GPIO;
        else if (categoryFilter == "analog") category = PeripheralCategory::CATEGORY_ANALOG_SIGNAL;
        else if (categoryFilter == "debug") category = PeripheralCategory::CATEGORY_DEBUG;
        else if (categoryFilter == "special") category = PeripheralCategory::CATEGORY_SPECIAL;
        pm.forEachPeripheral([&ptrs, category, enabledOnly](const PeripheralConfig& p) {
            if (getPeripheralCategory(p.type) == category &&
                p.type != PeripheralType::MODBUS_DEVICE &&
                (!enabledOnly || p.enabled)) {
                ptrs.push_back(&p);
            }
        });
    } else {
        pm.forEachPeripheral([&ptrs, enabledOnly](const PeripheralConfig& p) {
            if (p.type != PeripheralType::MODBUS_DEVICE &&
                (!enabledOnly || p.enabled)) {
                ptrs.push_back(&p);
            }
        });
    }

    // 排序：启用的排前面，然后按名称排序
    std::sort(ptrs.begin(), ptrs.end(), [](const PeripheralConfig* a, const PeripheralConfig* b) {
        if (a->enabled != b->enabled) return a->enabled > b->enabled;
        return strcmp(a->name.c_str(), b->name.c_str()) < 0;
    });

    int total = ptrs.size();

    // 解析分页参数
    int page = 1;
    int pageSize = 10;
    if (request->hasParam("page")) {
        page = request->getParam("page")->value().toInt();
        if (page < 1) page = 1;
    }
    if (request->hasParam("pageSize")) {
        pageSize = request->getParam("pageSize")->value().toInt();
        if (pageSize < 1) pageSize = 1;
        if (pageSize > (compact ? 50 : 20)) pageSize = compact ? 50 : 20;
    }

    // 分页步长始终与前端请求保持一致，避免数据间隙（items被跳过无法显示）。
    // 内存不足时由 compact 模式减少每项序列化字段来节省空间，而非减少条数。
    // 如果连单条序列化都无法完成，由下方 isListMemoryCriticallyLow() 返回 503。
    int effectivePageSize = pageSize;

    // Deletion/filter changes can remove the requested page. Clamp before
    // multiplying to also avoid overflow from an excessively large page.
    page = std::min(page, std::max(1, (total + effectivePageSize - 1) / effectivePageSize));

    // 计算分页范围（统一使用 effectivePageSize）
    int startIdx = (page - 1) * effectivePageSize;
    int endIdx = (startIdx < total) ? std::min(startIdx + effectivePageSize, total) : startIdx;

    // Chunked responses can operate with small contiguous blocks. Only reject
    // when even per-item scratch strings are likely to fail.
    if (isListMemoryCriticallyLow()) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Low memory: heap=%lu maxAlloc=%lu",
                 (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMaxAllocHeap());
        HandlerUtils::sendJsonError(request, 503, msg, 5);
        return;
    }
    
    // 两遍法：先序列化每个项计算精确大小，再用精确 reserve 构建响应
    int itemCount = endIdx - startIdx;
    
    // 第一遍：序列化所有项到临时 String，累计精确大小
    std::vector<String> serializedItems;
    serializedItems.reserve(itemCount);
    size_t totalItemsSize = 0;
    
    for (int i = startIdx; i < endIdx; i++) {
        const auto& config = *ptrs[i];
        JsonDocument itemDoc;
        addPeripheralListIdentity(itemDoc, config);
        addPeripheralDiagnosticCapability(itemDoc.as<JsonObject>(), config);
        addPassiveBuzzerFlag(itemDoc, config);

        if (!compact) {
            JsonArray pins = itemDoc["pins"].to<JsonArray>();
            for (int j = 0; j < config.pinCount && j < 8; j++) {
                if (config.pins[j] != 255) {
                    pins.add(config.pins[j]);
                }
            }

            addPeripheralListParams(itemDoc, config);

            PeripheralRuntimeState runtimeState;
            if (pm.getRuntimeStateSnapshot(config.id, runtimeState)) {
                itemDoc["status"] = static_cast<int>(runtimeState.status);
                itemDoc["memorySuspended"] = runtimeState.memorySuspended;
                if (config.type == PeripheralType::UART) {
                    addUartRuntime(itemDoc.as<JsonObject>(), runtimeState, config.enabled);
                }
                if (config.type == PeripheralType::ENCODER) {
                    JsonObject encoder = itemDoc["reading"].to<JsonObject>();
                    FastBee::setFixedJsonField(encoder, "count", runtimeState.state.encoder.count);
                    FastBee::setFixedJsonField(encoder, "delta", runtimeState.state.encoder.delta);
                    FastBee::setFixedJsonField(encoder, "direction", runtimeState.state.encoder.direction);
                    FastBee::setFixedJsonField(encoder, "frequencyHz", runtimeState.state.encoder.frequencyHz);
                    FastBee::setFixedJsonField(encoder, "rpm", runtimeState.state.encoder.rpm);
                }
            } else {
                itemDoc["status"] = 0;
                itemDoc["memorySuspended"] = false;
            }
        }

        addGenericPeripheralFields(itemDoc.as<JsonObject>(), config);
        String itemStr;
        serializeJson(itemDoc, itemStr);
        totalItemsSize += itemStr.length();
        serializedItems.push_back(std::move(itemStr));
    }
    
     // ============ Chunked 流式响应（复用 HandlerUtils::sendJsonListChunked）============
    // 旧方案：先构建一个 totalJsonSize 大小的连续 String（~10-20KB），对碎片化堆极不友好。
    // 新方案：items 已在 vector<String> 中，按 index 按需复制到 TCP 发送缓冲（~256B），
    //         堆峰 = items 合计（~6KB），不再产生额外大 String。
    size_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < 4096) {
        LOG_ERRORF("[Periph] Low heap for chunked response: free=%u", (unsigned)freeHeap);
        HandlerUtils::sendJsonError(request, 503, "Low memory for response");
        return;
    }

    // 构建 header：{"success":true,"total":N,"page":P,"pageSize":S,"data":[
    const int profileMax = static_cast<int>(FastBee::ResourceProfile::MAX_PERIPHERALS);
    const int profileUsed = static_cast<int>(pm.getPeripheralCount());
    const int profileRemaining = std::max(0, profileMax - profileUsed);
    const bool runtimeDegraded = HandlerUtils::memoryDegradationActive();

    String header;
    header.reserve(420);
    header = F("{\"success\":true,\"total\":");
    header += String(total);
    header += F(",\"page\":");
    header += String(page);
    header += F(",\"pageSize\":");
    header += String(effectivePageSize);
    if (degraded || runtimeDegraded) {
        header += F(",\"degraded\":true");
    }
    HandlerUtils::appendMemoryDegradationStatus(header);
    header += F(",\"profile\":{\"name\":\"");
    header += FastBee::ResourceProfile::NAME;
    header += F("\",\"max\":");
    header += String(profileMax);
    header += F(",\"used\":");
    header += String(profileUsed);
    header += F(",\"remaining\":");
    header += String(profileRemaining);
    header += F("}");
    header += F(",\"data\":[");

    if (!HandlerUtils::sendJsonListChunked(request, header, std::move(serializedItems))) {
        LOG_ERRORF("[Periph] beginChunkedResponse failed, heap=%u", (unsigned)ESP.getFreeHeap());
        HandlerUtils::sendJsonError(request, 503, "Failed to create streaming response");
    }
}

void PeripheralRouteHandler::handleGetPeripheral(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    String id = ctx->getParamValue(request, "id", "");
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Missing id parameter");
        return;
    }

    PeripheralManager& pm = PeripheralManager::getInstance();
    PeripheralConfig configSnapshot;
    const PeripheralConfig* config = pm.getPeripheralSnapshot(id, configSnapshot) ? &configSnapshot : nullptr;

    if (!config) {
        ctx->sendError(request, 404, "Peripheral not found");
        return;
    }

    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();

    FastBee::setFixedJsonField(data, "id", config->id);
    FastBee::setFixedJsonField(data, "name", config->name);
    FastBee::setFixedJsonField(data, "type", static_cast<int>(config->type));
    FastBee::setFixedJsonField(data, "typeName", getPeripheralTypeName(config->type));
    FastBee::setFixedJsonField(data, "enabled", config->enabled);
    addPeripheralDiagnosticCapability(data, *config);
    addPassiveBuzzerFlag(data, *config);

    JsonArray pins = data["pins"].to<JsonArray>();
    for (int i = 0; i < config->pinCount && i < 8; i++) {
        if (config->pins[i] != 255) {
            pins.add(config->pins[i]);
        }
    }

    // 类型特定参数
    JsonObject params = data["params"].to<JsonObject>();
    if (config->type == PeripheralType::UART) {
        addUartParams(params, *config);
    } else if (config->type == PeripheralType::ENCODER) {
        addEncoderParams(params, *config);
    } else if (config->type == PeripheralType::I2C) {
        FastBee::setFixedJsonField(params, "frequency", config->params.i2c.frequency);
        FastBee::setFixedJsonField(params, "address", config->params.i2c.address);
        FastBee::setFixedJsonField(params, "isMaster", config->params.i2c.isMaster);
    } else if (config->type == PeripheralType::SPI) {
        FastBee::setFixedJsonField(params, "frequency", config->params.spi.frequency);
        FastBee::setFixedJsonField(params, "mode", config->params.spi.mode);
        FastBee::setFixedJsonField(params, "msbFirst", config->params.spi.msbFirst);
    } else if (config->type == PeripheralType::PWM_SERVO) {
        addServoParams(params, *config);
    } else if (config->isGPIOPeripheral() && !isAnalogInputConfigType(config->type)) {
        FastBee::setFixedJsonField(params, "initialState", static_cast<int>(config->params.gpio.initialState));
        FastBee::setFixedJsonField(params, "pwmChannel", config->params.gpio.pwmChannel);
        FastBee::setFixedJsonField(params, "pwmFrequency", config->params.gpio.pwmFrequency);
        FastBee::setFixedJsonField(params, "pwmResolution", config->params.gpio.pwmResolution);
        FastBee::setFixedJsonField(params, "defaultDuty", config->params.gpio.defaultDuty);
        if (isDigitalInputPeripheral(config->type)) {
            addDigitalInputParams(params, *config);
        }
    } else if (isAnalogInputConfigType(config->type)) {
        addAnalogInputParams(params, *config);
    } else if (config->type == PeripheralType::DAC) {
        FastBee::setFixedJsonField(params, "channel", config->params.dac.channel);
        FastBee::setFixedJsonField(params, "defaultValue", config->params.dac.defaultValue);
    } else if (config->type == PeripheralType::STEPPER_MOTOR) {
        FastBee::setFixedJsonField(params, "stepsPerRevolution", config->params.stepper.stepsPerRevolution);
        FastBee::setFixedJsonField(params, "speed", config->params.stepper.speed);
    } else if (config->type == PeripheralType::LCD) {
        FastBee::setFixedJsonField(params, "width", config->params.lcd.width);
        FastBee::setFixedJsonField(params, "height", config->params.lcd.height);
        FastBee::setFixedJsonField(params, "interface", config->params.lcd.interface);
    } else if (config->type == PeripheralType::NEO_PIXEL) {
        FastBee::setFixedJsonField(params, "count", config->params.neopixel.count);
        FastBee::setFixedJsonField(params, "brightness", config->params.neopixel.brightness);
    } else if (config->type == PeripheralType::RF_MODULE) {
        FastBee::setFixedJsonField(params, "mode", config->params.rf.mode);
        FastBee::setFixedJsonField(params, "pulseWidth", config->params.rf.pulseWidth);
        FastBee::setFixedJsonField(params, "repeat", config->params.rf.repeat);
        FastBee::setFixedJsonField(params, "bitLength", config->params.rf.bitLength);
        FastBee::setFixedJsonField(params, "activeHigh", config->params.rf.activeHigh);
    } else if (config->type == PeripheralType::RADAR_SENSOR) {
        FastBee::setFixedJsonField(params, "mode", config->params.radar.mode);
        FastBee::setFixedJsonField(params, "activeHigh", config->params.radar.activeHigh);
        FastBee::setFixedJsonField(params, "debounceMs", config->params.radar.debounceMs);
        FastBee::setFixedJsonField(params, "holdMs", config->params.radar.holdMs);
    } else if (config->type == PeripheralType::SEVEN_SEGMENT_TM1637) {
        FastBee::setFixedJsonField(params, "brightness", config->params.segment.brightness);
    } else if (config->type == PeripheralType::SENSOR_GENERIC ||
               config->type == PeripheralType::SDIO ||
               config->type == PeripheralType::DS1302_RTC ||
               config->type == PeripheralType::LCD1602 ||
               config->type == PeripheralType::RFID_MFRC522) {
        addP1PeripheralParams(params, *config);
    }
    addGenericPeripheralFields(data, *config);

    PeripheralRuntimeState runtimeSnapshot;
    const PeripheralRuntimeState* runtimeState =
        pm.getRuntimeStateSnapshot(id, runtimeSnapshot) ? &runtimeSnapshot : nullptr;
    if (runtimeState) {
        JsonObject status = data["status"].to<JsonObject>();
        FastBee::setFixedJsonField(status, "state", static_cast<int>(runtimeState->status));
        FastBee::setFixedJsonField(status, "initTime", runtimeState->initTime);
        FastBee::setFixedJsonField(status, "lastActivity", runtimeState->lastActivity);
        FastBee::setFixedJsonField(status, "errorCount", runtimeState->errorCount);
        FastBee::setFixedJsonField(status, "memorySuspended", runtimeState->memorySuspended);
        if (config->type == PeripheralType::UART) {
            FastBee::setFixedJsonField(status, "framing", uartFramingName(*config));
            addUartRuntime(status, *runtimeState, config->enabled);
        } else if (config->type == PeripheralType::ENCODER) {
            EncoderReading reading;
            if (pm.getEncoderReading(id, reading)) addEncoderReading(status, reading);
        }
    }

    HandlerUtils::sendJsonStream(request, doc);
}

void PeripheralRouteHandler::handleAddPeripheral(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

    WebPeripheralFormFields form(ctx, request);
    PeripheralConfig config;
    config.id = ctx->getParamValue(request, "id", "");
    config.name = ctx->getParamValue(request, "name", "");
    config.type = static_cast<PeripheralType>(ctx->getParamInt(request, "type", 0));
    config.enabled = ctx->getParamBool(request, "enabled", false);

    const char* typeError = userConfigurablePeripheralTypeError(config.type);
    if (typeError) {
        ctx->sendError(request, 422, typeError);
        return;
    }

    String pinsStr = ctx->getParamValue(request, "pins", "");
    String errorMsg;
    if (!parsePeripheralPins(pinsStr, config.pins, config.pinCount, errorMsg)) {
        ctx->sendError(request, 400, errorMsg);
        return;
    }

    if (!FastBee::applyAddTypeParams(form, config, errorMsg)) {
        ctx->sendError(request, 400, errorMsg);
        return;
    }

    // Keep the mutation and standalone preflight endpoints on one final
    // parameter interpretation path while preserving legacy validation above.
    if (!FastBee::applyPeripheralRequestParameters(form, config, false,
                                                   errorMsg)) {
        ctx->sendError(request, 400, errorMsg);
        return;
    }

    if (config.id.isEmpty()) {
        config.id = "periph_" + String(millis());
    }

    PeripheralManager& pm = PeripheralManager::getInstance();
    MutexGuard resultLock(preflightMutex, pdMS_TO_TICKS(500));
    if (!resultLock.isLocked()) {
        ctx->sendError(request, 503, "PREFLIGHT_BUSY: resource planner is busy");
        return;
    }
    RecursiveMutexGuard managerLock(pm.getMutex(), pdMS_TO_TICKS(500));
    if (!managerLock.isLocked()) {
        ctx->sendError(request, 503, "PREFLIGHT_BUSY: peripheral state is busy");
        return;
    }
    int requestedAcks = ctx->getParamInt(request, "acknowledgements", 0);
    if (requestedAcks < 0) requestedAcks = 0;
    if (compiledBoardProfileSupported()) {
        const FastBee::BoardProfile& profile = FastBee::getCompiledBoardProfile();
        std::unique_ptr<FastBee::PreflightResult> workspace;
        if (!allocatePreflightWorkspace(ctx, request, workspace)) return;
        FastBee::PreflightResult& preflightResult = *workspace;
        const std::vector<PeripheralConfig> existing = pm.getAllPeripherals();
        FastBee::preflightPeripheralChange(
            profile, existing.data(), existing.size(), config,
            FastBee::PreflightOperation::ADD, "",
            makePreflightContext(
                profile,
                static_cast<uint32_t>(requestedAcks) & PREFLIGHT_ACK_MASK, ctx->networkManager),
            preflightResult);
        if (sendBlockedPreflight(request, profile, preflightResult, config,
                                 FastBee::PreflightOperation::ADD)) {
            return;
        }
    }
    if (pm.addPeripheralAndSave(config, errorMsg)) {
        ctx->sendSuccess(request, "Peripheral added");
    } else {
        String msg = errorMsg.isEmpty() ? String("Failed to add peripheral")
                                        : (String("Failed to add peripheral: ") + errorMsg);
        ctx->sendError(request, mutationErrorStatus(errorMsg), msg);
    }
}

void PeripheralRouteHandler::handleUpdatePeripheral(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

    String id = ctx->getParamValue(request, "id", "");
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Missing id parameter");
        return;
    }

    PeripheralManager& pm = PeripheralManager::getInstance();
    MutexGuard resultLock(preflightMutex, pdMS_TO_TICKS(500));
    if (!resultLock.isLocked()) {
        ctx->sendError(request, 503, "PREFLIGHT_BUSY: resource planner is busy");
        return;
    }
    RecursiveMutexGuard managerLock(pm.getMutex(), pdMS_TO_TICKS(500));
    if (!managerLock.isLocked()) {
        ctx->sendError(request, 503, "PREFLIGHT_BUSY: peripheral state is busy");
        return;
    }
    PeripheralConfig config;
    if (!pm.getPeripheralSnapshot(id, config)) {
        ctx->sendError(request, 404, "Peripheral not found");
        return;
    }
    if (config.type == PeripheralType::MODBUS_DEVICE) {
        ctx->sendError(request, 422,
                       userConfigurablePeripheralTypeError(config.type));
        return;
    }

    WebPeripheralFormFields form(ctx, request);
    String name = ctx->getParamValue(request, "name", "");
    if (!name.isEmpty()) config.name = name;

    // 更新外设类型（类型变更时重置参数为默认值）
    if (request->hasParam("type", true)) {
        PeripheralType newType = static_cast<PeripheralType>(ctx->getParamInt(request, "type", static_cast<int>(config.type)));
        if (newType != config.type) {
            config.type = newType;
            memset(&config.params, 0, sizeof(config.params));
            if (isAnalogInputConfigType(config.type)) {
                FastBee::PeripheralManagerConfigDetail::initializeAdcDefaults(config);
            } else if (config.type == PeripheralType::UART) {
                initializeUartDefaults(config);
            } else if (config.type == PeripheralType::ENCODER) {
                initializeEncoderDefaults(config);
            } else if (config.type == PeripheralType::PWM_SERVO) {
                initializeServoDefaults(config);
            } else if (config.type == PeripheralType::SDIO) {
                initializeSdCardDefaults(config);
            } else if (config.type == PeripheralType::DS1302_RTC) {
                initializeDs1302Defaults(config);
            } else if (config.type == PeripheralType::LCD1602) {
                initializeLcd1602Defaults(config);
            } else if (config.type == PeripheralType::RFID_MFRC522) {
                initializeRfidDefaults(config);
            } else if (config.type == PeripheralType::SENSOR_GENERIC) {
                initializeSensorDefaults(config);
            }
        }
    }

    const char* typeError = userConfigurablePeripheralTypeError(config.type);
    if (typeError) {
        ctx->sendError(request, 422, typeError);
        return;
    }

    config.enabled = ctx->getParamBool(request, "enabled", config.enabled);

    String pinsStr = ctx->getParamValue(request, "pins", "");
    String errorMsg;
    if (request->hasParam("pins", true)) {
        if (!parsePeripheralPins(pinsStr, config.pins, config.pinCount, errorMsg)) {
            ctx->sendError(request, 400, errorMsg);
            return;
        }
    }

    if (!FastBee::applyUpdateTypeParams(form, config, errorMsg)) {
        ctx->sendError(request, 400, errorMsg);
        return;
    }

    if (!FastBee::applyPeripheralRequestParameters(form, config, false,
                                                   errorMsg)) {
        ctx->sendError(request, 400, errorMsg);
        return;
    }

    int requestedAcks = ctx->getParamInt(request, "acknowledgements", 0);
    if (requestedAcks < 0) requestedAcks = 0;
    if (compiledBoardProfileSupported()) {
        const FastBee::BoardProfile& profile = FastBee::getCompiledBoardProfile();
        std::unique_ptr<FastBee::PreflightResult> workspace;
        if (!allocatePreflightWorkspace(ctx, request, workspace)) return;
        FastBee::PreflightResult& preflightResult = *workspace;
        const std::vector<PeripheralConfig> existing = pm.getAllPeripherals();
        FastBee::preflightPeripheralChange(
            profile, existing.data(), existing.size(), config,
            FastBee::PreflightOperation::UPDATE, id,
            makePreflightContext(
                profile,
                static_cast<uint32_t>(requestedAcks) & PREFLIGHT_ACK_MASK, ctx->networkManager),
            preflightResult);
        if (sendBlockedPreflight(request, profile, preflightResult, config,
                                 FastBee::PreflightOperation::UPDATE)) {
            return;
        }
    }

    if (pm.updatePeripheralAndSave(id, config, errorMsg)) {
        ctx->sendSuccess(request, "Peripheral updated");
    } else {
        ctx->sendError(request, mutationErrorStatus(errorMsg),
                       errorMsg.isEmpty() ? "Failed to update peripheral" : errorMsg);
    }
}

void PeripheralRouteHandler::handleDeletePeripheral(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

    String id = ctx->getParamValue(request, "id", "");
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Missing id parameter");
        return;
    }

    PeripheralManager& pm = PeripheralManager::getInstance();
    PeripheralConfig config;
    if (!pm.getPeripheralSnapshot(id, config)) {
        ctx->sendError(request, 404, "Peripheral not found");
        return;
    }
    if (config.type == PeripheralType::MODBUS_DEVICE) {
        ctx->sendError(request, 422,
                       userConfigurablePeripheralTypeError(config.type));
        return;
    }
    String errorMsg;
    if (pm.removePeripheralAndSave(id, errorMsg)) {
        ctx->sendSuccess(request, "Peripheral deleted");
    } else {
        const int status = errorMsg.startsWith("外设 '")
            ? 404 : mutationErrorStatus(errorMsg);
        ctx->sendError(request, status,
                       errorMsg.isEmpty() ? "Peripheral not found" : errorMsg);
    }
}

void PeripheralRouteHandler::handleEnablePeripheral(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

    String id = ctx->getParamValue(request, "id", "");
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Missing id parameter");
        return;
    }

    PeripheralManager& pm = PeripheralManager::getInstance();
    MutexGuard resultLock(preflightMutex, pdMS_TO_TICKS(500));
    if (!resultLock.isLocked()) {
        ctx->sendError(request, 503, "PREFLIGHT_BUSY: resource planner is busy");
        return;
    }
    RecursiveMutexGuard managerLock(pm.getMutex(), pdMS_TO_TICKS(500));
    if (!managerLock.isLocked()) {
        ctx->sendError(request, 503, "PREFLIGHT_BUSY: peripheral state is busy");
        return;
    }
    PeripheralConfig config;
    if (!pm.getPeripheralSnapshot(id, config)) {
        ctx->sendError(request, 404, "Peripheral not found");
        return;
    }
    const char* typeError = userConfigurablePeripheralTypeError(config.type);
    if (typeError) {
        ctx->sendError(request, 422, typeError);
        return;
    }
    config.enabled = true;
    int requestedAcks = ctx->getParamInt(request, "acknowledgements", 0);
    if (requestedAcks < 0) requestedAcks = 0;
    if (compiledBoardProfileSupported()) {
        const FastBee::BoardProfile& profile = FastBee::getCompiledBoardProfile();
        std::unique_ptr<FastBee::PreflightResult> workspace;
        if (!allocatePreflightWorkspace(ctx, request, workspace)) return;
        FastBee::PreflightResult& preflightResult = *workspace;
        const std::vector<PeripheralConfig> existing = pm.getAllPeripherals();
        FastBee::preflightPeripheralChange(
            profile, existing.data(), existing.size(), config,
            FastBee::PreflightOperation::ENABLE, id,
            makePreflightContext(
                profile,
                static_cast<uint32_t>(requestedAcks) & PREFLIGHT_ACK_MASK, ctx->networkManager),
            preflightResult);
        if (sendBlockedPreflight(request, profile, preflightResult, config,
                                 FastBee::PreflightOperation::ENABLE)) {
            return;
        }
    }
    String errorMsg;
    const bool group = config.type == PeripheralType::IO_EXPANDER &&
        strcasecmp(config.params.ioExpander.driver, "MCP23017") == 0;
    std::vector<String> channelIds;
    if (group) {
        const String expected = ctx->getParamValue(request, "groupToken", "");
        if (expected.length() != 48 || expected != mcpGroupReviewToken(pm.getConfigurationReviewVersion())) {
            ctx->sendError(request, 409, "IO_GROUP_REVIEW_STALE: reload the group preview and confirm again"); return;
        }
        const String encoded = ctx->getParamValue(request, "channelIds", "");
        JsonDocument selection;
        if (encoded.isEmpty() || encoded.length() > 4096 ||
            deserializeJson(selection, encoded.c_str(), encoded.length()) || !selection.is<JsonArray>() ||
            selection.as<JsonArray>().size() == 0 || selection.as<JsonArray>().size() > 16) {
            ctx->sendError(request, 400, "IO_GROUP_REQUIRED: channelIds must be a JSON array of 1..16 IDs");
            return;
        }
        for (JsonVariantConst value : selection.as<JsonArrayConst>()) {
            const JsonString selected = value.as<JsonString>();
            if (!value.is<const char*>() || selected.size() == 0 || selected.size() > 40 ||
                strlen(selected.c_str()) != selected.size()) {
                ctx->sendError(request, 400, "IO_GROUP_INVALID: invalid channel ID"); return;
            }
            channelIds.emplace_back(selected.c_str());
        }
    }
    if (group ? pm.enableIoExpanderGroupAndSave(id, channelIds, errorMsg)
              : pm.enablePeripheralAndSave(id, errorMsg)) {
        ctx->sendSuccess(request, "Peripheral enabled");
    } else {
        ctx->sendError(request, mutationErrorStatus(errorMsg),
                       errorMsg.isEmpty() ? "Failed to enable peripheral" : errorMsg);
    }
}

void PeripheralRouteHandler::handleDisablePeripheral(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

    String id = ctx->getParamValue(request, "id", "");
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Missing id parameter");
        return;
    }

    PeripheralManager& pm = PeripheralManager::getInstance();
    PeripheralConfig config;
    if (!pm.getPeripheralSnapshot(id, config)) {
        ctx->sendError(request, 404, "Peripheral not found");
        return;
    }
    if (config.type == PeripheralType::MODBUS_DEVICE) {
        ctx->sendError(request, 422,
                       userConfigurablePeripheralTypeError(config.type));
        return;
    }
    String errorMsg;
    if (pm.disablePeripheralAndSave(id, errorMsg)) {
        ctx->sendSuccess(request, "Peripheral disabled");
    } else {
        ctx->sendError(request, mutationErrorStatus(errorMsg),
                       errorMsg.isEmpty() ? "Failed to disable peripheral" : errorMsg);
    }
}

void PeripheralRouteHandler::handleGetPeripheralStatus(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    String id = ctx->getParamValue(request, "id", "");
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Missing id parameter");
        return;
    }

    PeripheralManager& pm = PeripheralManager::getInstance();
    if (ctx->getParamValue(request, "groupPreview", "0") == "1") {
        JsonDocument preview;
        String error;
        if (!buildMcpGroupPreview(pm, id, preview, error)) {
            ctx->sendError(request, error.startsWith("IO_GROUP_BUSY") ? 503 : 422, error); return;
        }
        // The snapshot lock has been released before allocating/sending HTTP.
        HandlerUtils::sendJsonStream(request, preview);
        return;
    }
    PeripheralConfig config;
    if (!pm.getPeripheralSnapshot(id, config)) {
        ctx->sendError(request, 404, "Peripheral not found");
        return;
    }
    EncoderReading encoderReading;
    const bool hasEncoderReading = config.type == PeripheralType::ENCODER &&
                                   pm.getEncoderReading(id, encoderReading);
    PeripheralRuntimeState runtimeState;
    const bool hasRuntimeState = pm.getRuntimeStateSnapshot(id, runtimeState);

    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();

    FastBee::setFixedJsonField(data, "id", id);
    FastBee::setFixedJsonField(data, "enabled", config.enabled);
    if (config.type == PeripheralType::PCA9685_PWM &&
        !pm.getPca9685Status(id, data["pca9685"].to<JsonObject>())) {
        ctx->sendError(request, 503, "PCA_STATUS_BUSY"); return;
    }
    FastBee::setFixedJsonField(data, "status", hasRuntimeState ? static_cast<int>(runtimeState.status) : 0);
    FastBee::setFixedJsonField(data, "groupActivation", config.type == PeripheralType::IO_EXPANDER &&
        strcasecmp(config.params.ioExpander.driver, "MCP23017") == 0);
    FastBee::setFixedJsonField(data, "memorySuspended", hasRuntimeState && runtimeState.memorySuspended);
    FastBee::setFixedJsonField(data, "controlOutputStatusAvailable", hasRuntimeState);
    if (hasRuntimeState) {
        FastBee::setFixedJsonField(data, "controlOutputClaimed", runtimeState.controlOutputClaimed);
        FastBee::setFixedJsonField(data, "controlOutputFault", runtimeState.controlOutputFault);
        FastBee::setFixedJsonField(data, "controlOutputRetiring", runtimeState.controlOutputRetiring);
        FastBee::setFixedJsonField(data, "controlOutputRetirementAttempts", runtimeState.controlOutputRetirementAttempts);
        FastBee::setFixedJsonField(data, "controlOutputRetirementError", runtimeState.controlOutputRetirementFailed
            ? "CONTROL_OUTPUT_RETIRE_UNCONFIRMED" : "");
    }

    if (hasRuntimeState) {
        FastBee::setFixedJsonField(data, "initTime", runtimeState.initTime);
        FastBee::setFixedJsonField(data, "lastActivity", runtimeState.lastActivity);
        FastBee::setFixedJsonField(data, "errorCount", runtimeState.errorCount);
        if (!runtimeState.lastError.isEmpty()) {
            FastBee::setFixedJsonField(data, "lastError", runtimeState.lastError);
        }
        if (config.type == PeripheralType::PWM_SERVO) {
            FastBee::setFixedJsonField(data, "angle", runtimeState.state.servo.currentAngle);
            FastBee::setFixedJsonField(data, "pulseUs", runtimeState.state.servo.currentPulseUs);
        } else if (config.type == PeripheralType::UART) {
            FastBee::setFixedJsonField(data, "framing", uartFramingName(config));
            addUartRuntime(data, runtimeState, config.enabled);
        }
    }
    if (hasEncoderReading) addEncoderReading(data, encoderReading);

    if (config.type == PeripheralType::SDIO) {
        const SDCardStatus status = SDCardManager::getInstance().getStatus();
        FastBee::writeSdCardStatusJson(data, status, id);
    } else if (config.type == PeripheralType::DS1302_RTC) {
#if FASTBEE_ENABLE_DS1302
        FastBee::RtcAccess rtcAccess(id);
        FastBee::setFixedJsonField(data, "initialized", rtcAccess.driver() != nullptr);
        if (rtcAccess.driver()) FastBee::setFixedJsonField(data, "running", rtcAccess.driver()->isRunning());
        else FastBee::setFixedJsonField(data, "driverError", rtcAccess.error());
#else
        FastBee::setFixedJsonField(data, "initialized", false);
#endif
    } else if (config.type == PeripheralType::LCD1602) {
#if FASTBEE_ENABLE_LCD1602
        FastBee::setFixedJsonField(data, "initialized", LCD1602Driver::getInstance().isInitialized());
        FastBee::setFixedJsonField(data, "cols", LCD1602Driver::getInstance().getCols());
        FastBee::setFixedJsonField(data, "rows", LCD1602Driver::getInstance().getRows());
        FastBee::setFixedJsonField(data, "backlight", LCD1602Driver::getInstance().isBacklightOn());
#else
        FastBee::setFixedJsonField(data, "initialized", false);
#endif
    } else if (config.type == PeripheralType::RFID_MFRC522) {
#if FASTBEE_ENABLE_RFID
        RFIDDriver::Status rfidStatus;
        String rfidStatusError;
        const bool available = RFIDDriver::getInstance().getStatus(rfidStatus, rfidStatusError);
        FastBee::setFixedJsonField(data, "statusAvailable", available);
        if (available) {
            FastBee::setFixedJsonField(data, "initialized", rfidStatus.initialized);
            FastBee::setFixedJsonField(data, "cardPresent", rfidStatus.cardPresent);
            FastBee::setFixedJsonField(data, "uid", rfidStatus.uid);
            FastBee::setFixedJsonField(data, "cardType", rfidStatus.cardType);
            if (!rfidStatus.lastError.isEmpty()) FastBee::setFixedJsonField(data, "driverError", rfidStatus.lastError);
        } else {
            FastBee::setFixedJsonField(data, "driverError", rfidStatusError);
        }
#else
        FastBee::setFixedJsonField(data, "initialized", false);
#endif
    } else if (config.type == PeripheralType::SENSOR_GENERIC &&
               config.params.sensor.driver[0] != '\0') {
        FastBee::setFixedJsonField(data, "driver", config.params.sensor.driver);
        FastBee::setFixedJsonField(data, "address", config.params.sensor.address);
        FastBee::setFixedJsonField(data, "mode", config.params.sensor.mode);
        SensorReading sample;
        bool stale = true;
        String sampleError;
        const bool sampled = FastBee::SensorDriverCache::instance().getSnapshot(id, sample, stale, sampleError);
        FastBee::setFixedJsonField(data, "sampled", sampled);
        FastBee::setFixedJsonField(data, "stale", stale);
        FastBee::setFixedJsonField(data, "timestamp", sampled ? sample.timestamp : 0);
        if (!sampleError.isEmpty()) FastBee::setFixedJsonField(data, "driverError", sampleError);
        if (sampled) {
            const auto* metadata = FastBee::DriverRegistry::getInstance().getMetadata(config.params.sensor.driver);
            JsonArray channels = data["channels"].to<JsonArray>();
            for (uint8_t i = 0; metadata && i < sample.channelCount && i < 4; ++i) {
                const auto* descriptor = metadata->getChannel(i, config.params.sensor.mode, config.params.sensor.scanMask);
                if (!descriptor) break;
                JsonObject channel = channels.add<JsonObject>();
                FastBee::setFixedJsonField(channel, "name", descriptor->name);
                FastBee::setFixedJsonField(channel, "unit", descriptor->unit);
                FastBee::setFixedJsonField(channel, "value", sample.values[i]);
                FastBee::setFixedJsonField(channel, "valid", !stale);
                if (sample.channelMetadata) {
                    FastBee::setFixedJsonField(channel, "timestamp", sample.channelTimestamps[i]);
                    FastBee::setFixedJsonField(channel, "quality", !stale ? "good" : "scan_invalid");
                    FastBee::setFixedJsonField(channel, "error", sensorChannelErrorName(sample.channelErrors[i]));
                }
            }
        }
    }

    HandlerUtils::sendJsonStream(request, doc);
}

void PeripheralRouteHandler::handleValidatePins(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    String typeStr = ctx->getParamValue(request, "type", "");
    String pinsStr = ctx->getParamValue(request, "pins", "");
    String excludeId = ctx->getParamValue(request, "excludeId", "");

    if (typeStr.isEmpty() || pinsStr.isEmpty()) {
        request->send(400, "application/json", "{\"success\":false,\"message\":\"Missing type or pins\"}");
        return;
    }

    for (size_t i = 0; i < typeStr.length(); ++i) {
        if (typeStr[i] < '0' || typeStr[i] > '9') {
            ctx->sendError(request, 400, "INVALID_PERIPHERAL_TYPE: type must be numeric");
            return;
        }
    }
    PeripheralType type = static_cast<PeripheralType>(typeStr.toInt());
    const char* typeError = userConfigurablePeripheralTypeError(type);
    if (typeError) {
        ctx->sendError(request, 422, typeError);
        return;
    }
    PeripheralManager& pm = PeripheralManager::getInstance();

    uint8_t parsedPins[8] = {255, 255, 255, 255, 255, 255, 255, 255};
    uint8_t parsedPinCount = 0;
    String parseError;
    if (!parsePeripheralPins(pinsStr, parsedPins, parsedPinCount, parseError) || parsedPinCount == 0) {
        ctx->sendError(request, 400,
                       parseError.isEmpty() ? "INVALID_PIN_LIST: 至少需要一个引脚" : parseError);
        return;
    }

    String items = "";
    bool allValid = true;
    for (uint8_t pinIdx = 0; pinIdx < parsedPinCount; ++pinIdx) {
        const uint8_t pin = parsedPins[pinIdx];
        String pinError;
        String warning;
        bool pinOk = pm.validatePinForType(pin, type, pinError);
        if (!pinOk) allValid = false;

        const String conflictInfo = pm.getPinConflictInfo(pin, excludeId);
        if (!conflictInfo.isEmpty()) {
            if (conflictInfo.startsWith("警告:")) {
                warning = conflictInfo;
            } else {
                pinError = conflictInfo;
                pinOk = false;
                allValid = false;
            }
        }

        if (pinIdx > 0) items += ",";
        items += F("{\"pin\":");
        items += String(pin);
        items += F(",\"valid\":");
        items += pinOk ? F("true") : F("false");
        if (!pinError.isEmpty()) {
            items += F(",\"error\":\"");
            items += escapePeriphJsonString(pinError);
            items += F("\"");
        }
        if (!warning.isEmpty()) {
            items += F(",\"warning\":\"");
            items += escapePeriphJsonString(warning);
            items += F("\"");
        }
        items += F("}");
    }

    String response;
    response.reserve(64 + items.length());
    response += F("{\"success\":true,\"allValid\":");
    response += allValid ? F("true") : F("false");
    response += F(",\"pins\":[");
    response += items;
    response += F("]}");
    request->send(200, "application/json", response);
}

void PeripheralRouteHandler::handleWritePeripheral(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

    String id = ctx->getParamValue(request, "id", "");
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Missing id parameter");
        return;
    }

    PeripheralManager& pm = PeripheralManager::getInstance();
    PeripheralConfig config;
    uint32_t writeConfigVersion = 0;
    if (!pm.getPeripheralSnapshot(id, config, &writeConfigVersion)) {
        ctx->sendError(request, 404, "Peripheral not found");
        return;
    }

    bool success = false;
    String message;
    const char* uartWriteError = nullptr;

    if (config.type == PeripheralType::PCA9685_PWM) {
        const String command = ctx->getParamValue(request, "action", "");
        JsonDocument response;
        JsonObject data = response["data"].to<JsonObject>();
        String error;
        if (command == "prepare") {
            success = pm.preparePca9685(id, data["review"].to<JsonObject>(), error);
        } else if (command == "start") {
            success = pm.startPca9685(id, ctx->getParamValue(request, "token", ""),
                ctx->getParamValue(request, "powerCycleConfirmed", "") == "true", error);
        } else {
            uint32_t channel = 0;
            double value = 0;
            if (command == "duty" || command == "pulse" || command == "angle") {
                if (ctx->getParamValue(request, "channel", "").isEmpty()) {
                    ctx->sendError(request, 422, "PCA_CHANNEL_REQUIRED"); return;
                }
                if (!readIoRequestInteger(ctx, request, "channel", 0, 0, 15, channel, error)) {
                    ctx->sendError(request, 422, error); return;
                }
                const String text = ctx->getParamValue(request, "value", "");
                char* end = nullptr;
                value = strtod(text.c_str(), &end);
                if (text.isEmpty() || end != text.c_str() + text.length() || !std::isfinite(value)) {
                    ctx->sendError(request, 422, "PCA_VALUE_INVALID"); return;
                }
            }
            FastBee::OrdinaryControlContext control;
            FastBee::OrdinaryControlResult controlResult;
            const bool emergency = command == "disable" || command == "stop";
            if (!emergency && !FastBee::beginHttpPcaOutput(*ctx, request, id, writeConfigVersion, control)) return;
            success = pm.commandPca9685(id, command, channel, value, error,
                emergency ? nullptr : &control, &controlResult);
            if (!emergency) pm.finishOrdinaryControl(control, false);
            FastBee::writeOrdinaryControlJson(data["control"].to<JsonObject>(), controlResult);
            FastBee::setFixedJsonField(data, "configEpoch", writeConfigVersion);
        }
        response["success"] = success;
        if (!success) response["error"] = error;
        FastBee::setFixedJsonField(data, "id", id); FastBee::setFixedJsonField(data, "physicalOutputVerified", false);
        FastBee::setFixedJsonField(data, "statusAvailable", pm.getPca9685Status(id, data["status"].to<JsonObject>()));
        if (response.overflowed()) { ctx->sendError(request, 503, "PCA_RESPONSE_UNCONFIRMED"); return; }
        HandlerUtils::sendJsonStream(request, response);
        return;
    }

    if (config.type == PeripheralType::SENSOR_GENERIC &&
        config.params.sensor.driver[0] != '\0' &&
        FastBee::stringEqualsIgnoreCaseLiteral(ctx->getParamValue(request, "action", ""), "release")) {
        success = pm.releaseSensorCache(id);
        message = success ? "Sensor driver cache released" : "SENSOR_CONFIG_BUSY: retry release";
    } else if (config.type == PeripheralType::SDIO) {
        if (!config.enabled) {
            ctx->sendError(request, 409, "SD_NOT_READY: enable the SD peripheral first");
            return;
        }
        FastBee::SdAccess storage(id, writeConfigVersion);
        if (!storage.driver()) { ctx->sendError(request, 409, storage.error()); return; }
        const String action = ctx->getParamValue(request, "action", "");
        String errorMsg;
        if (FastBee::stringEqualsIgnoreCaseLiteral(action, "mount")) {
            success = SDCardManager::getInstance().mount(errorMsg, id);
            message = success ? "SD card mounted" : errorMsg;
        } else if (FastBee::stringEqualsIgnoreCaseLiteral(action, "recover")) {
            success = storage.driver()->recover(id, errorMsg);
            message = success ? "SD remounted; prior file operation was not replayed" : errorMsg;
        } else if (FastBee::stringEqualsIgnoreCaseLiteral(action, "unmount")) {
            success = SDCardManager::getInstance().unmount(id, &errorMsg, false);
            message = success ? "SD card unmounted" : errorMsg;
        } else if (FastBee::stringEqualsIgnoreCaseLiteral(action, "write") ||
                   FastBee::stringEqualsIgnoreCaseLiteral(action, "write-file")) {
            const String path = ctx->getParamValue(request, "path", "");
            const String content = ctx->getParamValue(request, "content", "");
            if (content.isEmpty()) {
                ctx->sendError(request, 400, "SD_WRITE_SIZE_INVALID: content is empty");
                return;
            }
            if (content.length() > SDCardManager::MAX_FILE_BYTES) {
                ctx->sendError(request, 413, "SD_WRITE_SIZE_INVALID: maximum 16384 bytes");
                return;
            }
            const bool append = ctx->getParamBool(request, "append", false);
            success = SDCardManager::getInstance().writeFile(
                path, reinterpret_cast<const uint8_t*>(content.c_str()),
                content.length(), append, errorMsg, id);
            message = success ? String("SD file written: ") +
                                    String(content.length()) + " bytes"
                              : errorMsg;
        } else if (FastBee::stringEqualsIgnoreCaseLiteral(action, "delete") ||
                   FastBee::stringEqualsIgnoreCaseLiteral(action, "delete-file")) {
            const String path = ctx->getParamValue(request, "path", "");
            const String confirmation = ctx->getParamValue(request, "confirm", "");
            if (confirmation != "DELETE") {
                ctx->sendError(request, 400,
                               "SD_DELETE_CONFIRMATION_REQUIRED: use confirm=DELETE");
                return;
            }
            success = SDCardManager::getInstance().removeFile(path, errorMsg, id);
            message = success ? "SD path deleted" : errorMsg;
        } else {
            ctx->sendError(
                request, 400,
                "SD_ACTION_INVALID: expected mount, recover, unmount, write-file or delete-file");
            return;
        }
    } else if (config.type == PeripheralType::DS1302_RTC) {
        if (!config.enabled) {
            ctx->sendError(request, 409, "DS1302_NOT_READY: enable the RTC first");
            return;
        }
#if FASTBEE_ENABLE_DS1302
        FastBee::RtcAccess rtcAccess(id);
        if (!rtcAccess.driver()) {
            ctx->sendError(request, 409, rtcAccess.error());
            return;
        }
        DS1302Driver& rtc = *rtcAccess.driver();
        const String action = ctx->getParamValue(request, "action", "");
        if (FastBee::stringEqualsIgnoreCaseLiteral(action, "setDateTime") ||
            FastBee::stringEqualsIgnoreCaseLiteral(action, "set-datetime")) {
            const int year = ctx->getParamInt(request, "year", -1);
            const int month = ctx->getParamInt(request, "month", -1);
            const int day = ctx->getParamInt(request, "day", -1);
            const int hour = ctx->getParamInt(request, "hour", -1);
            const int minute = ctx->getParamInt(request, "minute", -1);
            const int second = ctx->getParamInt(request, "second", -1);
            int dayOfWeek = ctx->getParamInt(request, "dayOfWeek", 0);
            if (year < 2000 || year > 2099 || month < 1 || month > 12 ||
                day < 1 || day > daysInMonth(year, month) || hour < 0 ||
                hour > 23 || minute < 0 || minute > 59 || second < 0 ||
                second > 59 || dayOfWeek < 0 || dayOfWeek > 7) {
                ctx->sendError(request, 400, "DS1302_DATETIME_INVALID");
                return;
            }
            if (dayOfWeek == 0) {
                dayOfWeek = calculateDayOfWeek(year, month, day);
            }
            DS1302DateTime value = {
                static_cast<uint8_t>(year - 2000), static_cast<uint8_t>(month),
                static_cast<uint8_t>(day), static_cast<uint8_t>(hour),
                static_cast<uint8_t>(minute), static_cast<uint8_t>(second),
                static_cast<uint8_t>(dayOfWeek)
            };
            success = rtc.setDateTime(value);
            message = success ? "DS1302 date/time updated" : "DS1302_WRITE_FAILED";
        } else if (FastBee::stringEqualsIgnoreCaseLiteral(action, "setRunning") ||
                   FastBee::stringEqualsIgnoreCaseLiteral(action, "set-running")) {
            if (!request->hasParam("running", true)) {
                ctx->sendError(request, 400, "DS1302_RUNNING_REQUIRED");
                return;
            }
            const bool running = ctx->getParamBool(request, "running", true);
            if (!rtc.readDateTime().isValid()) {
                ctx->sendError(request, 502, "DS1302_INVALID_DATA: set a valid date/time first");
                return;
            }
            rtc.setRunning(running);
            success = rtc.isRunning() == running;
            message = success ? (running ? "DS1302 clock started"
                                         : "DS1302 clock stopped")
                              : "DS1302_RUNNING_VERIFY_FAILED";
        } else {
            ctx->sendError(
                request, 400,
                "DS1302_ACTION_INVALID: expected set-datetime or set-running");
            return;
        }
#else
        ctx->sendError(request, 501, "DS1302_NOT_COMPILED");
        return;
#endif
    } else if (config.type == PeripheralType::LCD1602) {
        if (!config.enabled) {
            ctx->sendError(request, 409,
                           "LCD1602_NOT_READY: enable the display first");
            return;
        }
#if FASTBEE_ENABLE_LCD1602
        LCD1602Driver& lcd = LCD1602Driver::getInstance();
        if (!lcd.isInitialized()) {
            ctx->sendError(request, 409, "LCD1602_NOT_INITIALIZED");
            return;
        }
        const String action = ctx->getParamValue(request, "action", "");
        if (FastBee::stringEqualsIgnoreCaseLiteral(action, "print")) {
            const String content = ctx->getParamValue(request, "text", "");
            const int row = ctx->getParamInt(request, "row", 0);
            const int col = ctx->getParamInt(request, "col", 0);
            if (content.isEmpty() || content.length() > LCD1602_API_MAX_BYTES ||
                row < 0 || row >= lcd.getRows() || col < 0 ||
                col >= lcd.getCols()) {
                ctx->sendError(
                    request, 400,
                    "LCD1602_PRINT_INVALID: check text, row and column");
                return;
            }
            success = lcd.printAt(static_cast<uint8_t>(col), static_cast<uint8_t>(row), content);
            message = success ? "LCD1602 text printed" : "LCD1602_PRINT_FAILED";
        } else if (FastBee::stringEqualsIgnoreCaseLiteral(action, "clear")) {
            success = lcd.clear();
            message = success ? "LCD1602 cleared" : "LCD1602_CLEAR_UNCONFIRMED";
        } else if (FastBee::stringEqualsIgnoreCaseLiteral(action, "backlight")) {
            if (!request->hasParam("enabled", true)) {
                ctx->sendError(request, 400,
                               "LCD1602_BACKLIGHT_VALUE_REQUIRED");
                return;
            }
            success = lcd.setBacklight(ctx->getParamBool(request, "enabled", true));
            message = success ? "LCD1602 backlight updated" : "LCD1602_BACKLIGHT_UNCONFIRMED";
        } else {
            ctx->sendError(
                request, 400,
                "LCD1602_ACTION_INVALID: expected print, clear or backlight");
            return;
        }
#else
        ctx->sendError(request, 501, "LCD1602_NOT_COMPILED");
        return;
#endif
    } else if (config.type == PeripheralType::RFID_MFRC522) {
        if (!config.enabled) {
            ctx->sendError(request, 409, "RFID_NOT_READY: enable the reader first");
            return;
        }
#if FASTBEE_ENABLE_RFID
        RFIDDriver& reader = RFIDDriver::getInstance();
        if (!reader.isInitialized()) {
            ctx->sendError(request, 409, "RFID_NOT_INITIALIZED");
            return;
        }
        const String action = ctx->getParamValue(request, "action", "");
        const int blockValue = ctx->getParamInt(request, "block", -1);
        String keyText = ctx->getParamValue(request, "key", "");
        uint8_t key[6] = {0};
        size_t keyLength = 0;
        String errorMsg;
        const bool keyParsed =
            parseStrictHex(keyText, key, sizeof(key), keyLength, errorMsg);
        keyText = "";
        if (blockValue < 1 || blockValue > 62) {
            memset(key, 0, sizeof(key));
            ctx->sendError(
                request, 400,
                "RFID_BLOCK_INVALID: block must be between 1 and 62");
            return;
        }
        if (!keyParsed || keyLength != sizeof(key)) {
            memset(key, 0, sizeof(key));
            ctx->sendError(
                request, 400,
                keyLength != sizeof(key)
                    ? "RFID_KEY_INVALID: exactly six bytes are required"
                    : errorMsg);
            return;
        }

        if (FastBee::stringEqualsIgnoreCaseLiteral(action, "readBlock") ||
            FastBee::stringEqualsIgnoreCaseLiteral(action, "read-block")) {
            uint8_t blockData[16] = {0};
            success = reader.readBlock(static_cast<uint8_t>(blockValue), key,
                                       blockData, errorMsg);
            memset(key, 0, sizeof(key));
            if (!success) {
                ctx->sendError(request, 409, errorMsg);
                return;
            }
            JsonDocument response;
            response["success"] = true;
            JsonObject payload = response["data"].to<JsonObject>();
            FastBee::setFixedJsonField(payload, "id", id);
            FastBee::setFixedJsonField(payload, "block", blockValue);
            FastBee::setFixedJsonField(payload, "hex", bytesToHex(blockData, sizeof(blockData)));
            FastBee::setFixedJsonField(payload, "uid", reader.getLastUID());
            FastBee::setFixedJsonField(payload, "cardType", reader.getLastCardType());
            memset(blockData, 0, sizeof(blockData));
            HandlerUtils::sendJsonStream(request, response);
            return;
        }

        if (FastBee::stringEqualsIgnoreCaseLiteral(action, "writeBlock") ||
            FastBee::stringEqualsIgnoreCaseLiteral(action, "write-block")) {
            const String confirmation =
                ctx->getParamValue(request, "confirm", "");
            if (confirmation != "WRITE_BLOCK") {
                memset(key, 0, sizeof(key));
                ctx->sendError(
                    request, 400,
                    "RFID_WRITE_CONFIRMATION_REQUIRED: use confirm=WRITE_BLOCK");
                return;
            }
            const String dataText = ctx->getParamValue(request, "data", "");
            uint8_t blockData[16] = {0};
            size_t dataLength = 0;
            const bool dataParsed = parseStrictHex(
                dataText, blockData, sizeof(blockData), dataLength, errorMsg);
            if (!dataParsed || dataLength != sizeof(blockData)) {
                memset(key, 0, sizeof(key));
                memset(blockData, 0, sizeof(blockData));
                ctx->sendError(
                    request, 400,
                    dataLength != sizeof(blockData)
                        ? "RFID_DATA_INVALID: exactly 16 bytes are required"
                        : errorMsg);
                return;
            }
            success = reader.writeBlockVerified(
                static_cast<uint8_t>(blockValue), key, blockData, errorMsg);
            memset(key, 0, sizeof(key));
            memset(blockData, 0, sizeof(blockData));
            message = success ? "RFID block written and verified" : errorMsg;
        } else {
            memset(key, 0, sizeof(key));
            ctx->sendError(
                request, 400,
                "RFID_ACTION_INVALID: expected read-block or write-block");
            return;
        }
#else
        ctx->sendError(request, 501, "RFID_NOT_COMPILED");
        return;
#endif
    } else if (config.type == PeripheralType::ENCODER) {
        String action = ctx->getParamValue(request, "action", "");
        const bool resetRequested = ctx->getParamBool(request, "reset", false) ||
                                    FastBee::stringEqualsIgnoreCaseLiteral(action, "reset");
        if (!resetRequested) {
            ctx->sendError(request, 400, "ENCODER_READ_ONLY: use action=reset or reset=1");
            return;
        }
        success = pm.resetEncoder(id);
        message = success ? "Encoder reset" : "Encoder reset failed";
    } else if (config.type == PeripheralType::PWM_SERVO) {
        FastBee::handleHttpServoOutput(*ctx, request, id, writeConfigVersion);
        return;
    } else if (config.type == PeripheralType::GPIO_DIGITAL_OUTPUT &&
               !config.isExpanderChannel() && !config.params.gpio.passive) {
        FastBee::handleHttpNativeDigital(*ctx, request, id, writeConfigVersion);
        return;
    } else if (!config.isModbusPeripheral() && !config.isExpanderChannel() &&
               (config.type == PeripheralType::GPIO_PWM_OUTPUT ||
                config.type == PeripheralType::GPIO_ANALOG_OUTPUT || config.type == PeripheralType::DAC ||
                (config.type == PeripheralType::GPIO_DIGITAL_OUTPUT && config.params.gpio.passive))) {
        FastBee::handleHttpNativeScalar(*ctx, request, id, writeConfigVersion);
        return;
    } else if (config.isGPIOPeripheral()) {
        if (request->hasParam("state", true)) {
            int stateValue = ctx->getParamInt(request, "state", 0);
            GPIOState state = (stateValue == 1) ? GPIOState::STATE_HIGH : GPIOState::STATE_LOW;
            success = pm.writePin(id, state);
            message = success ? "GPIO state set" : "GPIO write failed";
        } else if (request->hasParam("toggle", true)) {
            success = pm.togglePin(id);
            message = success ? "GPIO toggled" : "GPIO toggle failed";
        } else if (request->hasParam("pwm", true) &&
                 (config.type == PeripheralType::GPIO_PWM_OUTPUT ||
                  config.type == PeripheralType::GPIO_ANALOG_OUTPUT ||
                  config.type == PeripheralType::PWM_SERVO)) {
            uint32_t dutyCycle = ctx->getParamInt(request, "pwm", 0);
            success = pm.writePWM(id, dutyCycle);
            message = success ? "PWM duty set" : "PWM write failed";
        } else {
            ctx->sendError(request, 400, "Missing parameter (state/toggle/pwm)");
            return;
        }
    } else if (config.type == PeripheralType::DAC) {
        if (request->hasParam("value", true)) {
            const String text = ctx->getParamValue(request, "value", "");
            uint8_t value = 0;
            if (!FastBee::parseDacActionValue(text.c_str(), text.length(), value)) {
                ctx->sendError(request, 400, "DAC value must be an integer from 0 to 255"); return;
            }
            success = FastBee::writeDacAtVersion(pm, id, writeConfigVersion, value);
            message = success ? "DAC write accepted (SDK only)" : "DAC write unconfirmed or configuration changed";
        } else {
            ctx->sendError(request, 400, "Missing value parameter");
            return;
        }
    } else if (config.type == PeripheralType::RF_MODULE) {
        FastBee::handleHttpRfOutput(*ctx, request, id, writeConfigVersion);
        return;
    } else if (config.isCommunicationPeripheral()) {
        const auto writeCommunication = [&](const uint8_t* bytes, size_t length) {
            if (config.type != PeripheralType::UART) return pm.writeData(id, bytes, length);
            RecursiveMutexGuard lock(pm.getMutex(), 0);
            PeripheralConfig current;
            PeripheralRuntimeState runtime;
            uint32_t currentVersion = 0;
            if (!lock.isLocked()) uartWriteError = "UART_BUSY: no bytes submitted";
            else if (!pm.getPeripheralSnapshot(id, current, &currentVersion) ||
                     currentVersion != writeConfigVersion || current.type != PeripheralType::UART)
                uartWriteError = "UART_CONFIG_CHANGED: no bytes submitted";
            else if (!current.enabled || !pm.getRuntimeStateSnapshot(id, runtime) ||
                     runtime.memorySuspended || !runtime.state.comm.isConnected)
                uartWriteError = "UART_NOT_READY: no bytes submitted";
            else return pm.writeData(id, bytes, length);
            return false;
        };
        if (request->hasParam("data", true)) {
            String dataStr = ctx->getParamValue(request, "data", "");
            if (!dataStr.isEmpty()) {
                if (request->hasParam("hex", true) && ctx->getParamInt(request, "hex", 0) == 1) {
                    uint8_t bytes[UART_API_MAX_BYTES];
                    size_t byteCount = 0;
                    String parseError;
                    if (!parseStrictHex(dataStr, bytes, sizeof(bytes), byteCount, parseError)) {
                        ctx->sendError(request,
                            parseError.startsWith("PAYLOAD_TOO_LARGE") ? 413 : 400,
                            parseError);
                        return;
                    }
                    success = writeCommunication(bytes, byteCount);
                    message = success ? String("Sent ") + String(byteCount) + " bytes (HEX)" : "Send failed";
                } else {
                    if (config.type == PeripheralType::UART &&
                        dataStr.length() > UART_API_MAX_BYTES) {
                        ctx->sendError(request, 413,
                            String("PAYLOAD_TOO_LARGE: maximum ") +
                            String(UART_API_MAX_BYTES) + " bytes");
                        return;
                    }
                    success = writeCommunication((const uint8_t*)dataStr.c_str(), dataStr.length());
                    message = success ? String("Sent ") + String(dataStr.length()) + " bytes" : "Send failed";
                }
            } else {
                ctx->sendError(request, 400, "Data is empty");
                return;
            }
        } else {
            ctx->sendError(request, 400, "Missing data parameter");
            return;
        }
    } else {
        if (request->hasParam("value", true)) {
            int value = ctx->getParamInt(request, "value", 0);
            uint8_t bytes[4];
            bytes[0] = value & 0xFF;
            bytes[1] = (value >> 8) & 0xFF;
            bytes[2] = (value >> 16) & 0xFF;
            bytes[3] = (value >> 24) & 0xFF;

            size_t len = 1;
            if (value > 0xFF) len = 2;
            if (value > 0xFFFF) len = 4;

            success = pm.writeData(id, bytes, len);
            message = success ? "Data written" : "Data write failed";
        } else {
            ctx->sendError(request, 400, "Missing value parameter");
            return;
        }
    }

    if (config.type == PeripheralType::UART) {
        message = uartWriteError ? uartWriteError : success ? "UART_LOCAL_ACCEPTED: bytes accepted by local driver; peer not verified"
                          : "UART_WRITE_UNCONFIRMED: bytes may have been partially sent; do not retry blindly";
    }
    if (success) {
        ctx->sendSuccess(request, message);
    } else {
        ctx->sendError(request, 400, message);
    }
}
