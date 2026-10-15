#ifndef FASTBEE_PERIPHERAL_FORM_FIELDS_H
#define FASTBEE_PERIPHERAL_FORM_FIELDS_H

#include "core/PeripheralConfig.h"

namespace FastBee {

// HTTP-neutral source of form fields for peripheral parameter parsing. The
// route layer adapts WebHandlerContext/AsyncWebServerRequest to this
// interface; host tests adapt plain maps. Implementations must preserve the
// route layer's lookup semantics (POST body first, then URL query).
class PeripheralFormFields {
public:
    virtual ~PeripheralFormFields() = default;
    virtual bool has(const char* name) const = 0;
    virtual String value(const char* name, const char* fallback) const = 0;
    virtual int intValue(const char* name, int fallback) const = 0;
    virtual bool boolValue(const char* name, bool fallback) const = 0;
};

} // namespace FastBee

#endif // FASTBEE_PERIPHERAL_FORM_FIELDS_H
