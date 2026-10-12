#include "core/H1ParameterContract.h"
#include "core/ThermostatConfigMetadata.h"

// The function name differs in each translation unit. Do not make this an
// inline helper: that would hide accidental internal-linkage table copies.
#ifndef METADATA_ADDRESS_FUNCTION
#error Define a unique translation-unit function name
#endif
const void* METADATA_ADDRESS_FUNCTION(unsigned index) {
    using namespace FastBee;
    switch (index) {
    case 0: return &H1Parameter::ioAddress;
    case 1: return &H1Parameter::ioInterval;
    case 2: return &H1Parameter::ioBus;
    case 3: return &H1Parameter::mcpReset;
    case 4: return H1Parameter::pcaFields;
    case 5: return &H1Parameter::channel;
    case 6: return &H1Parameter::duty;
    case 7: return &H1Parameter::pulse;
    case 8: return &H1Parameter::angle;
    case 9: return H1Parameter::commands;
    case 10: return kThermostatNumbers;
    case 11: return kThermostatPolicies;
    default: return nullptr;
    }
}
#undef METADATA_ADDRESS_FUNCTION
