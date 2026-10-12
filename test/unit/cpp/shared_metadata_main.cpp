#define METADATA_ADDRESS_FUNCTION metadataAddressMain
#include "shared_metadata_address_probe.inc"
#include <cassert>
#include <cstring>
#include "shared_metadata_before_h1.inc"
#include "shared_metadata_before_thermostat.inc"

const void* metadataAddressPeer(unsigned);
using namespace FastBee;

// Shared storage must retain constexpr access and the original array bounds.
static_assert(sizeof(H1Parameter::pcaFields) / sizeof(H1Parameter::pcaFields[0]) == 11);
static_assert(sizeof(H1Parameter::commands) / sizeof(H1Parameter::commands[0]) == 6);
static_assert(H1Parameter::ioAddress.defaultValue == 0x20);
static_assert(H1Parameter::ioInterval.defaultValue == 20);
static_assert(H1Parameter::commands[0].value == &H1Parameter::duty);
static_assert(H1Parameter::commands[1].value == &H1Parameter::pulse);
static_assert(H1Parameter::commands[2].value == &H1Parameter::angle);
static_assert(kThermostatConfigFieldCount == 10);
static_assert(kThermostatNumbers[ThermostatSetpoint].defaultValue == 25);
static_assert(kThermostatNumbers[ThermostatMaxAge].maxValue == 3600);


void compareText(const char* actual, const char* before) {
    assert((actual == nullptr) == (before == nullptr));
    if (actual) assert(std::strcmp(actual, before) == 0);
}
template <typename Actual, typename Before>
void compareField(const Actual& actual, const Before& before) {
    compareText(actual.key, before.key);
    compareText(actual.unit, before.unit);
    assert(static_cast<unsigned>(actual.kind) == static_cast<unsigned>(before.kind));
    assert(actual.defaultValue == before.defaultValue);
    assert(actual.minimum == before.minimum && actual.maximum == before.maximum);
    assert(actual.hasMinimum == before.hasMinimum && actual.hasMaximum == before.hasMaximum);
    assert(actual.exclusiveMinimum == before.exclusiveMinimum);
    assert(actual.required == before.required && actual.maxBytes == before.maxBytes);
}
void compareContents() {
    namespace Before = FrozenMetadata::H1Parameter;
    compareField(H1Parameter::ioAddress, Before::ioAddress);
    compareField(H1Parameter::ioInterval, Before::ioInterval);
    compareField(H1Parameter::ioBus, Before::ioBus);
    compareField(H1Parameter::mcpReset, Before::mcpReset);
    compareField(H1Parameter::channel, Before::channel);
    compareField(H1Parameter::duty, Before::duty);
    compareField(H1Parameter::pulse, Before::pulse);
    compareField(H1Parameter::angle, Before::angle);
    for (size_t index = 0; index < 11; ++index)
        compareField(H1Parameter::pcaFields[index], Before::pcaFields[index]);
    for (size_t index = 0; index < 6; ++index) {
        const auto& actual = H1Parameter::commands[index];
        const auto& before = Before::commands[index];
        compareText(actual.id, before.id);
        assert((actual.value == nullptr) == (before.value == nullptr));
        if (actual.value) compareField(*actual.value, *before.value);
    }
    for (size_t index = 0; index < 5; ++index) {
        const auto& actual = kThermostatNumbers[index];
        const auto& before = FrozenMetadata::kThermostatNumbers[index];
        compareText(actual.key, before.key);
        compareText(actual.unit, before.unit);
        assert(actual.defaultValue == before.defaultValue);
        assert(actual.minValue == before.minValue && actual.maxValue == before.maxValue);
        assert(actual.integer == before.integer);
    }
    for (size_t index = 0; index < 4; ++index) {
        const auto& actual = kThermostatPolicies[index];
        const auto& before = FrozenMetadata::kThermostatPolicies[index];
        compareText(actual.key, before.key);
        compareText(actual.defaultValue, before.defaultValue);
        assert(actual.count == before.count);
        for (size_t choice = 0; choice < 2; ++choice)
            compareText(actual.choices[choice], before.choices[choice]);
    }
}

int main() {
    compareContents();
    for (unsigned index = 0; index < 12; ++index) {
        assert(metadataAddressMain(index));
        assert(metadataAddressMain(index) == metadataAddressPeer(index));
    }
    assert(!metadataAddressMain(12) && !metadataAddressPeer(12));
    const auto* peerCommands =
        static_cast<const H1Parameter::Command*>(metadataAddressPeer(9));
    assert(peerCommands[0].value == &H1Parameter::duty);
    assert(peerCommands[1].value == &H1Parameter::pulse);
    assert(peerCommands[2].value == &H1Parameter::angle);
    assert(peerCommands[3].value == nullptr && peerCommands[4].value == nullptr &&
           peerCommands[5].value == nullptr);
}
