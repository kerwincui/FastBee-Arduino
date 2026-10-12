/** W5500 lifecycle on the project's pinned Arduino core with checked ETH handles. */
#include "network/EthernetAdapter.h"

#if FASTBEE_ENABLE_ETHERNET
#include "core/ChipConfig.h"
#include "core/SpiBusInitializer.h"
#include "core/FeatureInstanceRegistry.h"
#include "systems/LoggerSystem.h"
#include <driver/gpio.h>
#include <new>

namespace {
// This transport/token outlives adapters: failed cleanup cannot dangle a lease.
struct EthernetTransport {
    SPIClass* spi = nullptr;
    bool leased = false;
    bool attempted = false;
    wifi_event_id_t eventId = 0;
    std::atomic<bool> acceptingEvents{false};
    std::atomic<bool> initialized{false};
    EthernetConfig pins;
    String addressKey;
} transport;

#if defined(ETH_SPI_SUPPORTS_CUSTOM) && ETH_SPI_SUPPORTS_CUSTOM
bool samePins(const EthernetConfig& a, const EthernetConfig& b) {
    return a.spiSck == b.spiSck && a.spiMiso == b.spiMiso && a.spiMosi == b.spiMosi &&
           a.csPin == b.csPin && a.rstPin == b.rstPin && a.intPin == b.intPin;
}

bool validatePins(const EthernetConfig& pins, String& error) {
    const int8_t values[]{pins.spiSck, pins.spiMiso, pins.spiMosi, pins.csPin, pins.rstPin, pins.intPin};
    for (size_t i = 0; i < 6; ++i) {
        if (i >= 4 && values[i] == -1) continue;
        const bool output = i == 0 || i == 2 || i == 3 || i == 4;
        if (values[i] < 0 || !GPIO_IS_VALID_GPIO(values[i]) ||
            chipIsAbsoluteForbiddenPin(static_cast<uint8_t>(values[i])) ||
            (output && !GPIO_IS_VALID_OUTPUT_GPIO(values[i]))) {
            error = "ETH_PINS_INVALID: invalid or reserved GPIO"; return false;
        }
        for (size_t j = 0; j < i; ++j) {
            if (values[j] == values[i]) {
                error = "ETH_PINS_INVALID: signal/control pins overlap"; return false;
            }
        }
    }
    return true;
}

// Caller holds SpiBusOperation. ETH workers use the SPIClass transaction lock,
// not this outer lifecycle lock, so stopping them does not invert its order.
bool stopTransport(String& error) {
    transport.acceptingEvents = false;
    transport.initialized = false;
    if (transport.eventId) {
        WiFi.removeEvent(transport.eventId);
        transport.eventId = 0;
    }
    if (transport.attempted) {
        ETH.end();
        // end() is void and may return early. A delay does not prove cleanup.
        if (ETH.handle() != nullptr || ETH.netif() != nullptr) {
            error = "ETH_STOP_PENDING: driver resources retained; retry stop"; return false;
        }
        transport.attempted = false;
    }
#if CHIP_ETHERNET_DEDICATED_SPI
    if (transport.spi) {
        transport.spi->end();
        delete transport.spi;
    }
#endif
    transport.spi = nullptr;
    if (transport.leased && !FastBee::SpiBusInitializer::instance().releaseNetwork(&transport, error)) return false;
    transport.leased = false;
    transport.addressKey = "";
    error = "";
    return true;
}
#endif
} // namespace

std::atomic<EthernetAdapter*> EthernetAdapter::_instance{nullptr};
EthernetAdapter::EthernetAdapter() {
    static const FastBee::FeatureInstanceObserver observer{"ethernet", [](const void*) {
        using namespace FastBee;
        SpiBusObservation guard;
        if (guard.unused()) return FeatureInstanceSnapshot{FeatureInstanceState::NotInitialized, 0};
        if (!guard.locked()) return FeatureInstanceSnapshot{FeatureInstanceState::Busy, 0};
        if (transport.initialized && transport.spi && transport.spi->bus())
            return FeatureInstanceSnapshot{FeatureInstanceState::Initialized, 1};
        return FeatureInstanceSnapshot{transport.leased || transport.attempted
            ? FeatureInstanceState::CleanupPending : FeatureInstanceState::NotInitialized, 0};
    }, nullptr};
    static const bool registered = FastBee::FeatureInstanceRegistry::instance().registerObserver(&observer);
    (void)registered;
}
EthernetAdapter::~EthernetAdapter() {
    disconnect();
    auto* expected = this;
    _instance.compare_exchange_strong(expected, nullptr);
    // Failed cleanup stays in the static transport, never in a freed adapter.
}

