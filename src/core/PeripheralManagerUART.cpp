#include "core/PeripheralManager.h"
#include "core/AsyncExecTypes.h"
#include "core/ChipConfig.h"
#include "core/UartPortPolicy.h"
#include "core/SensorConfigEpoch.h"
#include "PeripheralManagerUartSupport.h"
#include "systems/LoggerSystem.h"
#include <esp_arduino_version.h>

namespace FastBee { namespace PeripheralUart {
HardwareSerial* serialForPort(uint8_t port) {
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
    // ESP32-S3: Serial is HWCDC (USB), Serial0 is UART0
    if (port == 0) return &Serial0;
#else
    if (port == 0) return &Serial;
#endif
#if CHIP_UART_COUNT >= 2
    if (port == 1) return &Serial1;
#endif
#if CHIP_UART_COUNT >= 3
    if (port == 2) return &Serial2;
#endif
    return nullptr;
}

uint16_t boundedUartAvailable(HardwareSerial* serial) {
    if (!serial) return 0;
    const int available = serial->available();
    if (available <= 0) return 0;
    return available > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(available);
}
} }

namespace {
bool installUartReceiveFaultObserver(HardwareSerial* serial, uint8_t port, uint32_t generation) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    if (!serial || !port || port >= 3 || !generation) return false;
    serial->onReceiveError([port, generation](hardwareSerial_error_t error) {
        using Fault = FastBee::UartReceiveFault;
        switch (error) {
            case UART_FIFO_OVF_ERROR: Fault::observe(port, generation, Fault::Error::FifoOverflow); break;
            case UART_BUFFER_FULL_ERROR: Fault::observe(port, generation, Fault::Error::BufferFull); break;
            case UART_FRAME_ERROR: Fault::observe(port, generation, Fault::Error::Frame); break;
            case UART_PARITY_ERROR: Fault::observe(port, generation, Fault::Error::Parity); break;
            case UART_BREAK_ERROR: Fault::observe(port, generation, Fault::Error::Break); break;
            default: break;
        }
    });
    // The installed Core API returns void and exposes no event-task creation
    // result. This confirms callback registration only, never observer readiness.
    return true;
#else
    (void)serial; (void)port; (void)generation;
    return false; // Unverified SDKs must not pretend to monitor hardware errors.
#endif
}
constexpr uint8_t UART_PORT_UNASSIGNED = 255;
uint32_t uartSerialConfig(const PeripheralConfig& config) {
    const uint8_t dataBits = config.params.uart.dataBits;
    const uint8_t parity = config.params.uart.parity;
    const bool twoStopBits = config.params.uart.stopBits == 2;

    switch (dataBits) {
        case 5:
            if (parity == 1) return twoStopBits ? SERIAL_5O2 : SERIAL_5O1;
            if (parity == 2) return twoStopBits ? SERIAL_5E2 : SERIAL_5E1;
            return twoStopBits ? SERIAL_5N2 : SERIAL_5N1;
        case 6:
            if (parity == 1) return twoStopBits ? SERIAL_6O2 : SERIAL_6O1;
            if (parity == 2) return twoStopBits ? SERIAL_6E2 : SERIAL_6E1;
            return twoStopBits ? SERIAL_6N2 : SERIAL_6N1;
        case 7:
            if (parity == 1) return twoStopBits ? SERIAL_7O2 : SERIAL_7O1;
            if (parity == 2) return twoStopBits ? SERIAL_7E2 : SERIAL_7E1;
            return twoStopBits ? SERIAL_7N2 : SERIAL_7N1;
        case 8:
        default:
            if (parity == 1) return twoStopBits ? SERIAL_8O2 : SERIAL_8O1;
            if (parity == 2) return twoStopBits ? SERIAL_8E2 : SERIAL_8E1;
            return twoStopBits ? SERIAL_8N2 : SERIAL_8N1;
    }
}

bool isProtocolOwnedUartId(const String& id) {
    return FastBee::isProtocolUartId(id.c_str());
}
}
using FastBee::PeripheralUart::serialForPort;
using FastBee::PeripheralUart::boundedUartAvailable;

