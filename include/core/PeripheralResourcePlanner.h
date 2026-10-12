#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

#include "BoardProfile.h"

struct PeripheralConfig;

namespace FastBee {

constexpr size_t PREFLIGHT_MAX_CLAIMS = 160;
constexpr size_t PREFLIGHT_MAX_ISSUES = 48;

enum class PreflightOperation : uint8_t {
    ADD = 0,
    UPDATE,
    ENABLE,
    DISABLE,
    IMPORT,
    TEMPLATE_APPLY,
    REMOVE
};

enum class ClaimState : uint8_t {
    DORMANT = 0,
    ACTIVE = 1
};

enum class ResourceKind : uint8_t {
    GPIO = 0,
    I2C_BUS,
    I2C_ADDRESS,
    SPI_BUS,
    SPI_CS,
    UART_PORT,
    LEDC_CHANNEL,
    LEDC_TIMER,
    RMT_CHANNEL,
    ADC_CHANNEL,
    ENCODER_SLOT,
    SDMMC_HOST,
    IO_CHANNEL,
    CONTROL_OUTPUT
};

enum class ResourceShareMode : uint8_t {
    EXCLUSIVE = 0,
    SHARED_IF_COMPATIBLE = 1
};

enum class PinRole : uint8_t {
    GENERIC = 0,
    INPUT_SIGNAL,
    OUTPUT_SIGNAL,
    I2C_SDA,
    I2C_SCL,
    SPI_MISO,
    SPI_MOSI,
    SPI_SCK,
    SPI_CS,
    UART_RX,
    UART_TX,
    ADC_INPUT,
    PWM_OUTPUT,
    RMT_SIGNAL,
    ENCODER_A,
    ENCODER_B,
    SDMMC_SIGNAL
};

enum class PreflightSeverity : uint8_t {
    INFO = 0,
    WARNING = 1,
    WARNING_REQUIRES_CONFIRMATION = 2,
    ERROR = 3
};

enum class PreflightIssueCode : uint8_t {
    NONE = 0,
    PIN_REQUIRED,
    PIN_DUPLICATED,
    PIN_UNKNOWN,
    PIN_HARD_FORBIDDEN,
    PIN_INPUT_UNSUPPORTED,
    PIN_OUTPUT_UNSUPPORTED,
    PIN_PULL_UNSUPPORTED,
    PIN_ADC_UNSUPPORTED,
    PIN_STRAPPING_ACK_REQUIRED,
    PIN_BOARD_RESOURCE_ACK_REQUIRED,
    PIN_UART0_ACTIVE,
    PIN_USB_SERIAL_JTAG_ACTIVE,
    PIN_JTAG_ACTIVE,
    ADC2_WIFI_ACK_REQUIRED,
    GPIO_CONFLICT,
    DORMANT_GPIO_CONFLICT,
    I2C_BUS_UNAVAILABLE,
    I2C_BUS_PINS_MISMATCH,
    I2C_ADDRESS_CONFLICT,
    SPI_BUS_UNAVAILABLE,
    SPI_BUS_PINS_MISMATCH,
    SPI_CS_CONFLICT,
    UART_PORT_EXHAUSTED,
    LEDC_CHANNEL_INVALID,
    LEDC_CHANNEL_CONFLICT,
    LEDC_TIMER_CONFLICT,
    RMT_CHANNEL_EXHAUSTED,
    ADC_CHANNEL_CONFLICT,
    ENCODER_SLOT_EXHAUSTED,
    SDMMC_UNSUPPORTED,
    SDMMC_HOST_CONFLICT,
    DORMANT_RESOURCE_CONFLICT,
    CLAIM_CAPACITY_EXCEEDED,
    ISSUE_CAPACITY_EXCEEDED,
    RESOURCE_SHARED,
    DISPLAY_CONTROL_INVALID,
    I2C_REFERENCE_INVALID,
    IO_REFERENCE_INVALID,
    IO_CHANNEL_CONFLICT,
    CONTROL_REFERENCE_INVALID,
    CONTROL_OUTPUT_CONFLICT
};

enum PreflightAcknowledgement : uint32_t {
    ACK_NONE = 0,
    ACK_STRAPPING_PIN = 1UL << 0,
    ACK_BOARD_SHARED_PIN = 1UL << 1,
    ACK_UART0_CONSOLE = 1UL << 2,
    ACK_ADC2_WITH_WIFI = 1UL << 3
};

// Configuration intent only; this is not a snapshot of a running driver.
struct EthernetResourceConfig {
    bool enabled = false;
    int8_t miso = -1;
    int8_t mosi = -1;
    int8_t sck = -1;
    int8_t cs = -1;
    int8_t rst = -1;
    int8_t irq = -1;
};

struct CellularResourceConfig {
    bool enabled = false;
    int8_t rx = -1;
    int8_t tx = -1;
    int8_t power = -1;
};

struct PreflightContext {
    bool wifiActive = true;
    bool uart0ConsoleActive = true;
    bool usbSerialJtagActive = true;
    bool jtagActive = false;
    uint32_t acknowledgements = ACK_NONE;
    EthernetResourceConfig ethernet;
    CellularResourceConfig cellular;
};

// ownerId is borrowed from PeripheralConfig::id (or from the caller for an
// explicit service claim). It must remain valid until the result is consumed.
struct ResourceClaim {
    ResourceKind kind = ResourceKind::GPIO;
    ClaimState state = ClaimState::DORMANT;
    ResourceShareMode shareMode = ResourceShareMode::EXCLUSIVE;
    PinRole pinRole = PinRole::GENERIC;
    uint8_t unit = 0;
    uint8_t index = 0;
    uint8_t pinCount = 0;
    uint8_t pins[4] = {BOARD_PIN_NONE, BOARD_PIN_NONE, BOARD_PIN_NONE, BOARD_PIN_NONE};
    uint32_t parameterA = 0;
    uint32_t parameterB = 0;
    const char* ownerId = nullptr;
};

struct PreflightIssue {
    PreflightSeverity severity = PreflightSeverity::INFO;
    PreflightIssueCode code = PreflightIssueCode::NONE;
    ResourceKind resourceKind = ResourceKind::GPIO;
    uint8_t unit = 0;
    uint8_t index = 0;
    const char* ownerId = nullptr;
    const char* conflictingOwnerId = nullptr;
    const char* message = nullptr;
};

struct PreflightResult {
    bool allowed = true;
    bool requiresConfirmation = false;
    bool claimsTruncated = false;
    bool issuesTruncated = false;
    size_t claimCount = 0;
    size_t issueCount = 0;
    ResourceClaim claims[PREFLIGHT_MAX_CLAIMS];
    PreflightIssue issues[PREFLIGHT_MAX_ISSUES];