bool EthernetAdapter::begin(const WiFiConfig& config) {
#if !defined(ETH_SPI_SUPPORTS_CUSTOM) || !ETH_SPI_SUPPORTS_CUSTOM
    (void)config;
    _lastError = "ETH_CORE_UNSUPPORTED: use the project's pinned custom-SPI Arduino core";
    LOG_ERROR(_lastError);
    return false;
#else
    // ETH workers transact outside SpiBusOperation, so the underlying Arduino
    // SPIClass transaction mutex is required even on a dedicated controller.
#if CONFIG_DISABLE_HAL_LOCKS
    _lastError = "ETH_LOCKS_DISABLED: Arduino SPI transaction locks are required";
    return false;
#endif
    FastBee::SpiBusOperation operation;
    if (!operation.locked()) { _lastError = "SPI_BUSY: Ethernet startup deferred"; return false; }
    auto* owner = _instance.load();
    if (owner && owner != this) { _lastError = "ETH_IN_USE: another adapter owns Ethernet"; return false; }
    if (!validatePins(config.ethernet, _lastError)) return false;
    IPAddress ip, gateway, subnet, dns1, dns2;
    const bool staticAddress = config.ipConfigType == IPConfigType::STATIC;
    if (config.ipConfigType != IPConfigType::DHCP && !staticAddress) {
        _lastError = "ETH_IP_INVALID: unknown address mode"; return false;
    }
    if (staticAddress && (!ip.fromString(config.staticIP) || !gateway.fromString(config.gateway) ||
        !subnet.fromString(config.subnet) ||
        !dns1.fromString(config.dns1.isEmpty() ? "8.8.8.8" : config.dns1) ||
        !dns2.fromString(config.dns2.isEmpty() ? "8.8.4.4" : config.dns2))) {
        _lastError = "ETH_IP_INVALID: explicit valid IPv4 settings required"; return false;
    }
    const String addressKey = staticAddress
        ? config.staticIP + "/" + config.gateway + "/" + config.subnet + "/" + config.dns1 + "/" + config.dns2
        : String("DHCP");
    if (transport.initialized && owner == this) {
        if (!samePins(transport.pins, config.ethernet) || transport.addressKey != addressKey) {
            _lastError = "ETH_CONFIG_CHANGED: stop before reconfiguration"; return false;
        }
        const auto& p = config.ethernet;
        return FastBee::SpiBusInitializer::instance().acquireNetwork(&transport,
            CHIP_ETHERNET_DEDICATED_SPI, p.spiSck, p.spiMiso, p.spiMosi,
            p.csPin, p.rstPin, p.intPin, _lastError);
    }
    if ((transport.leased || transport.attempted) && !stopTransport(_lastError)) return false;
    if (ETH.handle() || ETH.netif()) {
        _lastError = "ETH_UNMANAGED: existing Ethernet driver is not owned here"; return false;
    }
    _instance = this;
    transport.pins = config.ethernet;
    const auto& p = transport.pins;
    if (!FastBee::SpiBusInitializer::instance().acquireNetwork(&transport,
        CHIP_ETHERNET_DEDICATED_SPI, p.spiSck, p.spiMiso, p.spiMosi,
        p.csPin, p.rstPin, p.intPin, _lastError)) { _instance = nullptr; return false; }
    transport.leased = true;
    auto failed = [&](const char* reason) {
        String cleanupError;
        const bool stopped = stopTransport(cleanupError);
        _lastError = reason;
        if (!stopped) _lastError += String("; ") + cleanupError;
        LOG_ERROR(_lastError);
        return false;
    };
#if CHIP_ETHERNET_DEDICATED_SPI
    transport.spi = new (std::nothrow) SPIClass(HSPI);
    if (!transport.spi || !transport.spi->begin(p.spiSck, p.spiMiso, p.spiMosi, p.csPin))
        return failed("ETH_SPI_INIT_FAILED");
#else
    transport.spi = &SPI;
#endif
    transport.eventId = WiFi.onEvent(onEthEvent);
    if (!transport.eventId) return failed("ETH_EVENT_REGISTER_FAILED");
    transport.acceptingEvents = true;
    transport.attempted = true;
    if (!ETH.begin(ETH_PHY_W5500, 0, p.csPin, p.intPin, p.rstPin, *transport.spi))
        return failed("ETH_BEGIN_FAILED");
    if (staticAddress && !ETH.config(ip, gateway, subnet, dns1, dns2))
        return failed("ETH_IP_CONFIG_FAILED");
    transport.addressKey = addressKey;
    transport.initialized = true;
    _lastError = "";
    LOG_INFO("EthernetAdapter: Initialized, waiting for link");
    return true;
#endif
}

