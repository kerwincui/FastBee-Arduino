#include "core/PeripheralPreflightRouteSupport.h"
#include "core/AsyncExecTypes.h"
#include "core/BoardProfile.h"
#include "core/ChipConfig.h"
#include "core/PeripheralManager.h"
#include "core/PeripheralFormFields.h"
#include "core/PeripheralParamApply.h"
#include "core/PeripheralRouteMapper.h"
#include "PeripheralManagerConfigHelpers.h"
#include "network/FixedJsonField.h"
#include <esp_system.h>

namespace FastBee {

bool sameText(const char* left, const char* right) {
    if (!left || !right) return left == right;
    return strcmp(left, right) == 0;
}

bool compiledBoardProfileSupported() {
#if CHIP_IS_CLASSIC_ESP32 || CHIP_IS_ESP32C3 || CHIP_IS_ESP32C6 || CHIP_IS_ESP32S3
    return true;
#else
    return false;
#endif
}

const FastBee::BoardProfile* resolveBoardProfile(const String& profileId) {
    if (profileId.isEmpty()) {
        return compiledBoardProfileSupported()
            ? &FastBee::getCompiledBoardProfile() : nullptr;
    }
    const FastBee::BoardProfile& pz = FastBee::getPuzhongPzEsp32Profile();
    const FastBee::BoardProfile& c6 = FastBee::getEsp32C6DevKitC1Profile();
    if (profileId == pz.key || profileId == "0") return &pz;
    if (profileId == c6.key || profileId == "1") return &c6;
    const FastBee::BoardProfile& c3 = FastBee::getEsp32C3Profile();
    if (profileId == c3.key || profileId == "3") return &c3;
    const FastBee::BoardProfile& s3Octal = FastBee::getEsp32S3Profile(true);
    const FastBee::BoardProfile& s3 = FastBee::getEsp32S3Profile(false);
    if (profileId == s3Octal.key || profileId == "2") return &s3Octal;
    if (profileId == s3.key) return &s3;
    const auto& classic = FastBee::getEsp32ModuleProfile(false);
    const auto& classicPsram = FastBee::getEsp32ModuleProfile(true);
    if (profileId == classic.key || profileId == "4") return &classic;
    if (profileId == classicPsram.key || profileId == "5") return &classicPsram;
    return nullptr;
}

namespace {
const char* pinRoleName(FastBee::PinRole role) {
    switch (role) {
        case FastBee::PinRole::GENERIC: return "generic";
        case FastBee::PinRole::INPUT_SIGNAL: return "input";
        case FastBee::PinRole::OUTPUT_SIGNAL: return "output";
        case FastBee::PinRole::I2C_SDA: return "i2c_sda";
        case FastBee::PinRole::I2C_SCL: return "i2c_scl";
        case FastBee::PinRole::SPI_MISO: return "spi_miso";
        case FastBee::PinRole::SPI_MOSI: return "spi_mosi";
        case FastBee::PinRole::SPI_SCK: return "spi_sck";
        case FastBee::PinRole::SPI_CS: return "spi_cs";
        case FastBee::PinRole::UART_RX: return "uart_rx";
        case FastBee::PinRole::UART_TX: return "uart_tx";
        case FastBee::PinRole::ADC_INPUT: return "adc_input";
        case FastBee::PinRole::PWM_OUTPUT: return "pwm_output";
        case FastBee::PinRole::RMT_SIGNAL: return "rmt_signal";
        case FastBee::PinRole::ENCODER_A: return "encoder_a";
        case FastBee::PinRole::ENCODER_B: return "encoder_b";
        case FastBee::PinRole::SDMMC_SIGNAL: return "sdmmc_signal";
        default: return "unknown";
    }
}

const char* claimStateName(FastBee::ClaimState state) {
    return state == FastBee::ClaimState::ACTIVE ? "active" : "dormant";
}

const char* shareModeName(FastBee::ResourceShareMode mode) {
    return mode == FastBee::ResourceShareMode::SHARED_IF_COMPATIBLE
        ? "shared_if_compatible" : "exclusive";
}

bool sameResourceIdentity(const FastBee::ResourceClaim& left,
                          const FastBee::ResourceClaim& right) {
    if (left.kind != right.kind) return false;
    switch (left.kind) {
        case FastBee::ResourceKind::GPIO:
            return left.index == right.index;
        case FastBee::ResourceKind::I2C_ADDRESS:
        case FastBee::ResourceKind::SPI_CS:
        case FastBee::ResourceKind::ADC_CHANNEL:
            return left.unit == right.unit && left.index == right.index;
        default:
            return left.unit == right.unit;
    }
}

size_t countUniqueClaims(const FastBee::PreflightResult& result,
                         FastBee::ResourceKind kind, bool activeOnly) {
    size_t count = 0;
    for (size_t i = 0; i < result.claimCount; ++i) {
        const FastBee::ResourceClaim& claim = result.claims[i];
        if (claim.kind != kind ||
            (activeOnly && claim.state != FastBee::ClaimState::ACTIVE)) {
            continue;
        }
        bool duplicate = false;
        for (size_t j = 0; j < i; ++j) {
            const FastBee::ResourceClaim& earlier = result.claims[j];
            if (earlier.kind != kind ||
                (activeOnly && earlier.state != FastBee::ClaimState::ACTIVE)) {
                continue;
            }
            if (sameResourceIdentity(earlier, claim)) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) ++count;
    }
    return count;
}

size_t countProfilePins(const FastBee::BoardProfile& profile,
                        uint16_t capability) {
    size_t count = 0;
    for (size_t i = 0; i < profile.pinCount; ++i) {
        const FastBee::BoardPinProfile& pin = profile.pins[i];
        if (pin.policy != FastBee::PinPolicy::HARD_FORBIDDEN &&
            (pin.capabilities & capability) == capability) {
            ++count;
        }
    }
    return count;
}

size_t resourceCapacity(const FastBee::BoardProfile& profile,
                        FastBee::ResourceKind kind) {
    switch (kind) {
        case FastBee::ResourceKind::GPIO:
            return countProfilePins(profile, FastBee::PIN_CAP_INPUT);
        case FastBee::ResourceKind::I2C_BUS:
            return profile.resources.i2cBusCount;
        case FastBee::ResourceKind::I2C_ADDRESS:
            return 127;
        case FastBee::ResourceKind::SPI_BUS:
            return profile.resources.spiBusCount;
        case FastBee::ResourceKind::SPI_CS:
            return countProfilePins(profile, FastBee::PIN_CAP_OUTPUT);
        case FastBee::ResourceKind::UART_PORT:
            return profile.resources.genericUartCount;
        case FastBee::ResourceKind::LEDC_CHANNEL:
            return profile.resources.ledcChannelCount;
        case FastBee::ResourceKind::LEDC_TIMER:
            return profile.resources.ledcTimerCount;
        case FastBee::ResourceKind::RMT_CHANNEL:
            return profile.resources.rmtChannelCount;
        case FastBee::ResourceKind::ADC_CHANNEL:
            return countProfilePins(profile, FastBee::PIN_CAP_ADC);
        case FastBee::ResourceKind::ENCODER_SLOT:
            return profile.resources.encoderSlotCount;
        case FastBee::ResourceKind::SDMMC_HOST:
            return profile.resources.sdmmcSupported ? 1 : 0;
        default:
            return 0;
    }
}

const char* resourceLabel(FastBee::ResourceKind kind) {
    switch (kind) {
        case FastBee::ResourceKind::GPIO: return "GPIO";
        case FastBee::ResourceKind::I2C_BUS: return "I2C buses";
        case FastBee::ResourceKind::I2C_ADDRESS: return "I2C addresses";
        case FastBee::ResourceKind::SPI_BUS: return "SPI buses";
        case FastBee::ResourceKind::SPI_CS: return "SPI chip selects";
        case FastBee::ResourceKind::UART_PORT: return "UART ports";
        case FastBee::ResourceKind::LEDC_CHANNEL: return "LEDC channels";
        case FastBee::ResourceKind::LEDC_TIMER: return "LEDC timers";
        case FastBee::ResourceKind::RMT_CHANNEL: return "RMT channels";
        case FastBee::ResourceKind::ADC_CHANNEL: return "ADC channels";
        case FastBee::ResourceKind::ENCODER_SLOT: return "Encoder slots";
        case FastBee::ResourceKind::SDMMC_HOST: return "SDMMC hosts";
        default: return "Unknown";
    }
}
} // namespace

void addResourceRows(JsonArray rows, const FastBee::BoardProfile& profile,
                     const FastBee::PreflightResult& result) {
    static constexpr FastBee::ResourceKind KINDS[] = {
        FastBee::ResourceKind::GPIO,
        FastBee::ResourceKind::I2C_BUS,
        FastBee::ResourceKind::I2C_ADDRESS,
        FastBee::ResourceKind::SPI_BUS,
        FastBee::ResourceKind::SPI_CS,
        FastBee::ResourceKind::UART_PORT,
        FastBee::ResourceKind::LEDC_CHANNEL,
        FastBee::ResourceKind::LEDC_TIMER,
        FastBee::ResourceKind::RMT_CHANNEL,
        FastBee::ResourceKind::ADC_CHANNEL,
        FastBee::ResourceKind::ENCODER_SLOT,
        FastBee::ResourceKind::SDMMC_HOST
    };
    for (FastBee::ResourceKind kind : KINDS) {
        const size_t used = countUniqueClaims(result, kind, true);
        const size_t configured = countUniqueClaims(result, kind, false);
        const size_t capacity = resourceCapacity(profile, kind);
        JsonObject row = rows.add<JsonObject>();
        FastBee::setFixedJsonField(row, "id", FastBee::resourceKindName(kind));
        FastBee::setFixedJsonField(row, "label", resourceLabel(kind));
        FastBee::setFixedJsonField(row, "used", used);
        FastBee::setFixedJsonField(row, "configured", configured);
        FastBee::setFixedJsonField(row, "capacity", capacity);
        FastBee::setFixedJsonField(row, "available", used < capacity ? capacity - used : 0);
        FastBee::setFixedJsonField(row, "status", capacity == 0 ? "unavailable"
            : (used > capacity ? "overcommitted"
                : (used == capacity ? "full" : "available")));
    }
}

uint32_t requiredAcknowledgements(const FastBee::PreflightResult& result) {
    uint32_t required = FastBee::ACK_NONE;
    for (size_t i = 0; i < result.issueCount; ++i) {
        const FastBee::PreflightIssue& issue = result.issues[i];
        if (issue.severity !=
            FastBee::PreflightSeverity::WARNING_REQUIRES_CONFIRMATION) {
            continue;
        }
        switch (issue.code) {
            case FastBee::PreflightIssueCode::PIN_STRAPPING_ACK_REQUIRED:
                required |= FastBee::ACK_STRAPPING_PIN;
                break;
            case FastBee::PreflightIssueCode::PIN_BOARD_RESOURCE_ACK_REQUIRED:
                required |= FastBee::ACK_BOARD_SHARED_PIN;
                break;
            case FastBee::PreflightIssueCode::PIN_UART0_ACTIVE:
                required |= FastBee::ACK_UART0_CONSOLE;
                break;
            case FastBee::PreflightIssueCode::ADC2_WIFI_ACK_REQUIRED:
                required |= FastBee::ACK_ADC2_WITH_WIFI;
                break;
            default:
                break;
        }
    }
    return required;
}

bool hasHardPreflightError(const FastBee::PreflightResult& result) {
    if (result.claimsTruncated || result.issuesTruncated) return true;
    for (size_t i = 0; i < result.issueCount; ++i) {
        if (result.issues[i].severity == FastBee::PreflightSeverity::ERROR) {
            return true;
        }
    }
    return false;
}

void addPreflightIssues(JsonArray issues,
                        const FastBee::PreflightResult& result) {
    for (size_t i = 0; i < result.issueCount; ++i) {
        const FastBee::PreflightIssue& issue = result.issues[i];
        JsonObject item = issues.add<JsonObject>();
        FastBee::setFixedJsonField(item, "code", FastBee::preflightIssueCodeName(issue.code));
        FastBee::setFixedJsonField(item, "severity", FastBee::preflightSeverityName(issue.severity));
        FastBee::setFixedJsonField(item, "requiresConfirmation", issue.severity ==
            FastBee::PreflightSeverity::WARNING_REQUIRES_CONFIRMATION);
        FastBee::setFixedJsonField(item, "resourceKind", FastBee::resourceKindName(issue.resourceKind));
        FastBee::setFixedJsonField(item, "unit", issue.unit);
        FastBee::setFixedJsonField(item, "index", issue.index);
        if (issue.ownerId) FastBee::setFixedJsonField(item, "ownerId", issue.ownerId);
        if (issue.conflictingOwnerId) {
            FastBee::setFixedJsonField(item, "conflictingOwnerId", issue.conflictingOwnerId);
        }
        FastBee::setFixedJsonField(item, "message", issue.message ? issue.message : "");
    }
}

void addCandidateClaims(JsonArray claims,
                         const FastBee::PreflightResult& result,
                         const char* candidateId) {
    for (size_t i = 0; i < result.claimCount; ++i) {
        const FastBee::ResourceClaim& claim = result.claims[i];
        if (!sameText(claim.ownerId, candidateId)) continue;
        JsonObject item = claims.add<JsonObject>();
        FastBee::setFixedJsonField(item, "resourceKind", FastBee::resourceKindName(claim.kind));
        FastBee::setFixedJsonField(item, "state", claimStateName(claim.state));
        FastBee::setFixedJsonField(item, "shareMode", shareModeName(claim.shareMode));
        FastBee::setFixedJsonField(item, "role", pinRoleName(claim.pinRole));
        FastBee::setFixedJsonField(item, "unit", claim.unit);
        FastBee::setFixedJsonField(item, "index", claim.index);
        FastBee::setFixedJsonField(item, "ownerId", claim.ownerId ? claim.ownerId : "");
        FastBee::setFixedJsonField(item, "parameterA", claim.parameterA);
        FastBee::setFixedJsonField(item, "parameterB", claim.parameterB);
        JsonArray pins = item["pins"].to<JsonArray>();
        for (uint8_t pinIndex = 0; pinIndex < claim.pinCount; ++pinIndex) {
            if (claim.pins[pinIndex] != FastBee::BOARD_PIN_NONE) {
                pins.add(claim.pins[pinIndex]);
            }
        }
    }
}

namespace {
const char* resolvedPinRole(const PeripheralConfig& config, uint8_t index) {
    switch (config.type) {
        case PeripheralType::UART: return index == 0 ? "RX" : "TX";
        case PeripheralType::I2C:
        case PeripheralType::LCD1602:
            return index == 0 ? "SDA" : "SCL";
        case PeripheralType::SPI:
            if (index == 0) return "MISO";
            if (index == 1) return "MOSI";
            if (index == 2) return "SCK";
            return "CS";
        case PeripheralType::SDIO:
            if (config.params.sdcard.interface == 1) {
                if (index == 0) return "SCK";
                if (index == 1) return "MOSI";
                if (index == 2) return "MISO";
                return "CS";
            }
            return "SDMMC";
        case PeripheralType::SENSOR_GENERIC:
            if (config.params.sensor.driver[0] != '\0') {
                return index == 0 ? "SDA" : "SCL";
            }
            if (config.params.sensor.sensorType == 3) {
                return index == 0 ? "TRIG" : "ECHO";
            }
            return "DATA";
        case PeripheralType::PWM_SERVO: return "PWM";
        case PeripheralType::STEPPER_MOTOR:
            if (index == 0) return "IN1";
            if (index == 1) return "IN2";
            if (index == 2) return "IN3";
            return "IN4";
        case PeripheralType::ENCODER: return index == 0 ? "A" : "B";
        case PeripheralType::SEVEN_SEGMENT_TM1637:
            return index == 0 ? "CLK" : "DIO";
        case PeripheralType::DS1302_RTC:
            if (index == 0) return "CE";
            if (index == 1) return "IO";
            return "SCLK";
        case PeripheralType::RFID_MFRC522:
            if (index == 0) return "SCK";
            if (index == 1) return "MOSI";
            if (index == 2) return "MISO";
            if (index == 3) return "SS";
            return "RST";
        default:
            return "GPIO";
    }
}
} // namespace

void addResolvedPins(JsonObject data, const PeripheralConfig& candidate) {
    JsonArray pins = data["resolvedPins"].to<JsonArray>();
    JsonArray roles = data["pinRoles"].to<JsonArray>();
    const uint8_t count = candidate.pinCount > 8 ? 8 : candidate.pinCount;
    for (uint8_t index = 0; index < count; ++index) {
        if (candidate.pins[index] == FastBee::BOARD_PIN_NONE) continue;
        pins.add(candidate.pins[index]);
        roles.add(resolvedPinRole(candidate, index));
    }
}

const char* preflightOperationName(FastBee::PreflightOperation operation) {
    switch (operation) {
        case FastBee::PreflightOperation::ADD: return "add";
        case FastBee::PreflightOperation::UPDATE: return "update";
        case FastBee::PreflightOperation::ENABLE: return "enable";
        default: return "unsupported";
    }
}

void resetPeripheralTypeDefaults(PeripheralConfig& config) {
    memset(&config.params, 0, sizeof(config.params));
    if (FastBee::PeripheralManagerConfigDetail::isAdcInputType(config.type)) {
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
    } else if (config.type == PeripheralType::SCREEN_DISPLAY) {
        initializeDisplayDefaults(config);
    }
}

bool buildPreflightCandidate(const PeripheralFormFields& form,
                             PeripheralManager& manager,
                             FastBee::PreflightOperation operation,
                             PeripheralConfig& candidate,
                             String& excludeId, String& errorMsg,
                             int& errorStatus) {
    bool resetDefaults = false;
    if (operation == FastBee::PreflightOperation::ADD) {
        candidate.id = form.value("id", "");
        candidate.name = form.value("name", "");
        candidate.type = static_cast<PeripheralType>(
            form.intValue("type", 0));
        candidate.enabled = form.boolValue("enabled", false);
        resetDefaults = true;
    } else {
        excludeId = form.value("id", "");
        if (excludeId.isEmpty()) {
            errorMsg = "Missing id parameter";
            errorStatus = 400;
            return false;
        }
        if (!manager.getPeripheralSnapshot(excludeId, candidate)) {
            errorMsg = "Peripheral not found";
            errorStatus = 404;
            return false;
        }
        if (candidate.type == PeripheralType::MODBUS_DEVICE) {
            errorMsg = userConfigurablePeripheralTypeError(candidate.type);
            errorStatus = 422;
            return false;
        }
        if (operation == FastBee::PreflightOperation::ENABLE) {
            candidate.enabled = true;
        } else {
            const String name = form.value("name", "");
            if (!name.isEmpty()) candidate.name = name;
            if (form.has("type")) {
                const PeripheralType newType = static_cast<PeripheralType>(
                    form.intValue("type",
                                     static_cast<int>(candidate.type)));
                if (newType != candidate.type) {
                    candidate.type = newType;
                    resetPeripheralTypeDefaults(candidate);
                    resetDefaults = true;
                }
            }
            candidate.enabled = form.boolValue(
                "enabled", candidate.enabled);
        }
    }

    const char* typeError = userConfigurablePeripheralTypeError(candidate.type);
    if (typeError) {
        errorMsg = typeError;
        errorStatus = 422;
        return false;
    }

    if (operation != FastBee::PreflightOperation::ENABLE) {
        const String pins = form.value("pins", "");
        const bool shouldParse = operation == FastBee::PreflightOperation::ADD ||
            form.has("pins");
        if (shouldParse &&
            !parsePeripheralPins(pins, candidate.pins, candidate.pinCount,
                                 errorMsg)) {
            errorStatus = 400;
            return false;
        }
        if (!applyPeripheralRequestParameters(form, candidate,
                                               resetDefaults, errorMsg)) {
            errorStatus = 400;
            return false;
        }
        if (candidate.type == PeripheralType::PWM_SERVO &&
            operation == FastBee::PreflightOperation::ADD) {
            candidate.enabled = false;
        }
    }

    if (candidate.id.isEmpty()) candidate.id = "__preflight_candidate";
    return true;
}

void addPreflightResponse(JsonDocument& doc,
                          const FastBee::BoardProfile& profile,
                          const FastBee::PreflightResult& result,
                          const PeripheralConfig& candidate,
                          FastBee::PreflightOperation operation,
                          bool success, bool allowed,
                          const char* message) {
    doc["success"] = success;
    doc["allowed"] = allowed;
    doc["requiresConfirmation"] = result.requiresConfirmation;
    doc["requiredAcknowledgements"] = requiredAcknowledgements(result);
    if (message) doc["message"] = message;
    JsonObject data = doc["data"].to<JsonObject>();
    FastBee::setFixedJsonField(data, "operation", preflightOperationName(operation));
    FastBee::setFixedJsonField(data, "profileId", profile.key);
    FastBee::setFixedJsonField(data, "allowed", allowed);
    FastBee::setFixedJsonField(data, "requiresConfirmation", result.requiresConfirmation);
    FastBee::setFixedJsonField(data, "requiredAcknowledgements", requiredAcknowledgements(result));
    FastBee::setFixedJsonField(data, "claimsTruncated", result.claimsTruncated);
    FastBee::setFixedJsonField(data, "issuesTruncated", result.issuesTruncated);
    addPreflightIssues(data["issues"].to<JsonArray>(), result);
    addCandidateClaims(data["claims"].to<JsonArray>(), result,
                        candidate.id.c_str());
    addResolvedPins(data, candidate);
    addResourceRows(data["resources"].to<JsonArray>(), profile, result);
}

String mcpGroupReviewToken(uint64_t version) {
    if (!version) return "";
    // An optimistic review token, not an authorization credential. Auth and
    // developer-mode admission remain mandatory on the mutation endpoint.
    static const uint32_t boot[] = {esp_random(), esp_random(), esp_random(), esp_random()};
    char value[49];
    snprintf(value, sizeof(value), "%08lx%08lx%08lx%08lx%08lx%08lx",
        static_cast<unsigned long>(boot[0]), static_cast<unsigned long>(boot[1]),
        static_cast<unsigned long>(boot[2]), static_cast<unsigned long>(boot[3]),
        static_cast<unsigned long>(version >> 32), static_cast<unsigned long>(version & UINT32_MAX));
    return String(value);
}

bool buildMcpGroupPreview(PeripheralManager& manager, const String& id,
                          JsonDocument& doc, String& error) {
    RecursiveMutexGuard lock(manager.getMutex(), pdMS_TO_TICKS(50));
    if (!lock.isLocked()) { error = "IO_GROUP_BUSY: configuration unavailable"; return false; }
    PeripheralConfig parent;
    if (!manager.getPeripheralSnapshot(id, parent) || parent.type != PeripheralType::IO_EXPANDER ||
        strcasecmp(parent.params.ioExpander.driver, "MCP23017") != 0 || parent.enabled) {
        error = "IO_GROUP_INVALID: select a disabled MCP provider"; return false;
    }
    const String token = mcpGroupReviewToken(manager.getConfigurationReviewVersion());
    if (token.length() != 48) { error = "IO_GROUP_BUSY: review identity unavailable"; return false; }
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();
    FastBee::setFixedJsonField(data, "id", id);
    FastBee::setFixedJsonField(data, "groupToken", token);
    auto describe = [](JsonObject object, const PeripheralConfig& config) {
        FastBee::setFixedJsonField(object, "id", config.id);
        FastBee::setFixedJsonField(object, "name", config.name);
        FastBee::setFixedJsonField(object, "type", static_cast<int>(config.type));
        FastBee::setFixedJsonField(object, "enabled", config.enabled);
        addGenericPeripheralFields(object, config);
        JsonArray pins = object["pins"].to<JsonArray>();
        for (uint8_t i = 0; i < config.pinCount && i < 8; ++i) pins.add(config.pins[i]);
        if (config.type == PeripheralType::I2C) {
            FastBee::setFixedJsonField(object, "frequency", config.params.i2c.frequency ? config.params.i2c.frequency : 100000);
        }
    };
    describe(data["provider"].to<JsonObject>(), parent);
    PeripheralConfig dependency;
    if (manager.getPeripheralSnapshot(parent.params.ioExpander.resetPeripheralId, dependency))
        describe(data["reset"].to<JsonObject>(), dependency);
    if (parent.params.ioExpander.busPeripheralId[0] &&
        manager.getPeripheralSnapshot(parent.params.ioExpander.busPeripheralId, dependency))
        describe(data["bus"].to<JsonObject>(), dependency);
    JsonArray channels = data["channels"].to<JsonArray>();
    size_t count = 0;
    manager.forEachPeripheral([&](const PeripheralConfig& child) {
        if (!child.isExpanderChannel() ||
            strncmp(child.params.gpio.providerId, id.c_str(), sizeof(child.params.gpio.providerId)) != 0) return;
        if (++count <= 16) describe(channels.add<JsonObject>(), child);
    });
    if (count == 0 || count > 16 || doc.overflowed()) {
        error = "IO_GROUP_INVALID: group incomplete or preview capacity exceeded"; return false;
    }
    return true;
}

} // namespace FastBee