// Called under the existing PeripheralManager lock; lease and I/O semantics
// are unchanged from the monolithic manager implementation.
bool PeripheralManager::validateUartCapacity(const PeripheralConfig& candidate,
                                             const String& excludeId,
                                             String& errorMsg) const {
    if (candidate.type != PeripheralType::UART || !candidate.enabled ||
        isProtocolOwnedUartId(candidate.id)) {
        return true;
    }

#if CHIP_UART_COUNT <= 1
    errorMsg = "UART_RESOURCE_EXHAUSTED: UART0 reserved for logging/USB; no generic UART port available";
    return false;
#else
    size_t enabledGenericUarts = 0;
    for (const auto& pair : peripherals) {
        if (!excludeId.isEmpty() && pair.first == excludeId) continue;
        const PeripheralConfig& existing = pair.second;
        if (existing.enabled && existing.type == PeripheralType::UART &&
            !isProtocolOwnedUartId(existing.id)) {
            ++enabledGenericUarts;
        }
    }

    bool protocolConfigured = false;
    for (const auto& pair : peripherals) {
        if (pair.second.enabled && pair.second.type == PeripheralType::UART &&
            isProtocolOwnedUartId(pair.second.id)) protocolConfigured = true;
    }
    size_t genericCapacity = static_cast<size_t>(CHIP_UART_COUNT - 1);
    for (uint8_t port = 1; port < CHIP_UART_COUNT && port < FastBee::UartPortLease::PORT_COUNT; ++port) {
        if ((protocolConfigured && port == FastBee::serviceUartPort(CHIP_UART_COUNT)) ||
            (!FastBee::UartPortLease::available(port) && !uartPortLeases[port].owned())) --genericCapacity;
    }
    if (enabledGenericUarts >= genericCapacity) {
        errorMsg = String("UART_RESOURCE_EXHAUSTED: all ") + String(genericCapacity) +
                   " generic UART port(s) are in use; UART0 is reserved for logging/USB";
        return false;
    }
    return true;
#endif
}

HardwareSerial* PeripheralManager::getUartSerial(const String& id) {
    auto it = uartPortById.find(id);
    if (it == uartPortById.end()) return nullptr;
    if (it->second >= FastBee::UartPortLease::PORT_COUNT || !uartPortLeases[it->second].owned()) return nullptr;
    return serialForPort(it->second);
}

bool PeripheralManager::isUartEventOriginCurrent(const FastBee::UartEventOrigin& origin) const {
    if (origin.unbound()) return true;
    if (!origin.required || !origin.configEpoch || !origin.receiveGeneration) return false;
    RecursiveMutexGuard lock(_mutex, 0);
    if (!lock.isLocked()) return false;
    const uint32_t epoch = FastBee::SensorConfigEpoch::current();
    // No callbacks or I/O under this lock. The allocator never reuses a receive
    // generation, even when a controller changes owners with the same config.
    for (const auto& entry : uartPortById) {
        const uint8_t port = entry.second;
        if (port > 0 && port < FastBee::UartPortLease::PORT_COUNT &&
            uartPortLeases[port].owned() &&
            origin.matches(epoch, uartReceiveGenerations.current(port)) &&
            FastBee::UartReceiveFault::healthy(port, origin.receiveGeneration)) return true;
    }
    return false;
}

uint32_t PeripheralManager::getUartReceiveGeneration(const String& id) const {
    RecursiveMutexGuard guard(_mutex, 0);
    if (!guard.isLocked()) return 0;
    const auto it = uartPortById.find(id);
    if (it == uartPortById.end() || it->second >= FastBee::UartPortLease::PORT_COUNT ||
        !uartPortLeases[it->second].owned()) return 0;
    const uint32_t generation = uartReceiveGenerations.current(it->second);
    return FastBee::UartReceiveFault::healthy(it->second, generation) ? generation : 0;
}