bool EthernetAdapter::waitForConnection(uint32_t timeoutMs) {
    if (_instance.load() != this || !transport.initialized) return false;
    const unsigned long started = millis();
    while (!isConnected() && millis() - started < timeoutMs) delay(100);
    return isConnected();
}

bool EthernetAdapter::isConnected() const {
#if defined(ETH_SPI_SUPPORTS_CUSTOM) && ETH_SPI_SUPPORTS_CUSTOM
    FastBee::SpiBusOperation operation(0);
    return operation.locked() && _instance.load() == this && transport.initialized && ETH.linkUp() && ETH.hasIP();
#else
    return false;
#endif
}
IPAddress EthernetAdapter::localIP() const {
    FastBee::SpiBusOperation operation(0);
    return operation.locked() && isConnected() ? ETH.localIP() : IPAddress();
}
String EthernetAdapter::macAddress() const {
    FastBee::SpiBusOperation operation(0);
    return operation.locked() && _instance.load() == this && transport.initialized ? ETH.macAddress() : String();
}

bool EthernetAdapter::stop(String& error) {
    FastBee::SpiBusOperation operation;
    if (!operation.locked()) { error = _lastError = "SPI_BUSY: Ethernet transport retained"; return false; }
    if (_instance.load() != this) { error = ""; return true; }
    _ethClient.stop();
#if defined(ETH_SPI_SUPPORTS_CUSTOM) && ETH_SPI_SUPPORTS_CUSTOM
    if (!stopTransport(error)) { _lastError = error; return false; }
#endif
    _instance = nullptr;
    error = _lastError = "";
    return true;
}
void EthernetAdapter::disconnect() {
    String error;
    if (!stop(error)) LOG_ERROR(error);
}
Client* EthernetAdapter::getClient() { return &_ethClient; }
void EthernetAdapter::update() {}
String EthernetAdapter::getStatusString() const {
    FastBee::SpiBusOperation operation(0);
    if (!operation.locked()) return "SPI_BUSY: Ethernet status unavailable";
    if (!_lastError.isEmpty()) return _lastError;
    if (_instance.load() != this || !transport.initialized) return "uninitialized";
    return isConnected() ? "connected" : "disconnected";
}
void EthernetAdapter::onEthEvent(arduino_event_id_t event, arduino_event_info_t info) {
    (void)info;
    if (!transport.acceptingEvents) return;
    // Read connection status from ETH, not delayed events. Never dereference an
    // adapter from this event task, including a callback queued before removal.
    if (event == ARDUINO_EVENT_ETH_GOT_IP) LOG_INFO("EthernetAdapter: IP event received");
    else if (event == ARDUINO_EVENT_ETH_DISCONNECTED) LOG_WARNING("EthernetAdapter: Link disconnected");
}
#endif // FASTBEE_ENABLE_ETHERNET