    void reset();
    void resetIssuesPreservingClaims();
    bool hasIssue(PreflightIssueCode code) const;
};

class PeripheralResourcePlanner {
public:
    PeripheralResourcePlanner(const BoardProfile& profile,
                              const PreflightContext& context,
                              PreflightResult& result,
                              const PeripheralConfig* existing = nullptr,
                              size_t existingCount = 0);

    bool addPeripheral(const PeripheralConfig& config, ClaimState state,
                       const char* ownerId = nullptr);

    // Allows runtime services (logging, protocol stacks, or future drivers) to
    // participate in the same graph without manufacturing a PeripheralConfig.
    bool addClaim(const ResourceClaim& claim);
    bool addEthernet(const EthernetResourceConfig& config);
    bool addCellular(const CellularResourceConfig& config);

private:
    const BoardProfile& _profile;
    const PreflightContext _context;
    PreflightResult& _result;
    const PeripheralConfig* _existing;
    size_t _existingCount;

    void addIssue(PreflightSeverity severity, PreflightIssueCode code,
                  ResourceKind kind, uint8_t unit, uint8_t index,
                  const char* ownerId, const char* conflictingOwnerId,
                  const char* message);
    void validatePin(uint8_t gpio, uint16_t requiredCapabilities,
                     const char* ownerId);
    void validatePinRisks(const BoardPinProfile& pin,
                          uint16_t requiredCapabilities,
                          const char* ownerId);
    void addGpioClaim(uint8_t gpio, PinRole role, ClaimState state,
                      const char* ownerId, ResourceShareMode shareMode,
                      ResourceKind shareDomain = ResourceKind::GPIO,
                      uint8_t shareUnit = 0);
    bool allocateFirstFree(ResourceKind kind, uint8_t firstUnit,
                           uint8_t count, ClaimState state,
                           const char* ownerId, PinRole role = PinRole::GENERIC);
    bool claimConflicts(const ResourceClaim& existing,
                        const ResourceClaim& candidate,
                        PreflightIssueCode& code,
                        bool& compatibleShare) const;
    bool claimIdentityMatches(const ResourceClaim& left,
                              const ResourceClaim& right) const;
};

ClaimState candidateClaimState(const PeripheralConfig& candidate,
                               PreflightOperation operation);

void preflightPeripheralChange(const BoardProfile& profile,
                               const PeripheralConfig* existing,
                               size_t existingCount,
                               const PeripheralConfig& candidate,
                               PreflightOperation operation,
                               const char* excludeId,
                               const PreflightContext& context,
                               PreflightResult& result);

inline void preflightPeripheralChange(const BoardProfile& profile,
                                      const PeripheralConfig* existing,
                                      size_t existingCount,
                                      const PeripheralConfig& candidate,
                                      PreflightOperation operation,
                                      const String& excludeId,
                                      const PreflightContext& context,
                                      PreflightResult& result) {
    preflightPeripheralChange(profile, existing, existingCount, candidate,
                              operation, excludeId.c_str(), context, result);
}

const char* preflightIssueCodeName(PreflightIssueCode code);
const char* preflightSeverityName(PreflightSeverity severity);
const char* resourceKindName(ResourceKind kind);

}  // namespace FastBee