bool PeripheralManager::setupUartHardware(const PeripheralConfig& config) {
    if (config.pinCount < 2) {
        LOG_WARNINGF("Peripheral Manager: UART '%s' requires RX/TX pins", config.id.c_str());
        return false;
    }

    if (isProtocolOwnedUartId(config.id)) {
        LOG_INFOF("Peripheral Manager: UART '%s' reserved for protocol stack, skip generic serial init",
                  config.id.c_str());
        auto stateIt = runtimeStates.find(config.id);
        if (stateIt != runtimeStates.end()) {
            stateIt->second.state.comm.isConnected = false;
            stateIt->second.state.comm.port = UART_PORT_UNASSIGNED;
            stateIt->second.state.comm.rxAvailable = 0;
        }
        return true;
    }

    String capacityError;
    if (!validateUartCapacity(config, config.id, capacityError)) {
        lastEnableError = capacityError;
        LOG_WARNINGF("Peripheral Manager: %s", capacityError.c_str());
        return false;
    }

    uint8_t rxPin = config.pins[0];
    uint8_t txPin = config.pins[1];
    uint8_t port = UART_PORT_UNASSIGNED;

    auto existing = uartPortById.find(config.id);
    if (existing != uartPortById.end()) {
        port = existing->second;
        if (port >= FastBee::UartPortLease::PORT_COUNT || !uartPortLeases[port].owned()) {
            lastEnableError = "UART_LEASE_LOST: refusing to reinitialize another owner";
            return false;
        }
    } else {
        bool used[CHIP_UART_COUNT] = {false};
        used[0] = true; // UART0 is exclusively reserved for logging/USB.
        // Configuration intent reserves the protocol's fixed controller even
        // before Modbus starts. Runtime leases still arbitrate actual owners.
        for (const auto& pair : peripherals) {
            if (pair.second.enabled && pair.second.type == PeripheralType::UART &&
                isProtocolOwnedUartId(pair.second.id)) {
                const uint8_t reserved = FastBee::serviceUartPort(CHIP_UART_COUNT);
                if (reserved < CHIP_UART_COUNT) used[reserved] = true;
            }
        }
        for (const auto& pair : uartPortById) {
            if (pair.second < CHIP_UART_COUNT) used[pair.second] = true;
        }
        for (uint8_t i = 1; i < CHIP_UART_COUNT && i < FastBee::UartPortLease::PORT_COUNT; i++) {
            if (!used[i] && uartPortLeases[i].acquire(i, FastBee::ResourceConsumerRole::Peripheral,
                                                    config.id.c_str())) {
                port = i;
                break;
            }
        }
    }

    HardwareSerial* serial = serialForPort(port);
    if (!serial) {
        if (port < FastBee::UartPortLease::PORT_COUNT && uartPortLeases[port].owned())
            uartPortLeases[port].releaseAfterStop();
        lastEnableError = "UART_RESOURCE_BUSY: controller is owned by another service";
        LOG_WARNINGF("Peripheral Manager: No free UART port for '%s'", config.id.c_str());
        return false;
    }

    // Every actual begin is a new receive stream, including re-beginning an
    // already owned controller without a configuration epoch change.
    if (!uartReceiveGenerations.begin(port)) {
        if (existing == uartPortById.end()) uartPortLeases[port].releaseAfterStop();
        lastEnableError = "UART_RECEIVE_GENERATION_EXHAUSTED";
        return false;
    }
    const uint32_t generation = uartReceiveGenerations.current(port);
    // Even same-config begin must remove old driver bytes/events and callbacks.
    // The controller lease remains held throughout this explicit restart.
    if (existing != uartPortById.end()) serial->end();
    FastBee::UartReceiveFault::arm(port, generation);
    if (!installUartReceiveFaultObserver(serial, port, generation)) {
        FastBee::UartReceiveFault::disarm(port, generation);
        uartReceiveGenerations.end(port);
        serial->end();
        uartPortLeases[port].releaseAfterStop();
        uartPortById.erase(config.id);
        lastEnableError = "UART_RX_ERROR_OBSERVER_UNSUPPORTED";
        return false;
    }
    uartPortById[config.id] = port;
    uint32_t baud = config.params.uart.baudRate > 0
        ? config.params.uart.baudRate : UART_DEFAULT_BAUD_RATE;
    serial->begin(baud, uartSerialConfig(config), rxPin, txPin);
    if (!*serial) {
        FastBee::UartReceiveFault::disarm(port, generation);
        serial->end();
        uartReceiveGenerations.end(port);
        uartPortLeases[port].releaseAfterStop();
        uartPortById.erase(config.id);
        lastEnableError = "UART_DRIVER_INIT_FAILED";
        return false;
    }
    serial->setTimeout(0);
    auto stateIt = runtimeStates.find(config.id);
    if (stateIt != runtimeStates.end()) {
        stateIt->second.state.comm.bytesSent = 0;
        stateIt->second.state.comm.bytesReceived = 0;
        stateIt->second.state.comm.isConnected = true;
        stateIt->second.state.comm.port = port;
        stateIt->second.state.comm.rxAvailable = boundedUartAvailable(serial);
    }
    LOG_INFOF("Peripheral Manager: UART '%s' initialized on Serial%d RX=%d TX=%d baud=%lu framing=%s",
              config.id.c_str(), port, rxPin, txPin, (unsigned long)baud,
              uartFramingName(config).c_str());
    return true;
}
