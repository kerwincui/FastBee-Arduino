#ifndef FASTBEE_PERIPHERAL_PARAM_APPLY_H
#define FASTBEE_PERIPHERAL_PARAM_APPLY_H

#include <ArduinoJson.h>
#include "core/PeripheralConfig.h"
#include "core/PeripheralFormFields.h"

namespace FastBee {

// Pure form-field interpretation for peripheral parameter mutations. The
// route layer adapts HTTP requests to PeripheralFormFields; host tests adapt
// plain maps. No HTTP or Arduino types beyond String are referenced here.
float getRequestFloat(const PeripheralFormFields& form, const char* name, float fallback);
int clampParamInt(int value, int minimum, int maximum);

bool applyServoParams(const PeripheralFormFields& form, PeripheralConfig& config,
                      bool resetDefaults, String& errorMsg);
bool applyP1PeripheralParams(const PeripheralFormFields& form,
                             PeripheralConfig& config, bool resetDefaults,
                             String& errorMsg);
void applyDigitalInputParams(const PeripheralFormFields& form, PeripheralConfig& config);
void applyAnalogInputParams(const PeripheralFormFields& form, PeripheralConfig& config,
                            bool resetDefaults = false);

bool readDisplayRequestInteger(const PeripheralFormFields& form,
                               const char* name, int32_t fallback,
                               int32_t minimum, int32_t maximum,
                               int32_t& value, String& error);
bool applyDisplayRequestParameters(const PeripheralFormFields& form,
                                   PeripheralConfig& config,
                                   bool resetDefaults, String& error);

bool readIoFormInteger(const PeripheralFormFields& form,
                       const char* name, uint32_t fallback, uint32_t minimum,
                       uint32_t maximum, uint32_t& value, String& error,
                       bool allowHex = false);
bool applyIoPeripheralRequestParameters(const PeripheralFormFields& form,
                                        PeripheralConfig& config, bool resetDefaults,
                                        String& error);
bool applyPeripheralRequestParameters(const PeripheralFormFields& form,
                                      PeripheralConfig& config,
                                      bool resetDefaults, String& errorMsg);
bool applyAddTypeParams(const PeripheralFormFields& form, PeripheralConfig& config,
                        String& errorMsg);
bool applyUpdateTypeParams(const PeripheralFormFields& form, PeripheralConfig& config,
                           String& errorMsg);

} // namespace FastBee

#endif // FASTBEE_PERIPHERAL_PARAM_APPLY_H
