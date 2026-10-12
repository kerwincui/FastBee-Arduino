#include "peripherals/DigitalIoProviderFactory.h"
#include "core/PeripheralConfig.h"
#include "core/H1ParameterContract.h"
#include "peripherals/IoExpanderProvider.h"
#include "peripherals/MCP23017Provider.h"
#include "peripherals/drivers/H1ControlOutputs.h"
#include <cstring>
#include <new>

namespace FastBee {
std::unique_ptr<DigitalIoProvider> createDigitalIoProvider(
    const PeripheralConfig& config, uint8_t sda, uint8_t scl,
    uint32_t frequency, String& error) {
    const auto& params = config.params.ioExpander;
    if (config.type != PeripheralType::IO_EXPANDER ||
        !std::memchr(params.driver, '\0', sizeof(params.driver)) ||
        !std::memchr(params.resetPeripheralId, '\0', sizeof(params.resetPeripheralId)) ||
        !H1Parameter::accepts(H1Parameter::ioAddress, params.address) || sda == scl || sda == 255 || scl == 255 ||
        (frequency != 100000 && frequency != 400000)) {
        error = "IO_PROVIDER_INVALID"; return nullptr;
    }
    if (strcasecmp(params.driver, "PCF8574") == 0) {
        if (frequency != 100000 || params.resetPeripheralId[0]) {
            error = "IO_PROVIDER_INVALID: PCF requires 100kHz and no RESET reference"; return nullptr;
        }
        std::unique_ptr<IoExpanderProvider> provider(new (std::nothrow) IoExpanderProvider(params.address));
        if (!provider) { error = "IO_ALLOCATION_FAILED"; return nullptr; }
        if (!provider->acquire(sda, scl, error)) return nullptr;
        return provider;
    }
    if (strcasecmp(params.driver, "MCP23017") != 0 ||
        !std::memchr(params.resetPeripheralId, '\0', sizeof(params.resetPeripheralId)) ||
        !params.resetPeripheralId[0]) {
        error = "IO_PROVIDER_INVALID"; return nullptr;
    }
    std::unique_ptr<MCP23017NativeReset> reset(new (std::nothrow) MCP23017NativeReset);
    if (!reset) { error = "IO_ALLOCATION_FAILED"; return nullptr; }
    if (!reset->begin(params.resetPeripheralId, config.id)) { error = "RESET_CONTROL_UNAVAILABLE"; return nullptr; }
    std::unique_ptr<DigitalIoProvider> provider(new (std::nothrow)
        MCP23017Provider(sda, scl, frequency, params.address, std::move(reset)));
    if (!provider) error = "IO_ALLOCATION_FAILED";
    return provider;
}
}
