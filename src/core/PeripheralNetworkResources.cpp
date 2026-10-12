#include "core/PeripheralResourcePlanner.h"
#include "network/CellularUartPort.h"

namespace FastBee {

bool PeripheralResourcePlanner::addEthernet(const EthernetResourceConfig& config) {
    if (!config.enabled) return _result.allowed;
    constexpr const char* owner = "runtime:ethernet";
    // Mirrors ChipConfig's classic HSPI versus shared global SPI policy.
    const bool dedicated = _profile.id == BoardProfileId::PUZHONG_PZ_ESP32;
    const uint8_t host = dedicated ? 1 : 0;
    if (_profile.resources.spiBusCount == 0) {
        addIssue(PreflightSeverity::ERROR, PreflightIssueCode::SPI_BUS_UNAVAILABLE,
                 ResourceKind::SPI_BUS, host, 0, owner, nullptr,
                 "Ethernet requires an available SPI controller");
        return false;
    }
    const int8_t pins[] = {config.miso, config.mosi, config.sck,
                           config.cs, config.rst, config.irq};
    const PinRole roles[] = {PinRole::SPI_MISO, PinRole::SPI_MOSI, PinRole::SPI_SCK,
                             PinRole::SPI_CS, PinRole::OUTPUT_SIGNAL, PinRole::INPUT_SIGNAL};
    for (uint8_t i = 0; i < 6; ++i) {
        if (pins[i] < 0) {
            if (i < 4 || pins[i] != -1) {
                addIssue(PreflightSeverity::ERROR, PreflightIssueCode::PIN_REQUIRED,
                         ResourceKind::GPIO, 0, i, owner, nullptr,
                         "Ethernet requires MISO/MOSI/SCK/CS; optional pins use -1");
            }
            continue;
        }
        const uint8_t pin = static_cast<uint8_t>(pins[i]);
        for (uint8_t j = 0; j < i; ++j) {
            if (pins[j] == pins[i]) {
                addIssue(PreflightSeverity::ERROR, PreflightIssueCode::PIN_DUPLICATED,
                         ResourceKind::GPIO, 0, pin, owner, owner,
                         "Ethernet signals cannot share the same GPIO");
            }
        }
        validatePin(pin, (i == 0 || i == 5) ? PIN_CAP_INPUT : PIN_CAP_OUTPUT, owner);
        const bool sharedSignal = i < 3 && !dedicated;
        addGpioClaim(pin, roles[i], ClaimState::ACTIVE, owner,
                     sharedSignal ? ResourceShareMode::SHARED_IF_COMPATIBLE
                                  : ResourceShareMode::EXCLUSIVE,
                     i < 3 ? ResourceKind::SPI_BUS : ResourceKind::GPIO,
                     i < 3 ? host : 0);
    }
    ResourceClaim bus;
    bus.kind = ResourceKind::SPI_BUS;
    bus.state = ClaimState::ACTIVE;
    bus.shareMode = ResourceShareMode::SHARED_IF_COMPATIBLE;
    bus.unit = host;
    bus.pinCount = 3;
    bus.pins[0] = config.miso < 0 ? BOARD_PIN_NONE : static_cast<uint8_t>(config.miso);
    bus.pins[1] = config.mosi < 0 ? BOARD_PIN_NONE : static_cast<uint8_t>(config.mosi);
    bus.pins[2] = config.sck < 0 ? BOARD_PIN_NONE : static_cast<uint8_t>(config.sck);
    bus.ownerId = owner;
    addClaim(bus);
    if (config.cs >= 0) {
        ResourceClaim cs;
        cs.kind = ResourceKind::SPI_CS;
        cs.state = ClaimState::ACTIVE;
        cs.unit = host;
        cs.index = static_cast<uint8_t>(config.cs);
        cs.pinRole = PinRole::SPI_CS;
        cs.ownerId = owner;
        addClaim(cs);
    }
    return _result.allowed;
}

bool PeripheralResourcePlanner::addCellular(const CellularResourceConfig& config) {
    if (!config.enabled) return _result.allowed;
    constexpr const char* owner = "runtime:cellular";
    const uint8_t port = cellularUartPort(
        _profile.resources.firstGenericUartPort + _profile.resources.genericUartCount);
    if (!_profile.resources.genericUartCount || port == UINT8_MAX ||
        port < _profile.resources.firstGenericUartPort) {
        addIssue(PreflightSeverity::ERROR, PreflightIssueCode::UART_PORT_EXHAUSTED,
                 ResourceKind::UART_PORT, port, 0, owner, nullptr,
                 "Cellular requires a non-console UART controller");
    } else {
        ResourceClaim uart;
        uart.kind = ResourceKind::UART_PORT;
        uart.state = ClaimState::ACTIVE;
        uart.unit = port;
        uart.ownerId = owner;
        addClaim(uart);
    }
    const int8_t pins[] = {config.rx, config.tx, config.power};
    const PinRole roles[] = {PinRole::UART_RX, PinRole::UART_TX, PinRole::OUTPUT_SIGNAL};
    for (uint8_t i = 0; i < 3; ++i) {
        if (pins[i] < 0) {
            if (i < 2 || pins[i] != -1) {
                addIssue(PreflightSeverity::ERROR, PreflightIssueCode::PIN_REQUIRED,
                         ResourceKind::GPIO, 0, i, owner, nullptr,
                         "Cellular requires RX/TX; optional power pin uses -1");
            }
            continue;
        }
        const uint8_t pin = static_cast<uint8_t>(pins[i]);
        for (uint8_t j = 0; j < i; ++j) {
            if (pins[j] == pins[i]) {
                addIssue(PreflightSeverity::ERROR, PreflightIssueCode::PIN_DUPLICATED,
                         ResourceKind::GPIO, 0, pin, owner, owner,
                         "Cellular signals cannot share the same GPIO");
            }
        }
        validatePin(pin, i == 0 ? PIN_CAP_INPUT : PIN_CAP_OUTPUT, owner);
        addGpioClaim(pin, roles[i], ClaimState::ACTIVE, owner, ResourceShareMode::EXCLUSIVE);
    }
    return _result.allowed;
}

} // namespace FastBee
